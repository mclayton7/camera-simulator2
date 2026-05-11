// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#include "Diagnostics/PipelineLatencyTracker.h"

// -------------------------------------------------------------------------
// FPipelineLatencyTrackerConcurrencyTest
//
// Stresses the cross-thread write paths of FPipelineLatencyTracker:
//
//   - Three "producer" threads each write a disjoint subset of stages via
//     Mark() at ~10 kHz. The tracker design says each stage is written by
//     exactly one thread; we model that here.
//   - One "committer" thread calls CommitFrame() at ~1 kHz.
//   - The main thread polls ComputePercentiles() while the workers run.
//
// On x86 the test passes today because aligned 64-bit stores are atomic at
// the hardware level. On ARM (Apple Silicon, ARM Linux) torn reads of the
// non-atomic CurrentFrame.Stages slots are permitted by the C++ memory
// model. After Task 2 of the Phase 1 plan, all per-stage writes use
// SeqCst on TAtomic<uint64> (UE5's EMemoryOrder has only Relaxed and
// SequentiallyConsistent — SeqCst is the project-wide substitute for
// release/acquire; see CamSimCamera.h:191-193), so the test passes
// correctly on both platforms.
//
// This test is also intended to be run under ThreadSanitizer when available.
// On a build with -fsanitize=thread, the pre-fix code reports a data race
// on CurrentFrame.Stages; the post-fix code reports zero races.
// -------------------------------------------------------------------------

namespace
{

struct FStageProducer : public FRunnable
{
	FPipelineLatencyTracker* Tracker;
	EPipelineStage           StageA;
	EPipelineStage           StageB;
	FThreadSafeBool          bStopRequested;
	int64                    IterationCount = 0;

	FStageProducer(FPipelineLatencyTracker* InTracker, EPipelineStage A, EPipelineStage B)
		: Tracker(InTracker), StageA(A), StageB(B) {}

	virtual uint32 Run() override
	{
		while (!bStopRequested)
		{
			Tracker->Mark(StageA);
			Tracker->Mark(StageB);
			++IterationCount;
		}
		return 0;
	}

	virtual void Stop() override { bStopRequested = true; }
};

struct FCommitter : public FRunnable
{
	FPipelineLatencyTracker* Tracker;
	FThreadSafeBool          bStopRequested;
	int64                    CommitCount = 0;

	explicit FCommitter(FPipelineLatencyTracker* InTracker) : Tracker(InTracker) {}

	virtual uint32 Run() override
	{
		while (!bStopRequested)
		{
			Tracker->CommitFrame();
			++CommitCount;
			FPlatformProcess::SleepNoStats(0.001f);  // ~1 kHz
		}
		return 0;
	}

	virtual void Stop() override { bStopRequested = true; }
};

}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineLatencyTrackerConcurrencyTest,
	"CamSim.Phase1.Latency.ConcurrencyStress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPipelineLatencyTrackerConcurrencyTest::RunTest(const FString& Parameters)
{
	FPipelineLatencyTracker Tracker(/*BufferSize=*/512);

	// Three producers: each writes a disjoint pair of stages. Matches the
	// real call pattern documented in the tracker header.
	FStageProducer ProdA(&Tracker, EPipelineStage::CigiDequeue,      EPipelineStage::GameTickStart);
	FStageProducer ProdB(&Tracker, EPipelineStage::ReadbackIssue,    EPipelineStage::ReadbackComplete);
	FStageProducer ProdC(&Tracker, EPipelineStage::SensorStart,      EPipelineStage::SensorEnd);
	FCommitter     Committer(&Tracker);

	FRunnableThread* ThreadA  = FRunnableThread::Create(&ProdA,     TEXT("LatTestProdA"));
	FRunnableThread* ThreadB  = FRunnableThread::Create(&ProdB,     TEXT("LatTestProdB"));
	FRunnableThread* ThreadC  = FRunnableThread::Create(&ProdC,     TEXT("LatTestProdC"));
	FRunnableThread* ThreadCm = FRunnableThread::Create(&Committer, TEXT("LatTestCommit"));

	const bool bAllCreated =
		(ThreadA != nullptr) && (ThreadB != nullptr) &&
		(ThreadC != nullptr) && (ThreadCm != nullptr);

	if (!TestTrue(TEXT("all four threads created"), bAllCreated))
	{
		// Cleanly shut down any threads that DID start before unwinding,
		// otherwise they'd keep running against stack-allocated runnables.
		ProdA.Stop();      ProdB.Stop();      ProdC.Stop();      Committer.Stop();
		if (ThreadA)  { ThreadA->WaitForCompletion();  delete ThreadA;  }
		if (ThreadB)  { ThreadB->WaitForCompletion();  delete ThreadB;  }
		if (ThreadC)  { ThreadC->WaitForCompletion();  delete ThreadC;  }
		if (ThreadCm) { ThreadCm->WaitForCompletion(); delete ThreadCm; }
		return false;
	}

	// Run for ~2 seconds, polling ComputePercentiles() from the main thread
	// the whole time. This is the exact contention pattern the subsystem
	// uses in production.
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	int32 PollCount = 0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FPipelineLatencyTracker::FLatencyPercentiles P = Tracker.ComputePercentiles();
		(void)P;
		++PollCount;
		FPlatformProcess::SleepNoStats(0.005f);
	}

	ProdA.Stop();      ProdB.Stop();      ProdC.Stop();      Committer.Stop();
	ThreadA->WaitForCompletion();  delete ThreadA;
	ThreadB->WaitForCompletion();  delete ThreadB;
	ThreadC->WaitForCompletion();  delete ThreadC;
	ThreadCm->WaitForCompletion(); delete ThreadCm;

	// All workers made progress.
	TestTrue(TEXT("ProdA made progress"),     ProdA.IterationCount     > 0);
	TestTrue(TEXT("ProdB made progress"),     ProdB.IterationCount     > 0);
	TestTrue(TEXT("ProdC made progress"),     ProdC.IterationCount     > 0);
	TestTrue(TEXT("Committer made progress"), Committer.CommitCount    > 0);
	TestTrue(TEXT("Main thread polled"),      PollCount                > 0);

	// Final percentile read must not crash; tracker must hold at least one
	// committed record. We don't assert exact percentile values — under
	// heavy contention some Mark() writes land between CommitFrame() reads
	// and produce skipped or zero deltas, which is acceptable for a
	// best-effort sampling tracker.
	FPipelineLatencyTracker::FLatencyPercentiles Final = Tracker.ComputePercentiles();
	TestTrue(TEXT("Committed count > 0"), Tracker.GetCommittedCount() > 0);
	(void)Final;

	return true;
}
