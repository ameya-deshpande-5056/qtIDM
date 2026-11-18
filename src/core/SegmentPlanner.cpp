#include "core/SegmentPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qtidm {

namespace {
SegmentPlanner::PlannedSegment makeFreshSegment(std::int64_t start,
                                                std::int64_t end,
                                                std::optional<SegmentPlanner::WorkerId> worker,
                                                std::int64_t nowMs)
{
    SegmentPlanner::PlannedSegment seg;
    seg.start = start;
    seg.cursor = start;
    seg.reserved = start;
    seg.end = end;
    seg.worker = worker;
    seg.receiving = false;
    seg.assignedAtMs = worker.has_value() ? nowMs : 0;
    seg.speedBytesPerSec = 0.0;
    return seg;
}
} // namespace

SegmentPlanner::SegmentPlanner(std::int64_t totalSize,
                               std::int64_t minSplitBytes,
                               std::int64_t preemptAfterMs)
    : totalSize_(std::max<std::int64_t>(0, totalSize))
    , minSplitBytes_(std::max<std::int64_t>(1, minSplitBytes))
    , preemptAfterMs_(std::max<std::int64_t>(0, preemptAfterMs))
{
    if (totalSize_ > 0) {
        segments_.push_back(makeFreshSegment(0, totalSize_, std::nullopt, 0));
    }
}

std::optional<SegmentPlanner> SegmentPlanner::resume(std::int64_t totalSize,
                                                     std::int64_t minSplitBytes,
                                                     const std::vector<SavedSegmentRange>& saved,
                                                     std::int64_t preemptAfterMs)
{
    if (totalSize < 0) {
        return std::nullopt;
    }
    std::int64_t expectedStart = 0;
    for (const auto& s : saved) {
        if (s.start != expectedStart || s.cursor < s.start || s.cursor > s.end || s.end <= s.start) {
            return std::nullopt;
        }
        expectedStart = s.end;
    }
    if (expectedStart != totalSize) {
        return std::nullopt;
    }

    SegmentPlanner planner(0, minSplitBytes, preemptAfterMs);
    planner.totalSize_ = totalSize;
    planner.segments_.reserve(saved.size());
    for (const auto& s : saved) {
        PlannedSegment seg;
        seg.start = s.start;
        seg.cursor = s.cursor;
        seg.reserved = s.cursor;
        seg.end = s.end;
        seg.worker = std::nullopt;
        seg.receiving = false;
        seg.assignedAtMs = 0;
        planner.segments_.push_back(seg);
    }
    return planner;
}

QVector<SegmentInfo> SegmentPlanner::planInitialSegments(std::int64_t totalSize,
                                                         int segmentCount,
                                                         std::int64_t alignment)
{
    QVector<SegmentInfo> result;
    const int count = std::max(1, segmentCount);
    if (totalSize <= 0) {
        SegmentInfo single;
        single.index = 0;
        single.start = 0;
        single.end = -1;
        single.status = DownloadStatus::Queued;
        result.append(single);
        return result;
    }

    const int effectiveCount = static_cast<int>(std::min<std::int64_t>(count, totalSize));
    const std::int64_t align = std::max<std::int64_t>(1, alignment);
    const bool useAlignment = (align > 1) && (totalSize >= static_cast<std::int64_t>(effectiveCount) * align);

    result.reserve(effectiveCount);
    std::int64_t cursor = 0;
    for (int i = 0; i < effectiveCount; ++i) {
        const int remainingParts = effectiveCount - i;
        const std::int64_t remainingBytes = totalSize - cursor;
        std::int64_t nextEndExclusive = totalSize;
        if (remainingParts > 1) {
            const std::int64_t idealChunk = (remainingBytes + remainingParts - 1) / remainingParts;
            std::int64_t candidate = cursor + idealChunk;
            if (useAlignment) {
                std::int64_t aligned = (candidate / align) * align;
                const std::int64_t minValid = cursor + align;
                const std::int64_t maxValid = totalSize - static_cast<std::int64_t>(remainingParts - 1) * align;
                if (aligned < minValid) {
                    aligned = ((minValid + align - 1) / align) * align;
                }
                if (aligned > maxValid) {
                    aligned = (maxValid / align) * align;
                }
                if (aligned > cursor && aligned <= totalSize - (remainingParts - 1)) {
                    candidate = aligned;
                }
            }
            nextEndExclusive = std::clamp<std::int64_t>(candidate, cursor + 1, totalSize - (remainingParts - 1));
        }

        SegmentInfo seg;
        seg.index = i;
        seg.start = cursor;
        seg.end = nextEndExclusive - 1;
        seg.written = 0;
        seg.durableWritten = 0;
        seg.status = DownloadStatus::Queued;
        result.append(seg);
        cursor = nextEndExclusive;
    }
    return result;
}

std::optional<std::int64_t> SegmentPlanner::computeAlignedSplitPoint(std::int64_t reservedPos,
                                                                     std::int64_t exclusiveEnd,
                                                                     std::int64_t minSplitBytes,
                                                                     std::int64_t alignment)
{
    const std::int64_t minSplit = std::max<std::int64_t>(1, minSplitBytes);
    if (reservedPos < 0 || exclusiveEnd <= reservedPos) {
        return std::nullopt;
    }
    const std::int64_t remaining = exclusiveEnd - reservedPos;
    const std::int64_t half = remaining / 2;
    if (half < minSplit) {
        return std::nullopt;
    }

    const std::int64_t rawMid = reservedPos + half;
    const std::int64_t align = std::max<std::int64_t>(1, alignment);
    if (align > 1) {
        const std::int64_t alignedDown = rawMid - (rawMid % align);
        if (alignedDown >= reservedPos + minSplit && (exclusiveEnd - alignedDown) >= minSplit) {
            return alignedDown;
        }
        const std::int64_t alignedUp = alignedDown + align;
        if (alignedUp >= reservedPos + minSplit && (exclusiveEnd - alignedUp) >= minSplit) {
            return alignedUp;
        }
    }
    if (rawMid > reservedPos && rawMid < exclusiveEnd) {
        return rawMid;
    }
    return std::nullopt;
}

void SegmentPlanner::setPreemptAfterMs(std::int64_t preemptAfterMs) noexcept
{
    preemptAfterMs_ = std::max<std::int64_t>(0, preemptAfterMs);
}

void SegmentPlanner::recordTtfbSample(std::int64_t ttfbMs,
                                      std::int64_t minPreemptMs,
                                      std::int64_t maxPreemptMs) noexcept
{
    const double sample = static_cast<double>(std::max<std::int64_t>(1, ttfbMs));
    if (!hasTtfbSample_) {
        ewmaTtfbMs_ = sample;
        hasTtfbSample_ = true;
    } else {
        ewmaTtfbMs_ = 0.7 * ewmaTtfbMs_ + 0.3 * sample;
    }
    const auto adaptive = static_cast<std::int64_t>(std::llround(ewmaTtfbMs_ * 2.0));
    preemptAfterMs_ = std::clamp(adaptive, std::max<std::int64_t>(0, minPreemptMs), std::max(minPreemptMs, maxPreemptMs));
}

std::vector<SegmentPlanner::SavedSegmentRange> SegmentPlanner::save() const
{
    std::vector<SavedSegmentRange> out;
    out.reserve(segments_.size());
    for (const auto& seg : segments_) {
        out.push_back({ seg.start, seg.cursor, seg.end });
    }
    return out;
}

std::int64_t SegmentPlanner::downloadedBytes() const noexcept
{
    std::int64_t total = 0;
    for (const auto& seg : segments_) {
        total += seg.committed();
    }
    return total;
}

bool SegmentPlanner::isComplete() const noexcept
{
    return std::all_of(segments_.cbegin(), segments_.cend(),
        [](const PlannedSegment& s) { return s.isComplete(); });
}

int SegmentPlanner::activeWorkers() const noexcept
{
    return static_cast<int>(std::count_if(segments_.cbegin(), segments_.cend(),
        [](const PlannedSegment& s) { return s.worker.has_value(); }));
}

int SegmentPlanner::receivingWorkers() const noexcept
{
    return static_cast<int>(std::count_if(segments_.cbegin(), segments_.cend(),
        [](const PlannedSegment& s) { return s.worker.has_value() && s.receiving; }));
}

int SegmentPlanner::findWorkerSegment(WorkerId worker) const noexcept
{
    for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
        if (segments_[i].worker == worker) {
            return i;
        }
    }
    return -1;
}

SegmentPlanner::Assignment SegmentPlanner::giveSegment(int index,
                                                       WorkerId worker,
                                                       std::optional<WorkerId> preempted,
                                                       std::int64_t nowMs)
{
    auto& seg = segments_[index];
    // Reset any uncommitted reservation from a prior worker before reassignment
    seg.reserved = seg.cursor;
    seg.worker = worker;
    seg.receiving = false;
    seg.assignedAtMs = nowMs;
    seg.speedBytesPerSec = 0.0;

    Assignment assignment;
    assignment.segmentIndex = index;
    assignment.from = seg.cursor;
    assignment.to = seg.end;
    assignment.alreadyWritten = seg.cursor - seg.start;
    assignment.preemptedWorker = preempted;
    assignment.wasSplit = false;
    assignment.splitSourceIndex = -1;
    return assignment;
}

std::optional<SegmentPlanner::Assignment> SegmentPlanner::assign(WorkerId worker,
                                                                 std::optional<std::int64_t> afterEnd,
                                                                 std::int64_t nowMs)
{
    release(worker);

    // 1. Unassigned, unfinished segment — prefer the segment continuing immediately after `afterEnd`.
    auto isOpen = [](const PlannedSegment& s) {
        return !s.worker.has_value() && s.unclaimed() > 0;
    };
    int pick = -1;
    if (afterEnd.has_value()) {
        for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
            if (isOpen(segments_[i]) && segments_[i].start == *afterEnd) {
                pick = i;
                break;
            }
        }
    }
    if (pick < 0) {
        for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
            if (isOpen(segments_[i])) {
                pick = i;
                break;
            }
        }
    }
    if (pick >= 0) {
        return giveSegment(pick, worker, std::nullopt, nowMs);
    }

    // 2. Preempt a stalled pending connection (has not received first byte after `preemptAfterMs_`).
    //    Prefer the adjacent segment at `afterEnd`, else the longest-waiting stalled segment.
    auto isPreemptable = [&](const PlannedSegment& s) {
        return s.worker.has_value()
            && !s.receiving
            && s.unclaimed() > 0
            && (nowMs - s.assignedAtMs) >= preemptAfterMs_;
    };
    int preemptIdx = -1;
    if (afterEnd.has_value()) {
        for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
            if (segments_[i].start == *afterEnd && isPreemptable(segments_[i])) {
                preemptIdx = i;
                break;
            }
        }
    }
    if (preemptIdx < 0) {
        std::int64_t longestWait = -1;
        for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
            if (!isPreemptable(segments_[i])) {
                continue;
            }
            const std::int64_t wait = nowMs - segments_[i].assignedAtMs;
            if (wait > longestWait) {
                longestWait = wait;
                preemptIdx = i;
            }
        }
    }
    if (preemptIdx >= 0) {
        const auto oldWorker = segments_[preemptIdx].worker;
        return giveSegment(preemptIdx, worker, oldWorker, nowMs);
    }

    // 3. Split the best remaining segment at a 4096-byte aligned boundary and steal the upper half.
    int bestIdx = -1;
    std::int64_t bestSplitPoint = -1;
    double bestScore = -1.0;
    std::int64_t bestUnclaimed = -1;
    bool anyHasSpeed = false;
    for (const auto& seg : segments_) {
        if (seg.speedBytesPerSec > 0.0) {
            anyHasSpeed = true;
            break;
        }
    }

    for (int i = 0; i < static_cast<int>(segments_.size()); ++i) {
        const auto& seg = segments_[i];
        const auto splitPoint = computeAlignedSplitPoint(seg.reserved, seg.end, minSplitBytes_, kPageAlign);
        if (!splitPoint.has_value()) {
            continue;
        }
        const std::int64_t rem = seg.unclaimed();
        if (anyHasSpeed) {
            const double speed = std::max(1024.0, seg.speedBytesPerSec);
            const double eta = static_cast<double>(rem) / speed;
            if (eta > bestScore) {
                bestScore = eta;
                bestUnclaimed = rem;
                bestIdx = i;
                bestSplitPoint = *splitPoint;
            }
        } else {
            if (rem > bestUnclaimed) {
                bestUnclaimed = rem;
                bestIdx = i;
                bestSplitPoint = *splitPoint;
            }
        }
    }

    if (bestIdx < 0 || bestSplitPoint < 0) {
        return std::nullopt;
    }

    const std::int64_t originalEnd = segments_[bestIdx].end;
    segments_[bestIdx].end = bestSplitPoint;
    segments_.insert(segments_.begin() + bestIdx + 1,
                     makeFreshSegment(bestSplitPoint, originalEnd, worker, nowMs));

    Assignment assignment;
    assignment.segmentIndex = bestIdx + 1;
    assignment.from = bestSplitPoint;
    assignment.to = originalEnd;
    assignment.alreadyWritten = 0;
    assignment.preemptedWorker = std::nullopt;
    assignment.wasSplit = true;
    assignment.splitSourceIndex = bestIdx;
    return assignment;
}

std::optional<std::int64_t> SegmentPlanner::room(WorkerId worker, std::int64_t at) const noexcept
{
    const int idx = findWorkerSegment(worker);
    if (idx < 0) {
        return std::nullopt;
    }
    const auto& seg = segments_[idx];
    return seg.end > at ? (seg.end - at) : 0;
}

SegmentPlanner::ClaimResult SegmentPlanner::claim(WorkerId worker, std::int64_t pos, std::int64_t len)
{
    const int idx = findWorkerSegment(worker);
    if (idx < 0 || len < 0) {
        return { true, 0, false };
    }
    auto& seg = segments_[idx];
    if (pos != seg.reserved) {
        return { true, 0, false };
    }
    const std::int64_t granted = std::min(len, seg.unclaimed());
    seg.reserved += granted;
    if (granted > 0) {
        seg.receiving = true;
    }
    return { false, granted, seg.reserved == seg.end };
}

bool SegmentPlanner::commit(WorkerId worker, std::int64_t pos, std::int64_t len)
{
    const int idx = findWorkerSegment(worker);
    if (idx < 0 || len < 0) {
        return false;
    }
    auto& seg = segments_[idx];
    if (pos != seg.cursor || seg.cursor + len > seg.reserved) {
        return false;
    }
    seg.cursor += len;
    return true;
}

bool SegmentPlanner::abortClaim(WorkerId worker, std::int64_t pos, std::int64_t len)
{
    const int idx = findWorkerSegment(worker);
    if (idx < 0 || len < 0) {
        return false;
    }
    auto& seg = segments_[idx];
    if (pos + len != seg.reserved || pos < seg.cursor) {
        return false;
    }
    seg.reserved = pos;
    return true;
}

void SegmentPlanner::release(WorkerId worker)
{
    const int idx = findWorkerSegment(worker);
    if (idx < 0) {
        return;
    }
    auto& seg = segments_[idx];
    seg.reserved = seg.cursor;
    seg.worker = std::nullopt;
    seg.receiving = false;
    seg.assignedAtMs = 0;
    seg.speedBytesPerSec = 0.0;
}

void SegmentPlanner::updateWorkerSpeed(WorkerId worker, double speedBytesPerSec)
{
    const int idx = findWorkerSegment(worker);
    if (idx < 0) {
        return;
    }
    segments_[idx].speedBytesPerSec = std::max(0.0, speedBytesPerSec);
}

} // namespace qtidm
