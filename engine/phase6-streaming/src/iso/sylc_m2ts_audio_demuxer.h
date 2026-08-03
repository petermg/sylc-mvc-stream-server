#pragma once

#include "m2ts_reader.h"
#include "sylc_truehd_framer.h"

#include <cstdint>
#include <deque>
#include <istream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sylc_audio {

struct AudioTrackInfo {
    int index = -1;
    std::uint16_t pid = 0;
    std::uint8_t stream_type = 0;
    std::string profile;
    std::string mime;
    std::string language;
    int channels = 0;
    int sample_rate = 0;
    bool default_main = false;
    bool supported = false;
    bool truehd_major_sync = false;
    bool embedded_ac3_core = false;
    std::string bridge_format;
    std::string decode_path;
    std::string probe;
    std::int64_t first_raw_pts_us = -1;
};

struct CompressedAudioSample {
    std::vector<std::uint8_t> data;
    std::int64_t pts_us = -1;
    bool end_of_stream = false;
};

/**
 * Small Blu-ray M2TS audio demuxer derived from SyLC's native M2TS reader path.
 *
 * Android MediaExtractor is deliberately not involved. PAT/PMT tables identify
 * the Blu-ray audio PIDs and this class reassembles timestamped PES payloads for
 * DTS/DTS-HD/AC-3/E-AC-3 decoding by the app's existing audio backends.
 */
class M2TSAudioDemuxer {
public:
    M2TSAudioDemuxer();
    ~M2TSAudioDemuxer();

    bool openStream(std::unique_ptr<std::istream> stream, std::uint64_t file_size,
                    const std::string& label, std::string* error);
    bool openStream(std::unique_ptr<std::istream> stream, std::uint64_t file_size,
                    const std::string& label,
                    const std::vector<AudioTrackInfo>& declared_tracks,
                    std::uint64_t catalog_probe_byte,
                    std::string* error);
    void close();

    const std::vector<AudioTrackInfo>& tracks() const { return tracks_; }
    const std::string& label() const { return label_; }

    bool selectTrack(std::size_t track_index, std::int64_t target_global_us,
                     std::int64_t global_offset_us, std::string* error);
    bool selectTrackAt(std::size_t track_index, std::int64_t target_global_us,
                       std::int64_t global_offset_us, std::uint64_t byte_offset,
                       std::int64_t clip_in_raw_pts_us,
                       std::int64_t anchor_raw_pts_us,
                       std::uint64_t read_budget_bytes, std::string* error);
    bool readNextSample(CompressedAudioSample& sample, std::string* error);

    std::uint64_t packetCount() const { return packet_count_; }
    std::uint64_t pesCount() const { return pes_count_; }
    std::uint64_t payloadBytes() const { return payload_bytes_; }
    std::uint64_t skippedBeforeTarget() const { return skipped_before_target_; }
    std::int64_t firstPtsUs() const { return first_pts_us_; }
    std::int64_t lastPtsUs() const { return last_pts_us_; }
    std::string diagnostics() const;

private:
    struct PESState {
        std::vector<std::uint8_t> buffer;
        bool started = false;
    };

    struct ProbeState {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint8_t> truehd_bytes;
        std::vector<std::uint8_t> ac3_core_bytes;
        int samples = 0;
        int truehd_pes = 0;
        int ac3_core_pes = 0;
        std::int64_t first_pts90k = -1;
    };

    std::unique_ptr<mvc_demux::M2TSReader> reader_;
    std::string label_;
    std::vector<AudioTrackInfo> tracks_;
    std::map<std::uint16_t, PESState> scan_pes_;
    std::map<std::uint16_t, ProbeState> probes_;

    std::size_t selected_track_ = static_cast<std::size_t>(-1);
    std::uint16_t selected_pid_ = 0;
    std::uint8_t selected_stream_type_ = 0;
    bool selected_is_hd_ = false;
    bool selected_is_truehd_ = false;
    bool selected_truehd_core_fallback_ = false;
    PESState selected_pes_;
    std::vector<std::uint8_t> elementary_buffer_;
    TrueHdAccessUnitFramer truehd_framer_;
    std::deque<CompressedAudioSample> ready_samples_;
    std::int64_t raw_origin_pts_us_ = -1;
    std::int64_t next_frame_pts_us_ = -1;
    std::int64_t last_pes_raw_pts_us_ = -1;
    bool eof_flushed_ = false;
    std::int64_t target_global_us_ = 0;
    std::int64_t global_offset_us_ = 0;
    bool physical_seek_active_ = false;
    std::uint64_t physical_seek_byte_ = 0;
    std::uint64_t physical_seek_budget_bytes_ = 0;
    std::int64_t clip_in_raw_pts_us_ = -1;
    std::int64_t anchor_raw_pts_us_ = -1;
    std::int64_t anchor_global_pts_us_ = -1;
    std::uint64_t physical_seek_bytes_before_first_sample_ = 0;
    bool physical_seek_budget_exceeded_ = false;

    std::uint64_t packet_count_ = 0;
    std::uint64_t pes_count_ = 0;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t skipped_before_target_ = 0;
    std::uint64_t access_units_emitted_ = 0;
    std::uint64_t truehd_pes_seen_ = 0;
    std::uint64_t truehd_ac3_core_pes_seen_ = 0;
    std::uint64_t truehd_frames_emitted_ = 0;
    std::uint64_t truehd_major_syncs_seen_ = 0;
    std::uint64_t partial_frame_waits_ = 0;
    std::uint64_t bytes_discarded_before_sync_ = 0;
    std::size_t maximum_access_unit_bytes_ = 0;
    std::int64_t first_pts_us_ = -1;
    std::int64_t last_pts_us_ = -1;
    std::string catalog_discovery_method_ = "not attempted";
    std::uint64_t catalog_probe_byte_ = 0;
    std::size_t declared_track_count_ = 0;
    std::size_t pat_pmt_track_count_ = 0;
    std::size_t directly_verified_track_count_ = 0;

    bool resetSelectedTrack(std::size_t track_index, std::int64_t target_global_us,
                            std::int64_t global_offset_us, std::string* error);
    std::int64_t normalizeRawPts(std::int64_t raw_pts_us) const;
    bool scanCatalog(const std::vector<AudioTrackInfo>& declared_tracks,
                     std::uint64_t catalog_probe_byte,
                     std::string* error);
    void refreshTracksFromPrograms();
    void processProbePacket(const mvc_demux::M2TSReader::TSPacket& packet);
    void finalizeProbePES(std::uint16_t pid, PESState& state);
    void processSelectedPacket(const mvc_demux::M2TSReader::TSPacket& packet);
    void finalizeSelectedPES();
    void extractAccessUnits(bool end_of_stream);
    void extractTrueHdAccessUnits(bool end_of_stream);
    bool popReadySample(CompressedAudioSample& sample);

    static bool isAudioStreamType(std::uint8_t stream_type);
    static std::string defaultProfile(std::uint8_t stream_type);
    static std::string mimeForStreamType(std::uint8_t stream_type);
    static bool isSupportedStreamType(std::uint8_t stream_type);
    static bool parsePES(const std::vector<std::uint8_t>& pes,
                         std::vector<std::uint8_t>* payload,
                         std::int64_t* pts90k,
                         int* extended_stream_id = nullptr);
    static void inspectTrackProbe(AudioTrackInfo& track, const ProbeState& probe);
};

}  // namespace sylc_audio
