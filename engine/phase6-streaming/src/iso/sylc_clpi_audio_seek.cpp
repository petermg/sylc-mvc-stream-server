#include "sylc_clpi_audio_seek.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace sylc_iso {
namespace {

std::uint32_t be32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) return 0;
    return (static_cast<std::uint32_t>(data[offset]) << 24U)
         | (static_cast<std::uint32_t>(data[offset + 1]) << 16U)
         | (static_cast<std::uint32_t>(data[offset + 2]) << 8U)
         | static_cast<std::uint32_t>(data[offset + 3]);
}

class Bits {
public:
    Bits(const std::vector<std::uint8_t>& data, std::size_t byte_offset)
        : data_(data), bit_position_(byte_offset * 8U) {}

    bool read(unsigned count, std::uint32_t* value) {
        if (!value || count > 32U || bit_position_ + count > data_.size() * 8U) return false;
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t byte = bit_position_ >> 3U;
            const unsigned shift = 7U - static_cast<unsigned>(bit_position_ & 7U);
            result = (result << 1U) | ((data_[byte] >> shift) & 1U);
            ++bit_position_;
        }
        *value = result;
        return true;
    }

    bool skip(unsigned count) {
        if (bit_position_ + count > data_.size() * 8U) return false;
        bit_position_ += count;
        return true;
    }

    std::size_t bytePosition() const { return bit_position_ >> 3U; }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t bit_position_ = 0;
};

struct EntryHeader {
    std::uint16_t pid = 0;
    std::uint32_t coarse_count = 0;
    std::uint32_t fine_count = 0;
    std::uint32_t start_address = 0;
};

std::int64_t ticks45kToUs(std::uint32_t ticks) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(ticks) * 1000000ULL
            + 22500ULL) / 45000ULL);
}

}  // namespace

bool parseClpiEntryPointMap(const std::vector<std::uint8_t>& data,
                            std::uint16_t wanted_pid,
                            ClpiEntryPointMap* output,
                            std::string* error) {
    if (!output) {
        if (error) *error = "CLPI output is null";
        return false;
    }
    *output = {};
    if (data.size() < 28 || std::memcmp(data.data(), "HDMV", 4) != 0) {
        if (error) *error = "CLPI header is missing or truncated";
        return false;
    }
    const std::uint32_t cpi_start = be32(data, 16);
    if (cpi_start == 0 || cpi_start >= data.size()) {
        if (error) *error = "CLPI contains no CPI/EP_map section";
        return false;
    }

    Bits bits(data, cpi_start);
    std::uint32_t cpi_length = 0;
    std::uint32_t ignored = 0;
    std::uint32_t stream_count = 0;
    if (!bits.read(32, &cpi_length) || cpi_length == 0
            || !bits.skip(12) || !bits.read(4, &ignored)) {
        if (error) *error = "CLPI CPI header is malformed";
        return false;
    }
    const std::size_t ep_map_start = bits.bytePosition();
    if (!bits.skip(8) || !bits.read(8, &stream_count)
            || stream_count == 0 || stream_count > 64) {
        if (error) *error = "CLPI EP_map stream table is absent or unreasonable";
        return false;
    }

    std::vector<EntryHeader> headers;
    headers.reserve(stream_count);
    for (std::uint32_t i = 0; i < stream_count; ++i) {
        std::uint32_t pid = 0;
        std::uint32_t coarse = 0;
        std::uint32_t fine = 0;
        std::uint32_t start = 0;
        if (!bits.read(16, &pid) || !bits.skip(10) || !bits.read(4, &ignored)
                || !bits.read(16, &coarse) || !bits.read(18, &fine)
                || !bits.read(32, &start)) {
            if (error) *error = "CLPI EP_map stream header is truncated";
            return false;
        }
        headers.push_back({static_cast<std::uint16_t>(pid), coarse, fine, start});
    }

    const EntryHeader* selected = nullptr;
    if (wanted_pid != 0) {
        for (const auto& header : headers) {
            if (header.pid == wanted_pid) {
                selected = &header;
                break;
            }
        }
    }
    if (!selected) selected = &headers.front();
    if (selected->coarse_count == 0 || selected->fine_count == 0
            || selected->coarse_count > 1000000U || selected->fine_count > 4000000U) {
        if (error) *error = "CLPI EP_map entry counts are invalid";
        return false;
    }

    const std::size_t table_base = ep_map_start + selected->start_address;
    if (table_base + 4 > data.size()) {
        if (error) *error = "CLPI EP_map table address is outside the file";
        return false;
    }
    Bits coarse_bits(data, table_base);
    std::uint32_t fine_table_start = 0;
    if (!coarse_bits.read(32, &fine_table_start)) {
        if (error) *error = "CLPI coarse EP_map table is truncated";
        return false;
    }

    struct Coarse { std::uint32_t ref_fine = 0; std::uint32_t pts = 0; std::uint32_t spn = 0; };
    std::vector<Coarse> coarse_entries;
    coarse_entries.reserve(selected->coarse_count);
    for (std::uint32_t i = 0; i < selected->coarse_count; ++i) {
        Coarse entry;
        if (!coarse_bits.read(18, &entry.ref_fine)
                || !coarse_bits.read(14, &entry.pts)
                || !coarse_bits.read(32, &entry.spn)) {
            if (error) *error = "CLPI coarse EP_map entries are truncated";
            return false;
        }
        coarse_entries.push_back(entry);
    }

    if (table_base + fine_table_start >= data.size()) {
        if (error) *error = "CLPI fine EP_map table address is outside the file";
        return false;
    }
    Bits fine_bits(data, table_base + fine_table_start);
    struct Fine { std::uint32_t pts = 0; std::uint32_t spn = 0; };
    std::vector<Fine> fine_entries;
    fine_entries.reserve(selected->fine_count);
    for (std::uint32_t i = 0; i < selected->fine_count; ++i) {
        Fine entry;
        if (!fine_bits.skip(1) || !fine_bits.skip(3)
                || !fine_bits.read(11, &entry.pts)
                || !fine_bits.read(17, &entry.spn)) {
            if (error) *error = "CLPI fine EP_map entries are truncated";
            return false;
        }
        fine_entries.push_back(entry);
    }

    output->pid = selected->pid;
    output->raw_pts_us.reserve(fine_entries.size());
    output->byte_offsets.reserve(fine_entries.size());
    for (std::size_t coarse_index = 0; coarse_index < coarse_entries.size(); ++coarse_index) {
        const std::size_t begin = std::min<std::size_t>(
                coarse_entries[coarse_index].ref_fine, fine_entries.size());
        const std::size_t end = coarse_index + 1 < coarse_entries.size()
                ? std::min<std::size_t>(coarse_entries[coarse_index + 1].ref_fine,
                                        fine_entries.size())
                : fine_entries.size();
        for (std::size_t fine_index = begin; fine_index < end; ++fine_index) {
            const std::uint64_t pts90k = (((static_cast<std::uint64_t>(
                    coarse_entries[coarse_index].pts & ~1U)) << 18U)
                    + (static_cast<std::uint64_t>(fine_entries[fine_index].pts) << 8U)) * 2ULL;
            const std::uint64_t spn = (static_cast<std::uint64_t>(
                    coarse_entries[coarse_index].spn & ~0x1FFFFU))
                    + fine_entries[fine_index].spn;
            const std::int64_t pts_us = static_cast<std::int64_t>(
                    (pts90k * 1000000ULL + 45000ULL) / 90000ULL);
            const std::uint64_t byte = spn * 192ULL;
            if (!output->raw_pts_us.empty()) {
                if (pts_us < output->raw_pts_us.back() || byte < output->byte_offsets.back()) {
                    continue;
                }
                if (pts_us == output->raw_pts_us.back() && byte == output->byte_offsets.back()) {
                    continue;
                }
            }
            output->raw_pts_us.push_back(pts_us);
            output->byte_offsets.push_back(byte);
        }
    }
    if (output->raw_pts_us.empty()
            || output->raw_pts_us.size() != output->byte_offsets.size()) {
        *output = {};
        if (error) *error = "CLPI EP_map produced no usable entries";
        return false;
    }
    return true;
}

bool chooseClpiAudioSeekAnchor(const ClpiEntryPointMap& map,
                               std::uint32_t clip_in_45k,
                               std::int64_t requested_global_us,
                               std::int64_t global_offset_us,
                               std::uint64_t m2ts_file_size,
                               ClpiAudioSeekAnchor* output,
                               std::string* error) {
    if (!output) {
        if (error) *error = "CLPI audio seek output is null";
        return false;
    }
    *output = {};
    if (map.raw_pts_us.empty() || map.raw_pts_us.size() != map.byte_offsets.size()) {
        if (error) *error = "CLPI audio seek map is empty";
        return false;
    }
    const std::int64_t local_us = std::max<std::int64_t>(0, requested_global_us - global_offset_us);
    const std::int64_t clip_in_us = ticks45kToUs(clip_in_45k);
    const std::int64_t desired_raw_us = clip_in_us + local_us;
    auto upper = std::upper_bound(map.raw_pts_us.begin(), map.raw_pts_us.end(), desired_raw_us);
    std::size_t index = upper == map.raw_pts_us.begin()
            ? 0 : static_cast<std::size_t>((upper - map.raw_pts_us.begin()) - 1);

    // Ignore malformed tail entries that point beyond this physical M2TS.
    while (index > 0 && map.byte_offsets[index] >= m2ts_file_size) --index;
    if (map.byte_offsets[index] >= m2ts_file_size) {
        if (error) *error = "CLPI audio seek entry points beyond the M2TS file";
        return false;
    }

    output->available = true;
    output->pid = map.pid;
    output->entry_index = index;
    output->entry_count = map.raw_pts_us.size();
    output->requested_local_us = local_us;
    output->desired_raw_pts_us = desired_raw_us;
    output->anchor_raw_pts_us = map.raw_pts_us[index];
    output->clip_in_us = clip_in_us;
    output->anchor_global_us = global_offset_us
            + std::max<std::int64_t>(0, output->anchor_raw_pts_us - clip_in_us);
    output->byte_offset = (map.byte_offsets[index] / 192ULL) * 192ULL;
    std::ostringstream detail;
    detail << "CLPI EP_map PID=0x" << std::hex << output->pid << std::dec
           << " entry=" << output->entry_index << '/' << output->entry_count
           << " raw=" << output->anchor_raw_pts_us << " us"
           << " global=" << output->anchor_global_us << " us"
           << " byte=" << output->byte_offset;
    output->detail = detail.str();
    return true;
}


bool parseClpiExtentStarts(const std::vector<std::uint8_t>& data,
                           std::vector<std::uint32_t>* starts,
                           std::string* error) {
    if (!starts) {
        if (error) *error = "CLPI extent output is null";
        return false;
    }
    starts->clear();
    if (data.size() < 28 || std::memcmp(data.data(), "HDMV", 4) != 0) {
        if (error) *error = "CLPI header is missing or truncated";
        return false;
    }
    const std::uint32_t extension_start = be32(data, 24);
    if (extension_start == 0
            || static_cast<std::uint64_t>(extension_start) + 12ULL > data.size()
            || be32(data, extension_start) == 0) {
        if (error) *error = "CLPI contains no extent-start extension";
        return false;
    }
    std::size_t cursor = static_cast<std::size_t>(extension_start) + 4;
    if (cursor + 8 > data.size()) {
        if (error) *error = "CLPI extension header is truncated";
        return false;
    }
    cursor += 7;  // data_block_start_address (u32) + reserved (24 bits)
    const std::uint8_t extension_entries = data[cursor++];
    std::vector<std::uint32_t> best;
    for (std::uint32_t i = 0; i < extension_entries; ++i) {
        if (cursor + 12 > data.size()) break;
        const std::uint32_t extension_offset = be32(data, cursor + 4);
        cursor += 12;
        const std::size_t block_offset = static_cast<std::size_t>(extension_start)
                + extension_offset;
        const std::size_t count_offsets[2] = {block_offset + 4, block_offset};
        for (const std::size_t count_offset : count_offsets) {
            if (count_offset + 4 > data.size()) continue;
            const std::uint32_t count = be32(data, count_offset);
            const std::size_t points_offset = count_offset + 4;
            if (count < 2 || count > 2000000U
                    || points_offset + static_cast<std::uint64_t>(count) * 4ULL
                            > data.size()) {
                continue;
            }
            std::vector<std::uint32_t> candidate;
            candidate.reserve(count);
            bool monotonic = true;
            for (std::uint32_t j = 0; j < count; ++j) {
                const std::uint32_t point = be32(
                        data, points_offset + static_cast<std::size_t>(j) * 4U);
                if (!candidate.empty() && candidate.back() > point) monotonic = false;
                candidate.push_back(point);
            }
            if (monotonic && candidate.size() > best.size()) best = std::move(candidate);
            break;
        }
    }
    *starts = std::move(best);
    if (starts->size() < 2) {
        starts->clear();
        if (error) *error = "CLPI extent extension produced fewer than two points";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool buildExactSsifSeekTable(const std::vector<std::int64_t>& raw_pts_us,
                             const std::vector<std::uint64_t>& base_bytes,
                             const std::vector<std::uint32_t>& base_starts,
                             const std::vector<std::uint32_t>& dependent_starts,
                             std::uint64_t base_size,
                             std::uint64_t dependent_size,
                             std::vector<std::int64_t>* out_raw_pts_us,
                             std::vector<std::uint64_t>* out_ssif_bytes,
                             std::string* error) {
    if (!out_raw_pts_us || !out_ssif_bytes) {
        if (error) *error = "SSIF seek-table output is null";
        return false;
    }
    out_raw_pts_us->clear();
    out_ssif_bytes->clear();
    if (raw_pts_us.empty() || raw_pts_us.size() != base_bytes.size()
            || base_starts.size() < 2 || dependent_starts.size() < 2
            || base_size < 192 || dependent_size < 192) {
        if (error) *error = "Insufficient CLPI data for exact SSIF seek table";
        return false;
    }
    const std::size_t count = std::min(base_starts.size(), dependent_starts.size());
    const std::uint64_t base_packets = base_size / 192ULL;
    const std::uint64_t dependent_packets = dependent_size / 192ULL;
    auto extent_length = [](const std::vector<std::uint32_t>& starts,
                            std::size_t index, std::uint64_t total) -> std::uint64_t {
        const std::uint64_t next = index + 1 < starts.size()
                ? starts[index + 1] : total;
        return next >= starts[index] ? next - starts[index] : 0;
    };
    std::vector<std::uint64_t> cumulative_packets(count, 0);
    for (std::size_t i = 1; i < count; ++i) {
        cumulative_packets[i] = cumulative_packets[i - 1]
                + extent_length(dependent_starts, i - 1, dependent_packets)
                + extent_length(base_starts, i - 1, base_packets);
    }
    out_raw_pts_us->reserve(raw_pts_us.size());
    out_ssif_bytes->reserve(raw_pts_us.size());
    for (std::size_t i = 0; i < raw_pts_us.size(); ++i) {
        const std::uint64_t source_packet = base_bytes[i] / 192ULL;
        const auto upper = std::upper_bound(
                base_starts.begin(), base_starts.begin() + count,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        source_packet, std::numeric_limits<std::uint32_t>::max())));
        if (upper == base_starts.begin()) continue;
        const std::size_t unit = static_cast<std::size_t>(
                (upper - base_starts.begin()) - 1);
        if (unit >= count) continue;
        const std::uint64_t byte = cumulative_packets[unit] * 192ULL;
        if (!out_ssif_bytes->empty() && byte < out_ssif_bytes->back()) continue;
        out_raw_pts_us->push_back(raw_pts_us[i]);
        out_ssif_bytes->push_back(byte);
    }
    if (out_raw_pts_us->empty()
            || out_raw_pts_us->size() != out_ssif_bytes->size()) {
        out_raw_pts_us->clear();
        out_ssif_bytes->clear();
        if (error) *error = "CLPI metadata produced no exact SSIF seek entries";
        return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace sylc_iso
