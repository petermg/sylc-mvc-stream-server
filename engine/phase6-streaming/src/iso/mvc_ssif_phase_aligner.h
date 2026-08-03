#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "mvc_ssif_demuxer.h"

// Converts a stable signed dependent-minus-base SSIF timestamp offset into
// temporal frame alignment before edge264 sees the access units.
//
// A positive one-frame offset means the demuxer emitted:
//   base N + dependent N+1
// The aligner retains dependent N+1 and emits it with base N+1 from the next
// raw pair. A negative one-frame offset performs the symmetric operation.
class MvcSsifPhaseAligner final {
public:
    using FramePair = mvc_demux::MVCSSIFDemuxer::FramePair;

    enum class Mode {
        Unconfigured,
        Passthrough,
        DependentAheadOneFrame,
        DependentBehindOneFrame,
        Unsupported,
    };

    bool configure(std::int64_t calibrated_offset_ms,
                   std::int64_t nominal_frame_interval_ms,
                   std::int64_t residual_tolerance_ms) {
        reset();
        calibrated_offset_ms_ = calibrated_offset_ms;
        frame_interval_ms_ = std::max<std::int64_t>(1, nominal_frame_interval_ms);
        residual_tolerance_ms_ = std::max<std::int64_t>(0, residual_tolerance_ms);

        const double shift = static_cast<double>(calibrated_offset_ms_)
                / static_cast<double>(frame_interval_ms_);
        const long rounded_shift = std::lround(shift);
        if (rounded_shift < -1 || rounded_shift > 1) {
            mode_ = Mode::Unsupported;
            return false;
        }

        phase_shift_frames_ = static_cast<int>(rounded_shift);
        phase_residual_ms_ = calibrated_offset_ms_
                - static_cast<std::int64_t>(phase_shift_frames_) * frame_interval_ms_;
        if (std::llabs(phase_residual_ms_) > residual_tolerance_ms_) {
            mode_ = Mode::Unsupported;
            return false;
        }

        mode_ = phase_shift_frames_ > 0
                ? Mode::DependentAheadOneFrame
                : phase_shift_frames_ < 0
                        ? Mode::DependentBehindOneFrame
                        : Mode::Passthrough;
        locked_ = true;
        return true;
    }

    void reset() {
        mode_ = Mode::Unconfigured;
        locked_ = false;
        calibrated_offset_ms_ = 0;
        frame_interval_ms_ = 0;
        residual_tolerance_ms_ = 0;
        phase_shift_frames_ = 0;
        phase_residual_ms_ = 0;
        have_held_dependent_ = false;
        held_dependent_.clear();
        held_dependent_timestamp_ = 0;
        have_held_base_ = false;
        held_base_.clear();
        held_base_timestamp_ = 0;
        held_base_keyframe_ = false;
        raw_pairs_ = 0;
        corrected_pairs_ = 0;
        leading_base_discards_ = 0;
        leading_dependent_discards_ = 0;
        corrected_delta_first_ms_ = 0;
        corrected_delta_min_ms_ = 0;
        corrected_delta_max_ms_ = 0;
        corrected_delta_samples_.clear();
    }

    bool push(FramePair&& input, FramePair& output) {
        ++raw_pairs_;
        switch (mode_) {
            case Mode::Passthrough:
                output = std::move(input);
                recordCorrectedDelta(output);
                return true;

            case Mode::DependentAheadOneFrame: {
                if (!have_held_dependent_) {
                    held_dependent_ = std::move(input.dependentData);
                    held_dependent_timestamp_ = input.depTimestamp;
                    have_held_dependent_ = true;
                    ++leading_base_discards_;
                    return false;
                }
                output.baseData = std::move(input.baseData);
                output.timestamp = input.timestamp;
                output.isKeyframe = input.isKeyframe;
                output.dependentData = std::move(held_dependent_);
                output.depTimestamp = held_dependent_timestamp_;
                held_dependent_ = std::move(input.dependentData);
                held_dependent_timestamp_ = input.depTimestamp;
                recordCorrectedDelta(output);
                return true;
            }

            case Mode::DependentBehindOneFrame: {
                if (!have_held_base_) {
                    held_base_ = std::move(input.baseData);
                    held_base_timestamp_ = input.timestamp;
                    held_base_keyframe_ = input.isKeyframe;
                    have_held_base_ = true;
                    ++leading_dependent_discards_;
                    return false;
                }
                output.baseData = std::move(held_base_);
                output.timestamp = held_base_timestamp_;
                output.isKeyframe = held_base_keyframe_;
                output.dependentData = std::move(input.dependentData);
                output.depTimestamp = input.depTimestamp;
                held_base_ = std::move(input.baseData);
                held_base_timestamp_ = input.timestamp;
                held_base_keyframe_ = input.isKeyframe;
                recordCorrectedDelta(output);
                return true;
            }

            case Mode::Unconfigured:
            case Mode::Unsupported:
                return false;
        }
        return false;
    }

    void loseLock() { locked_ = false; }

    Mode mode() const { return mode_; }
    bool locked() const { return locked_; }
    std::int64_t calibratedOffsetMs() const { return calibrated_offset_ms_; }
    std::int64_t frameIntervalMs() const { return frame_interval_ms_; }
    int phaseShiftFrames() const { return phase_shift_frames_; }
    std::int64_t phaseResidualMs() const { return phase_residual_ms_; }
    std::uint64_t rawPairs() const { return raw_pairs_; }
    std::uint64_t correctedPairs() const { return corrected_pairs_; }
    std::uint64_t leadingBaseDiscards() const { return leading_base_discards_; }
    std::uint64_t leadingDependentDiscards() const { return leading_dependent_discards_; }
    std::int64_t correctedDeltaFirstMs() const { return corrected_delta_first_ms_; }
    std::int64_t correctedDeltaMinimumMs() const { return corrected_delta_min_ms_; }
    std::int64_t correctedDeltaMaximumMs() const { return corrected_delta_max_ms_; }

    std::int64_t correctedDeltaMedianMs() const {
        if (corrected_delta_samples_.empty()) return 0;
        auto ordered = corrected_delta_samples_;
        std::sort(ordered.begin(), ordered.end());
        return ordered[ordered.size() / 2];
    }

private:
    void recordCorrectedDelta(const FramePair& output) {
        const std::int64_t delta = static_cast<std::int64_t>(output.depTimestamp)
                - static_cast<std::int64_t>(output.timestamp);
        if (corrected_pairs_ == 0) {
            corrected_delta_first_ms_ = delta;
            corrected_delta_min_ms_ = delta;
            corrected_delta_max_ms_ = delta;
        } else {
            corrected_delta_min_ms_ = std::min(corrected_delta_min_ms_, delta);
            corrected_delta_max_ms_ = std::max(corrected_delta_max_ms_, delta);
        }
        if (corrected_delta_samples_.size() < 256) corrected_delta_samples_.push_back(delta);
        ++corrected_pairs_;
    }

    Mode mode_ = Mode::Unconfigured;
    bool locked_ = false;
    std::int64_t calibrated_offset_ms_ = 0;
    std::int64_t frame_interval_ms_ = 0;
    std::int64_t residual_tolerance_ms_ = 0;
    int phase_shift_frames_ = 0;
    std::int64_t phase_residual_ms_ = 0;

    bool have_held_dependent_ = false;
    std::vector<std::uint8_t> held_dependent_;
    std::uint64_t held_dependent_timestamp_ = 0;

    bool have_held_base_ = false;
    std::vector<std::uint8_t> held_base_;
    std::uint64_t held_base_timestamp_ = 0;
    bool held_base_keyframe_ = false;

    std::uint64_t raw_pairs_ = 0;
    std::uint64_t corrected_pairs_ = 0;
    std::uint64_t leading_base_discards_ = 0;
    std::uint64_t leading_dependent_discards_ = 0;
    std::int64_t corrected_delta_first_ms_ = 0;
    std::int64_t corrected_delta_min_ms_ = 0;
    std::int64_t corrected_delta_max_ms_ = 0;
    std::vector<std::int64_t> corrected_delta_samples_;
};
