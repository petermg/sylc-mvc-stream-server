#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sylc_iso {

struct ClpiEntryPointMap {
    std::uint16_t pid = 0;
    std::vector<std::int64_t> raw_pts_us;
    std::vector<std::uint64_t> byte_offsets;
};

struct ClpiAudioSeekAnchor {
    bool available = false;
    std::uint16_t pid = 0;
    std::size_t entry_index = 0;
    std::size_t entry_count = 0;
    std::int64_t requested_local_us = 0;
    std::int64_t desired_raw_pts_us = 0;
    std::int64_t anchor_raw_pts_us = 0;
    std::int64_t anchor_global_us = 0;
    std::int64_t clip_in_us = 0;
    std::uint64_t byte_offset = 0;
    std::string detail;
};

// Parse one CLPI EP_map. If wanted_pid is absent (or zero), the first EP_map
// stream is used, which is normally the base AVC video stream. Its random-access
// packet positions are valid physical positions in the multiplexed base M2TS and
// therefore also provide efficient starting points for audio demuxing.
bool parseClpiEntryPointMap(const std::vector<std::uint8_t>& data,
                            std::uint16_t wanted_pid,
                            ClpiEntryPointMap* output,
                            std::string* error);

// Choose the nearest EP_map entry at or before the requested playlist time.
// clip_in_45k is the MPLS IN_time. global_offset_us is the start of this clip in
// the concatenated selected-title timeline.

// Parse the CLPI extent-start-point extension used by Blu-ray 3D interleave
// metadata. The returned values are source-packet starts (192-byte packets).
bool parseClpiExtentStarts(const std::vector<std::uint8_t>& data,
                           std::vector<std::uint32_t>* starts,
                           std::string* error);

// Combine base/dependent extent starts with the base AVC EP_map to map each
// base IDR timestamp to the corresponding byte-aligned SSIF interleave unit.
bool buildExactSsifSeekTable(const std::vector<std::int64_t>& raw_pts_us,
                             const std::vector<std::uint64_t>& base_bytes,
                             const std::vector<std::uint32_t>& base_starts,
                             const std::vector<std::uint32_t>& dependent_starts,
                             std::uint64_t base_size,
                             std::uint64_t dependent_size,
                             std::vector<std::int64_t>* out_raw_pts_us,
                             std::vector<std::uint64_t>* out_ssif_bytes,
                             std::string* error);

bool chooseClpiAudioSeekAnchor(const ClpiEntryPointMap& map,
                               std::uint32_t clip_in_45k,
                               std::int64_t requested_global_us,
                               std::int64_t global_offset_us,
                               std::uint64_t m2ts_file_size,
                               ClpiAudioSeekAnchor* output,
                               std::string* error);

}  // namespace sylc_iso
