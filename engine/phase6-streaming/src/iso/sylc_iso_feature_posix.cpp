#include "sylc_iso_feature_posix.h"
#include "sylc_libbluray_probe.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <streambuf>
#include <tuple>
#include <utility>
#include <vector>

#include <udfread/udfread.h>

namespace sylc_iso {
namespace {

constexpr int kUdfSeekSet = 0;
constexpr double kDecoyReplayRatio = 1.5;
constexpr std::size_t kMaxMplsBytes = 8u << 20;

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8U) | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24U)
            | (static_cast<std::uint32_t>(p[1]) << 16U)
            | (static_cast<std::uint32_t>(p[2]) << 8U)
            | static_cast<std::uint32_t>(p[3]);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool endsWithIgnoreCase(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    return lower(value.substr(value.size() - suffix.size())) == lower(suffix);
}

std::string stemOf(const std::string& name) {
    const std::size_t slash = name.find_last_of("/\\");
    const std::size_t begin = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = name.find_last_of('.');
    const std::size_t end = dot == std::string::npos || dot < begin ? name.size() : dot;
    return name.substr(begin, end - begin);
}

std::string nextClipId(const std::string& clip_id) {
    if (clip_id.size() != 5) return {};
    char* end = nullptr;
    const long value = std::strtol(clip_id.c_str(), &end, 10);
    if (!end || *end != '\0' || value < 0 || value >= 99999) return {};
    char output[6]{};
    std::snprintf(output, sizeof(output), "%05ld", value + 1);
    return output;
}

std::uint64_t udfFileSize(udfread* udf, const std::string& path) {
    if (!udf || path.empty()) return 0;
    UDFFILE* file = udfread_file_open(udf, path.c_str());
    if (!file) return 0;
    const std::int64_t size = udfread_file_size(file);
    udfread_file_close(file);
    return size > 0 ? static_cast<std::uint64_t>(size) : 0;
}
struct IndexedFile {
    std::string path;
    std::uint64_t size = 0;
};

struct PlaylistSegment {
    std::string clip;
    std::uint32_t in45k = 0;
    std::uint32_t out45k = 0;
    std::vector<DeclaredAudioStream> declared_audio;
};

struct PlaylistCandidate {
    std::string name;
    double duration_s = 0.0;
    std::vector<PlaylistSegment> segments;
    bool decoy = false;
    bool has_3d_extension = false;
};

std::vector<std::string> listDirectory(udfread* udf, const std::string& path) {
    std::vector<std::string> result;
    UDFDIR* dir = udfread_opendir(udf, path.c_str());
    if (!dir) return result;
    udfread_dirent entry{};
    while (udfread_readdir(dir, &entry)) {
        if (!entry.d_name) continue;
        std::string name(entry.d_name);
        if (name == "." || name == "..") continue;
        result.push_back(std::move(name));
    }
    udfread_closedir(dir);
    return result;
}

std::vector<std::uint8_t> readWholeFile(udfread* udf, const std::string& path,
                                        std::size_t maximum = kMaxMplsBytes) {
    std::vector<std::uint8_t> data;
    UDFFILE* file = udfread_file_open(udf, path.c_str());
    if (!file) return data;
    const std::int64_t size = udfread_file_size(file);
    if (size <= 0 || static_cast<std::uint64_t>(size) > maximum) {
        udfread_file_close(file);
        return data;
    }
    data.resize(static_cast<std::size_t>(size));
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t got = udfread_file_read(file, data.data() + done, data.size() - done);
        if (got <= 0) break;
        done += static_cast<std::size_t>(got);
    }
    udfread_file_close(file);
    if (done != data.size()) data.clear();
    return data;
}

bool parsePesPts90k(const std::uint8_t* pes, std::size_t size,
                      std::uint64_t* pts90k) {
    if (!pes || !pts90k || size < 14 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1) {
        return false;
    }
    const std::uint8_t pts_dts_flags = static_cast<std::uint8_t>((pes[7] >> 6U) & 0x03U);
    if (pts_dts_flags < 2 || pes[8] < 5) return false;
    const std::uint8_t* value = pes + 9;
    if ((value[0] & 1U) == 0 || (value[2] & 1U) == 0 || (value[4] & 1U) == 0) {
        return false;
    }
    *pts90k = ((static_cast<std::uint64_t>((value[0] >> 1U) & 0x07U)) << 30U)
            | (static_cast<std::uint64_t>(value[1]) << 22U)
            | (static_cast<std::uint64_t>((value[2] >> 1U) & 0x7fU) << 15U)
            | (static_cast<std::uint64_t>(value[3]) << 7U)
            | static_cast<std::uint64_t>((value[4] >> 1U) & 0x7fU);
    return true;
}

bool scanM2tsTailMaxPts90k(udfread* udf, const std::string& path,
                           std::uint64_t last_clpi_byte,
                           std::uint64_t first_raw_pts90k,
                           std::uint64_t* max_raw_pts90k,
                           std::uint64_t* scanned_bytes) {
    if (!udf || !max_raw_pts90k) return false;
    UDFFILE* file = udfread_file_open(udf, path.c_str());
    if (!file) return false;
    const std::int64_t size_signed = udfread_file_size(file);
    if (size_signed <= 0) {
        udfread_file_close(file);
        return false;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(size_signed);
    constexpr std::uint64_t kPacket = 192;
    constexpr std::uint64_t kMaxTail = 128ULL << 20U;
    constexpr std::uint64_t kClpiBackoff = 2ULL << 20U;
    const std::uint64_t from_clpi = last_clpi_byte > kClpiBackoff
            ? last_clpi_byte - kClpiBackoff : 0;
    const std::uint64_t from_tail = file_size > kMaxTail ? file_size - kMaxTail : 0;
    std::uint64_t start = std::max(from_clpi, from_tail);
    start -= start % kPacket;
    if (udfread_file_seek(file, static_cast<std::int64_t>(start), kUdfSeekSet) < 0) {
        udfread_file_close(file);
        return false;
    }
    constexpr std::size_t kPacketsPerRead = 4096;
    std::vector<std::uint8_t> buffer(kPacketsPerRead * kPacket);
    std::uint64_t total = 0;
    std::uint64_t best = first_raw_pts90k;
    bool found = false;
    constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;
    constexpr std::uint64_t kHalfWrap = 1ULL << 32U;
    const std::uint64_t first_mod = first_raw_pts90k & kPtsMask;
    while (start + total < file_size) {
        const std::uint64_t remaining = file_size - (start + total);
        const std::size_t wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), remaining));
        const ssize_t got_signed = udfread_file_read(file, buffer.data(), wanted);
        if (got_signed <= 0) break;
        const std::size_t got = static_cast<std::size_t>(got_signed);
        total += got;
        const std::size_t packets = got / kPacket;
        for (std::size_t i = 0; i < packets; ++i) {
            const std::uint8_t* ts = buffer.data() + i * kPacket + 4;
            if (ts[0] != 0x47 || (ts[1] & 0x40U) == 0) continue;
            const std::uint8_t adaptation_control =
                    static_cast<std::uint8_t>((ts[3] >> 4U) & 0x03U);
            if (adaptation_control == 0 || adaptation_control == 2) continue;
            std::size_t payload = 4;
            if (adaptation_control == 3) {
                const std::size_t adaptation_length = ts[payload];
                payload += 1 + adaptation_length;
            }
            if (payload >= 188) continue;
            std::uint64_t pts = 0;
            if (!parsePesPts90k(ts + payload, 188 - payload, &pts)) continue;
            const std::uint64_t delta = (pts - first_mod) & kPtsMask;
            if (delta >= kHalfWrap) continue;
            best = std::max(best, first_raw_pts90k + delta);
            found = true;
        }
        if (got < wanted) break;
    }
    udfread_file_close(file);
    if (scanned_bytes) *scanned_bytes = total;
    if (!found || best <= first_raw_pts90k) return false;
    *max_raw_pts90k = best;
    return true;
}

void applySingleClipPhysicalDurationFallback(udfread* udf,
                                             FeatureSelection* selection) {
    if (!udf || !selection || !selection->libbluray_authoritative
            || selection->authoritative_title_count != 1
            || selection->segments.size() != 1) {
        return;
    }
    FeatureSegment& segment = selection->segments.front();
    const std::string clpi_path = "/BDMV/CLIPINF/" + segment.clip + ".clpi";
    const auto clpi = readWholeFile(udf, clpi_path, 32u << 20u);
    ClpiEntryPointMap map;
    std::string parse_error;
    if (clpi.empty() || !parseClpiEntryPointMap(clpi, 0, &map, &parse_error)
            || map.raw_pts_us.size() < 2
            || map.raw_pts_us.size() != map.byte_offsets.size()) {
        return;
    }
    const auto usTo90k = [](std::int64_t us) -> std::uint64_t {
        return us > 0 ? (static_cast<std::uint64_t>(us) * 90ULL + 500ULL) / 1000ULL : 0;
    };
    const std::uint64_t first_raw90k = usTo90k(map.raw_pts_us.front());
    std::uint64_t last_raw90k = usTo90k(map.raw_pts_us.back());
    std::uint64_t tail_max90k = 0;
    std::uint64_t tail_scanned = 0;
    const bool tail_ok = scanM2tsTailMaxPts90k(
            udf, segment.audio_path, map.byte_offsets.back(), first_raw90k,
            &tail_max90k, &tail_scanned);
    if (tail_ok) last_raw90k = std::max(last_raw90k, tail_max90k);
    const std::uint64_t physical_duration90k = last_raw90k > first_raw90k
            ? (last_raw90k - first_raw90k) + 3750ULL : 0;
    const std::uint64_t playlist_duration90k = static_cast<std::uint64_t>(
            selection->duration_s * 90000.0 + 0.5);
    const bool implausibly_short = physical_duration90k >= 15ULL * 60ULL * 90000ULL
            && playlist_duration90k > 0
            && physical_duration90k > playlist_duration90k + 60ULL * 90000ULL
            && physical_duration90k > playlist_duration90k * 3ULL / 2ULL;
    std::ostringstream detail;
    detail << "physical-timeline probe: CLPI entries=" << map.raw_pts_us.size()
           << " first=" << (first_raw90k / 90000.0)
           << "s last-CLPI=" << (usTo90k(map.raw_pts_us.back()) / 90000.0)
           << "s tail-PES=" << (tail_ok ? tail_max90k / 90000.0 : -1.0)
           << "s scanned=" << tail_scanned
           << " bytes; derived duration=" << (physical_duration90k / 90000.0)
           << "s; fallback=" << (implausibly_short ? "APPLIED" : "not-needed");
    if (!selection->candidate_summary.empty()) selection->candidate_summary += '\n';
    selection->candidate_summary += detail.str();
    if (!implausibly_short) return;
    selection->duration_s = physical_duration90k / 90000.0;
    segment.duration_s = selection->duration_s;
    segment.in45k = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            first_raw90k / 2ULL, UINT32_MAX));
    segment.out45k = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            (first_raw90k + physical_duration90k) / 2ULL, UINT32_MAX));
    selection->method =
            "single-title physical CLPI/PES timeline fallback over truncated MPLS duration";
}

std::map<std::string, IndexedFile> indexFiles(udfread* udf, const std::string& directory,
                                               const std::vector<std::string>& extensions) {
    std::map<std::string, IndexedFile> result;
    for (const std::string& name : listDirectory(udf, directory)) {
        bool accepted = false;
        for (const std::string& extension : extensions) {
            if (endsWithIgnoreCase(name, extension)) { accepted = true; break; }
        }
        if (!accepted) continue;
        const std::string path = directory + "/" + name;
        UDFFILE* file = udfread_file_open(udf, path.c_str());
        if (!file) continue;
        const std::int64_t size = udfread_file_size(file);
        udfread_file_close(file);
        if (size > 0) result[stemOf(name)] = {path, static_cast<std::uint64_t>(size)};
    }
    return result;
}

bool parseMplsStream(const std::vector<std::uint8_t>& data, std::size_t* cursor,
                     DeclaredAudioStream* audio) {
    if (!cursor || !audio || *cursor >= data.size()) return false;
    const std::size_t entry_length = data[(*cursor)++];
    const std::size_t entry_start = *cursor;
    if (entry_length == 0 || entry_start + entry_length > data.size()) return false;
    const std::uint8_t stream_entry_type = data[entry_start];
    switch (stream_entry_type) {
        case 1:
            if (entry_length < 3) return false;
            audio->pid = be16(data.data() + entry_start + 1);
            break;
        case 2:
            if (entry_length < 5) return false;
            audio->pid = be16(data.data() + entry_start + 3);
            break;
        case 3:
        case 4:
            if (entry_length < 4) return false;
            audio->pid = be16(data.data() + entry_start + 2);
            break;
        default:
            return false;
    }
    *cursor = entry_start + entry_length;
    if (*cursor >= data.size()) return false;
    const std::size_t attr_length = data[(*cursor)++];
    const std::size_t attr_start = *cursor;
    if (attr_length == 0 || attr_start + attr_length > data.size()) return false;
    audio->coding_type = data[attr_start];
    if (attr_length >= 2) {
        audio->format = static_cast<std::uint8_t>((data[attr_start + 1] >> 4U) & 0x0fU);
        audio->rate = static_cast<std::uint8_t>(data[attr_start + 1] & 0x0fU);
    }
    if (attr_length >= 5) {
        audio->language.assign(reinterpret_cast<const char*>(data.data() + attr_start + 2), 3);
        while (!audio->language.empty() && audio->language.back() == '\0') {
            audio->language.pop_back();
        }
    }
    *cursor = attr_start + attr_length;
    return audio->pid != 0;
}

bool parseMplsPrimaryAudio(const std::vector<std::uint8_t>& data,
                           std::size_t play_item_body, std::size_t play_item_length,
                           std::vector<DeclaredAudioStream>* output) {
    if (!output || play_item_length < 32 || play_item_body + play_item_length > data.size()) {
        return false;
    }
    // Fixed PlayItem fields through still_time occupy 32 bytes.  Multi-angle
    // items append angle_count/flags and ten bytes per additional angle before STN.
    const std::uint16_t flags = be16(data.data() + play_item_body + 9);
    const bool multi_angle = (flags & 0x0010U) != 0;
    std::size_t cursor = play_item_body + 32;
    if (multi_angle) {
        if (cursor + 2 > play_item_body + play_item_length) return false;
        const std::uint8_t angle_count = std::max<std::uint8_t>(1, data[cursor]);
        cursor += 2;
        const std::size_t extra_angles = static_cast<std::size_t>(angle_count - 1) * 10U;
        if (cursor + extra_angles > play_item_body + play_item_length) return false;
        cursor += extra_angles;
    }
    if (cursor + 16 > play_item_body + play_item_length) return false;
    const std::uint16_t stn_length = be16(data.data() + cursor);
    const std::size_t stn_body = cursor + 2;
    const std::size_t stn_end = stn_body + stn_length;
    if (stn_length < 14 || stn_end > play_item_body + play_item_length || stn_end > data.size()) {
        return false;
    }
    const std::uint8_t num_video = data[stn_body + 2];
    const std::uint8_t num_audio = data[stn_body + 3];
    cursor = stn_body + 14;

    auto skipStream = [&]() -> bool {
        if (cursor >= stn_end) return false;
        const std::size_t entry_length = data[cursor++];
        if (cursor + entry_length > stn_end) return false;
        cursor += entry_length;
        if (cursor >= stn_end) return false;
        const std::size_t attr_length = data[cursor++];
        if (cursor + attr_length > stn_end) return false;
        cursor += attr_length;
        return true;
    };
    for (std::uint8_t i = 0; i < num_video; ++i) {
        if (!skipStream()) return false;
    }
    for (std::uint8_t i = 0; i < num_audio; ++i) {
        DeclaredAudioStream stream;
        if (!parseMplsStream(data, &cursor, &stream) || cursor > stn_end) return false;
        output->push_back(std::move(stream));
    }
    return true;
}

bool parseMpls(const std::vector<std::uint8_t>& data, PlaylistCandidate* output) {
    if (!output || data.size() < 20 || std::memcmp(data.data(), "MPLS", 4) != 0) return false;
    const std::uint32_t playlist_start = be32(data.data() + 8);
    if (playlist_start + 10 > data.size()) return false;
    const std::uint16_t count = be16(data.data() + playlist_start + 6);
    std::size_t q = static_cast<std::size_t>(playlist_start) + 10;
    std::uint64_t total45k = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (q + 2 > data.size()) return false;
        const std::uint16_t item_len = be16(data.data() + q);
        const std::size_t body = q + 2;
        if (body + item_len > data.size()) return false;
        if (item_len >= 20) {
            std::string clip(reinterpret_cast<const char*>(data.data() + body), 5);
            const std::uint32_t in_t = be32(data.data() + body + 12);
            const std::uint32_t out_t = be32(data.data() + body + 16);
            if (out_t > in_t) {
                PlaylistSegment segment;
                segment.clip = std::move(clip);
                segment.in45k = in_t;
                segment.out45k = out_t;
                parseMplsPrimaryAudio(data, body, item_len, &segment.declared_audio);
                output->segments.push_back(std::move(segment));
                total45k += static_cast<std::uint64_t>(out_t - in_t);
            }
        }
        q = body + item_len;
    }
    output->duration_s = total45k / 45000.0;
    if (output->segments.empty() || output->duration_s <= 0.0) return false;

    std::uint64_t unique45k = 0;
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t>> unique;
    for (const auto& segment : output->segments) {
        if (unique.emplace(segment.clip, segment.in45k, segment.out45k).second) {
            unique45k += static_cast<std::uint64_t>(segment.out45k - segment.in45k);
        }
    }
    output->decoy = unique45k > 0
            && static_cast<double>(total45k) > static_cast<double>(unique45k) * kDecoyReplayRatio;
    return true;
}

bool has3dExtension(const std::vector<std::uint8_t>& data) {
    if (data.size() < 20 || std::memcmp(data.data(), "MPLS", 4) != 0) return false;
    const std::uint32_t ext_start = be32(data.data() + 16);
    if (ext_start == 0 || static_cast<std::size_t>(ext_start) + 12 > data.size()) return false;
    std::uint32_t entries = be32(data.data() + ext_start + 8) & 0xffffU;
    entries = std::min<std::uint32_t>(entries, 64);
    std::size_t p = static_cast<std::size_t>(ext_start) + 12;
    for (std::uint32_t i = 0; i < entries && p + 4 <= data.size(); ++i, p += 12) {
        if (be16(data.data() + p) == 2 && be16(data.data() + p + 2) == 1) return true;
    }
    return false;
}

void apply3dPreference(std::vector<PlaylistCandidate>* candidates) {
    if (!candidates) return;
    for (std::size_t i = 0; i < candidates->size(); ++i) {
        if ((*candidates)[i].has_3d_extension) continue;
        for (std::size_t j = i + 1; j < candidates->size(); ++j) {
            const auto& a = (*candidates)[i];
            const auto& b = (*candidates)[j];
            bool same_segments = a.segments.size() == b.segments.size();
            if (same_segments) {
                for (std::size_t k = 0; k < a.segments.size(); ++k) {
                    const auto& x = a.segments[k];
                    const auto& y = b.segments[k];
                    if (x.clip != y.clip || x.in45k != y.in45k || x.out45k != y.out45k) {
                        same_segments = false;
                        break;
                    }
                }
            }
            const bool close_duration = a.duration_s > 0.0
                    && std::abs(b.duration_s - a.duration_s) <= 0.01 * a.duration_s;
            if ((same_segments || close_duration) && b.has_3d_extension) {
                std::swap((*candidates)[i], (*candidates)[j]);
                break;
            }
        }
    }
}

FeatureSelection resolveFeature(const std::string& iso_path, udfread* udf, std::string* error) {
    FeatureSelection result;
    const auto ssif = indexFiles(udf, "/BDMV/STREAM/SSIF", {".ssif"});
    const auto m2ts = indexFiles(udf, "/BDMV/STREAM", {".m2ts", ".mts"});
    result.ssif_candidates = ssif.size();
    result.m2ts_candidates = m2ts.size();
    if (ssif.empty() && m2ts.empty()) {
        if (error) *error = "ISO has no BDMV/STREAM SSIF or M2TS files";
        return result;
    }

    // Use the same authoritative libbluray TITLES_ALL selector developed for
    // the Android player. It prefers real MVC features, rejects replay-loop
    // decoys, and preserves playlist clip in/out times and declared audio.
    sylc_bluray_posix::Selection authoritative;
    std::string authoritative_error;
    if (sylc_bluray_posix::probeFeaturePath(iso_path, &authoritative,
                                            &authoritative_error)
            && authoritative.valid && !authoritative.segments.empty()) {
        std::vector<FeatureSegment> segments;
        segments.reserve(authoritative.segments.size());
        bool complete = true;
        for (const auto& source : authoritative.segments) {
            const auto dependent = ssif.find(source.clip);
            const auto base = m2ts.find(source.clip);
            if (dependent == ssif.end() || base == m2ts.end()
                    || source.out_time90k <= source.in_time90k) {
                complete = false;
                break;
            }
            FeatureSegment segment;
            segment.clip = source.clip;
            segment.video_path = dependent->second.path;
            segment.audio_path = base->second.path;
            const std::string dependent_clip = nextClipId(source.clip);
            const auto dependent_m2ts = m2ts.find(dependent_clip);
            if (dependent_m2ts != m2ts.end()) {
                segment.dependent_path = dependent_m2ts->second.path;
            }
            segment.duration_s = (source.out_time90k - source.in_time90k) / 90000.0;
            segment.start90k = source.start_time90k;
            segment.in45k = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    source.in_time90k / 2ULL, UINT32_MAX));
            segment.out45k = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    source.out_time90k / 2ULL, UINT32_MAX));
            segment.base_video_pid = source.base_video_pid;
            for (const auto& audio : source.primary_audio) {
                segment.declared_audio.push_back({audio.pid, audio.coding_type,
                                                  audio.format, audio.rate,
                                                  audio.language});
            }
            for (const auto& subtitle : source.presentation_graphics) {
                segment.declared_subtitles.push_back({subtitle.pid, subtitle.coding_type,
                                                      subtitle.language});
            }
            segments.push_back(std::move(segment));
        }
        if (complete && !segments.empty()) {
            char playlist[16]{};
            std::snprintf(playlist, sizeof(playlist), "%05u.mpls",
                          authoritative.playlist);
            result.playlist = playlist;
            result.method = "libbluray authoritative TITLES_ALL MVC/decoy selector: "
                    + authoritative.selection_rule;
            result.kind = "ssif";
            result.duration_s = authoritative.duration90k / 90000.0;
            result.playlist_candidates = authoritative.title_count;
            result.decoys_filtered = authoritative.decoys_seen;
            result.libbluray_authoritative = true;
            result.authoritative_title_index = authoritative.title_index;
            result.authoritative_title_count = authoritative.title_count;
            result.authoritative_main_title = authoritative.main_title;
            result.libbluray_major = authoritative.version_major;
            result.libbluray_minor = authoritative.version_minor;
            result.libbluray_micro = authoritative.version_micro;
            result.candidate_summary = authoritative.candidate_summary;
            result.segments = std::move(segments);
            applySingleClipPhysicalDurationFallback(udf, &result);
            if (error) error->clear();
            return result;
        }
        authoritative_error = "libbluray selected playlist could not be mapped to matching SSIF + base M2TS files";
    }
    result.fallback_detail = authoritative_error.empty()
            ? "libbluray authoritative metadata was unavailable"
            : authoritative_error;

    std::vector<PlaylistCandidate> real;
    std::vector<PlaylistCandidate> decoys;
    for (const std::string& name : listDirectory(udf, "/BDMV/PLAYLIST")) {
        if (!endsWithIgnoreCase(name, ".mpls")) continue;
        const auto bytes = readWholeFile(udf, "/BDMV/PLAYLIST/" + name);
        PlaylistCandidate candidate;
        candidate.name = name;
        if (!parseMpls(bytes, &candidate)) continue;
        candidate.has_3d_extension = has3dExtension(bytes);
        (candidate.decoy ? decoys : real).push_back(std::move(candidate));
    }
    auto by_duration = [](const PlaylistCandidate& a, const PlaylistCandidate& b) {
        return a.duration_s > b.duration_s;
    };
    std::stable_sort(real.begin(), real.end(), by_duration);
    std::stable_sort(decoys.begin(), decoys.end(), by_duration);
    apply3dPreference(&real);
    apply3dPreference(&decoys);
    result.playlist_candidates = real.size() + decoys.size();
    result.decoys_filtered = decoys.size();
    std::vector<PlaylistCandidate> ranked = real;
    ranked.insert(ranked.end(), decoys.begin(), decoys.end());

    auto choose = [&](const PlaylistCandidate& candidate,
                      const std::map<std::string, IndexedFile>& video_index,
                      const char* kind) -> bool {
        std::vector<FeatureSegment> segments;
        std::uint64_t timeline90k = 0;
        for (const auto& item : candidate.segments) {
            const auto video = video_index.find(item.clip);
            if (video == video_index.end()) continue;
            const auto audio = m2ts.find(item.clip);
            FeatureSegment segment;
            segment.clip = item.clip;
            segment.video_path = video->second.path;
            segment.audio_path = audio == m2ts.end() ? std::string() : audio->second.path;
            const auto dependent = m2ts.find(nextClipId(item.clip));
            if (dependent != m2ts.end()) segment.dependent_path = dependent->second.path;
            segment.duration_s = (item.out45k - item.in45k) / 45000.0;
            segment.start90k = timeline90k;
            segment.in45k = item.in45k;
            segment.out45k = item.out45k;
            segment.declared_audio = item.declared_audio;
            timeline90k += static_cast<std::uint64_t>(item.out45k - item.in45k) * 2ULL;
            segments.push_back(std::move(segment));
        }
        if (segments.empty()) return false;
        result.playlist = candidate.name;
        result.method = "SyLC MPLS duration + 3D/decoy heuristic";
        result.kind = kind;
        result.duration_s = candidate.duration_s;
        result.segments = std::move(segments);
        return true;
    };

    // This is the same ordering used by SyLC bluray_disc.py: longest non-decoy
    // playlist resolving to SSIF first, then M2TS, then largest-file fallback.
    for (const auto& candidate : ranked) if (choose(candidate, ssif, "ssif")) return result;
    for (const auto& candidate : ranked) if (choose(candidate, m2ts, "m2ts")) return result;

    auto chooseLargest = [&](const std::map<std::string, IndexedFile>& index,
                             const char* kind) -> bool {
        if (index.empty()) return false;
        const auto best = std::max_element(index.begin(), index.end(),
                [](const auto& a, const auto& b) { return a.second.size < b.second.size; });
        const auto audio = m2ts.find(best->first);
        result.method = "SyLC largest-file fallback";
        result.kind = kind;
        FeatureSegment segment;
        segment.clip = best->first;
        segment.video_path = best->second.path;
        segment.audio_path = audio == m2ts.end() ? std::string() : audio->second.path;
        const auto dependent = m2ts.find(nextClipId(best->first));
        if (dependent != m2ts.end()) segment.dependent_path = dependent->second.path;
        result.segments = {std::move(segment)};
        return true;
    };
    if (chooseLargest(ssif, "ssif") || chooseLargest(m2ts, "m2ts")) return result;
    if (error) *error = "SyLC feature resolver found no playable Blu-ray feature";
    return result;
}


}  // namespace

class FeatureVolume : public std::enable_shared_from_this<FeatureVolume> {
public:
    ~FeatureVolume() {
        if (udf_) udfread_close(udf_);
    }

    udfread* udf_ = nullptr;
    std::string iso_path_;
    std::string volume_id_;
    FeatureSelection feature_;
    struct ClpiCache {
        bool attempted = false;
        bool available = false;
        ClpiEntryPointMap map;
        std::string detail;
    };
    std::vector<ClpiCache> clpi_cache_;
    mutable std::mutex clpi_mutex_;
};

class UdfFileStreamBuf final : public std::streambuf {
public:
    UdfFileStreamBuf(std::shared_ptr<FeatureVolume> volume, UDFFILE* file, std::uint64_t size)
        : volume_(std::move(volume)), file_(file), size_(size) {}
    ~UdfFileStreamBuf() override { if (file_) udfread_file_close(file_); }

protected:
    std::streamsize xsgetn(char* output, std::streamsize count) override {
        if (!file_ || count <= 0 || position_ >= size_) return 0;
        const std::uint64_t wanted = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(count), size_ - position_);
        std::uint64_t total = 0;
        while (total < wanted) {
            if (udfread_file_tell(file_) != static_cast<std::int64_t>(position_)
                    && udfread_file_seek(file_, static_cast<std::int64_t>(position_), kUdfSeekSet) < 0) {
                break;
            }
            const std::size_t remaining = static_cast<std::size_t>(wanted - total);
            const ssize_t got = udfread_file_read(file_, output + total, remaining);
            if (got <= 0) break;
            position_ += static_cast<std::uint64_t>(got);
            total += static_cast<std::uint64_t>(got);
        }
        return static_cast<std::streamsize>(total);
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir direction,
                     std::ios_base::openmode which) override {
        if (!(which & std::ios_base::in) || !file_) return pos_type(off_type(-1));
        std::int64_t base = 0;
        if (direction == std::ios_base::cur) base = static_cast<std::int64_t>(position_);
        else if (direction == std::ios_base::end) base = static_cast<std::int64_t>(size_);
        const std::int64_t next = base + static_cast<std::int64_t>(off);
        if (next < 0 || static_cast<std::uint64_t>(next) > size_) return pos_type(off_type(-1));
        if (udfread_file_seek(file_, next, kUdfSeekSet) < 0) return pos_type(off_type(-1));
        position_ = static_cast<std::uint64_t>(next);
        return pos_type(next);
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, which);
    }

    std::streamsize showmanyc() override {
        return static_cast<std::streamsize>(std::min<std::uint64_t>(
                size_ - std::min(position_, size_),
                static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())));
    }

private:
    std::shared_ptr<FeatureVolume> volume_;
    UDFFILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
};

class UdfFileIStream final : public std::istream {
public:
    UdfFileIStream(std::shared_ptr<FeatureVolume> volume, UDFFILE* file, std::uint64_t size)
        : std::istream(nullptr), buffer_(std::move(volume), file, size) {
        rdbuf(&buffer_);
        clear();
    }
private:
    UdfFileStreamBuf buffer_;
};

std::shared_ptr<FeatureVolume> openFeatureVolumePath(
        const std::string& iso_path, std::string* error) {
    auto volume = std::make_shared<FeatureVolume>();
    volume->iso_path_ = iso_path;
    volume->udf_ = udfread_init();
    if (!volume->udf_ || udfread_open(volume->udf_, iso_path.c_str()) < 0) {
        if (error) *error = "libudfread could not open the ISO UDF filesystem: " + iso_path;
        return {};
    }
    const char* volume_id = udfread_get_volume_id(volume->udf_);
    if (volume_id) volume->volume_id_ = volume_id;
    volume->feature_ = resolveFeature(iso_path, volume->udf_, error);
    if (volume->feature_.segments.empty()) return {};
    volume->clpi_cache_.resize(volume->feature_.segments.size());
    if (error) error->clear();
    return volume;
}

const FeatureSelection& selection(const std::shared_ptr<FeatureVolume>& volume) {
    static const FeatureSelection empty;
    return volume ? volume->feature_ : empty;
}

std::string diagnostics(const std::shared_ptr<FeatureVolume>& volume) {
    if (!volume) return "SyLC ISO/UDF feature session unavailable";
    std::ostringstream out;
    const auto& f = volume->feature_;
    out << "SyLC direct ISO feature source (POSIX/libudfread)\n"
        << "ISO path: " << volume->iso_path_ << "\n"
        << "UDF volume: " << (volume->volume_id_.empty() ? "unknown" : volume->volume_id_) << "\n"
        << "Selection method: " << f.method << "\n"
        << "Authoritative libbluray selection: " << (f.libbluray_authoritative ? "yes" : "no") << "\n";
    if (f.libbluray_authoritative) {
        out << "libbluray version/title/main: " << f.libbluray_major << '.'
            << f.libbluray_minor << '.' << f.libbluray_micro << " / "
            << f.authoritative_title_index << " of " << f.authoritative_title_count
            << " / " << f.authoritative_main_title << "\n";
    } else if (!f.fallback_detail.empty()) {
        out << "Authoritative-selection fallback reason: " << f.fallback_detail << "\n";
    }
    out << "Selected playlist: " << (f.playlist.empty() ? "none" : f.playlist) << "\n"
        << "Selected kind: " << f.kind << "\n"
        << "Selected duration: " << f.duration_s << " s\n"
        << "Playlist candidates/decoys filtered: " << f.playlist_candidates << " / "
        << f.decoys_filtered << "\n"
        << "SSIF/M2TS candidates: " << f.ssif_candidates << " / " << f.m2ts_candidates << "\n";
    if (!f.candidate_summary.empty()) out << "Title candidates:\n" << f.candidate_summary << "\n";
    out << "Feature segments: " << f.segments.size();
    for (std::size_t i = 0; i < f.segments.size(); ++i) {
        const auto& s = f.segments[i];
        out << "\n  #" << i << " clip=" << s.clip << " duration=" << s.duration_s
            << " s; playlist-start=" << (s.start90k / 90000.0)
            << " s; MPLS in/out=" << s.in45k << '/' << s.out45k
            << " (45 kHz); base PID=0x" << std::hex << s.base_video_pid << std::dec
            << "\n    video=" << s.video_path << "\n    audio=" << s.audio_path
            << "\n    dependent=" << (s.dependent_path.empty() ? "unknown" : s.dependent_path);
        if (!s.declared_audio.empty()) {
            out << "\n    MPLS primary audio:";
            for (const auto& a : s.declared_audio) {
                out << " PID=0x" << std::hex << a.pid << std::dec
                    << "/type=0x" << std::hex << static_cast<int>(a.coding_type) << std::dec;
                if (!a.language.empty()) out << '/' << a.language;
            }
        }
    }
    return out.str();
}

bool loadVideoSeekMetadata(const std::shared_ptr<FeatureVolume>& volume,
                           std::size_t segment_index,
                           VideoSeekMetadata* metadata,
                           std::string* error) {
    if (!metadata) {
        if (error) *error = "Blu-ray video-seek metadata output is null";
        return false;
    }
    *metadata = {};
    if (!volume || segment_index >= volume->feature_.segments.size()) {
        if (error) *error = "Invalid Blu-ray video-seek metadata request";
        return false;
    }
    const FeatureSegment& segment = volume->feature_.segments[segment_index];
    // MPLS IN_time is 45 kHz. It is a reliable raw-timestamp origin fallback
    // when a disc omits the CLPI EP_map extension.
    metadata->timeline_origin_ms = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(segment.in45k) * 1000ULL + 22500ULL)
                    / 45000ULL);

    const std::string base_clpi_path = "/BDMV/CLIPINF/" + segment.clip + ".clpi";
    const std::vector<std::uint8_t> base_clpi = readWholeFile(
            volume->udf_, base_clpi_path, 32u << 20u);
    ClpiEntryPointMap base_map;
    std::string base_error;
    if (!base_clpi.empty()
            && parseClpiEntryPointMap(base_clpi, segment.base_video_pid,
                                      &base_map, &base_error)) {
        metadata->base_ep_available = true;
        metadata->timeline_origin_ms = base_map.raw_pts_us.front() / 1000;
        metadata->base_bytes = base_map.byte_offsets;
        metadata->base_raw_pts_ms.reserve(base_map.raw_pts_us.size());
        for (const std::int64_t pts_us : base_map.raw_pts_us) {
            metadata->base_raw_pts_ms.push_back(pts_us / 1000);
        }
    }

    std::vector<std::uint32_t> base_extents;
    std::vector<std::uint32_t> dependent_extents;
    std::string base_extent_error;
    std::string dependent_extent_error;
    const bool base_extent_ok = !base_clpi.empty()
            && parseClpiExtentStarts(base_clpi, &base_extents, &base_extent_error);
    bool dependent_extent_ok = false;
    std::string dependent_clip = segment.dependent_path.empty()
            ? nextClipId(segment.clip) : stemOf(segment.dependent_path);
    std::vector<std::uint8_t> dependent_clpi;
    if (!dependent_clip.empty()) {
        dependent_clpi = readWholeFile(volume->udf_,
                "/BDMV/CLIPINF/" + dependent_clip + ".clpi", 32u << 20u);
        dependent_extent_ok = !dependent_clpi.empty()
                && parseClpiExtentStarts(dependent_clpi, &dependent_extents,
                                         &dependent_extent_error);
    }

    std::string exact_error;
    const std::uint64_t base_size = udfFileSize(volume->udf_, segment.audio_path);
    const std::uint64_t dependent_size = udfFileSize(
            volume->udf_, segment.dependent_path);
    if (metadata->base_ep_available && base_extent_ok && dependent_extent_ok
            && base_size > 0 && dependent_size > 0
            && buildExactSsifSeekTable(
                    base_map.raw_pts_us, base_map.byte_offsets,
                    base_extents, dependent_extents, base_size, dependent_size,
                    &metadata->ssif_raw_pts_ms, &metadata->ssif_bytes,
                    &exact_error)) {
        // buildExactSsifSeekTable preserves the input timestamp unit. Convert
        // the microsecond input values to the millisecond ABI expected by the
        // SyLC SSIF demuxer.
        for (std::int64_t& value : metadata->ssif_raw_pts_ms) value /= 1000;
        metadata->exact_ssif_available = true;
    }

    std::ostringstream detail;
    detail << "clip=" << segment.clip
           << "; timeline-origin=" << metadata->timeline_origin_ms << "ms"
           << "; base-EP=" << (metadata->base_ep_available ? "yes" : "no")
           << "(" << metadata->base_raw_pts_ms.size() << ')'
           << "; base/dep extents=" << base_extents.size() << '/'
           << dependent_extents.size()
           << "; dependent=" << (dependent_clip.empty() ? "unknown" : dependent_clip)
           << "; exact-SSIF=" << (metadata->exact_ssif_available ? "yes" : "no")
           << "(" << metadata->ssif_raw_pts_ms.size() << ')';
    if (!base_error.empty()) detail << "; base-EP-note=" << base_error;
    if (!base_extent_error.empty()) detail << "; base-extent-note=" << base_extent_error;
    if (!dependent_extent_error.empty()) detail << "; dep-extent-note=" << dependent_extent_error;
    if (!exact_error.empty()) detail << "; exact-note=" << exact_error;
    metadata->detail = detail.str();
    if (error) error->clear();
    return metadata->timeline_origin_ms >= 0;
}

bool planAudioSeek(const std::shared_ptr<FeatureVolume>& volume,
                   std::size_t segment_index,
                   std::int64_t requested_global_us,
                   std::int64_t global_offset_us,
                   ClpiAudioSeekAnchor* anchor,
                   std::string* error) {
    if (!volume || !anchor || segment_index >= volume->feature_.segments.size()) {
        if (error) *error = "Invalid Blu-ray CLPI audio seek request";
        return false;
    }
    const FeatureSegment& segment = volume->feature_.segments[segment_index];
    UDFFILE* audio_file = udfread_file_open(volume->udf_, segment.audio_path.c_str());
    if (!audio_file) {
        if (error) *error = "Could not open base M2TS for CLPI seek planning";
        return false;
    }
    const std::int64_t audio_size_signed = udfread_file_size(audio_file);
    udfread_file_close(audio_file);
    if (audio_size_signed <= 0) {
        if (error) *error = "Base M2TS has no usable size";
        return false;
    }
    ClpiEntryPointMap map;
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(volume->clpi_mutex_);
        FeatureVolume::ClpiCache& cache = volume->clpi_cache_[segment_index];
        if (!cache.attempted) {
            cache.attempted = true;
            const std::string path = "/BDMV/CLIPINF/" + segment.clip + ".clpi";
            const std::vector<std::uint8_t> bytes = readWholeFile(volume->udf_, path, 32u << 20u);
            if (bytes.empty()) {
                cache.detail = "CLPI file unavailable: " + path;
            } else {
                std::string parse_error;
                cache.available = parseClpiEntryPointMap(bytes, 0, &cache.map, &parse_error);
                if (cache.available) {
                    std::ostringstream text;
                    text << "parsed " << cache.map.raw_pts_us.size()
                         << " CLPI EP_map entries from " << path
                         << " using PID=0x" << std::hex << cache.map.pid << std::dec;
                    cache.detail = text.str();
                } else {
                    cache.detail = parse_error.empty() ? "CLPI EP_map parse failed: " + path : parse_error;
                }
            }
        }
        if (!cache.available) {
            if (error) *error = cache.detail;
            return false;
        }
        map = cache.map;
        detail = cache.detail;
    }
    if (!chooseClpiAudioSeekAnchor(map, segment.in45k, requested_global_us,
                                   global_offset_us,
                                   static_cast<std::uint64_t>(audio_size_signed),
                                   anchor, error)) {
        return false;
    }
    anchor->detail = detail + "; " + anchor->detail;
    return true;
}

std::unique_ptr<std::istream> openVideoStream(
        const std::shared_ptr<FeatureVolume>& volume, std::size_t segment_index,
        std::uint64_t* size, std::string* label, std::string* error) {
    if (!volume || segment_index >= volume->feature_.segments.size()) {
        if (error) *error = "Invalid Blu-ray feature segment";
        return {};
    }
    const std::string& path = volume->feature_.segments[segment_index].video_path;
    UDFFILE* file = udfread_file_open(volume->udf_, path.c_str());
    if (!file) {
        if (error) *error = "Could not open feature video inside ISO: " + path;
        return {};
    }
    const std::int64_t file_size = udfread_file_size(file);
    if (file_size <= 0) {
        udfread_file_close(file);
        if (error) *error = "Feature video file is empty: " + path;
        return {};
    }
    if (size) *size = static_cast<std::uint64_t>(file_size);
    if (label) *label = path;
    return std::make_unique<UdfFileIStream>(volume, file, static_cast<std::uint64_t>(file_size));
}

std::unique_ptr<std::istream> openAudioStream(
        const std::shared_ptr<FeatureVolume>& volume, std::size_t segment_index,
        std::uint64_t* size, std::string* label, std::string* error) {
    if (!volume || segment_index >= volume->feature_.segments.size()) {
        if (error) *error = "Invalid Blu-ray audio feature segment";
        return {};
    }
    const std::string& path = volume->feature_.segments[segment_index].audio_path;
    if (path.empty()) {
        if (error) *error = "Selected feature segment has no base-view M2TS audio path";
        return {};
    }
    UDFFILE* file = udfread_file_open(volume->udf_, path.c_str());
    if (!file) {
        if (error) *error = "Could not open feature audio inside ISO: " + path;
        return {};
    }
    const std::int64_t file_size = udfread_file_size(file);
    if (file_size <= 0) {
        udfread_file_close(file);
        if (error) *error = "Feature audio file is empty: " + path;
        return {};
    }
    if (size) *size = static_cast<std::uint64_t>(file_size);
    if (label) *label = path;
    return std::make_unique<UdfFileIStream>(volume, file, static_cast<std::uint64_t>(file_size));
}

}  // namespace sylc_iso
