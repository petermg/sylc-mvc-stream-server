#pragma once

#include "m2ts_reader.h"

#include <cstdint>
#include <deque>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace sylc_pgs {

struct PgsSegment {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> payload;
    std::int64_t pts90k = -1;
    std::int64_t dts90k = -1;
};

/**
 * Minimal Blu-ray Presentation Graphics demuxer.
 *
 * The selected PGS PID is reassembled from M2TS PES packets, then split into
 * complete PGS segments. Segment boundaries are preserved across PES packets,
 * including large ODS object sequences. Timestamps are normalized onto the
 * selected playlist's global 90 kHz timeline.
 */
class M2TSPgsDemuxer {
public:
    M2TSPgsDemuxer();
    ~M2TSPgsDemuxer();

    bool openStream(std::unique_ptr<std::istream> stream,
                    std::uint64_t file_size,
                    const std::string& label,
                    std::uint16_t pid,
                    std::string* error);

    bool selectAt(std::int64_t global_offset90k,
                  std::uint64_t byte_offset,
                  std::int64_t clip_in_raw90k,
                  std::int64_t anchor_raw90k,
                  std::string* error);

    bool readNextSegment(PgsSegment& segment, std::string* error);

    std::uint64_t packetCount() const { return packet_count_; }
    std::uint64_t pesCount() const { return pes_count_; }
    std::uint64_t segmentCount() const { return segment_count_; }
    std::uint64_t payloadBytes() const { return payload_bytes_; }
    std::int64_t firstPts90k() const { return first_pts90k_; }
    std::int64_t lastPts90k() const { return last_pts90k_; }
    std::string diagnostics() const;

private:
    struct PESState {
        std::vector<std::uint8_t> buffer;
        bool started = false;
    };

    std::unique_ptr<mvc_demux::M2TSReader> reader_;
    std::string label_;
    std::uint16_t selected_pid_ = 0;
    PESState pes_;
    std::deque<PgsSegment> ready_;
    std::vector<std::uint8_t> partial_segment_;
    std::int64_t partial_pts90k_ = -1;
    std::int64_t partial_dts90k_ = -1;
    std::int64_t global_offset90k_ = 0;
    std::int64_t clip_in_raw90k_ = -1;
    std::int64_t anchor_raw90k_ = -1;
    std::int64_t raw_origin90k_ = -1;
    bool eof_flushed_ = false;

    std::uint64_t packet_count_ = 0;
    std::uint64_t pes_count_ = 0;
    std::uint64_t segment_count_ = 0;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t malformed_pes_ = 0;
    std::uint64_t malformed_segments_ = 0;
    std::int64_t first_pts90k_ = -1;
    std::int64_t last_pts90k_ = -1;

    void resetRuntime();
    void processPacket(const mvc_demux::M2TSReader::TSPacket& packet);
    void finalizePES();
    void feedPayload(const std::vector<std::uint8_t>& payload,
                     std::int64_t pts90k,
                     std::int64_t dts90k);
    bool popReady(PgsSegment& segment);
    std::int64_t normalizeRaw(std::int64_t raw90k) const;

    static bool parsePES(const std::vector<std::uint8_t>& pes,
                         std::vector<std::uint8_t>* payload,
                         std::int64_t* pts90k,
                         std::int64_t* dts90k);
};

}  // namespace sylc_pgs
