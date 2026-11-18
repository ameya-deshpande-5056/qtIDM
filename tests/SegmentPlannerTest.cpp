#include "core/SegmentPlanner.h"

#include <QtTest/QtTest>
#include <cstdint>
#include <vector>

using qtidm::SegmentPlanner;

class SegmentPlannerTest final : public QObject {
    Q_OBJECT

private slots:
    void initialSegmentsAlignAllInternalBoundariesTo4KiB()
    {
        // Odd unaligned file size (10,000,123 bytes) across 8 segments
        constexpr std::int64_t total = 10'000'123;
        const auto segs = SegmentPlanner::planInitialSegments(total, 8, SegmentPlanner::kPageAlign);
        QCOMPARE(segs.size(), 8);

        std::int64_t expectedStart = 0;
        for (int i = 0; i < segs.size(); ++i) {
            QCOMPARE(segs[i].index, i);
            QCOMPARE(segs[i].start, expectedStart);
            QVERIFY(segs[i].end >= segs[i].start);
            if (i > 0) {
                QCOMPARE(segs[i].start % SegmentPlanner::kPageAlign, 0);
            }
            expectedStart = segs[i].end + 1;
        }
        QCOMPARE(expectedStart, total);

        // Tiny file smaller than 4 KiB * segmentCount still tiles exactly with zero gaps/overlaps
        const auto tiny = SegmentPlanner::planInitialSegments(100, 8, SegmentPlanner::kPageAlign);
        QCOMPARE(tiny.size(), 8);
        std::int64_t tinyNext = 0;
        for (const auto& s : tiny) {
            QCOMPARE(s.start, tinyNext);
            QVERIFY(s.end >= s.start);
            tinyNext = s.end + 1;
        }
        QCOMPARE(tinyNext, 100);
    }

    void alignedSplitPointRespectsMinSplitAndPageBoundaries()
    {
        // Range [1234, 1'000'000), minSplit = 64 KiB -> split point MUST be 4096-aligned and >= 64 KiB each side
        const auto sp = SegmentPlanner::computeAlignedSplitPoint(1234, 1'000'000, 64 * 1024, 4096);
        QVERIFY(sp.has_value());
        QCOMPARE(*sp % 4096, 0);
        QVERIFY(*sp - 1234 >= 64 * 1024);
        QVERIFY(1'000'000 - *sp >= 64 * 1024);

        // Below 2 * minSplit -> refuses to split
        const auto tooSmall = SegmentPlanner::computeAlignedSplitPoint(0, 128 * 1024 - 1, 64 * 1024, 4096);
        QVERIFY(!tooSmall.has_value());

        // Exact 2 * minSplit with unaligned start -> falls back cleanly without violating minSplit
        const auto tight = SegmentPlanner::computeAlignedSplitPoint(100, 100 + 128 * 1024, 64 * 1024, 4096);
        QVERIFY(tight.has_value());
        QVERIFY(*tight - 100 >= 64 * 1024);
        QVERIFY((100 + 128 * 1024) - *tight >= 64 * 1024);
    }

    void twoPhaseClaimAndCommitPreventsSplittingActiveWrite()
    {
        SegmentPlanner planner(1024 * 1024, 64 * 1024, 1500);
        const auto a1 = planner.assign(1, std::nullopt, 1000);
        QVERIFY(a1.has_value());
        QCOMPARE(a1->from, 0);
        QCOMPARE(a1->to, 1024 * 1024);

        // Worker 1 claims [0, 500 KiB) — in flight, NOT committed yet!
        const auto c1 = planner.claim(1, 0, 500 * 1024);
        QVERIFY(!c1.lost);
        QCOMPARE(c1.granted, 500 * 1024);
        QCOMPARE(planner.downloadedBytes(), 0); // not committed yet

        // Worker 2 steals a split while Worker 1's 500 KiB claim is in flight
        const auto a2 = planner.assign(2, std::nullopt, 1200);
        QVERIFY(a2.has_value());
        QVERIFY(a2->wasSplit);
        // Split MUST be strictly >= reserved (500 KiB) AND 4096-aligned!
        QVERIFY(a2->from >= 500 * 1024);
        QCOMPARE(a2->from % 4096, 0);
        QCOMPARE(a2->to, 1024 * 1024);
        QCOMPARE(planner.segments()[0].end, a2->from);

        // Worker 1 commits its in-flight 500 KiB cleanly
        QVERIFY(planner.commit(1, 0, 500 * 1024));
        QCOMPARE(planner.downloadedBytes(), 500 * 1024);

        // Worker 1 tries to claim more than remaining room -> clamped cleanly to shrunk end!
        const std::int64_t remainingForW1 = a2->from - 500 * 1024;
        const auto c2 = planner.claim(1, 500 * 1024, 512 * 1024);
        QVERIFY(!c2.lost);
        QCOMPARE(c2.granted, remainingForW1);
        QVERIFY(c2.reachedBoundary);
        QVERIFY(planner.commit(1, 500 * 1024, remainingForW1));
        QVERIFY(planner.segments()[0].isComplete());
    }

    void abortClaimRollsBackReservationCleanly()
    {
        SegmentPlanner planner(256 * 1024, 64 * 1024, 1500);
        QVERIFY(planner.assign(1, std::nullopt, 0).has_value());
        const auto c = planner.claim(1, 0, 32 * 1024);
        QCOMPARE(c.granted, 32 * 1024);
        QCOMPARE(planner.segments()[0].reserved, 32 * 1024);
        QVERIFY(planner.abortClaim(1, 0, 32 * 1024));
        QCOMPARE(planner.segments()[0].reserved, 0);
        QCOMPARE(planner.segments()[0].cursor, 0);
    }

    void preemptsStalledPendingConnectionAfterThresholdAndNotBefore()
    {
        SegmentPlanner planner(1024 * 1024, 64 * 1024, 2000);
        // Worker 1 takes [0, 1 MiB) and starts receiving
        const auto a1 = planner.assign(1, std::nullopt, 1000);
        QVERIFY(a1.has_value());
        QCOMPARE(planner.claim(1, 0, 64 * 1024).granted, 64 * 1024);
        QVERIFY(planner.commit(1, 0, 64 * 1024));

        // Worker 2 splits and gets upper half [mid, 1 MiB) at t=1100, NEVER receives any bytes (stalled!)
        const auto a2 = planner.assign(2, std::nullopt, 1100);
        QVERIFY(a2.has_value());
        const std::int64_t splitPoint = a2->from;

        // Worker 1 finishes [0, splitPoint) at t=2500 (< 2000ms since worker 2 assigned at 1100!)
        const std::int64_t rest = splitPoint - 64 * 1024;
        QCOMPARE(planner.claim(1, 64 * 1024, rest).granted, rest);
        QVERIFY(planner.commit(1, 64 * 1024, rest));

        // At t=2500 (elapsed 1400ms < 2000ms), Worker 2 must NOT be preempted yet -> splits instead
        const auto early = planner.assign(3, splitPoint, 2500);
        QVERIFY(early.has_value());
        QVERIFY(!early->preemptedWorker.has_value());
        QVERIFY(early->wasSplit);

        // Now at t=3200 (elapsed 2100ms >= 2000ms), Worker 1 asks for work after `splitPoint` -> preempts stalled Worker 2!
        const auto preempt = planner.assign(1, splitPoint, 3200);
        QVERIFY(preempt.has_value());
        QVERIFY(preempt->preemptedWorker.has_value());
        QCOMPARE(*preempt->preemptedWorker, 2u);
        QCOMPARE(preempt->from, splitPoint);

        // Stalled Worker 2 waking up late gets ClaimResult::Lost!
        const auto lateClaim = planner.claim(2, splitPoint, 4096);
        QVERIFY(lateClaim.lost);
        QCOMPARE(lateClaim.granted, 0);
    }

    void speedAwareStealingPrefersSlowestSegmentAndAdaptiveTtfbWorks()
    {
        SegmentPlanner planner(4 * 1024 * 1024, 64 * 1024, 1500);
        QVERIFY(planner.assign(1, std::nullopt, 0).has_value());
        QVERIFY(planner.assign(2, std::nullopt, 0).has_value());
        // Both have ~2 MiB remaining, but Worker 1 is 100x slower than Worker 2
        planner.updateWorkerSpeed(1, 10'000.0);
        planner.updateWorkerSpeed(2, 10'000'000.0);

        const auto a3 = planner.assign(3, std::nullopt, 100);
        QVERIFY(a3.has_value());
        QVERIFY(a3->wasSplit);
        QCOMPARE(a3->splitSourceIndex, 0); // Must split slow Worker 1's segment!
        QCOMPARE(a3->from % 4096, 0);

        // Adaptive TTFB EWMA adjusts preemption window within [1500, 10000]
        planner.recordTtfbSample(2000);
        QCOMPARE(planner.preemptAfterMs(), 4000);
        planner.recordTtfbSample(100);
        QVERIFY(planner.preemptAfterMs() >= 1500 && planner.preemptAfterMs() < 4000);
    }

    void saveAndResumeRoundTripsAndRejectsCorruptLayouts()
    {
        SegmentPlanner planner(512 * 1024, 64 * 1024, 1500);
        QVERIFY(planner.assign(1, std::nullopt, 0).has_value());
        QVERIFY(planner.assign(2, std::nullopt, 0).has_value());
        QCOMPARE(planner.claim(1, 0, 16 * 1024).granted, 16 * 1024);
        QVERIFY(planner.commit(1, 0, 16 * 1024));

        const auto saved = planner.save();
        const auto restored = SegmentPlanner::resume(512 * 1024, 64 * 1024, saved, 1500);
        QVERIFY(restored.has_value());
        QCOMPARE(restored->downloadedBytes(), 16 * 1024);
        QCOMPARE(restored->save(), saved);

        // Gap or overlap must be rejected
        auto corrupt = saved;
        corrupt[0].end -= 10;
        QVERIFY(!SegmentPlanner::resume(512 * 1024, 64 * 1024, corrupt, 1500).has_value());
    }
};

QTEST_MAIN(SegmentPlannerTest)
#include "SegmentPlannerTest.moc"
