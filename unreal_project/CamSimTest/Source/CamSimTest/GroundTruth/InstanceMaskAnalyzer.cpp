// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/InstanceMaskAnalyzer.h"
#include "GroundTruth/MaskGeometry.h"

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
		CamSimMask::FOrientedBox Obb() const
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
			return CamSimMask::MinAreaRect(CamSimMask::ConvexHull(MoveTemp(Pts)));
		}
	};

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
			for (int32 Y = M.MinY; Y <= M.MaxY; ++Y) Append((Ids.At(X, Y) & 0xFF) == V, 1);
			Append(false, H - 1 - static_cast<uint32>(M.MaxY));
		}
		Append(false, static_cast<uint32>(Ids.Width - 1 - M.MaxX) * H);
		return CamSimMask::EncodeCocoRle(Runs);
	}
}

void FInstanceMaskAnalyzer::Analyze(const FInstanceIdImage& Ids, TArray<FEntityAnnotationData>& Entities,
	int32 MinVisiblePixels, bool bSegmentation)
{
	if (!Ids.IsValid()) return;

	// Only stencil values some entity in this frame's snapshot carries get accumulators.
	TArray<int32> EntityOf; EntityOf.Init(INDEX_NONE, 256);
	for (int32 I = 0; I < Entities.Num(); ++I)
	{
		if (Entities[I].StencilValue != 0) EntityOf[Entities[I].StencilValue] = I;
	}
	TMap<uint8, FAcc> Modal, Amodal;
	for (int32 V = 1; V < 256; ++V)
	{
		if (EntityOf[V] == INDEX_NONE) continue;
		Modal.Add(V).Init(Ids.Height);
		Amodal.Add(V).Init(Ids.Height);
	}
	if (Modal.Num() == 0) return;

	for (int32 Y = 0; Y < Ids.Height; ++Y)
	{
		for (int32 X = 0; X < Ids.Width; ++X)
		{
			const uint16 P = Ids.At(X, Y);
			if (P == 0) continue;
			if (FAcc* A = Amodal.Find(static_cast<uint8>(P >> 8))) A->Add(X, Y);
			if (FAcc* M = Modal.Find(static_cast<uint8>(P & 0xFF))) M->Add(X, Y);
		}
	}

	TArray<FEntityAnnotationData> Kept;
	Kept.Reserve(Entities.Num());
	for (FEntityAnnotationData& E : Entities)
	{
		const FAcc* M = E.StencilValue ? Modal.Find(E.StencilValue) : nullptr;
		if (!M) { Kept.Add(MoveTemp(E)); continue; }   // untagged: projection fallback
		const FAcc& A = Amodal[E.StencilValue];
		if (M->Count < FMath::Max(1, MinVisiblePixels)) continue;   // fully (or nearly) hidden: not labelled
		E.bMaskMeasured = true;
		E.VisiblePixels = M->Count;
		E.AmodalPixels  = A.Count;
		E.ScreenBBox    = M->Box();
		E.AmodalBBox    = A.Box();
		E.Obb           = M->Obb();
		E.ObbAmodal     = A.Obb();
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
