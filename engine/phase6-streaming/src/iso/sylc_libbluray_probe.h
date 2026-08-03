#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sylc_bluray_posix {

struct AudioStream {
    std::uint16_t pid = 0;
    std::uint8_t coding_type = 0;
    std::uint8_t format = 0;
    std::uint8_t rate = 0;
    std::string language;
};

struct SubtitleStream {
    std::uint16_t pid = 0;
    std::uint8_t coding_type = 0;
    std::string language;
};

struct Segment {
    std::string clip;
    std::uint64_t start_time90k = 0;
    std::uint64_t in_time90k = 0;
    std::uint64_t out_time90k = 0;
    std::uint16_t base_video_pid = 0;
    std::vector<AudioStream> primary_audio;
    std::vector<SubtitleStream> presentation_graphics;
};

struct Selection {
    bool valid = false;
    std::uint32_t title_count = 0;
    std::uint32_t title_index = 0;
    std::uint32_t playlist = 0;
    std::uint64_t duration90k = 0;
    int main_title = -1;
    int version_major = 0;
    int version_minor = 0;
    int version_micro = 0;
    std::size_t decoys_seen = 0;
    std::string selection_rule;
    std::string candidate_summary;
    std::vector<Segment> segments;
};

// Uses the host libbluray runtime through dlopen(), so the source package does
// not require libbluray development headers. This is metadata selection only;
// actual SSIF/M2TS bytes are read by the bundled libudfread/SyLC path.
bool probeFeaturePath(const std::string& iso_path, Selection* output,
                      std::string* error);

}  // namespace sylc_bluray_posix
