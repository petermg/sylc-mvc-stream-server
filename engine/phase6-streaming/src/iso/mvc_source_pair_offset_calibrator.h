#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Learns the stable signed dependent-minus-base PTS offset emitted by the SyLC
// SSIF matcher after a mid-stream seek. The demuxer already pairs nearest views
// within its Blu-ray-safe window; this layer verifies that the offset is stable
// rather than incorrectly requiring it to be near zero.
class MvcSourcePairOffsetCalibrator final {
public:
    enum class Status { Collecting, Calibrated, Failed };

    MvcSourcePairOffsetCalibrator(
            std::size_t minimum_inliers = 6,
            std::size_t maximum_samples = 12,
            std::int64_t maximum_absolute_offset_ms = 67,
            std::int64_t deviation_tolerance_ms = 10)
            : minimum_inliers_(std::max<std::size_t>(2, minimum_inliers)),
              maximum_samples_(std::max(maximum_samples, minimum_inliers_)),
              maximum_absolute_offset_ms_(std::max<std::int64_t>(1, maximum_absolute_offset_ms)),
              deviation_tolerance_ms_(std::max<std::int64_t>(0, deviation_tolerance_ms)) {}

    Status observe(std::int64_t signed_delta_ms) {
        if (status_ != Status::Collecting) return status_;
        samples_.push_back(signed_delta_ms);
        recomputeSummary();
        if (tryCalibrate()) return status_;
        if (samples_.size() >= maximum_samples_) status_ = Status::Failed;
        return status_;
    }

    bool accepts(std::int64_t signed_delta_ms) const {
        return status_ == Status::Calibrated
                && std::llabs(signed_delta_ms - calibrated_offset_ms_)
                        <= deviation_tolerance_ms_;
    }

    Status status() const { return status_; }
    bool calibrated() const { return status_ == Status::Calibrated; }
    bool failed() const { return status_ == Status::Failed; }
    std::size_t sampleCount() const { return samples_.size(); }
    std::size_t inlierCount() const { return calibrated_inliers_; }
    std::int64_t firstMs() const { return first_ms_; }
    std::int64_t minimumMs() const { return minimum_ms_; }
    std::int64_t medianMs() const { return median_ms_; }
    std::int64_t maximumMs() const { return maximum_ms_; }
    std::int64_t calibratedOffsetMs() const { return calibrated_offset_ms_; }
    std::int64_t deviationToleranceMs() const { return deviation_tolerance_ms_; }
    std::int64_t maximumAbsoluteOffsetMs() const { return maximum_absolute_offset_ms_; }

private:
    void recomputeSummary() {
        if (samples_.empty()) return;
        first_ms_ = samples_.front();
        auto ordered = samples_;
        std::sort(ordered.begin(), ordered.end());
        minimum_ms_ = ordered.front();
        maximum_ms_ = ordered.back();
        median_ms_ = ordered[ordered.size() / 2];
    }

    bool tryCalibrate() {
        if (samples_.size() < minimum_inliers_) return false;

        std::size_t best_count = 0;
        std::int64_t best_center = 0;
        std::vector<std::int64_t> best_cluster;
        for (const std::int64_t candidate : samples_) {
            std::vector<std::int64_t> cluster;
            for (const std::int64_t sample : samples_) {
                if (std::llabs(sample - candidate) <= deviation_tolerance_ms_) {
                    cluster.push_back(sample);
                }
            }
            if (cluster.size() > best_count
                    || (cluster.size() == best_count
                        && std::llabs(candidate) < std::llabs(best_center))) {
                best_count = cluster.size();
                best_center = candidate;
                best_cluster = std::move(cluster);
            }
        }

        if (best_count < minimum_inliers_) return false;
        std::sort(best_cluster.begin(), best_cluster.end());
        const std::int64_t robust_median = best_cluster[best_cluster.size() / 2];
        if (std::llabs(robust_median) > maximum_absolute_offset_ms_) return false;

        calibrated_offset_ms_ = robust_median;
        calibrated_inliers_ = best_cluster.size();
        status_ = Status::Calibrated;
        return true;
    }

    std::size_t minimum_inliers_;
    std::size_t maximum_samples_;
    std::int64_t maximum_absolute_offset_ms_;
    std::int64_t deviation_tolerance_ms_;
    std::vector<std::int64_t> samples_;
    Status status_ = Status::Collecting;
    std::size_t calibrated_inliers_ = 0;
    std::int64_t first_ms_ = 0;
    std::int64_t minimum_ms_ = 0;
    std::int64_t median_ms_ = 0;
    std::int64_t maximum_ms_ = 0;
    std::int64_t calibrated_offset_ms_ = 0;
};
