// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Encoder/EncoderThread.h"
#include "Encoder/IFrameSink.h"

// -------------------------------------------------------------------------
// ROADMAP 3A.1: at 30 fps input the encoder thread must keep up. Output
// pacing used to measure the interval from the END of the previous encode,
// so each frame cost interval + encode time (~26 fps with a 4 ms encode) and
// the 4-frame queue overflowed several times a second.
// -------------------------------------------------------------------------

namespace
{
	/** Sink that takes EncodeMs per frame, like a real encoder. */
	class FSlowCountingSink : public IFrameSink
	{
	public:
		explicit FSlowCountingSink(float InEncodeMs) : EncodeMs(InEncodeMs) {}
		virtual bool Open() override { return true; }
		virtual void EncodeFrame(const TArray<FColor>&, const FCamSimTelemetry&, uint64) override
		{
			FPlatformProcess::SleepNoStats(EncodeMs / 1000.0f);
			Encoded.Store(Encoded.Load() + 1);
		}
		virtual void Close() override {}
		virtual bool IsOpen() const override { return true; }
		virtual uint64 GetSuccessfulFrameCount() const override { return Encoded.Load(); }

		TAtomic<uint64> Encoded { 0 };
	private:
		float EncodeMs;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEncoderThreadKeepsUpTest,
	"CamSim.Encoder.Thread.KeepsUpAt30Fps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEncoderThreadKeepsUpTest::RunTest(const FString& Parameters)
{
	constexpr int32 Frames   = 60;       // 2 s at 30 fps
	constexpr double Interval = 1.0 / 30.0;

	FSlowCountingSink Sink(8.0f);        // 8 ms encode, well inside the 33 ms budget
	FEncoderThread Thread(&Sink, 30.0f);
	Thread.Start();

	const double Start = FPlatformTime::Seconds();
	for (int32 I = 0; I < Frames; ++I)
	{
		FProcessedFrame F;
		F.FrameIndex = I;
		Thread.Enqueue(MoveTemp(F));
		const double Next = Start + (I + 1) * Interval;  // steady 30 Hz producer
		const double Wait = Next - FPlatformTime::Seconds();
		if (Wait > 0.0) FPlatformProcess::SleepNoStats(static_cast<float>(Wait));
	}
	Thread.Stop();  // drains what's left

	TestEqual(TEXT("no frames dropped at 30 fps with an 8 ms encode"), Thread.GetDroppedFrameCount(), (uint64)0);
	TestEqual(TEXT("every frame encoded"), Sink.Encoded.Load(), (uint64)Frames);
	return true;
}
