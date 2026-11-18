#pragma once

#include "core/DownloadTypes.h"

#include <QVector>
#include <cstdint>
#include <optional>
#include <vector>

namespace qtidm {

class SegmentPlanner final {
public:
    using WorkerId = std::uint32_t;

    static constexpr std::int64_t kPageAlign = 4096;
    static constexpr std::int64_t kDefaultMinSplit = 64 * 1024;
    static constexpr std::int64_t kDefaultPreemptAfterMs = 1500;

    struct PlannedSegment {
        std::int64_t start = 0;       // Inclusive start byte
        std::int64_t cursor = 0;      // [start, cursor) durably committed
        std::int64_t reserved = 0;    // [cursor, reserved) claimed & actively being written
        std::int64_t end = 0;         // Exclusive end byte
        std::optional<WorkerId> worker;
        bool receiving = false;       // True once worker has claimed >= 1 byte
        std::int64_t assignedAtMs = 0;
        double speedBytesPerSec = 0.0;

        [[nodiscard]] std::int64_t unclaimed() const noexcept { return end - reserved; }
        [[nodiscard]] std::int64_t committed() const noexcept { return cursor - start; }
        [[nodiscard]] bool isComplete() const noexcept { return cursor == end; }
    };

    struct SavedSegmentRange {
        std::int64_t start = 0;
        std::int64_t cursor = 0;
        std::int64_t end = 0;

        bool operator==(const SavedSegmentRange&) const = default;
    };

    struct Assignment {
        int segmentIndex = -1;
        std::int64_t from = 0;           // Inclusive start byte for worker
        std::int64_t to = 0;             // Exclusive end byte for worker
        std::int64_t alreadyWritten = 0; // Bytes already committed before `from`
        std::optional<WorkerId> preemptedWorker;
        bool wasSplit = false;
        int splitSourceIndex = -1;
    };

    struct ClaimResult {
        bool lost = false;
        std::int64_t granted = 0;
        bool reachedBoundary = false;
    };

    explicit SegmentPlanner(std::int64_t totalSize,
                            std::int64_t minSplitBytes = kDefaultMinSplit,
                            std::int64_t preemptAfterMs = kDefaultPreemptAfterMs);

    static std::optional<SegmentPlanner> resume(std::int64_t totalSize,
                                                std::int64_t minSplitBytes,
                                                const std::vector<SavedSegmentRange>& saved,
                                                std::int64_t preemptAfterMs = kDefaultPreemptAfterMs);

    static QVector<SegmentInfo> planInitialSegments(std::int64_t totalSize,
                                                    int segmentCount,
                                                    std::int64_t alignment = kPageAlign);

    static std::optional<std::int64_t> computeAlignedSplitPoint(std::int64_t reservedPos,
                                                                std::int64_t exclusiveEnd,
                                                                std::int64_t minSplitBytes,
                                                                std::int64_t alignment = kPageAlign);

    void setPreemptAfterMs(std::int64_t preemptAfterMs) noexcept;
    [[nodiscard]] std::int64_t preemptAfterMs() const noexcept { return preemptAfterMs_; }
    void recordTtfbSample(std::int64_t ttfbMs,
                          std::int64_t minPreemptMs = 1500,
                          std::int64_t maxPreemptMs = 10000) noexcept;

    [[nodiscard]] std::int64_t totalSize() const noexcept { return totalSize_; }
    [[nodiscard]] std::int64_t minSplitBytes() const noexcept { return minSplitBytes_; }
    [[nodiscard]] const std::vector<PlannedSegment>& segments() const noexcept { return segments_; }
    [[nodiscard]] std::vector<SavedSegmentRange> save() const;
    [[nodiscard]] std::int64_t downloadedBytes() const noexcept;
    [[nodiscard]] bool isComplete() const noexcept;
    [[nodiscard]] int activeWorkers() const noexcept;
    [[nodiscard]] int receivingWorkers() const noexcept;

    std::optional<Assignment> assign(WorkerId worker,
                                     std::optional<std::int64_t> afterEnd = std::nullopt,
                                     std::int64_t nowMs = 0);

    [[nodiscard]] std::optional<std::int64_t> room(WorkerId worker, std::int64_t at) const noexcept;
    ClaimResult claim(WorkerId worker, std::int64_t pos, std::int64_t len);
    bool commit(WorkerId worker, std::int64_t pos, std::int64_t len);
    bool abortClaim(WorkerId worker, std::int64_t pos, std::int64_t len);
    void release(WorkerId worker);
    void updateWorkerSpeed(WorkerId worker, double speedBytesPerSec);

private:
    Assignment giveSegment(int index, WorkerId worker, std::optional<WorkerId> preempted, std::int64_t nowMs);
    int findWorkerSegment(WorkerId worker) const noexcept;

    std::int64_t totalSize_ = 0;
    std::int64_t minSplitBytes_ = kDefaultMinSplit;
    std::int64_t preemptAfterMs_ = kDefaultPreemptAfterMs;
    double ewmaTtfbMs_ = 0.0;
    bool hasTtfbSample_ = false;
    std::vector<PlannedSegment> segments_;
};

} // namespace qtidm
