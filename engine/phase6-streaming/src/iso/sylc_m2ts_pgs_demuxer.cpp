#include "sylc_m2ts_pgs_demuxer.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace sylc_pgs {
namespace {

constexpr std::size_t kMaximumPesBytes = 8u << 20u;
constexpr std::size_t kMaximumPgsSegmentBytes = 65535u;

std::int64_t parsePts(const std::uint8_t* value) {
    if (!value) return -1;
    return (static_cast<std::int64_t>((value[0] >> 1U) & 0x07U) << 30U)
            | (static_cast<std::int64_t>(value[1]) << 22U)
            | (static_cast<std::int64_t>((value[2] >> 1U) & 0x7fU) << 15U)
            | (static_cast<std::int64_t>(value[3]) << 7U)
            | static_cast<std::int64_t>((value[4] >> 1U) & 0x7fU);
}

}  // namespace

M2TSPgsDemuxer::M2TSPgsDemuxer() = default;
M2TSPgsDemuxer::~M2TSPgsDemuxer() = default;

void M2TSPgsDemuxer::resetRuntime() {
    pes_ = {};
    ready_.clear();
    partial_segment_.clear();
    partial_pts90k_ = -1;
    partial_dts90k_ = -1;
    raw_origin90k_ = -1;
    eof_flushed_ = false;
    packet_count_ = 0;
    pes_count_ = 0;
    segment_count_ = 0;
    payload_bytes_ = 0;
    malformed_pes_ = 0;
    malformed_segments_ = 0;
    first_pts90k_ = -1;
    last_pts90k_ = -1;
}

bool M2TSPgsDemuxer::openStream(std::unique_ptr<std::istream> stream,
                                std::uint64_t file_size,
                                const std::string& label,
                                std::uint16_t pid,
                                std::string* error) {
    reader_ = std::make_unique<mvc_demux::M2TSReader>();
    if (!reader_->openStream(std::move(stream), file_size, label)) {
        reader_.reset();
        if (error) *error = "Could not open M2TS source for PGS subtitle extraction";
        return false;
    }
    if (pid == 0) {
        reader_.reset();
        if (error) *error = "PGS subtitle PID is zero";
        return false;
    }
    label_ = label;
    selected_pid_ = pid;
    global_offset90k_ = 0;
    clip_in_raw90k_ = -1;
    anchor_raw90k_ = -1;
    resetRuntime();
    if (error) error->clear();
    return true;
}

bool M2TSPgsDemuxer::selectAt(std::int64_t global_offset90k,
                              std::uint64_t byte_offset,
                              std::int64_t clip_in_raw90k,
                              std::int64_t anchor_raw90k,
                              std::string* error) {
    if (!reader_) {
        if (error) *error = "PGS M2TS reader is not open";
        return false;
    }
    resetRuntime();
    global_offset90k_ = std::max<std::int64_t>(0, global_offset90k);
    clip_in_raw90k_ = clip_in_raw90k >= 0 ? clip_in_raw90k : -1;
    anchor_raw90k_ = anchor_raw90k >= 0 ? anchor_raw90k : -1;
    const std::uint64_t packet_size = static_cast<std::uint64_t>(
            std::max(1, reader_->getPacketSize()));
    const std::uint64_t aligned = (byte_offset / packet_size) * packet_size;
    if (aligned >= reader_->getFileSize()) {
        if (error) *error = "PGS seek byte is outside the selected M2TS clip";
        return false;
    }
    if (!reader_->seek(aligned)) {
        if (error) *error = "Could not seek M2TS reader for PGS extraction";
        return false;
    }
    if (error) error->clear();
    return true;
}

std::int64_t M2TSPgsDemuxer::normalizeRaw(std::int64_t raw90k) const {
    if (raw90k < 0) {
        if (last_pts90k_ >= 0) return last_pts90k_;
        if (anchor_raw90k_ >= 0 && clip_in_raw90k_ >= 0) {
            return global_offset90k_
                    + std::max<std::int64_t>(0, anchor_raw90k_ - clip_in_raw90k_);
        }
        return global_offset90k_;
    }
    if (clip_in_raw90k_ >= 0) {
        constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;
        constexpr std::uint64_t kHalfWrap = 1ULL << 32U;
        const std::uint64_t raw = static_cast<std::uint64_t>(raw90k) & kPtsMask;
        const std::uint64_t origin = static_cast<std::uint64_t>(clip_in_raw90k_) & kPtsMask;
        const std::uint64_t delta = (raw - origin) & kPtsMask;
        return global_offset90k_ + static_cast<std::int64_t>(delta < kHalfWrap ? delta : 0);
    }
    if (raw_origin90k_ >= 0) {
        return global_offset90k_
                + std::max<std::int64_t>(0, raw90k - raw_origin90k_);
    }
    return global_offset90k_;
}

void M2TSPgsDemuxer::processPacket(
        const mvc_demux::M2TSReader::TSPacket& packet) {
    if (packet.pid != selected_pid_ || !packet.payloadExists || packet.payload.empty()) {
        return;
    }
    if (packet.payloadUnitStartIndicator) {
        if (pes_.started && !pes_.buffer.empty()) finalizePES();
        pes_.buffer.clear();
        pes_.started = true;
    }
    if (!pes_.started) return;
    if (pes_.buffer.size() + packet.payload.size() > kMaximumPesBytes) {
        ++malformed_pes_;
        pes_ = {};
        return;
    }
    pes_.buffer.insert(pes_.buffer.end(), packet.payload.begin(), packet.payload.end());
}

void M2TSPgsDemuxer::finalizePES() {
    if (!pes_.started || pes_.buffer.empty()) {
        pes_ = {};
        return;
    }
    std::vector<std::uint8_t> payload;
    std::int64_t pts90k = -1;
    std::int64_t dts90k = -1;
    const bool valid = parsePES(pes_.buffer, &payload, &pts90k, &dts90k);
    pes_ = {};
    if (!valid || payload.empty()) {
        ++malformed_pes_;
        return;
    }
    ++pes_count_;
    payload_bytes_ += payload.size();
    if (raw_origin90k_ < 0 && pts90k >= 0) raw_origin90k_ = pts90k;
    const std::int64_t normalized_pts = normalizeRaw(pts90k);
    const std::int64_t normalized_dts = dts90k >= 0 ? normalizeRaw(dts90k) : normalized_pts;
    feedPayload(payload, normalized_pts, normalized_dts);
}

void M2TSPgsDemuxer::feedPayload(const std::vector<std::uint8_t>& payload,
                                 std::int64_t pts90k,
                                 std::int64_t dts90k) {
    std::size_t cursor = 0;

    if (!partial_segment_.empty()) {
        const std::size_t before = partial_segment_.size();
        partial_segment_.insert(partial_segment_.end(), payload.begin(), payload.end());
        if (partial_segment_.size() >= 3) {
            const std::size_t length = (static_cast<std::size_t>(partial_segment_[1]) << 8U)
                    | partial_segment_[2];
            const std::size_t total = 3u + length;
            if (length > kMaximumPgsSegmentBytes || total < 3u) {
                ++malformed_segments_;
                partial_segment_.clear();
                partial_pts90k_ = partial_dts90k_ = -1;
            } else if (partial_segment_.size() >= total) {
                PgsSegment segment;
                segment.type = partial_segment_[0];
                segment.payload.assign(partial_segment_.begin() + 3,
                                       partial_segment_.begin() + static_cast<std::ptrdiff_t>(total));
                segment.pts90k = partial_pts90k_;
                segment.dts90k = partial_dts90k_;
                ready_.push_back(std::move(segment));
                ++segment_count_;
                const std::size_t used_from_payload = total > before ? total - before : 0;
                cursor = std::min(used_from_payload, payload.size());
                partial_segment_.clear();
                partial_pts90k_ = partial_dts90k_ = -1;
            } else {
                return;
            }
        } else {
            return;
        }
    }

    while (cursor < payload.size()) {
        const std::size_t remaining = payload.size() - cursor;
        if (remaining < 3) {
            partial_segment_.assign(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                                    payload.end());
            partial_pts90k_ = pts90k;
            partial_dts90k_ = dts90k;
            return;
        }
        const std::uint8_t type = payload[cursor];
        const std::size_t length = (static_cast<std::size_t>(payload[cursor + 1]) << 8U)
                | payload[cursor + 2];
        const std::size_t total = 3u + length;
        if (length > kMaximumPgsSegmentBytes || total < 3u) {
            ++malformed_segments_;
            ++cursor;
            continue;
        }
        if (remaining < total) {
            partial_segment_.assign(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                                    payload.end());
            partial_pts90k_ = pts90k;
            partial_dts90k_ = dts90k;
            return;
        }
        PgsSegment segment;
        segment.type = type;
        segment.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(cursor + 3),
                               payload.begin() + static_cast<std::ptrdiff_t>(cursor + total));
        segment.pts90k = pts90k;
        segment.dts90k = dts90k;
        ready_.push_back(std::move(segment));
        ++segment_count_;
        cursor += total;
    }
}

bool M2TSPgsDemuxer::popReady(PgsSegment& segment) {
    if (ready_.empty()) return false;
    segment = std::move(ready_.front());
    ready_.pop_front();
    if (first_pts90k_ < 0) first_pts90k_ = segment.pts90k;
    last_pts90k_ = segment.pts90k;
    return true;
}

bool M2TSPgsDemuxer::readNextSegment(PgsSegment& segment, std::string* error) {
    segment = {};
    if (!reader_) {
        if (error) *error = "PGS M2TS reader is not open";
        return false;
    }
    if (popReady(segment)) return true;

    mvc_demux::M2TSReader::TSPacket packet;
    while (reader_->readPacket(packet)) {
        ++packet_count_;
        processPacket(packet);
        if (popReady(segment)) return true;
    }
    if (!eof_flushed_) {
        eof_flushed_ = true;
        finalizePES();
        if (popReady(segment)) return true;
    }
    if (!partial_segment_.empty()) {
        ++malformed_segments_;
        partial_segment_.clear();
    }
    if (error) error->clear();
    return false;
}

bool M2TSPgsDemuxer::parsePES(const std::vector<std::uint8_t>& pes,
                              std::vector<std::uint8_t>* payload,
                              std::int64_t* pts90k,
                              std::int64_t* dts90k) {
    if (!payload || !pts90k || !dts90k || pes.size() < 9
            || pes[0] != 0 || pes[1] != 0 || pes[2] != 1) {
        return false;
    }
    const std::size_t header = 9u + pes[8];
    if (header > pes.size()) return false;
    *pts90k = -1;
    *dts90k = -1;
    const std::uint8_t flags = pes[7] & 0xc0U;
    if (flags == 0x80U) {
        if (14u > header) return false;
        *pts90k = parsePts(pes.data() + 9);
        *dts90k = *pts90k;
    } else if (flags == 0xc0U) {
        if (19u > header) return false;
        *pts90k = parsePts(pes.data() + 9);
        *dts90k = parsePts(pes.data() + 14);
    }
    payload->assign(pes.begin() + static_cast<std::ptrdiff_t>(header), pes.end());
    return true;
}

std::string M2TSPgsDemuxer::diagnostics() const {
    std::ostringstream out;
    out << "SyLC PGS demuxer: source=" << label_
        << " pid=0x" << std::hex << std::setw(4) << std::setfill('0')
        << selected_pid_ << std::dec
        << " packets=" << packet_count_
        << " pes=" << pes_count_
        << " segments=" << segment_count_
        << " payload_bytes=" << payload_bytes_
        << " malformed_pes=" << malformed_pes_
        << " malformed_segments=" << malformed_segments_
        << " first_pts90k=" << first_pts90k_
        << " last_pts90k=" << last_pts90k_;
    return out.str();
}

}  // namespace sylc_pgs
