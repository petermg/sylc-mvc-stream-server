#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mvc_demux {

/**
 * Reassembles one H.264/MVC access unit from one or more PES payload fragments.
 *
 * Blu-ray encoders are allowed to split one picture across multiple PES packets.
 * The first PES usually carries PTS, while subsequent fragments may repeat that
 * PTS or omit it. Feeding each PES independently to edge264 produces syntactically
 * decodable but visually incomplete pictures. This assembler retains Annex-B byte
 * order and closes the current picture only on a real next-picture boundary.
 */
class SsifAccessUnitAssembler {
public:
    enum class View {
        Base,
        Dependent,
    };

    struct CompletedAccessUnit {
        std::vector<std::uint8_t> data;
        std::int64_t pts = 0;
        bool hasPts = false;
        bool isKeyframe = false;
        std::uint32_t pesFragments = 0;
        std::uint32_t sliceNals = 0;
    };

    explicit SsifAccessUnitAssembler(View view) : view_(view) {}

    /**
     * Append one PES payload (PES header already removed). When this fragment proves
     * that a new picture has begun, the preceding complete access unit is returned.
     */
    bool consume(const std::vector<std::uint8_t>& annexB,
                 bool hasPts,
                 std::int64_t pts,
                 CompletedAccessUnit& emitted) {
        emitted = {};
        const NalSummary summary = summarize(annexB);
        if (annexB.empty()) return false;

        const bool ptsBoundary = currentHasVcl_ && hasPts && currentHasPts_
                && pts != currentPts_;
        const bool audBoundary = currentHasVcl_ && summary.startsWithAud;
        const bool boundary = ptsBoundary || audBoundary;

        bool didEmit = false;
        if (boundary) {
            didEmit = finalize(emitted);
            if (ptsBoundary) ++ptsBoundaries_;
            else ++audBoundaries_;
        } else if (!currentData_.empty()) {
            if (hasPts && currentHasPts_ && pts == currentPts_) {
                ++samePtsFragmentsMerged_;
            } else if (!hasPts) {
                ++noPtsContinuationFragmentsMerged_;
            }
        }

        // Bound malformed streams that repeat one PTS forever without an AUD. A real
        // 1080p Blu-ray picture is far below these limits; exceeding either means the
        // current access unit cannot be trusted.
        if (!boundary && !currentData_.empty()
                && (currentData_.size() + annexB.size() > kMaximumAccessUnitBytes
                        || currentPesFragments_ >= kMaximumPesFragments)) {
            ++incompleteAccessUnitsDiscarded_;
            clearCurrent();
        }

        // Parameter-set/AUD-only data before the first slice belongs to the following
        // picture. Do not discard it merely because the first slice-bearing PES supplies
        // a new PTS; retain byte order and adopt that first useful PTS.
        if (currentData_.empty()) {
            currentPts_ = pts;
            currentHasPts_ = hasPts;
        } else if (!currentHasVcl_ && hasPts
                && (!currentHasPts_ || pts != currentPts_)) {
            currentPts_ = pts;
            currentHasPts_ = true;
        }

        currentData_.insert(currentData_.end(), annexB.begin(), annexB.end());
        ++currentPesFragments_;
        // Re-scan the complete assembled byte sequence. PES boundaries may split an
        // Annex-B start code or NAL header, so fragment-local counting alone can miss
        // the first slice of a picture.
        const NalSummary completeSummary = summarize(currentData_);
        currentHasVcl_ = completeSummary.sliceNals > 0;
        currentIsKeyframe_ = completeSummary.idrSlices > 0;
        currentSliceNals_ = completeSummary.sliceNals;
        return didEmit;
    }

    /** Emit the final complete access unit, normally at EOF. */
    bool flush(CompletedAccessUnit& emitted) {
        emitted = {};
        return finalize(emitted);
    }

    /**
     * A real transport continuity failure invalidates the whole in-progress picture,
     * not just the current PES fragment.
     */
    void invalidate() {
        if (!currentData_.empty()) ++incompleteAccessUnitsDiscarded_;
        clearCurrent();
    }

    void reset(bool resetDiagnostics = true) {
        clearCurrent();
        if (resetDiagnostics) {
            samePtsFragmentsMerged_ = 0;
            noPtsContinuationFragmentsMerged_ = 0;
            ptsBoundaries_ = 0;
            audBoundaries_ = 0;
            incompleteAccessUnitsDiscarded_ = 0;
            firstPesFragments_ = 0;
            firstBytes_ = 0;
            firstSliceNals_ = 0;
            firstCaptured_ = false;
        }
    }

    std::uint64_t samePtsFragmentsMerged() const { return samePtsFragmentsMerged_; }
    std::uint64_t noPtsContinuationFragmentsMerged() const {
        return noPtsContinuationFragmentsMerged_;
    }
    std::uint64_t ptsBoundaries() const { return ptsBoundaries_; }
    std::uint64_t audBoundaries() const { return audBoundaries_; }
    std::uint64_t incompleteAccessUnitsDiscarded() const {
        return incompleteAccessUnitsDiscarded_;
    }
    std::uint32_t firstPesFragments() const { return firstPesFragments_; }
    std::uint64_t firstBytes() const { return firstBytes_; }
    std::uint32_t firstSliceNals() const { return firstSliceNals_; }

private:
    struct NalSummary {
        bool startsWithAud = false;
        std::uint32_t sliceNals = 0;
        std::uint32_t idrSlices = 0;
    };

    static bool startCodeAt(const std::vector<std::uint8_t>& data,
                            std::size_t offset,
                            std::size_t& length) {
        length = 0;
        if (offset + 3 > data.size() || data[offset] != 0 || data[offset + 1] != 0) {
            return false;
        }
        if (data[offset + 2] == 1) {
            length = 3;
            return true;
        }
        if (offset + 4 <= data.size() && data[offset + 2] == 0 && data[offset + 3] == 1) {
            length = 4;
            return true;
        }
        return false;
    }

    NalSummary summarize(const std::vector<std::uint8_t>& data) const {
        NalSummary summary;
        bool firstNal = true;
        for (std::size_t i = 0; i + 3 <= data.size();) {
            std::size_t startCodeLength = 0;
            if (!startCodeAt(data, i, startCodeLength)) {
                ++i;
                continue;
            }
            const std::size_t nalOffset = i + startCodeLength;
            if (nalOffset >= data.size()) break;
            const std::uint8_t nalType = data[nalOffset] & 0x1fU;
            if (firstNal) {
                summary.startsWithAud = nalType == 9;
                firstNal = false;
            }
            if (view_ == View::Base) {
                if (nalType == 1 || nalType == 5) {
                    ++summary.sliceNals;
                    if (nalType == 5) ++summary.idrSlices;
                }
            } else if (nalType == 20) {
                ++summary.sliceNals;
            }
            i = nalOffset + 1;
        }
        return summary;
    }

    bool finalize(CompletedAccessUnit& emitted) {
        if (!currentHasVcl_) {
            // Retain parameter-only data for the next slice-bearing fragment during normal
            // consume(). At EOF it cannot form a picture, so discard it quietly.
            clearCurrent();
            return false;
        }
        emitted.data = std::move(currentData_);
        emitted.pts = currentPts_;
        emitted.hasPts = currentHasPts_;
        emitted.isKeyframe = currentIsKeyframe_;
        emitted.pesFragments = currentPesFragments_;
        emitted.sliceNals = currentSliceNals_;
        if (!firstCaptured_) {
            firstPesFragments_ = emitted.pesFragments;
            firstBytes_ = emitted.data.size();
            firstSliceNals_ = emitted.sliceNals;
            firstCaptured_ = true;
        }
        clearCurrent();
        return true;
    }

    void clearCurrent() {
        currentData_.clear();
        currentPts_ = 0;
        currentHasPts_ = false;
        currentHasVcl_ = false;
        currentIsKeyframe_ = false;
        currentPesFragments_ = 0;
        currentSliceNals_ = 0;
    }

    static constexpr std::size_t kMaximumAccessUnitBytes = 32U * 1024U * 1024U;
    static constexpr std::uint32_t kMaximumPesFragments = 256U;

    View view_;
    std::vector<std::uint8_t> currentData_;
    std::int64_t currentPts_ = 0;
    bool currentHasPts_ = false;
    bool currentHasVcl_ = false;
    bool currentIsKeyframe_ = false;
    std::uint32_t currentPesFragments_ = 0;
    std::uint32_t currentSliceNals_ = 0;

    std::uint64_t samePtsFragmentsMerged_ = 0;
    std::uint64_t noPtsContinuationFragmentsMerged_ = 0;
    std::uint64_t ptsBoundaries_ = 0;
    std::uint64_t audBoundaries_ = 0;
    std::uint64_t incompleteAccessUnitsDiscarded_ = 0;
    std::uint32_t firstPesFragments_ = 0;
    std::uint64_t firstBytes_ = 0;
    std::uint32_t firstSliceNals_ = 0;
    bool firstCaptured_ = false;
};

}  // namespace mvc_demux
