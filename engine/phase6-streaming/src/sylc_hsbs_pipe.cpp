#include "edge264/edge264.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <istream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>
#include <immintrin.h>

namespace {

using Clock = std::chrono::steady_clock;

enum class OutputMode {
    HalfSbs,
    FullSbs,
    HalfOu,
    FullOu,
    LeftEye,
    RightEye,
};

const char* outputModeName(OutputMode mode) {
    switch (mode) {
        case OutputMode::HalfSbs: return "half-sbs";
        case OutputMode::FullSbs: return "full-sbs";
        case OutputMode::HalfOu: return "half-ou";
        case OutputMode::FullOu: return "full-ou";
        case OutputMode::LeftEye: return "left-eye";
        case OutputMode::RightEye: return "right-eye";
    }
    return "unknown";
}

OutputMode parseOutputMode(const std::string& value) {
    if (value == "half-sbs") return OutputMode::HalfSbs;
    if (value == "full-sbs") return OutputMode::FullSbs;
    if (value == "half-ou") return OutputMode::HalfOu;
    if (value == "full-ou") return OutputMode::FullOu;
    if (value == "left-eye") return OutputMode::LeftEye;
    if (value == "right-eye") return OutputMode::RightEye;
    throw std::invalid_argument("Unsupported output mode: " + value);
}

struct Options {
    std::string input;
    int threads = 0;
    int max_pairs = 0;
    int skip_pairs = 0;
    double source_fps = 24000.0 / 1001.0;
    OutputMode output_mode = OutputMode::HalfSbs;
    bool swap_eyes = false;
    bool discard_output = false;
};

struct Stats {
    std::uint64_t nals = 0;
    std::uint64_t decoder_outputs = 0;
    std::uint64_t pairs = 0;
    std::uint64_t emitted_pairs = 0;
    std::uint64_t skipped_pairs = 0;
    std::uint64_t base_only = 0;
    std::uint64_t mvc_only = 0;
    std::uint64_t poc_mismatches = 0;
    std::uint64_t errors = 0;
    std::uint64_t retries = 0;
    std::uint64_t ignored_tiny_type24 = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t output_hash = 1469598103934665603ULL;
    int width = 0;
    int height = 0;
    double wall_seconds = 0.0;
    double cpu_seconds = 0.0;
};

[[noreturn]] void usage(const char* program, const std::string& message = {}) {
    if (!message.empty()) std::cerr << "ERROR: " << message << "\n\n";
    std::cerr
        << "Usage: " << program << " --input FILE.h264|- [options]\n"
        << "  --threads N          edge264 workers (default 0)\n"
        << "  --source-fps FPS     source stereo-pair rate\n"
        << "  --max-pairs N        stop after N emitted pairs (0 = all)\n"
        << "  --skip-pairs N       decode but do not emit the first N pairs\n"
        << "  --mode MODE          half-sbs, full-sbs, half-ou, full-ou, left-eye, right-eye\n"
        << "  --swap-eyes          exchange the base/dependent eye assignment\n"
        << "  --discard-output     compose and hash without writing raw video\n";
    std::exit(message.empty() ? 0 : 2);
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) usage(argv[0], std::string("Missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--input") options.input = value("--input");
        else if (arg == "--threads") options.threads = std::stoi(value("--threads"));
        else if (arg == "--source-fps") options.source_fps = std::stod(value("--source-fps"));
        else if (arg == "--max-pairs") options.max_pairs = std::stoi(value("--max-pairs"));
        else if (arg == "--skip-pairs") options.skip_pairs = std::stoi(value("--skip-pairs"));
        else if (arg == "--mode") {
            try {
                options.output_mode = parseOutputMode(value("--mode"));
            } catch (const std::invalid_argument& error) {
                usage(argv[0], error.what());
            }
        }
        else if (arg == "--swap-eyes") options.swap_eyes = true;
        else if (arg == "--discard-output") options.discard_output = true;
        else if (arg == "--help" || arg == "-h") usage(argv[0]);
        else usage(argv[0], "Unknown option: " + arg);
    }
    if (options.input.empty()) usage(argv[0], "--input is required");
    if (options.threads != 0) usage(argv[0], "the Phase 6 streaming build currently requires --threads 0");
    if (options.max_pairs < 0) usage(argv[0], "--max-pairs must be non-negative");
    if (options.skip_pairs < 0) usage(argv[0], "--skip-pairs must be non-negative");
    if (!(options.source_fps > 0.0)) usage(argv[0], "--source-fps must be positive");
    return options;
}

bool findStartCode(const std::vector<std::uint8_t>& bytes, std::size_t from,
                   std::size_t& offset, std::size_t& length) {
    if (bytes.size() < 3 || from >= bytes.size()) return false;
    for (std::size_t i = from; i + 3 <= bytes.size(); ++i) {
        if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 &&
            bytes[i + 2] == 0 && bytes[i + 3] == 1) {
            offset = i;
            length = 4;
            return true;
        }
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            offset = i;
            length = 3;
            return true;
        }
    }
    return false;
}

class AnnexBReader {
public:
    explicit AnnexBReader(std::istream& input) : input_(input) {
        buffer_.reserve(kChunkSize * 2);
    }

    bool next(std::vector<std::uint8_t>& nal) {
        nal.clear();
        for (;;) {
            std::size_t first = 0;
            std::size_t first_length = 0;
            if (!findStartCode(buffer_, 0, first, first_length)) {
                if (eof_) return false;
                if (buffer_.size() > 3) {
                    buffer_.erase(buffer_.begin(), buffer_.end() - 3);
                }
                refill();
                continue;
            }
            if (first != 0) {
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(first));
            }

            std::size_t second = 0;
            std::size_t second_length = 0;
            if (findStartCode(buffer_, first_length, second, second_length)) {
                std::size_t end = second;
                while (end > first_length && buffer_[end - 1] == 0) --end;
                if (end > first_length) {
                    nal.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(first_length),
                               buffer_.begin() + static_cast<std::ptrdiff_t>(end));
                }
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(second));
                if (!nal.empty()) return true;
                continue;
            }

            if (eof_) {
                std::size_t end = buffer_.size();
                while (end > first_length && buffer_[end - 1] == 0) --end;
                if (end > first_length) {
                    nal.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(first_length),
                               buffer_.begin() + static_cast<std::ptrdiff_t>(end));
                }
                buffer_.clear();
                return !nal.empty();
            }
            if (buffer_.size() > kMaximumBufferedNal) {
                throw std::runtime_error("Annex-B NAL unit exceeded the 64 MiB safety limit");
            }
            refill();
        }
    }

private:
    void refill() {
        std::array<char, kChunkSize> chunk{};
        input_.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize count = input_.gcount();
        if (count > 0) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(chunk.data());
            buffer_.insert(buffer_.end(), begin, begin + count);
        }
        if (count == 0) {
            if (input_.bad()) throw std::runtime_error("Failed while reading Annex-B input");
            eof_ = true;
        }
    }

    static constexpr std::size_t kChunkSize = 1024 * 1024;
    static constexpr std::size_t kMaximumBufferedNal = 64 * 1024 * 1024;
    std::istream& input_;
    std::vector<std::uint8_t> buffer_;
    bool eof_ = false;
};

double timevalSeconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

void hashBytesSampled(std::uint64_t& hash, const std::uint8_t* bytes, std::size_t size) {
    // A full byte-by-byte hash of every raw 1080p frame would become a large,
    // artificial part of the measured workload. Sample evenly across each
    // frame instead; this remains useful for repeatability checks without
    // distorting real-time pipeline throughput.
    constexpr std::uint64_t prime = 1099511628211ULL;
    constexpr std::size_t samples = 4096;
    if (size == 0) return;
    const std::size_t step = std::max<std::size_t>(1, size / samples);
    for (std::size_t i = 0; i < size; i += step) {
        hash ^= bytes[i];
        hash *= prime;
    }
    hash ^= bytes[size - 1];
    hash *= prime;
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
        if (written < 0 && errno == EPIPE) throw std::runtime_error("Raw-video consumer closed the pipe");
        throw std::runtime_error(std::string("Raw-video write failed: ") + std::strerror(errno));
    }
}

inline std::uint8_t average2(std::uint8_t a, std::uint8_t b) {
    return static_cast<std::uint8_t>((static_cast<unsigned>(a) + static_cast<unsigned>(b) + 1U) >> 1U);
}

void downsampleRow2To1(const std::uint8_t* source, std::uint8_t* destination, int output_width) {
    int x = 0;
#if defined(__SSSE3__)
    const __m128i ones = _mm_set1_epi8(1);
    const __m128i round = _mm_set1_epi16(1);
    for (; x + 16 <= output_width; x += 16) {
        const std::uint8_t* input = source + x * 2;
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + 16));
        __m128i sum_a = _mm_maddubs_epi16(a, ones);
        __m128i sum_b = _mm_maddubs_epi16(b, ones);
        sum_a = _mm_srli_epi16(_mm_add_epi16(sum_a, round), 1);
        sum_b = _mm_srli_epi16(_mm_add_epi16(sum_b, round), 1);
        const __m128i packed = _mm_packus_epi16(sum_a, sum_b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + x), packed);
    }
#endif
    for (; x < output_width; ++x) {
        const int source_x = x * 2;
        destination[x] = average2(source[source_x], source[source_x + 1]);
    }
}

void averageRows2To1(const std::uint8_t* first, const std::uint8_t* second,
                     std::uint8_t* destination, int width) {
    int x = 0;
#if defined(__SSE2__)
    for (; x + 16 <= width; x += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(first + x));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(second + x));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + x), _mm_avg_epu8(a, b));
    }
#endif
    for (; x < width; ++x) destination[x] = average2(first[x], second[x]);
}

void composePlaneHalfSbs(const std::uint8_t* left, const std::uint8_t* right,
                         int source_width, int height, int source_stride,
                         std::uint8_t* destination) {
    if ((source_width & 1) != 0) throw std::runtime_error("Plane width is not divisible by two");
    const int eye_width = source_width / 2;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* left_row = left + static_cast<std::ptrdiff_t>(y) * source_stride;
        const std::uint8_t* right_row = right + static_cast<std::ptrdiff_t>(y) * source_stride;
        std::uint8_t* out = destination + static_cast<std::ptrdiff_t>(y) * source_width;
        downsampleRow2To1(left_row, out, eye_width);
        downsampleRow2To1(right_row, out + eye_width, eye_width);
    }
}

void composePlaneFullSbs(const std::uint8_t* left, const std::uint8_t* right,
                         int source_width, int height, int source_stride,
                         std::uint8_t* destination) {
    const int output_width = source_width * 2;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* left_row = left + static_cast<std::ptrdiff_t>(y) * source_stride;
        const std::uint8_t* right_row = right + static_cast<std::ptrdiff_t>(y) * source_stride;
        std::uint8_t* out = destination + static_cast<std::ptrdiff_t>(y) * output_width;
        std::memcpy(out, left_row, static_cast<std::size_t>(source_width));
        std::memcpy(out + source_width, right_row, static_cast<std::size_t>(source_width));
    }
}

void composePlaneHalfOu(const std::uint8_t* top, const std::uint8_t* bottom,
                        int width, int source_height, int source_stride,
                        std::uint8_t* destination) {
    if ((source_height & 1) != 0) throw std::runtime_error("Plane height is not divisible by two");
    const int eye_height = source_height / 2;
    for (int y = 0; y < eye_height; ++y) {
        const int source_y = y * 2;
        averageRows2To1(top + static_cast<std::ptrdiff_t>(source_y) * source_stride,
                        top + static_cast<std::ptrdiff_t>(source_y + 1) * source_stride,
                        destination + static_cast<std::ptrdiff_t>(y) * width, width);
        averageRows2To1(bottom + static_cast<std::ptrdiff_t>(source_y) * source_stride,
                        bottom + static_cast<std::ptrdiff_t>(source_y + 1) * source_stride,
                        destination + static_cast<std::ptrdiff_t>(y + eye_height) * width, width);
    }
}

void composePlaneFullOu(const std::uint8_t* top, const std::uint8_t* bottom,
                        int width, int height, int source_stride,
                        std::uint8_t* destination) {
    for (int y = 0; y < height; ++y) {
        std::memcpy(destination + static_cast<std::ptrdiff_t>(y) * width,
                    top + static_cast<std::ptrdiff_t>(y) * source_stride,
                    static_cast<std::size_t>(width));
        std::memcpy(destination + static_cast<std::ptrdiff_t>(y + height) * width,
                    bottom + static_cast<std::ptrdiff_t>(y) * source_stride,
                    static_cast<std::size_t>(width));
    }
}

struct EyeFrame {
    int bit_depth_y = 0;
    int bit_depth_c = 0;
    int width_y = 0;
    int width_c = 0;
    int height_y = 0;
    int height_c = 0;
    int poc = 0;
    int frame_id = 0;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
};

void copyPlane(std::vector<std::uint8_t>& destination, const std::uint8_t* source,
               int width, int height, int stride) {
    if (!source || width <= 0 || height <= 0 || stride < width)
        throw std::runtime_error("Decoder returned an invalid eye plane");
    destination.resize(static_cast<std::size_t>(width) * height);
    for (int row = 0; row < height; ++row) {
        std::memcpy(destination.data() + static_cast<std::size_t>(row) * width,
                    source + static_cast<std::size_t>(row) * stride,
                    static_cast<std::size_t>(width));
    }
}

EyeFrame copyEye(const Edge264Frame& frame, bool mvc) {
    EyeFrame eye;
    eye.bit_depth_y = frame.bit_depth_Y;
    eye.bit_depth_c = frame.bit_depth_C;
    eye.width_y = frame.width_Y;
    eye.width_c = frame.width_C;
    eye.height_y = frame.height_Y;
    eye.height_c = frame.height_C;
    eye.poc = mvc ? frame.PictureOrderCnt_mvc : frame.PictureOrderCnt;
    eye.frame_id = mvc ? frame.FrameId_mvc : frame.FrameId;
    const std::uint8_t* const* samples = mvc ? frame.samples_mvc : frame.samples;
    copyPlane(eye.y, samples[0], eye.width_y, eye.height_y, frame.stride_Y);
    copyPlane(eye.u, samples[1], eye.width_c, eye.height_c, frame.stride_C);
    copyPlane(eye.v, samples[2], eye.width_c, eye.height_c, frame.stride_C);
    return eye;
}

void validateEyes(const EyeFrame& base, const EyeFrame& mvc) {
    if (base.bit_depth_y != 8 || base.bit_depth_c != 8 ||
        mvc.bit_depth_y != 8 || mvc.bit_depth_c != 8)
        throw std::runtime_error("Phase 6 currently supports only 8-bit MVC video");
    if (base.width_y != mvc.width_y || base.height_y != mvc.height_y ||
        base.width_c != mvc.width_c || base.height_c != mvc.height_c)
        throw std::runtime_error("Matched MVC eyes have different dimensions");
    if (base.width_y <= 0 || base.height_y <= 0 || base.width_c <= 0 || base.height_c <= 0)
        throw std::runtime_error("Decoder returned invalid frame dimensions");
    if (base.width_c * 2 != base.width_y || base.height_c * 2 != base.height_y)
        throw std::runtime_error("Phase 6 currently requires 4:2:0 chroma");
}

struct OutputGeometry {
    int width_y = 0;
    int height_y = 0;
    int width_c = 0;
    int height_c = 0;
};

OutputGeometry outputGeometry(const EyeFrame& eye, OutputMode mode) {
    OutputGeometry geometry{eye.width_y, eye.height_y, eye.width_c, eye.height_c};
    if (mode == OutputMode::FullSbs) {
        geometry.width_y *= 2;
        geometry.width_c *= 2;
    } else if (mode == OutputMode::FullOu) {
        geometry.height_y *= 2;
        geometry.height_c *= 2;
    }
    return geometry;
}

OutputGeometry composeOutput(const EyeFrame& base, const EyeFrame& mvc,
                             const Options& options, std::vector<std::uint8_t>& output) {
    validateEyes(base, mvc);
    const EyeFrame& left = options.swap_eyes ? mvc : base;
    const EyeFrame& right = options.swap_eyes ? base : mvc;
    const OutputGeometry geometry = outputGeometry(base, options.output_mode);
    const std::size_t y_size = static_cast<std::size_t>(geometry.width_y) * geometry.height_y;
    const std::size_t c_size = static_cast<std::size_t>(geometry.width_c) * geometry.height_c;
    output.resize(y_size + 2 * c_size);

    auto compose_plane = [&](const std::vector<std::uint8_t>& left_plane,
                             const std::vector<std::uint8_t>& right_plane,
                             int width, int height, std::uint8_t* destination) {
        switch (options.output_mode) {
            case OutputMode::HalfSbs:
                composePlaneHalfSbs(left_plane.data(), right_plane.data(), width, height, width, destination);
                break;
            case OutputMode::FullSbs:
                composePlaneFullSbs(left_plane.data(), right_plane.data(), width, height, width, destination);
                break;
            case OutputMode::HalfOu:
                composePlaneHalfOu(left_plane.data(), right_plane.data(), width, height, width, destination);
                break;
            case OutputMode::FullOu:
                composePlaneFullOu(left_plane.data(), right_plane.data(), width, height, width, destination);
                break;
            case OutputMode::LeftEye:
                std::memcpy(destination, left_plane.data(), left_plane.size());
                break;
            case OutputMode::RightEye:
                std::memcpy(destination, right_plane.data(), right_plane.size());
                break;
        }
    };

    compose_plane(left.y, right.y, base.width_y, base.height_y, output.data());
    compose_plane(left.u, right.u, base.width_c, base.height_c, output.data() + y_size);
    compose_plane(left.v, right.v, base.width_c, base.height_c, output.data() + y_size + c_size);
    return geometry;
}

struct PendingEyes {
    std::map<int, EyeFrame> base;
    std::map<int, EyeFrame> mvc;
};

struct ProgressState {
    Clock::time_point started = Clock::now();
    Clock::time_point last_report = started;
    std::uint64_t last_pairs = 0;
};

void reportProgress(const Stats& stats, const Options& options, ProgressState& progress,
                    bool force = false) {
    const auto now = Clock::now();
    const double since_last = std::chrono::duration<double>(now - progress.last_report).count();
    if (!force && stats.pairs != 1 && stats.pairs - progress.last_pairs < 24 && since_last < 1.0) return;
    const double elapsed = std::chrono::duration<double>(now - progress.started).count();
    const double pair_fps = elapsed > 0.0 ? stats.pairs / elapsed : 0.0;
    const double realtime = pair_fps / options.source_fps;
    std::cerr << std::fixed << std::setprecision(3)
              << "PROGRESS pairs=" << stats.pairs
              << " emitted_pairs=" << stats.emitted_pairs
              << " skipped_pairs=" << stats.skipped_pairs
              << " pair_fps=" << pair_fps
              << " realtime_x=" << realtime
              << " base_only=" << stats.base_only
              << " mvc_only=" << stats.mvc_only
              << " poc_mismatches=" << stats.poc_mismatches
              << " errors=" << stats.errors
              << " ignored_tiny_type24=" << stats.ignored_tiny_type24 << "\n";
    progress.last_report = now;
    progress.last_pairs = stats.pairs;
}

bool emitPendingPairs(const Options& options, Stats& stats, std::vector<std::uint8_t>& output,
                      ProgressState& progress, PendingEyes& pending) {
    for (;;) {
        auto base_it = pending.base.end();
        auto mvc_it = pending.mvc.end();
        for (auto it = pending.base.begin(); it != pending.base.end(); ++it) {
            auto match = pending.mvc.find(it->first);
            if (match != pending.mvc.end()) {
                base_it = it;
                mvc_it = match;
                break;
            }
        }
        if (base_it == pending.base.end()) return false;
        EyeFrame base = std::move(base_it->second);
        EyeFrame mvc = std::move(mvc_it->second);
        pending.base.erase(base_it);
        pending.mvc.erase(mvc_it);
        validateEyes(base, mvc);
        const OutputGeometry geometry = outputGeometry(base, options.output_mode);
        stats.width = geometry.width_y;
        stats.height = geometry.height_y;
        ++stats.pairs;
        if (stats.skipped_pairs < static_cast<std::uint64_t>(options.skip_pairs)) {
            ++stats.skipped_pairs;
        } else {
            composeOutput(base, mvc, options, output);
            hashBytesSampled(stats.output_hash, output.data(), output.size());
            if (!options.discard_output) writeAll(output.data(), output.size());
            stats.output_bytes += output.size();
            ++stats.emitted_pairs;
        }
        reportProgress(stats, options, progress);
        if (options.max_pairs > 0 &&
            stats.emitted_pairs >= static_cast<std::uint64_t>(options.max_pairs))
            return true;
    }
}

bool drainFrames(Edge264Decoder* decoder, const Options& options, Stats& stats,
                 std::vector<std::uint8_t>& output, ProgressState& progress,
                 PendingEyes& pending) {
    for (;;) {
        Edge264Frame frame{};
        const int status = edge264_get_frame(decoder, &frame, 0);
        if (status == ENOMSG) return false;
        if (status != 0) {
            ++stats.errors;
            return false;
        }
        ++stats.decoder_outputs;
        if (frame.samples[0] != nullptr)
            pending.base.emplace(frame.PictureOrderCnt, copyEye(frame, false));
        if (frame.samples_mvc[0] != nullptr)
            pending.mvc.emplace(frame.PictureOrderCnt_mvc, copyEye(frame, true));
        if (emitPendingPairs(options, stats, output, progress, pending)) return true;
        constexpr std::size_t kMaxPendingEyes = 64;
        while (pending.base.size() > kMaxPendingEyes) {
            pending.base.erase(pending.base.begin());
            ++stats.base_only;
        }
        while (pending.mvc.size() > kMaxPendingEyes) {
            pending.mvc.erase(pending.mvc.begin());
            ++stats.mvc_only;
        }
    }
}

Stats runStream(std::istream& input, const Options& options) {
    Stats stats;
    std::vector<std::uint8_t> output;
    std::vector<std::uint8_t> nal;
    PendingEyes pending;
    std::vector<std::uint8_t> padded_nal;
    rusage before{};
    rusage after{};
    getrusage(RUSAGE_SELF, &before);
    const auto started = Clock::now();
    ProgressState progress{started, started, 0};

    Edge264Decoder* decoder = edge264_alloc(options.threads, nullptr, nullptr, 0, nullptr, nullptr, nullptr);
    if (!decoder) throw std::runtime_error("edge264_alloc failed");

    bool reached_limit = false;
    try {
        AnnexBReader reader(input);
        while (reader.next(nal)) {
            ++stats.nals;
            const std::uint8_t nal_type = nal.empty() ? 0 : (nal[0] & 0x1fU);
            // SyLC's SSIF access-unit stream carries one deterministic tiny type-24
            // framing marker per MVC access unit. It is transport framing, not H.264
            // picture data. The Android player ignores the same marker before calling
            // edge264; counting edge264's rejection here produced one false error per
            // otherwise-valid stereo pair during ISO playback.
            if (nal_type == 24 && nal.size() <= 2) {
                ++stats.ignored_tiny_type24;
                continue;
            }
            // edge264's vectorized RBSP reader inspects two bytes before the NAL
            // pointer. Supply explicit non-zero guard bytes so a 0x01 NAL header
            // cannot be mistaken for the tail of an Annex-B 00 00 01 start code.
            padded_nal.resize(nal.size() + 2);
            padded_nal[0] = 0xff;
            padded_nal[1] = 0xff;
            std::memcpy(padded_nal.data() + 2, nal.data(), nal.size());
            const std::uint8_t* safe_nal = padded_nal.data() + 2;
            int status = 0;
            unsigned attempts = 0;
            for (;;) {
                status = edge264_decode_NAL(decoder, safe_nal, safe_nal + nal.size(), nullptr, nullptr);
                if (status == 0) break;
                if (status == EAGAIN || status == EWOULDBLOCK || status == ENOBUFS) {
                    ++stats.retries;
                    reached_limit = drainFrames(decoder, options, stats, output, progress, pending);
                    if (reached_limit) break;
                    if (++attempts >= 8) break;
                    if (status == ENOBUFS) edge264_bump_frames(decoder);
                    std::this_thread::yield();
                    continue;
                }
                break;
            }
            if (reached_limit) break;
            if (status != 0) ++stats.errors;
            reached_limit = drainFrames(decoder, options, stats, output, progress, pending);
            if (reached_limit) break;
        }

        if (stats.nals == 0) throw std::runtime_error("No Annex-B NAL units were found");

        if (!reached_limit) {
            constexpr int max_drain_loops = 120000;
            int stable_idle = 0;
            for (int i = 0; i < max_drain_loops; ++i) {
                edge264_bump_frames(decoder);
                const std::uint64_t before_pairs = stats.pairs;
                reached_limit = drainFrames(decoder, options, stats, output, progress, pending);
                if (reached_limit) break;
                const unsigned busy = edge264_get_busy_tasks(decoder);
                if (busy == 0 && stats.pairs == before_pairs) {
                    if (++stable_idle >= 3) break;
                } else {
                    stable_idle = 0;
                }
                if (busy != 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (!reached_limit) drainFrames(decoder, options, stats, output, progress, pending);
        }
    } catch (...) {
        edge264_free(&decoder);
        throw;
    }
    if (!reached_limit) {
        stats.base_only += pending.base.size();
        stats.mvc_only += pending.mvc.size();
    }
    edge264_free(&decoder);

    const auto finished = Clock::now();
    getrusage(RUSAGE_SELF, &after);
    stats.wall_seconds = std::chrono::duration<double>(finished - started).count();
    stats.cpu_seconds =
        (timevalSeconds(after.ru_utime) + timevalSeconds(after.ru_stime)) -
        (timevalSeconds(before.ru_utime) + timevalSeconds(before.ru_stime));
    reportProgress(stats, options, progress, true);
    return stats;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::ios::sync_with_stdio(false);
        const Options options = parseOptions(argc, argv);
        std::ifstream file;
        std::istream* input = &std::cin;
        if (options.input != "-") {
            file.open(options.input, std::ios::binary);
            if (!file) throw std::runtime_error("Unable to open " + options.input);
            input = &file;
        }
        std::cerr << "SYLC_HSBS_PIPE version=4 streaming_input=" << options.input
                  << " threads=" << options.threads
                  << " mode=" << outputModeName(options.output_mode)
                  << " source_fps=" << std::fixed << std::setprecision(6) << options.source_fps
                  << " swap_eyes=" << (options.swap_eyes ? 1 : 0)
                  << " discard_output=" << (options.discard_output ? 1 : 0)
                  << " skip_pairs=" << options.skip_pairs
                  << " max_pairs=" << options.max_pairs << "\n";

        const Stats stats = runStream(*input, options);
        const double pair_fps = stats.wall_seconds > 0.0 ? stats.pairs / stats.wall_seconds : 0.0;
        const double realtime = pair_fps / options.source_fps;
        const double cpu_cores = stats.wall_seconds > 0.0 ? stats.cpu_seconds / stats.wall_seconds : 0.0;
        std::cerr << std::fixed << std::setprecision(3)
                  << "RESULT threads=" << options.threads
                  << " nals=" << stats.nals
                  << " outputs=" << stats.decoder_outputs
                  << " pairs=" << stats.pairs
                  << " emitted_pairs=" << stats.emitted_pairs
                  << " skipped_pairs=" << stats.skipped_pairs
                  << " base_only=" << stats.base_only
                  << " mvc_only=" << stats.mvc_only
                  << " poc_mismatches=" << stats.poc_mismatches
                  << " errors=" << stats.errors
                  << " retries=" << stats.retries
                  << " ignored_tiny_type24=" << stats.ignored_tiny_type24
                  << " wall_s=" << stats.wall_seconds
                  << " cpu_s=" << stats.cpu_seconds
                  << " avg_cpu_cores=" << cpu_cores
                  << " pair_fps=" << pair_fps
                  << " realtime_x=" << realtime
                  << " output_bytes=" << stats.output_bytes
                  << " output_sample_fnv1a64=0x" << std::hex << stats.output_hash << std::dec
                  << " dimensions=" << stats.width << "x" << stats.height
                  << "\n";

        if (stats.errors != 0 || stats.base_only != 0 || stats.mvc_only != 0 || stats.poc_mismatches != 0)
            return 3;
        if (options.max_pairs == 0 && stats.emitted_pairs == 0) return 4;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FATAL: " << exception.what() << "\n";
        return 1;
    }
}
