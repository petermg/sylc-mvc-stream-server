#include "sylc_libbluray_probe.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace sylc_bluray_posix {
namespace {

// Public libbluray ABI subset. These structures and function signatures are
// stable public API from VideoLAN libbluray. Keeping the small ABI declaration
// here lets the package use the host libbluray.so.2 without requiring -dev.
struct BLURAY;

struct BLURAY_STREAM_INFO {
    std::uint8_t coding_type;
    std::uint8_t format;
    std::uint8_t rate;
    std::uint8_t char_code;
    std::uint8_t lang[4];
    std::uint16_t pid;
    std::uint8_t aspect;
    std::uint8_t subpath_id;
};

struct BLURAY_CLIP_INFO {
    std::uint32_t pkt_count;
    std::uint8_t still_mode;
    std::uint16_t still_time;
    std::uint8_t video_stream_count;
    std::uint8_t audio_stream_count;
    std::uint8_t pg_stream_count;
    std::uint8_t ig_stream_count;
    std::uint8_t sec_audio_stream_count;
    std::uint8_t sec_video_stream_count;
    BLURAY_STREAM_INFO* video_streams;
    BLURAY_STREAM_INFO* audio_streams;
    BLURAY_STREAM_INFO* pg_streams;
    BLURAY_STREAM_INFO* ig_streams;
    BLURAY_STREAM_INFO* sec_audio_streams;
    BLURAY_STREAM_INFO* sec_video_streams;
    std::uint64_t start_time;
    std::uint64_t in_time;
    std::uint64_t out_time;
    char clip_id[6];
};

struct BLURAY_TITLE_CHAPTER;
struct BLURAY_TITLE_MARK;
struct BLURAY_TITLE_INFO {
    std::uint32_t idx;
    std::uint32_t playlist;
    std::uint64_t duration;
    std::uint32_t clip_count;
    std::uint8_t angle_count;
    std::uint32_t chapter_count;
    std::uint32_t mark_count;
    BLURAY_CLIP_INFO* clips;
    BLURAY_TITLE_CHAPTER* chapters;
    BLURAY_TITLE_MARK* marks;
    std::uint8_t mvc_base_view_r_flag;
};

constexpr std::uint8_t kTitlesAll = 0;
constexpr std::uint8_t kVideoH264 = 0x1b;
constexpr std::uint8_t kVideoMvc = 0x20;
constexpr double kReplayDecoyRatio = 1.5;

struct Api {
    void* handle = nullptr;
    using Open = BLURAY* (*)(const char*, const char*);
    using Close = void (*)(BLURAY*);
    using GetVersion = void (*)(int*, int*, int*);
    using GetTitles = std::uint32_t (*)(BLURAY*, std::uint8_t, std::uint32_t);
    using GetMainTitle = int (*)(BLURAY*);
    using GetTitleInfo = BLURAY_TITLE_INFO* (*)(BLURAY*, std::uint32_t, unsigned);
    using FreeTitleInfo = void (*)(BLURAY_TITLE_INFO*);

    Open open = nullptr;
    Close close = nullptr;
    GetVersion get_version = nullptr;
    GetTitles get_titles = nullptr;
    GetMainTitle get_main_title = nullptr;
    GetTitleInfo get_title_info = nullptr;
    FreeTitleInfo free_title_info = nullptr;

    ~Api() { if (handle) dlclose(handle); }
};

template <typename T>
bool loadSymbol(void* handle, const char* name, T* output, std::string* error) {
    dlerror();
    void* symbol = dlsym(handle, name);
    const char* detail = dlerror();
    if (!symbol || detail) {
        if (error) *error = std::string("libbluray symbol unavailable: ") + name
                + (detail ? std::string(" (") + detail + ')' : std::string());
        return false;
    }
    *output = reinterpret_cast<T>(symbol);
    return true;
}

std::unique_ptr<Api> loadApi(std::string* error) {
    auto api = std::make_unique<Api>();
    const char* candidates[] = {"libbluray.so.2", "libbluray.so"};
    for (const char* candidate : candidates) {
        api->handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (api->handle) break;
    }
    if (!api->handle) {
        const char* detail = dlerror();
        if (error) *error = std::string("host libbluray runtime is unavailable: ")
                + (detail ? detail : "dlopen failed");
        return {};
    }
    if (!loadSymbol(api->handle, "bd_open", &api->open, error)
            || !loadSymbol(api->handle, "bd_close", &api->close, error)
            || !loadSymbol(api->handle, "bd_get_version", &api->get_version, error)
            || !loadSymbol(api->handle, "bd_get_titles", &api->get_titles, error)
            || !loadSymbol(api->handle, "bd_get_main_title", &api->get_main_title, error)
            || !loadSymbol(api->handle, "bd_get_title_info", &api->get_title_info, error)
            || !loadSymbol(api->handle, "bd_free_title_info", &api->free_title_info, error)) {
        return {};
    }
    return api;
}

struct Candidate {
    std::uint32_t title_index = 0;
    std::uint32_t playlist = 0;
    std::uint64_t duration90k = 0;
    std::uint32_t clip_count = 0;
    std::uint32_t unique_playitem_count = 0;
    std::uint32_t repeated_playitem_count = 0;
    std::uint64_t playitem_duration90k = 0;
    std::uint64_t unique_playitem_duration90k = 0;
    double replay_ratio = 1.0;
    std::uint32_t video_streams = 0;
    std::uint32_t audio_streams = 0;
    std::uint32_t secondary_video_streams = 0;
    bool has_h264 = false;
    bool has_mvc = false;
    bool replay_decoy = false;
};

void populate(const BLURAY_TITLE_INFO& info, std::uint32_t title_index,
              Candidate* candidate) {
    *candidate = {};
    candidate->title_index = title_index;
    candidate->playlist = info.playlist;
    candidate->duration90k = info.duration;
    candidate->clip_count = info.clip_count;
    std::set<std::tuple<std::string, std::uint64_t, std::uint64_t>> unique;
    for (std::uint32_t c = 0; info.clips && c < info.clip_count; ++c) {
        const BLURAY_CLIP_INFO& clip = info.clips[c];
        candidate->video_streams += clip.video_stream_count;
        candidate->audio_streams += clip.audio_stream_count;
        candidate->secondary_video_streams += clip.sec_video_stream_count;
        for (std::uint32_t v = 0; clip.video_streams && v < clip.video_stream_count; ++v) {
            if (clip.video_streams[v].coding_type == kVideoH264) candidate->has_h264 = true;
        }
        for (std::uint32_t v = 0; clip.sec_video_streams && v < clip.sec_video_stream_count; ++v) {
            if (clip.sec_video_streams[v].coding_type == kVideoMvc) candidate->has_mvc = true;
        }
        const std::uint64_t duration = clip.out_time > clip.in_time
                ? clip.out_time - clip.in_time : 0;
        candidate->playitem_duration90k += duration;
        const auto key = std::make_tuple(std::string(clip.clip_id), clip.in_time, clip.out_time);
        if (unique.emplace(key).second) candidate->unique_playitem_duration90k += duration;
    }
    candidate->unique_playitem_count = static_cast<std::uint32_t>(unique.size());
    candidate->repeated_playitem_count = candidate->clip_count > candidate->unique_playitem_count
            ? candidate->clip_count - candidate->unique_playitem_count : 0;
    if (candidate->unique_playitem_duration90k > 0) {
        candidate->replay_ratio = static_cast<double>(candidate->playitem_duration90k)
                / static_cast<double>(candidate->unique_playitem_duration90k);
    }
    candidate->replay_decoy = candidate->repeated_playitem_count > 0
            && candidate->unique_playitem_duration90k > 0
            && static_cast<double>(candidate->playitem_duration90k)
                    > static_cast<double>(candidate->unique_playitem_duration90k)
                            * kReplayDecoyRatio;
}

int selectionClass(const Candidate& candidate) {
    const bool playable = candidate.duration90k > 0 && candidate.has_h264
            && candidate.audio_streams > 0;
    if (playable && !candidate.replay_decoy && candidate.has_mvc) return 5;
    if (playable && !candidate.replay_decoy) return 4;
    if (playable && candidate.replay_decoy && candidate.has_mvc) return 3;
    if (playable && candidate.replay_decoy) return 2;
    if (!candidate.replay_decoy && candidate.has_h264) return 1;
    return 0;
}

bool better(const Candidate& candidate, const Candidate& selected, bool have) {
    if (!have) return true;
    const int a = selectionClass(candidate);
    const int b = selectionClass(selected);
    if (a != b) return a > b;
    if (candidate.duration90k != selected.duration90k) return candidate.duration90k > selected.duration90k;
    if (candidate.unique_playitem_duration90k != selected.unique_playitem_duration90k) {
        return candidate.unique_playitem_duration90k > selected.unique_playitem_duration90k;
    }
    return candidate.clip_count > selected.clip_count;
}

std::string language(const std::uint8_t value[4]) {
    return std::string(reinterpret_cast<const char*>(value),
                       strnlen(reinterpret_cast<const char*>(value), 3));
}

}  // namespace

bool probeFeaturePath(const std::string& iso_path, Selection* output,
                      std::string* error) {
    if (!output) {
        if (error) *error = "libbluray output is null";
        return false;
    }
    *output = {};
    auto api = loadApi(error);
    if (!api) return false;

    BLURAY* disc = api->open(iso_path.c_str(), nullptr);
    if (!disc) {
        if (error) *error = "libbluray bd_open() could not open the ISO (encrypted or malformed disc)";
        return false;
    }
    struct DiscCloser {
        Api* api;
        void operator()(BLURAY* disc) const { if (disc) api->close(disc); }
    };
    std::unique_ptr<BLURAY, DiscCloser> holder(disc, DiscCloser{api.get()});

    api->get_version(&output->version_major, &output->version_minor, &output->version_micro);
    output->title_count = api->get_titles(disc, kTitlesAll, 0);
    output->main_title = output->title_count ? api->get_main_title(disc) : -1;
    if (output->title_count == 0) {
        if (error) *error = "libbluray found no title playlists";
        return false;
    }

    Candidate selected;
    bool have = false;
    std::vector<Candidate> candidates;
    candidates.reserve(output->title_count);
    for (std::uint32_t i = 0; i < output->title_count; ++i) {
        BLURAY_TITLE_INFO* info = api->get_title_info(disc, i, 0);
        if (!info) continue;
        Candidate candidate;
        populate(*info, i, &candidate);
        api->free_title_info(info);
        if (candidate.replay_decoy) ++output->decoys_seen;
        candidates.push_back(candidate);
        if (better(candidate, selected, have)) {
            selected = candidate;
            have = true;
        }
    }
    if (!have || selected.duration90k == 0) {
        if (error) *error = "libbluray found no usable feature playlist metadata";
        return false;
    }

    BLURAY_TITLE_INFO* selected_info = api->get_title_info(disc, selected.title_index, 0);
    if (!selected_info || !selected_info->clips || selected_info->clip_count == 0) {
        if (selected_info) api->free_title_info(selected_info);
        if (error) *error = "libbluray selected playlist has no readable clips";
        return false;
    }

    output->valid = true;
    output->title_index = selected.title_index;
    output->playlist = selected.playlist;
    output->duration90k = selected.duration90k > 0 ? selected.duration90k : selected_info->duration;
    const bool playable = selected.has_h264 && selected.audio_streams > 0;
    if (selected.replay_decoy) {
        output->selection_rule = playable
                ? "replay-decoy emergency fallback: longest playable AVC playlist"
                : "replay-decoy emergency fallback: longest available playlist";
    } else if (playable && selected.has_mvc) {
        output->selection_rule =
                "longest non-replay-decoy playlist with AVC base view + MVC dependent view + primary audio";
    } else if (playable) {
        output->selection_rule =
                "longest non-replay-decoy playable AVC playlist fallback (no MVC metadata exposed)";
    } else {
        output->selection_rule = "longest non-replay-decoy available playlist fallback";
    }

    std::ostringstream summary;
    summary << "libbluray=" << output->version_major << '.' << output->version_minor
            << '.' << output->version_micro
            << "; enumeration=TITLES_ALL; replay-decoy threshold=" << kReplayDecoyRatio
            << "x unique PlayItem duration\n"
            << "title/playlist/duration/audio/video/sec-video/MVC/clips/unique/repeated/unique-duration/replay-ratio/decoy";
    for (const Candidate& candidate : candidates) {
        summary << "\n  " << candidate.title_index << '/' << candidate.playlist << '/'
                << std::fixed << std::setprecision(3) << candidate.duration90k / 90000.0 << '/'
                << candidate.audio_streams << '/' << candidate.video_streams << '/'
                << candidate.secondary_video_streams << '/'
                << (candidate.has_mvc ? "yes" : "no") << '/' << candidate.clip_count << '/'
                << candidate.unique_playitem_count << '/' << candidate.repeated_playitem_count << '/'
                << candidate.unique_playitem_duration90k / 90000.0 << '/'
                << candidate.replay_ratio << '/' << (candidate.replay_decoy ? "YES" : "no");
    }
    output->candidate_summary = summary.str();

    output->segments.reserve(selected_info->clip_count);
    for (std::uint32_t i = 0; i < selected_info->clip_count; ++i) {
        const BLURAY_CLIP_INFO& clip = selected_info->clips[i];
        Segment segment;
        segment.clip.assign(clip.clip_id, strnlen(clip.clip_id, 5));
        segment.start_time90k = clip.start_time;
        segment.in_time90k = clip.in_time;
        segment.out_time90k = clip.out_time;
        segment.base_video_pid = (clip.video_streams && clip.video_stream_count > 0)
                ? clip.video_streams[0].pid : 0;
        for (std::uint32_t a = 0; clip.audio_streams && a < clip.audio_stream_count; ++a) {
            const BLURAY_STREAM_INFO& source = clip.audio_streams[a];
            segment.primary_audio.push_back({source.pid, source.coding_type,
                                             source.format, source.rate,
                                             language(source.lang)});
        }
        for (std::uint32_t p = 0; clip.pg_streams && p < clip.pg_stream_count; ++p) {
            const BLURAY_STREAM_INFO& source = clip.pg_streams[p];
            segment.presentation_graphics.push_back({source.pid, source.coding_type,
                                                     language(source.lang)});
        }
        output->segments.push_back(std::move(segment));
    }
    api->free_title_info(selected_info);
    if (output->segments.empty()) {
        *output = {};
        if (error) *error = "libbluray selected no feature segments";
        return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace sylc_bluray_posix
