#pragma once

#include <cstdint>
#include <istream>
#include <memory>
#include <string>
#include <vector>

#include "sylc_clpi_audio_seek.h"

namespace sylc_iso {

struct DeclaredAudioStream {
    std::uint16_t pid = 0;
    std::uint8_t coding_type = 0;
    std::uint8_t format = 0;
    std::uint8_t rate = 0;
    std::string language;
};

struct FeatureSegment {
    std::string clip;
    std::string video_path;
    std::string audio_path;
    std::string dependent_path;
    double duration_s = 0.0;
    std::uint64_t start90k = 0;
    std::uint32_t in45k = 0;
    std::uint32_t out45k = 0;
    std::uint16_t base_video_pid = 0;
    std::vector<DeclaredAudioStream> declared_audio;
};

struct FeatureSelection {
    std::string playlist;
    std::string method;
    std::string kind;
    double duration_s = 0.0;
    std::size_t playlist_candidates = 0;
    std::size_t decoys_filtered = 0;
    std::size_t ssif_candidates = 0;
    std::size_t m2ts_candidates = 0;
    bool libbluray_authoritative = false;
    std::uint32_t authoritative_title_index = 0;
    std::uint32_t authoritative_title_count = 0;
    int authoritative_main_title = -1;
    int libbluray_major = 0;
    int libbluray_minor = 0;
    int libbluray_micro = 0;
    std::string candidate_summary;
    std::string fallback_detail;
    std::vector<FeatureSegment> segments;
};

struct VideoSeekMetadata {
    bool base_ep_available = false;
    bool exact_ssif_available = false;
    std::int64_t timeline_origin_ms = -1;
    std::vector<std::int64_t> base_raw_pts_ms;
    std::vector<std::uint64_t> base_bytes;
    std::vector<std::int64_t> ssif_raw_pts_ms;
    std::vector<std::uint64_t> ssif_bytes;
    std::string detail;
};

class FeatureVolume;

std::shared_ptr<FeatureVolume> openFeatureVolumePath(
        const std::string& iso_path, std::string* error);
const FeatureSelection& selection(const std::shared_ptr<FeatureVolume>& volume);
std::string diagnostics(const std::shared_ptr<FeatureVolume>& volume);

bool loadVideoSeekMetadata(const std::shared_ptr<FeatureVolume>& volume,
                           std::size_t segment_index,
                           VideoSeekMetadata* metadata,
                           std::string* error);

bool planAudioSeek(const std::shared_ptr<FeatureVolume>& volume,
                   std::size_t segment_index,
                   std::int64_t requested_global_us,
                   std::int64_t global_offset_us,
                   ClpiAudioSeekAnchor* anchor,
                   std::string* error);

std::unique_ptr<std::istream> openVideoStream(
        const std::shared_ptr<FeatureVolume>& volume, std::size_t segment_index,
        std::uint64_t* size, std::string* label, std::string* error);
std::unique_ptr<std::istream> openAudioStream(
        const std::shared_ptr<FeatureVolume>& volume, std::size_t segment_index,
        std::uint64_t* size, std::string* label, std::string* error);

}  // namespace sylc_iso
