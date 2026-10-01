// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/InstanceMaskAnalyzer.h"
#include "GroundTruth/MaskGeometry.h"
#include "CamSimTest.h"
#include "Stats/Stats.h"

// `stat CamSimGroundTruth` / Insights: the task-thread cost per annotated frame (ROADMAP 2.7, M6).
DECLARE_STATS_GROUP(TEXT("CamSim GroundTruth"), STATGROUP_CamSimGroundTruth, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Instance mask analyze"), STAT_CamSimInstanceMaskAnalyze, STATGROUP_CamSimGroundTruth);

namespace
{
	/** Per stencil value: counts, boxes, and each row's extreme x (the hull of a pixel set is the hull of its row extremes). */
	struct FAcc
	{
		int32 Count = 0;
		int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = -1, MaxY = -1;
		TArray<int32> RowMin, RowMax;
		void Init(int32 H) { RowMin.Init(MAX_int32, H); RowMax.Init(-1, H); }
		void Add(int32 X, int32 Y)
		{
			++Count;
			MinX = FMath::Min(MinX, X); MaxX = FMath::Max(MaxX, X);
			MinY = FMath::Min(MinY, Y); MaxY = FMath::Max(MaxY, Y);
			RowMin[Y] = FMath::Min(RowMin[Y], X); RowMax[Y] = FMath::Max(RowMax[Y], X);
		}
		FBox2D Box() const { return FBox2D(FVector2D(MinX, MinY), FVector2D(MaxX + 1, MaxY + 1)); }
		/** The OBB: one side along Axis when given (see VehicleAxis), else the minimum-area rectangle. */
		CamSimMask::FOrientedBox Obb(const FVector2D* Axis) const
		{
			TArray<FVector2D> Pts;
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				if (RowMax[Y] < 0) continue;
				for (const int32 X : { RowMin[Y], RowMax[Y] })
				{
					Pts.Add(FVector2D(X, Y)); Pts.Add(FVector2D(X + 1, Y)); Pts.Add(FVector2D(X, Y + 1)); Pts.Add(FVector2D(X + 1, Y + 1));
				}
			}
			const TArray<FVector2D> Hull = CamSimMask::ConvexHull(MoveTemp(Pts));
			return Axis ? CamSimMask::RectAlongAxis(Hull, *Axis) : CamSimMask::MinAreaRect(Hull);
		}
	};

	/**
	 * The projected vehicle axis (final review I3): rear-face centre (corners 0, 1, 4, 5) to front-face centre
	 * (2, 3, 6, 7) of the projected box3d, in output pixels. Used when the corners are valid and the axis is at
	 * least 0.25 x the silhouette's longer AABB side; otherwise (head-on, foreshortened, no box) false and the
	 * minimum-area rectangle is used. Min-area on fragmented or wedge-shaped silhouettes gives slivers and tilts.
	 */
	bool VehicleAxis(const FEntityAnnotationData& E, const FBox2D& Silhouette, FVector2D& OutAxis)
	{
		if (!E.bCornersValid) return false;
		const FVector2D* C = E.CornersPx;
		const FVector2D Rear = 0.25 * (C[0] + C[1] + C[4] + C[5]);
		const FVector2D Front = 0.25 * (C[2] + C[3] + C[6] + C[7]);
		OutAxis = Front - Rear;
		const FVector2D Ext = Silhouette.GetSize();
		const double Len = OutAxis.Size();
		return FMath::IsFinite(Len) && Len > 0.0 && Len >= 0.25 * FMath::Max(Ext.X, Ext.Y);
	}

	/** Column-major runs of (visible == V) over the image, visiting only the modal box's columns/rows. */
	FString EncodeModal(const FInstanceIdImage& Ids, uint8 V, const FAcc& M)
	{
		TArray<uint32> Runs;
		bool bCur = false;
		auto Append = [&](bool bValue, uint32 Len)
		{
			if (Len == 0) return;
			if (Runs.Num() == 0) { if (bValue) Runs.Add(0); Runs.Add(Len); bCur = bValue; return; }
			if (bValue == bCur) { Runs.Last() += Len; } else { Runs.Add(Len); bCur = bValue; }
		};
		const uint32 H = static_cast<uint32>(Ids.Height);
		Append(false, static_cast<uint32>(M.MinX) * H);
		for (int32 X = M.MinX; X <= M.MaxX; ++X)
		{
			Append(false, static_cast<uint32>(M.MinY));
			int32 Y = M.MinY;
			while (Y <= M.MaxY)
			{
				const bool bOn = (Ids.At(X, Y) & 0xFF) == V;
				const int32 Start = Y;
				while (Y <= M.MaxY && ((Ids.At(X, Y) & 0xFF) == V) == bOn) ++Y;
				Append(bOn, static_cast<uint32>(Y - Start));
			}
			Append(false, H - 1 - static_cast<uint32>(M.MaxY));
		}
		Append(false, static_cast<uint32>(Ids.Width - 1 - M.MaxX) * H);
		return CamSimMask::EncodeCocoRle(Runs);
	}
}

void FInstanceMaskAnalyzer::Analyze(const FInstanceIdImage& Ids, TArray<FEntityAnnotationData>& Entities,
	int32 MinVisiblePixels, bool bSegmentation)
{
	SCOPE_CYCLE_COUNTER(STAT_CamSimInstanceMaskAnalyze);
	if (!Ids.IsValid()) return;

	// A stencil value carried by two entities cannot be told apart in the mask: both fall back to projection.
	int32 Carriers[256] = {};
	for (const FEntityAnnotationData& E : Entities) { if (E.StencilValue != 0) ++Carriers[E.StencilValue]; }
	bool bDuplicate = false;
	for (int32 V = 1; V < 256; ++V) { if (Carriers[V] > 1) bDuplicate = true; }
	if (bDuplicate)
	{
		UE_LOG(LogCamSim, Warning, TEXT("InstanceMaskAnalyzer: stencil value shared by several entities; those entities use the projection fallback"));
	}

	// Flat tables: only stencil values exactly one entity carries get accumulators.
	int32 Tracked = 0;
	for (int32 V = 1; V < 256; ++V) { if (Carriers[V] == 1) ++Tracked; }
	if (Tracked == 0) return;
	TArray<FAcc> ModalStore, AmodalStore;
	ModalStore.SetNum(Tracked); AmodalStore.SetNum(Tracked);
	FAcc* ModalOf[256] = {};
	FAcc* AmodalOf[256] = {};
	int32 Slot = 0;
	for (int32 V = 1; V < 256; ++V)
	{
		if (Carriers[V] != 1) continue;
		ModalStore[Slot].Init(Ids.Height); AmodalStore[Slot].Init(Ids.Height);
		ModalOf[V] = &ModalStore[Slot]; AmodalOf[V] = &AmodalStore[Slot];
		++Slot;
	}

	const int32 WordsPerRow = Ids.Width / 2;
	const uint32* WordPtr = Ids.Words.GetData();
	for (int32 Y = 0; Y < Ids.Height; ++Y)
	{
		for (int32 K = 0; K < WordsPerRow; ++K, ++WordPtr)
		{
			const uint32 W = *WordPtr;
			if (W == 0) continue;
			for (int32 Half = 0; Half < 2; ++Half)
			{
				const uint32 P = Half ? (W >> 16) : (W & 0xFFFFu);
				if (P == 0) continue;
				const int32 X = K * 2 + Half;
				if (FAcc* A = AmodalOf[P >> 8]) A->Add(X, Y);
				if (FAcc* M = ModalOf[P & 0xFF]) M->Add(X, Y);
			}
		}
	}

	TArray<FEntityAnnotationData> Kept;
	Kept.Reserve(Entities.Num());
	for (FEntityAnnotationData& E : Entities)
	{
		const FAcc* M = E.StencilValue ? ModalOf[E.StencilValue] : nullptr;
		if (!M) { Kept.Add(MoveTemp(E)); continue; }   // untagged: projection fallback
		const FAcc& A0 = *AmodalOf[E.StencilValue];
		const FAcc& A = A0.Count > 0 ? A0 : *M;   // contract guard: amodal channel empty -> use the modal one
		if (M->Count < FMath::Max(1, MinVisiblePixels)) continue;   // fully (or nearly) hidden: not labelled
		E.bMaskMeasured = true;
		E.VisiblePixels = M->Count;
		E.AmodalPixels  = A.Count;
		E.ScreenBBox    = M->Box();
		E.AmodalBBox    = A.Box();
		// One axis for both boxes (coaxial), gated on the full (amodal) silhouette.
		FVector2D Axis;
		const bool bAxis = VehicleAxis(E, E.AmodalBBox, Axis);
		E.Obb           = M->Obb(bAxis ? &Axis : nullptr);
		E.ObbAmodal     = A.Obb(bAxis ? &Axis : nullptr);
		if (bSegmentation) E.SegmentationRle = EncodeModal(Ids, E.StencilValue, *M);
		if (E.Truncation < 0.0)
		{
			E.bTruncated = A.MinX == 0 || A.MinY == 0 || A.MaxX == Ids.Width - 1 || A.MaxY == Ids.Height - 1;
		}
		else
		{
			E.bTruncated = E.Truncation > 0.01;
		}
		Kept.Add(MoveTemp(E));
	}
	Entities = MoveTemp(Kept);
}
