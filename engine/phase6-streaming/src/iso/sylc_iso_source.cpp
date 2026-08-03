#include "sylc_iso_feature_posix.h"
#include "sylc_m2ts_audio_demuxer.h"
#include "sylc_m2ts_pgs_demuxer.h"
#include "mvc_ssif_demuxer.h"
#include "mvc_source_pair_offset_calibrator.h"
#include "mvc_ssif_phase_aligner.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

class OutputClosed final : public std::exception {
public:
    const char* what() const noexcept override { return "consumer closed output pipe"; }
};

struct Options {
    enum class Mode { Probe, PlanVideoSeek, Video, Audio, Subtitle } mode = Mode::Probe;
    std::string input;
    double start_seconds = 0.0;
    std::size_t audio_track = 0;
    std::size_t subtitle_track = 0;
    std::size_t recovery_backoff_entries = 0;
};

[[noreturn]] void usage(const char* program, const std::string& error = {}) {
    if (!error.empty()) std::cerr << "ERROR: " << error << "\n\n";
    std::cerr << "Usage: " << program << " --input FILE.iso [--probe|--plan-video-seek|--video|--audio|--subtitle] [options]\n"
              << "  --start-seconds N   movie timeline start for video/audio\n"
              << "  --audio-track N     relative selected-title audio track (default 0)\n"
              << "  --subtitle-track N  relative selected-title PGS track (default 0)\n"
              << "  --recovery-backoff-entries N  preceding CLPI entries for nonzero ISO seek recovery\n";
    std::exit(error.empty() ? 0 : 2);
}

Options parseOptions(int argc, char** argv) {
    Options options;
    bool mode_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) usage(argv[0], std::string("Missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--input") options.input = value("--input");
        else if (arg == "--probe") { options.mode = Options::Mode::Probe; mode_seen = true; }
        else if (arg == "--plan-video-seek") { options.mode = Options::Mode::PlanVideoSeek; mode_seen = true; }
        else if (arg == "--video") { options.mode = Options::Mode::Video; mode_seen = true; }
        else if (arg == "--audio") { options.mode = Options::Mode::Audio; mode_seen = true; }
        else if (arg == "--subtitle") { options.mode = Options::Mode::Subtitle; mode_seen = true; }
        else if (arg == "--start-seconds") options.start_seconds = std::stod(value("--start-seconds"));
        else if (arg == "--audio-track") options.audio_track = static_cast<std::size_t>(std::stoull(value("--audio-track")));
        else if (arg == "--subtitle-track") options.subtitle_track = static_cast<std::size_t>(std::stoull(value("--subtitle-track")));
        else if (arg == "--recovery-backoff-entries") options.recovery_backoff_entries = static_cast<std::size_t>(std::stoull(value("--recovery-backoff-entries")));
        else if (arg == "--help" || arg == "-h") usage(argv[0]);
        else usage(argv[0], "Unknown option: " + arg);
    }
    (void)mode_seen;
    if (options.input.empty()) usage(argv[0], "--input is required");
    if (!(options.start_seconds >= 0.0)) usage(argv[0], "--start-seconds must be non-negative");
    return options;
}

void writeAll(const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(STDOUT_FILENO, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && errno == EPIPE) throw OutputClosed{};
        throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
    }
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string audioFormat(std::uint8_t type) {
    switch (type) {
        case 0x81: return "ac3";
        case 0x84:
        case 0xa1: return "eac3";
        case 0x82:
        case 0x85:
        case 0x86:
        case 0xa2: return "dts";
        case 0x83: return "truehd";
        default: return "unsupported";
    }
}

std::string audioFormat(const sylc_audio::AudioTrackInfo& track) {
    if (!track.bridge_format.empty()) return track.bridge_format;
    return audioFormat(track.stream_type);
}

std::vector<std::int64_t> segmentOffsetsUs(const sylc_iso::FeatureSelection& feature) {
    std::vector<std::int64_t> offsets(feature.segments.size(), 0);
    std::int64_t cumulative = 0;
    for (std::size_t i = 0; i < feature.segments.size(); ++i) {
        const auto& segment = feature.segments[i];
        const std::int64_t authoritative = static_cast<std::int64_t>(
                (segment.start90k * 1000000ULL + 45000ULL) / 90000ULL);
        offsets[i] = (i == 0 || authoritative >= cumulative) ? authoritative : cumulative;
        cumulative = offsets[i] + static_cast<std::int64_t>(
                segment.duration_s * 1000000.0 + 0.5);
    }
    return offsets;
}

std::size_t segmentForStart(const sylc_iso::FeatureSelection& feature,
                            const std::vector<std::int64_t>& offsets,
                            std::int64_t start_us) {
    std::size_t selected = 0;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (offsets[i] <= start_us) selected = i;
        else break;
    }
    return selected;
}

struct OpenedVideo {
    std::unique_ptr<mvc_demux::MVCSSIFDemuxer> demuxer =
            std::make_unique<mvc_demux::MVCSSIFDemuxer>();
    sylc_iso::VideoSeekMetadata seek;
    mvc_demux::MVCSSIFDemuxer::VideoInfo info{};
    std::int64_t requested_local_ms = 0;
    std::int64_t seek_anchor_ms = 0;
    std::size_t recovery_backoff_entries = 0;
    std::string seek_method = "zero-time";
};

std::int64_t recoveryAnchorMs(const sylc_iso::VideoSeekMetadata& seek,
                              std::int64_t requested_local_ms,
                              std::size_t backoff_entries,
                              std::string* method) {
    if (requested_local_ms <= 0) {
        if (method) *method = seek.exact_ssif_available
                ? "exact SSIF zero-time RAPI" : "zero-time byte origin";
        return 0;
    }
    const std::vector<std::int64_t>* table = nullptr;
    if (seek.exact_ssif_available && !seek.ssif_raw_pts_ms.empty()) {
        table = &seek.ssif_raw_pts_ms;
        if (method) *method = "exact CLPI/SSIF interleaved-unit map";
    } else if (seek.base_ep_available && !seek.base_raw_pts_ms.empty()) {
        table = &seek.base_raw_pts_ms;
        if (method) *method = "preceding CLPI base-EP anchor with SSIF PTS-search fallback";
    }
    if (table && !table->empty()) {
        const std::int64_t origin = seek.timeline_origin_ms >= 0
                ? seek.timeline_origin_ms : table->front();
        const std::int64_t target_raw = origin + requested_local_ms;
        std::size_t selected = 0;
        for (std::size_t i = 0; i < table->size(); ++i) {
            if ((*table)[i] <= target_raw) selected = i;
            else break;
        }
        selected = selected > backoff_entries ? selected - backoff_entries : 0;
        return std::max<std::int64_t>(0, (*table)[selected] - origin);
    }
    if (method) *method = "bounded millisecond fallback";
    return std::max<std::int64_t>(0, requested_local_ms
            - static_cast<std::int64_t>(backoff_entries) * 1000LL);
}

bool openVideoSegment(const std::shared_ptr<sylc_iso::FeatureVolume>& volume,
                      std::size_t index,
                      std::int64_t local_start_ms,
                      std::size_t recovery_backoff_entries,
                      OpenedVideo* opened,
                      std::string* error) {
    if (!opened) {
        if (error) *error = "video output is null";
        return false;
    }
    const auto& feature = sylc_iso::selection(volume);
    std::uint64_t ssif_size = 0;
    std::uint64_t base_size = 0;
    std::string ssif_label;
    std::string base_label;
    auto ssif = sylc_iso::openVideoStream(volume, index, &ssif_size, &ssif_label, error);
    auto base = sylc_iso::openAudioStream(volume, index, &base_size, &base_label, error);
    if (!ssif || !base) return false;
    opened->demuxer->setExternalDurationMs(static_cast<std::int64_t>(
            feature.segments[index].duration_s * 1000.0 + 0.5));
    if (!opened->demuxer->openStreams(std::move(ssif), ssif_size,
                                      std::move(base), base_size,
                                      ssif_label, base_label)) {
        if (error) *error = "SyLC SSIF demuxer failed for clip "
                + feature.segments[index].clip;
        return false;
    }
    std::string seek_error;
    if (sylc_iso::loadVideoSeekMetadata(volume, index, &opened->seek, &seek_error)) {
        if (opened->seek.base_ep_available) {
            opened->demuxer->setBaseSeekTable(opened->seek.base_raw_pts_ms,
                                              opened->seek.base_bytes);
        }
        if (opened->seek.timeline_origin_ms >= 0) {
            opened->demuxer->setTimelineOriginMs(opened->seek.timeline_origin_ms);
        }
        if (opened->seek.exact_ssif_available) {
            opened->demuxer->setSsifSeekTable(opened->seek.ssif_raw_pts_ms,
                                              opened->seek.ssif_bytes);
        }
    } else if (local_start_ms > 0) {
        if (error) *error = seek_error.empty()
                ? "Blu-ray seek metadata is unavailable" : seek_error;
        return false;
    }
    opened->requested_local_ms = local_start_ms;
    opened->recovery_backoff_entries = recovery_backoff_entries;
    opened->seek_anchor_ms = recoveryAnchorMs(opened->seek, local_start_ms,
                                               recovery_backoff_entries,
                                               &opened->seek_method);
    // For zero-time playback, an exact table still matters: it moves the SSIF
    // reader to the first clean interleaved random-access unit instead of byte 0.
    if ((local_start_ms > 0 || opened->seek.exact_ssif_available)
            && !opened->demuxer->seek(opened->seek_anchor_ms)) {
        if (error) *error = "SyLC SSIF seek failed for clip "
                + feature.segments[index].clip;
        return false;
    }
    opened->info = opened->demuxer->getVideoInfo();
    std::cerr << "ISO_VIDEO_SEEK " << opened->seek.detail
              << "; requested-local=" << local_start_ms << "ms"
              << "; recovery-anchor=" << opened->seek_anchor_ms << "ms"
              << "; backoff-entries=" << recovery_backoff_entries
              << "; method=" << opened->seek_method << "\n";
    if (error) error->clear();
    return true;
}

constexpr std::size_t kRecoveryStructuralValidationPairs = 6;
constexpr std::size_t kRecoveryAdditionalStabilizationPairs = 18;
constexpr std::size_t kRecoveryHiddenCleanPairs =
        kRecoveryStructuralValidationPairs + kRecoveryAdditionalStabilizationPairs;
constexpr std::size_t kRecoveryOffsetCalibrationMinimumPairs = 6;
constexpr std::size_t kRecoveryOffsetCalibrationMaximumPairs = 12;
constexpr std::int64_t kRecoveryMaximumAbsoluteSourceOffsetMs = 67;
constexpr std::int64_t kRecoverySourceOffsetDeviationMs = 10;
constexpr std::int64_t kRecoveryPhaseResidualToleranceMs = 10;
constexpr std::size_t kRecoveryMismatchLimit = 12;

struct RecoveryDiagnostics {
    bool enabled = false;
    std::size_t backoff_entries = 0;
    std::int64_t requested_local_ms = 0;
    std::int64_t anchor_local_ms = 0;
    std::string seek_method;
    std::size_t calibration_samples = 0;
    std::size_t calibration_inliers = 0;
    std::int64_t delta_first_ms = 0;
    std::int64_t delta_min_ms = 0;
    std::int64_t delta_median_ms = 0;
    std::int64_t delta_max_ms = 0;
    std::int64_t calibrated_offset_ms = 0;
    int phase_shift_frames = 0;
    std::int64_t phase_residual_ms = 0;
    std::uint64_t leading_base_discards = 0;
    std::uint64_t leading_dependent_discards = 0;
    std::uint64_t corrected_pairs = 0;
    std::uint64_t rejected_pairs = 0;
    std::int64_t corrected_delta_first_ms = 0;
    std::int64_t corrected_delta_min_ms = 0;
    std::int64_t corrected_delta_median_ms = 0;
    std::int64_t corrected_delta_max_ms = 0;
    std::int64_t first_raw_timestamp_ms = -1;
    std::int64_t first_corrected_timestamp_ms = -1;
};

class RecoveredVideoReader final {
public:
    using FramePair = mvc_demux::MVCSSIFDemuxer::FramePair;

    bool open(const std::shared_ptr<sylc_iso::FeatureVolume>& volume,
              std::size_t index,
              std::int64_t requested_local_ms,
              std::size_t backoff_entries,
              std::string* error) {
        diagnostics_ = RecoveryDiagnostics{};
        diagnostics_.enabled = requested_local_ms > 0;
        diagnostics_.backoff_entries = backoff_entries;
        diagnostics_.requested_local_ms = requested_local_ms;
        opened_ = OpenedVideo{};
        if (!openVideoSegment(volume, index, requested_local_ms, backoff_entries,
                              &opened_, error)) {
            return false;
        }
        diagnostics_.anchor_local_ms = opened_.seek_anchor_ms;
        diagnostics_.seek_method = opened_.seek_method;
        if (!diagnostics_.enabled) {
            if (error) error->clear();
            return true;
        }

        const std::int64_t frame_interval_ms = std::max<std::int64_t>(1,
                static_cast<std::int64_t>(std::llround(1000.0 / opened_.info.fps)));
        MvcSourcePairOffsetCalibrator calibrator(
                kRecoveryOffsetCalibrationMinimumPairs,
                kRecoveryOffsetCalibrationMaximumPairs,
                kRecoveryMaximumAbsoluteSourceOffsetMs,
                kRecoverySourceOffsetDeviationMs);
        std::vector<FramePair> pending;
        pending.reserve(kRecoveryOffsetCalibrationMaximumPairs);
        std::vector<std::int64_t> pending_deltas;
        pending_deltas.reserve(kRecoveryOffsetCalibrationMaximumPairs);

        FramePair pair;
        while (!calibrator.calibrated() && opened_.demuxer->readNextFramePair(pair)) {
            if (diagnostics_.first_raw_timestamp_ms < 0) {
                diagnostics_.first_raw_timestamp_ms =
                        static_cast<std::int64_t>(pair.timestamp);
            }
            if (pair.baseData.empty() || pair.dependentData.empty()) {
                ++diagnostics_.rejected_pairs;
                if (diagnostics_.rejected_pairs >= kRecoveryMismatchLimit) {
                    if (error) *error = "MVC recovery encountered too many missing-eye pairs";
                    return false;
                }
                continue;
            }
            const std::int64_t delta = static_cast<std::int64_t>(pair.depTimestamp)
                    - static_cast<std::int64_t>(pair.timestamp);
            pending.push_back(std::move(pair));
            pending_deltas.push_back(delta);
            const auto status = calibrator.observe(delta);
            if (status == MvcSourcePairOffsetCalibrator::Status::Failed) {
                if (error) *error = "MVC recovery source-pair PTS offset was unstable";
                return false;
            }
        }
        if (!calibrator.calibrated()) {
            if (error) *error = "MVC recovery ended before source-pair PTS calibration";
            return false;
        }
        diagnostics_.calibration_samples = calibrator.sampleCount();
        diagnostics_.calibration_inliers = calibrator.inlierCount();
        diagnostics_.delta_first_ms = calibrator.firstMs();
        diagnostics_.delta_min_ms = calibrator.minimumMs();
        diagnostics_.delta_median_ms = calibrator.medianMs();
        diagnostics_.delta_max_ms = calibrator.maximumMs();
        diagnostics_.calibrated_offset_ms = calibrator.calibratedOffsetMs();
        calibrated_offset_ms_ = calibrator.calibratedOffsetMs();
        offset_tolerance_ms_ = calibrator.deviationToleranceMs();

        if (!phase_aligner_.configure(calibrated_offset_ms_, frame_interval_ms,
                                      kRecoveryPhaseResidualToleranceMs)) {
            if (error) *error = "MVC recovery found an unsupported stable SSIF eye phase";
            return false;
        }
        diagnostics_.phase_shift_frames = phase_aligner_.phaseShiftFrames();
        diagnostics_.phase_residual_ms = phase_aligner_.phaseResidualMs();

        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (std::llabs(pending_deltas[i] - calibrated_offset_ms_) > offset_tolerance_ms_) {
                ++diagnostics_.rejected_pairs;
                continue;
            }
            FramePair corrected;
            if (phase_aligner_.push(std::move(pending[i]), corrected)) {
                if (!acceptCorrected(std::move(corrected), error)) return false;
            }
        }
        refreshAlignerDiagnostics();
        if (error) error->clear();
        return true;
    }

    bool next(FramePair& output, std::string* error) {
        if (!ready_.empty()) {
            output = std::move(ready_.front());
            ready_.pop_front();
            if (error) error->clear();
            return true;
        }
        FramePair raw;
        while (opened_.demuxer->readNextFramePair(raw)) {
            if (diagnostics_.first_raw_timestamp_ms < 0) {
                diagnostics_.first_raw_timestamp_ms =
                        static_cast<std::int64_t>(raw.timestamp);
            }
            if (!diagnostics_.enabled) {
                output = std::move(raw);
                if (diagnostics_.first_corrected_timestamp_ms < 0) {
                    diagnostics_.first_corrected_timestamp_ms =
                            static_cast<std::int64_t>(output.timestamp);
                }
                ++diagnostics_.corrected_pairs;
                if (error) error->clear();
                return true;
            }
            if (raw.baseData.empty() || raw.dependentData.empty()) {
                if (++diagnostics_.rejected_pairs >= kRecoveryMismatchLimit) {
                    if (error) *error = "MVC recovery lost an eye after phase lock";
                    return false;
                }
                continue;
            }
            const std::int64_t delta = static_cast<std::int64_t>(raw.depTimestamp)
                    - static_cast<std::int64_t>(raw.timestamp);
            if (std::llabs(delta - calibrated_offset_ms_) > offset_tolerance_ms_) {
                if (++diagnostics_.rejected_pairs >= kRecoveryMismatchLimit) {
                    if (error) *error = "MVC recovery source-pair offset deviation limit";
                    return false;
                }
                continue;
            }
            FramePair corrected;
            if (!phase_aligner_.push(std::move(raw), corrected)) {
                refreshAlignerDiagnostics();
                continue;
            }
            if (!acceptCorrected(std::move(corrected), error)) return false;
            output = std::move(ready_.front());
            ready_.pop_front();
            if (error) error->clear();
            return true;
        }
        if (error) error->clear();
        return false;
    }

    const OpenedVideo& opened() const { return opened_; }
    const RecoveryDiagnostics& diagnostics() const { return diagnostics_; }

private:
    bool acceptCorrected(FramePair&& corrected, std::string* error) {
        const std::int64_t corrected_delta = static_cast<std::int64_t>(corrected.depTimestamp)
                - static_cast<std::int64_t>(corrected.timestamp);
        if (std::llabs(corrected_delta) > kRecoveryPhaseResidualToleranceMs) {
            phase_aligner_.loseLock();
            if (error) *error = "MVC recovery corrected source-pair phase lock was lost";
            return false;
        }
        if (diagnostics_.first_corrected_timestamp_ms < 0) {
            diagnostics_.first_corrected_timestamp_ms =
                    static_cast<std::int64_t>(corrected.timestamp);
        }
        ready_.push_back(std::move(corrected));
        refreshAlignerDiagnostics();
        return true;
    }

    void refreshAlignerDiagnostics() {
        diagnostics_.phase_shift_frames = phase_aligner_.phaseShiftFrames();
        diagnostics_.phase_residual_ms = phase_aligner_.phaseResidualMs();
        diagnostics_.leading_base_discards = phase_aligner_.leadingBaseDiscards();
        diagnostics_.leading_dependent_discards = phase_aligner_.leadingDependentDiscards();
        diagnostics_.corrected_pairs = phase_aligner_.correctedPairs();
        diagnostics_.corrected_delta_first_ms = phase_aligner_.correctedDeltaFirstMs();
        diagnostics_.corrected_delta_min_ms = phase_aligner_.correctedDeltaMinimumMs();
        diagnostics_.corrected_delta_median_ms = phase_aligner_.correctedDeltaMedianMs();
        diagnostics_.corrected_delta_max_ms = phase_aligner_.correctedDeltaMaximumMs();
    }

    OpenedVideo opened_;
    RecoveryDiagnostics diagnostics_;
    MvcSsifPhaseAligner phase_aligner_;
    std::deque<FramePair> ready_;
    std::int64_t calibrated_offset_ms_ = 0;
    std::int64_t offset_tolerance_ms_ = 0;
};

struct OpenedAudio {
    std::unique_ptr<sylc_audio::M2TSAudioDemuxer> demuxer = std::make_unique<sylc_audio::M2TSAudioDemuxer>();
    std::vector<sylc_audio::AudioTrackInfo> tracks;
};

bool openAudioSegment(const std::shared_ptr<sylc_iso::FeatureVolume>& volume,
                      std::size_t index,
                      std::int64_t global_offset_us,
                      OpenedAudio* opened,
                      std::string* error) {
    std::uint64_t size = 0;
    std::string label;
    auto stream = sylc_iso::openAudioStream(volume, index, &size, &label, error);
    if (!stream) return false;
    std::vector<sylc_audio::AudioTrackInfo> declared;
    const auto& segment = sylc_iso::selection(volume).segments[index];
    for (const auto& source : segment.declared_audio) {
        sylc_audio::AudioTrackInfo track;
        track.index = static_cast<int>(declared.size());
        track.pid = source.pid;
        track.stream_type = source.coding_type;
        track.language = source.language;
        declared.push_back(std::move(track));
    }
    std::uint64_t catalog_probe_byte = 0;
    sylc_iso::ClpiAudioSeekAnchor anchor;
    std::string anchor_error;
    if (sylc_iso::planAudioSeek(volume, index, global_offset_us, global_offset_us,
                                &anchor, &anchor_error)) {
        catalog_probe_byte = anchor.byte_offset;
    }
    if (!opened->demuxer->openStream(std::move(stream), size, label, declared,
                                    catalog_probe_byte, error)) {
        return false;
    }
    opened->tracks = opened->demuxer->tracks();
    return true;
}

int runProbe(const Options& options,
             const std::shared_ptr<sylc_iso::FeatureVolume>& volume) {
    const auto& feature = sylc_iso::selection(volume);
    if (feature.kind != "ssif") {
        throw std::runtime_error("selected Blu-ray feature has no SSIF MVC stream");
    }
    std::string error;
    OpenedVideo video;
    if (!openVideoSegment(volume, 0, 0, 0, &video, &error)) {
        throw std::runtime_error(error.empty()
                ? "could not open first ISO feature segment" : error);
    }
    const auto info = video.info;
    OpenedAudio audio;
    if (!openAudioSegment(volume, 0, 0, &audio, &error)) {
        throw std::runtime_error(error.empty() ? "could not probe Blu-ray audio" : error);
    }
    std::ostringstream output;
    output << "{\n"
              << "  \"ok\": true,\n"
              << "  \"sourceType\": \"bluray-iso\",\n"
              << "  \"playlist\": \"" << jsonEscape(feature.playlist) << "\",\n"
              << "  \"selectionMethod\": \"" << jsonEscape(feature.method) << "\",\n"
              << "  \"durationSeconds\": " << std::fixed << std::setprecision(6) << feature.duration_s << ",\n"
              << "  \"width\": " << info.width << ",\n"
              << "  \"height\": " << info.height << ",\n"
              << "  \"fps\": " << std::setprecision(9) << info.fps << ",\n"
              << "  \"hasMVC\": " << (info.hasMVC ? "true" : "false") << ",\n"
              << "  \"baseVideoPid\": " << info.baseVideoPid << ",\n"
              << "  \"mvcVideoPid\": " << info.mvcVideoPid << ",\n"
              << "  \"segmentCount\": " << feature.segments.size() << ",\n"
              << "  \"decoysFiltered\": " << feature.decoys_filtered << ",\n"
              << "  \"libblurayAuthoritative\": " << (feature.libbluray_authoritative ? "true" : "false") << ",\n"
              << "  \"libblurayVersion\": \"" << feature.libbluray_major << '.' << feature.libbluray_minor << '.' << feature.libbluray_micro << "\",\n"
              << "  \"selectedTitleIndex\": " << feature.authoritative_title_index << ",\n"
              << "  \"titleCount\": " << feature.authoritative_title_count << ",\n"
              << "  \"mainTitleHint\": " << feature.authoritative_main_title << ",\n"
              << "  \"videoSeekBaseEpAvailable\": " << (video.seek.base_ep_available ? "true" : "false") << ",\n"
              << "  \"videoSeekExactSsifAvailable\": " << (video.seek.exact_ssif_available ? "true" : "false") << ",\n"
              << "  \"videoTimelineOriginMs\": " << video.seek.timeline_origin_ms << ",\n"
              << "  \"audioTracks\": [";
    for (std::size_t i = 0; i < audio.tracks.size(); ++i) {
        const auto& track = audio.tracks[i];
        if (i) output << ',';
        output << "\n    {\"index\":" << i
                  << ",\"pid\":" << track.pid
                  << ",\"streamType\":" << static_cast<unsigned>(track.stream_type)
                  << ",\"format\":\"" << audioFormat(track) << "\""
                  << ",\"profile\":\"" << jsonEscape(track.profile) << "\""
                  << ",\"language\":\"" << jsonEscape(track.language) << "\""
                  << ",\"channels\":" << track.channels
                  << ",\"sampleRate\":" << track.sample_rate
                  << ",\"supported\":" << (track.supported && audioFormat(track) != "unsupported" ? "true" : "false")
                  << ",\"decodePath\":\"" << jsonEscape(track.decode_path) << "\""
                  << ",\"truehdMajorSync\":" << (track.truehd_major_sync ? "true" : "false")
                  << ",\"embeddedAc3Core\":" << (track.embedded_ac3_core ? "true" : "false")
                  << '}';
    }
    if (!audio.tracks.empty()) output << '\n';
    output << "  ],\n"
              << "  \"subtitleTracks\": [";
    const auto& declared_subtitles = feature.segments.front().declared_subtitles;
    for (std::size_t i = 0; i < declared_subtitles.size(); ++i) {
        const auto& track = declared_subtitles[i];
        if (i) output << ',';
        output << "\n    {\"index\":" << i
                  << ",\"pid\":" << track.pid
                  << ",\"streamType\":" << static_cast<unsigned>(track.coding_type)
                  << ",\"format\":\"pgs\""
                  << ",\"profile\":\"Blu-ray PGS\""
                  << ",\"language\":\"" << jsonEscape(track.language) << "\""
                  << ",\"supported\":true}";
    }
    if (!declared_subtitles.empty()) output << '\n';
    output << "  ],\n"
              << "  \"diagnostics\": \"" << jsonEscape(sylc_iso::diagnostics(volume)) << "\"\n"
              << "}\n";
    const std::string text = output.str();
    writeAll(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return 0;
}

int runVideoPlan(const Options& options,
                 const std::shared_ptr<sylc_iso::FeatureVolume>& volume) {
    const auto& feature = sylc_iso::selection(volume);
    if (feature.kind != "ssif") {
        throw std::runtime_error("selected title is not an SSIF MVC feature");
    }
    const auto offsets = segmentOffsetsUs(feature);
    const std::int64_t start_us = static_cast<std::int64_t>(
            options.start_seconds * 1000000.0 + 0.5);
    const std::size_t first_segment = segmentForStart(feature, offsets, start_us);
    std::uint64_t corrected_index = 0;
    std::uint64_t preroll_pairs = 0;
    std::uint64_t skip_pairs = 0;
    std::int64_t first_demux_us = -1;
    std::int64_t first_corrected_us = -1;
    std::int64_t first_output_us = -1;
    std::string seek_detail;
    std::string first_clip;
    RecoveryDiagnostics recovery;
    bool recovery_captured = false;
    bool target_reached = false;
    constexpr std::uint64_t kMaximumPlanPairs = 4800;

    for (std::size_t index = first_segment;
         index < feature.segments.size() && first_output_us < 0; ++index) {
        const std::int64_t local_start_ms = index == first_segment
                ? std::max<std::int64_t>(0, (start_us - offsets[index]) / 1000) : 0;
        RecoveredVideoReader reader;
        std::string error;
        if (!reader.open(volume, index, local_start_ms,
                         index == first_segment ? options.recovery_backoff_entries : 0,
                         &error)) {
            throw std::runtime_error(error.empty() ? "could not plan ISO video seek" : error);
        }
        if (!recovery_captured) {
            recovery = reader.diagnostics();
            recovery_captured = true;
            first_clip = feature.segments[index].clip;
            seek_detail = reader.opened().seek.detail;
            if (recovery.first_raw_timestamp_ms >= 0) {
                first_demux_us = offsets[index] + recovery.first_raw_timestamp_ms * 1000LL;
            }
        }

        mvc_demux::MVCSSIFDemuxer::FramePair pair;
        for (;;) {
            std::string read_error;
            if (!reader.next(pair, &read_error)) {
                if (!read_error.empty()) throw std::runtime_error(read_error);
                break;
            }
            const std::int64_t global_pts_us = offsets[index]
                    + static_cast<std::int64_t>(pair.timestamp) * 1000;
            if (first_demux_us < 0) first_demux_us = global_pts_us;
            if (first_corrected_us < 0) first_corrected_us = global_pts_us;

            if (!target_reached) {
                if (global_pts_us + 500 < start_us) {
                    ++preroll_pairs;
                } else {
                    target_reached = true;
                    skip_pairs = std::max<std::uint64_t>(
                            preroll_pairs,
                            start_us > 0 ? kRecoveryHiddenCleanPairs : 0);
                }
            }
            if (target_reached && corrected_index >= skip_pairs) {
                first_output_us = global_pts_us;
                recovery = reader.diagnostics();
                break;
            }
            ++corrected_index;
            if (corrected_index > kMaximumPlanPairs) {
                throw std::runtime_error(
                        "Blu-ray seek recovery/preroll exceeded 4800 corrected stereo pairs");
            }
        }
    }
    if (first_demux_us < 0 || first_corrected_us < 0 || first_output_us < 0) {
        throw std::runtime_error("Blu-ray seek plan reached end of title before clean release");
    }

    std::ostringstream output;
    output << "{\n"
           << "  \"ok\": true,\n"
           << "  \"requestedStartSeconds\": " << std::fixed << std::setprecision(6)
           << options.start_seconds << ",\n"
           << "  \"actualDemuxStartSeconds\": " << first_demux_us / 1000000.0 << ",\n"
           << "  \"firstCorrectedPairSeconds\": " << first_corrected_us / 1000000.0 << ",\n"
           << "  \"firstOutputSeconds\": " << first_output_us / 1000000.0 << ",\n"
           << "  \"cleanReleaseSeconds\": " << first_output_us / 1000000.0 << ",\n"
           << "  \"targetPrerollPairs\": " << preroll_pairs << ",\n"
           << "  \"recoveryHiddenPairs\": "
           << (start_us > 0 ? kRecoveryHiddenCleanPairs : 0) << ",\n"
           << "  \"recoveryStructuralPairs\": "
           << (start_us > 0 ? kRecoveryStructuralValidationPairs : 0) << ",\n"
           << "  \"recoveryStabilizationPairs\": "
           << (start_us > 0 ? kRecoveryAdditionalStabilizationPairs : 0) << ",\n"
           << "  \"skipPairs\": " << skip_pairs << ",\n"
           << "  \"firstClip\": \"" << jsonEscape(first_clip) << "\",\n"
           << "  \"seekDetail\": \"" << jsonEscape(seek_detail) << "\",\n"
           << "  \"recoveryEnabled\": " << (recovery.enabled ? "true" : "false") << ",\n"
           << "  \"recoveryBackoffEntries\": " << recovery.backoff_entries << ",\n"
           << "  \"recoveryAnchorSeconds\": " << recovery.anchor_local_ms / 1000.0 << ",\n"
           << "  \"recoverySeekMethod\": \"" << jsonEscape(recovery.seek_method) << "\",\n"
           << "  \"calibrationSamples\": " << recovery.calibration_samples << ",\n"
           << "  \"calibrationInliers\": " << recovery.calibration_inliers << ",\n"
           << "  \"sourceDeltaFirstMs\": " << recovery.delta_first_ms << ",\n"
           << "  \"sourceDeltaMinimumMs\": " << recovery.delta_min_ms << ",\n"
           << "  \"sourceDeltaMedianMs\": " << recovery.delta_median_ms << ",\n"
           << "  \"sourceDeltaMaximumMs\": " << recovery.delta_max_ms << ",\n"
           << "  \"calibratedDependentMinusBaseMs\": "
           << recovery.calibrated_offset_ms << ",\n"
           << "  \"phaseShiftFrames\": " << recovery.phase_shift_frames << ",\n"
           << "  \"phaseResidualMs\": " << recovery.phase_residual_ms << ",\n"
           << "  \"leadingBaseDiscards\": " << recovery.leading_base_discards << ",\n"
           << "  \"leadingDependentDiscards\": "
           << recovery.leading_dependent_discards << ",\n"
           << "  \"correctedPairCountAtPlan\": " << recovery.corrected_pairs << ",\n"
           << "  \"rejectedSourcePairs\": " << recovery.rejected_pairs << ",\n"
           << "  \"correctedDeltaFirstMs\": " << recovery.corrected_delta_first_ms << ",\n"
           << "  \"correctedDeltaMinimumMs\": " << recovery.corrected_delta_min_ms << ",\n"
           << "  \"correctedDeltaMedianMs\": " << recovery.corrected_delta_median_ms << ",\n"
           << "  \"correctedDeltaMaximumMs\": " << recovery.corrected_delta_max_ms << "\n"
           << "}\n";
    const std::string text = output.str();
    writeAll(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return 0;
}

int runVideo(const Options& options,
             const std::shared_ptr<sylc_iso::FeatureVolume>& volume) {
    const auto& feature = sylc_iso::selection(volume);
    if (feature.kind != "ssif") {
        throw std::runtime_error("selected title is not an SSIF MVC feature");
    }
    const auto offsets = segmentOffsetsUs(feature);
    const std::int64_t start_us = static_cast<std::int64_t>(
            options.start_seconds * 1000000.0 + 0.5);
    const std::size_t first_segment = segmentForStart(feature, offsets, start_us);
    std::uint64_t pairs = 0;
    std::uint64_t bytes = 0;
    bool first_output = true;

    for (std::size_t index = first_segment; index < feature.segments.size(); ++index) {
        const std::int64_t local_start_ms = index == first_segment
                ? std::max<std::int64_t>(0, (start_us - offsets[index]) / 1000) : 0;
        RecoveredVideoReader reader;
        std::string error;
        if (!reader.open(volume, index, local_start_ms,
                         index == first_segment ? options.recovery_backoff_entries : 0,
                         &error)) {
            throw std::runtime_error(error.empty() ? "could not open ISO video segment" : error);
        }
        const auto& codec_private = reader.opened().demuxer->getCodecPrivate();
        if (!codec_private.empty()) {
            writeAll(codec_private.data(), codec_private.size());
            bytes += codec_private.size();
        }
        const auto initial = reader.diagnostics();
        if (initial.enabled) {
            std::cerr << "ISO_MVC_RECOVERY enabled=1"
                      << " clip=" << feature.segments[index].clip
                      << " requested_local_ms=" << initial.requested_local_ms
                      << " anchor_local_ms=" << initial.anchor_local_ms
                      << " backoff_entries=" << initial.backoff_entries
                      << " method=\"" << initial.seek_method << "\""
                      << " calibration_samples=" << initial.calibration_samples
                      << " calibration_inliers=" << initial.calibration_inliers
                      << " delta_first_ms=" << initial.delta_first_ms
                      << " delta_min_ms=" << initial.delta_min_ms
                      << " delta_median_ms=" << initial.delta_median_ms
                      << " delta_max_ms=" << initial.delta_max_ms
                      << " calibrated_offset_ms=" << initial.calibrated_offset_ms
                      << " phase_shift_frames=" << initial.phase_shift_frames
                      << " phase_residual_ms=" << initial.phase_residual_ms
                      << " structural_hidden_pairs=" << kRecoveryStructuralValidationPairs
                      << " stabilization_hidden_pairs="
                      << kRecoveryAdditionalStabilizationPairs
                      << " total_hidden_pairs=" << kRecoveryHiddenCleanPairs << "\n";
        }

        mvc_demux::MVCSSIFDemuxer::FramePair pair;
        for (;;) {
            std::string read_error;
            if (!reader.next(pair, &read_error)) {
                if (!read_error.empty()) throw std::runtime_error(read_error);
                break;
            }
            ++pairs;
            const std::int64_t global_pts_us = offsets[index]
                    + static_cast<std::int64_t>(pair.timestamp) * 1000;
            if (!pair.baseData.empty()) {
                writeAll(pair.baseData.data(), pair.baseData.size());
                bytes += pair.baseData.size();
            }
            if (!pair.dependentData.empty()) {
                writeAll(pair.dependentData.data(), pair.dependentData.size());
                bytes += pair.dependentData.size();
            }
            if (first_output) {
                std::cerr << "ISO_VIDEO_FIRST_CORRECTED source_pts_us=" << global_pts_us
                          << " clip=" << feature.segments[index].clip
                          << " base_bytes=" << pair.baseData.size()
                          << " dependent_bytes=" << pair.dependentData.size() << "\n";
                first_output = false;
            }
        }
        const auto final = reader.diagnostics();
        if (final.enabled) {
            std::cerr << "ISO_MVC_RECOVERY_RESULT"
                      << " corrected_pairs=" << final.corrected_pairs
                      << " rejected_pairs=" << final.rejected_pairs
                      << " phase_shift_frames=" << final.phase_shift_frames
                      << " phase_residual_ms=" << final.phase_residual_ms
                      << " leading_base_discards=" << final.leading_base_discards
                      << " leading_dependent_discards=" << final.leading_dependent_discards
                      << " corrected_delta_first_ms=" << final.corrected_delta_first_ms
                      << " corrected_delta_min_ms=" << final.corrected_delta_min_ms
                      << " corrected_delta_median_ms=" << final.corrected_delta_median_ms
                      << " corrected_delta_max_ms=" << final.corrected_delta_max_ms << "\n";
        }
    }
    std::cerr << "ISO_VIDEO_RESULT pairs=" << pairs
              << " bytes=" << bytes << " playlist=" << feature.playlist << "\n";
    return first_output ? 1 : 0;
}

std::size_t findTrack(const std::vector<sylc_audio::AudioTrackInfo>& tracks,
                      std::uint16_t pid, std::uint8_t type) {
    for (std::size_t i = 0; i < tracks.size(); ++i) if (tracks[i].pid == pid) return i;
    for (std::size_t i = 0; i < tracks.size(); ++i) if (tracks[i].stream_type == type) return i;
    return static_cast<std::size_t>(-1);
}


void writeBe32(std::uint32_t value) {
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    writeAll(bytes, sizeof(bytes));
}

void writeBe16(std::uint16_t value) {
    const std::uint8_t bytes[2] = {
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    writeAll(bytes, sizeof(bytes));
}

void writeSupSegment(const sylc_pgs::PgsSegment& segment, std::int64_t pts90k) {
    const std::uint8_t signature[2] = {'P', 'G'};
    writeAll(signature, sizeof(signature));
    const auto timestamp = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, pts90k));
    writeBe32(timestamp);
    writeBe32(timestamp);
    writeAll(&segment.type, 1);
    writeBe16(static_cast<std::uint16_t>(segment.payload.size()));
    if (!segment.payload.empty()) writeAll(segment.payload.data(), segment.payload.size());
}

class SupDisplayWriter {
public:
    explicit SupDisplayWriter(std::int64_t target90k) : target90k_(target90k) {}

    void feed(sylc_pgs::PgsSegment segment) {
        current_.push_back(std::move(segment));
        if (current_.back().type != 0x80) return;
        const std::int64_t pts = displayPts(current_);
        if (pts <= target90k_) {
            before_.push_back(current_);
        } else {
            ensureInitialState(&current_);
            emit(current_, pts - target90k_);
        }
        current_.clear();
    }

    void finish() {
        if (!current_.empty()) {
            const std::int64_t pts = displayPts(current_);
            if (pts <= target90k_) before_.push_back(current_);
            else {
                ensureInitialState(&current_);
                emit(current_, pts - target90k_);
            }
            current_.clear();
        }
        ensureInitialState(nullptr);
    }

    std::uint64_t displaySets() const { return display_sets_; }
    std::uint64_t segments() const { return segments_; }
    bool restoredInitialState() const { return restored_initial_state_; }
    bool wroteTimelineAnchor() const { return wrote_timeline_anchor_; }

private:
    std::int64_t target90k_ = 0;
    std::vector<sylc_pgs::PgsSegment> current_;
    std::vector<std::vector<sylc_pgs::PgsSegment>> before_;
    bool output_started_ = false;
    bool restored_initial_state_ = false;
    bool wrote_timeline_anchor_ = false;
    std::uint64_t display_sets_ = 0;
    std::uint64_t segments_ = 0;

    static std::int64_t displayPts(const std::vector<sylc_pgs::PgsSegment>& set) {
        for (const auto& segment : set) if (segment.pts90k >= 0) return segment.pts90k;
        return 0;
    }

    bool emitClearTimelineAnchor(
            const std::vector<sylc_pgs::PgsSegment>& reference) {
        for (const auto& segment : reference) {
            if (segment.type != 0x16 || segment.payload.size() < 11) continue;

            // A standalone SUP input whose first authored event begins later
            // than zero is rebased by FFmpeg to timestamp zero. That made the
            // first ISO PGS caption appear at movie startup. Seed the stream
            // with a transparent epoch-start PCS at zero so FFmpeg preserves
            // every later authored timestamp. The width, height, and frame-rate
            // fields come from the first real PCS; no objects are presented.
            sylc_pgs::PgsSegment clear_pcs;
            clear_pcs.type = 0x16;
            clear_pcs.payload.assign(segment.payload.begin(),
                                     segment.payload.begin() + 11);
            const std::uint16_t composition = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(clear_pcs.payload[5]) << 8U)
                    | clear_pcs.payload[6]);
            const std::uint16_t anchor_composition = static_cast<std::uint16_t>(
                    composition - 1U);
            clear_pcs.payload[5] = static_cast<std::uint8_t>(
                    (anchor_composition >> 8U) & 0xffU);
            clear_pcs.payload[6] = static_cast<std::uint8_t>(
                    anchor_composition & 0xffU);
            clear_pcs.payload[7] = 0x80;  // epoch start
            clear_pcs.payload[8] = 0x00;  // palette update flag
            clear_pcs.payload[9] = 0x00;  // palette id
            clear_pcs.payload[10] = 0x00; // no composition objects
            clear_pcs.pts90k = 0;
            clear_pcs.dts90k = 0;

            sylc_pgs::PgsSegment end;
            end.type = 0x80;
            end.pts90k = 0;
            end.dts90k = 0;

            emit({clear_pcs, end}, 0);
            wrote_timeline_anchor_ = true;
            return true;
        }
        return false;
    }

    void ensureInitialState(
            const std::vector<sylc_pgs::PgsSegment>* first_future_set) {
        if (output_started_) return;
        output_started_ = true;
        if (!before_.empty()) {
            // Replaying the bounded preroll display sets at timestamp zero
            // restores any palette/object/window definitions reused by the
            // active composition at the requested seek point. Emitting only
            // the final PCS is insufficient because Blu-ray PGS may reference
            // ODS/PDS data from an earlier display set.
            for (const auto& set : before_) emit(set, 0);
            restored_initial_state_ = true;
            return;
        }
        if (first_future_set) emitClearTimelineAnchor(*first_future_set);
    }

    void emit(const std::vector<sylc_pgs::PgsSegment>& set, std::int64_t pts90k) {
        for (const auto& segment : set) {
            writeSupSegment(segment, pts90k);
            ++segments_;
        }
        ++display_sets_;
    }
};

int runAudio(const Options& options,
             const std::shared_ptr<sylc_iso::FeatureVolume>& volume) {
    const auto& feature = sylc_iso::selection(volume);
    const auto offsets = segmentOffsetsUs(feature);
    const std::int64_t start_us = static_cast<std::int64_t>(options.start_seconds * 1000000.0 + 0.5);
    std::size_t segment = segmentForStart(feature, offsets, start_us);
    OpenedAudio opened;
    std::string error;
    if (!openAudioSegment(volume, segment, offsets[segment], &opened, &error)) {
        throw std::runtime_error(error.empty() ? "could not open ISO audio" : error);
    }
    if (options.audio_track >= opened.tracks.size()) {
        throw std::runtime_error("requested ISO audio track index is out of range");
    }
    const auto selected = opened.tracks[options.audio_track];
    if (audioFormat(selected) == "unsupported") {
        throw std::runtime_error("selected Blu-ray audio track is not supported by the HLS bridge");
    }

    auto selectCurrent = [&](std::int64_t target_us) {
        const std::size_t local = findTrack(opened.demuxer->tracks(), selected.pid, selected.stream_type);
        if (local == static_cast<std::size_t>(-1)) throw std::runtime_error("selected audio track is absent from a feature segment");
        sylc_iso::ClpiAudioSeekAnchor anchor;
        std::string plan_error;
        if ((target_us > offsets[segment] || feature.segments[segment].in45k > 0)
                && sylc_iso::planAudioSeek(volume, segment, target_us, offsets[segment], &anchor, &plan_error)) {
            constexpr std::uint64_t budget = 128ULL << 20U;
            if (opened.demuxer->selectTrackAt(local, target_us, offsets[segment],
                                             anchor.byte_offset, anchor.clip_in_us,
                                             anchor.anchor_raw_pts_us, budget, &error)) {
                std::cerr << "ISO_AUDIO_SEEK " << anchor.detail << "\n";
                return;
            }
        }
        error.clear();
        if (!opened.demuxer->selectTrack(local, target_us, offsets[segment], &error)) {
            throw std::runtime_error(error.empty() ? "ISO audio selection failed" : error);
        }
    };
    selectCurrent(start_us);

    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::int64_t first_pts = -1;
    for (;;) {
        sylc_audio::CompressedAudioSample sample;
        std::string read_error;
        if (opened.demuxer->readNextSample(sample, &read_error)) {
            if (sample.data.empty()) continue;
            if (first_pts < 0) {
                first_pts = sample.pts_us;
                std::cerr << "ISO_AUDIO_FIRST pts_us=" << first_pts
                          << " format=" << audioFormat(selected)
                          << " decode_path=\"" << selected.decode_path << "\""
                          << " pid=" << selected.pid << "\n";
            }
            writeAll(sample.data.data(), sample.data.size());
            ++samples;
            bytes += sample.data.size();
            continue;
        }
        if (!read_error.empty()) throw std::runtime_error(read_error);
        if (segment + 1 >= feature.segments.size()) break;
        ++segment;
        opened = OpenedAudio{};
        if (!openAudioSegment(volume, segment, offsets[segment], &opened, &error)) {
            throw std::runtime_error(error.empty() ? "could not open later ISO audio segment" : error);
        }
        selectCurrent(offsets[segment]);
    }
    std::cerr << opened.demuxer->diagnostics() << "\n";
    std::cerr << "ISO_AUDIO_RESULT samples=" << samples << " bytes=" << bytes
              << " first_pts_us=" << first_pts
              << " format=" << audioFormat(selected)
              << " decode_path=\"" << selected.decode_path << "\""
              << " truehd_major_sync=" << (selected.truehd_major_sync ? 1 : 0)
              << " embedded_ac3_core=" << (selected.embedded_ac3_core ? 1 : 0)
              << "\n";
    return samples == 0 ? 1 : 0;
}

int runSubtitle(const Options& options,
                const std::shared_ptr<sylc_iso::FeatureVolume>& volume) {
    const auto& feature = sylc_iso::selection(volume);
    if (feature.segments.empty()) throw std::runtime_error("selected title has no feature segments");
    if (options.subtitle_track >= feature.segments.front().declared_subtitles.size()) {
        throw std::runtime_error("requested ISO subtitle track index is out of range");
    }

    const auto offsets_us = segmentOffsetsUs(feature);
    const std::int64_t target_us = static_cast<std::int64_t>(
            options.start_seconds * 1000000.0 + 0.5);
    constexpr std::int64_t kSubtitlePrerollUs = 120LL * 1000000LL;
    const std::int64_t scan_start_us = std::max<std::int64_t>(0, target_us - kSubtitlePrerollUs);
    std::size_t segment_index = segmentForStart(feature, offsets_us, scan_start_us);
    SupDisplayWriter writer((target_us * 90000LL + 500000LL) / 1000000LL);
    std::uint64_t total_packets = 0;
    std::uint64_t total_pes = 0;
    std::uint64_t total_segments = 0;

    for (; segment_index < feature.segments.size(); ++segment_index) {
        const auto& feature_segment = feature.segments[segment_index];
        if (options.subtitle_track >= feature_segment.declared_subtitles.size()) {
            throw std::runtime_error("selected PGS track is absent from a later feature segment");
        }
        const auto& declared = feature_segment.declared_subtitles[options.subtitle_track];
        if (declared.coding_type != 0x90) {
            throw std::runtime_error("selected Blu-ray subtitle stream is not PGS");
        }

        std::uint64_t size = 0;
        std::string label;
        std::string error;
        auto stream = sylc_iso::openAudioStream(volume, segment_index, &size, &label, &error);
        if (!stream) throw std::runtime_error(error.empty() ? "could not open ISO PGS source" : error);

        sylc_pgs::M2TSPgsDemuxer demuxer;
        if (!demuxer.openStream(std::move(stream), size, label, declared.pid, &error)) {
            throw std::runtime_error(error.empty() ? "could not open ISO PGS demuxer" : error);
        }

        std::uint64_t byte_offset = 0;
        std::int64_t clip_in_raw90k = static_cast<std::int64_t>(feature_segment.in45k) * 2LL;
        std::int64_t anchor_raw90k = clip_in_raw90k;
        const std::int64_t segment_scan_us = segment_index == segmentForStart(feature, offsets_us, scan_start_us)
                ? scan_start_us : offsets_us[segment_index];
        sylc_iso::ClpiAudioSeekAnchor anchor;
        std::string plan_error;
        if ((segment_scan_us > offsets_us[segment_index] || feature_segment.in45k > 0)
                && sylc_iso::planAudioSeek(volume, segment_index, segment_scan_us,
                                           offsets_us[segment_index], &anchor, &plan_error)) {
            byte_offset = anchor.byte_offset;
            clip_in_raw90k = (anchor.clip_in_us * 90000LL + 500000LL) / 1000000LL;
            anchor_raw90k = (anchor.anchor_raw_pts_us * 90000LL + 500000LL) / 1000000LL;
            std::cerr << "ISO_SUBTITLE_SEEK " << anchor.detail << "\n";
        }
        const std::int64_t global_offset90k =
                (offsets_us[segment_index] * 90000LL + 500000LL) / 1000000LL;
        if (!demuxer.selectAt(global_offset90k, byte_offset, clip_in_raw90k,
                              anchor_raw90k, &error)) {
            throw std::runtime_error(error.empty() ? "ISO PGS selection failed" : error);
        }

        sylc_pgs::PgsSegment segment;
        while (demuxer.readNextSegment(segment, &error)) writer.feed(std::move(segment));
        if (!error.empty()) throw std::runtime_error(error);
        total_packets += demuxer.packetCount();
        total_pes += demuxer.pesCount();
        total_segments += demuxer.segmentCount();
        std::cerr << demuxer.diagnostics() << "\n";
    }

    writer.finish();
    std::cerr << "ISO_SUBTITLE_RESULT track=" << options.subtitle_track
              << " packets=" << total_packets
              << " pes=" << total_pes
              << " parsed_segments=" << total_segments
              << " output_display_sets=" << writer.displaySets()
              << " output_segments=" << writer.segments()
              << " restored_initial_state=" << (writer.restoredInitialState() ? 1 : 0)
              << " timeline_anchor=" << (writer.wroteTimelineAnchor() ? 1 : 0)
              << " preroll_seconds=120\n";
    return writer.segments() == 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::cout.rdbuf(std::cerr.rdbuf());
    try {
        const Options options = parseOptions(argc, argv);
        std::string error;
        auto volume = sylc_iso::openFeatureVolumePath(options.input, &error);
        if (!volume) throw std::runtime_error(error.empty() ? "could not open Blu-ray ISO" : error);
        switch (options.mode) {
            case Options::Mode::Probe: return runProbe(options, volume);
            case Options::Mode::PlanVideoSeek: return runVideoPlan(options, volume);
            case Options::Mode::Video: return runVideo(options, volume);
            case Options::Mode::Audio: return runAudio(options, volume);
            case Options::Mode::Subtitle: return runSubtitle(options, volume);
        }
    } catch (const OutputClosed&) {
        std::cerr << "ISO_SOURCE_RESULT consumer_closed=1 status=normal_downstream_shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FATAL: " << error.what() << "\n";
        return 1;
    }
    return 1;
}
