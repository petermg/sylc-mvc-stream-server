#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace sylc_audio {

struct TrueHdFrameInfo {
    std::size_t frame_size = 0;
    bool major_sync = false;
    int sample_rate = 0;
    int samples_per_frame = 0;
    int channels = 0;
    std::uint8_t stream_type = 0;
};

/**
 * Minimal raw MLP/TrueHD access-unit framer.
 *
 * The frame size is carried in the low 12 bits of the first 16-bit word and is
 * expressed in 16-bit words.  Resynchronization follows FFmpeg's public MLP
 * parser behavior: find the major-sync word f8 72 6f ba/bb at byte offset +4,
 * then consume sequential access units by their declared lengths.
 */
class TrueHdAccessUnitFramer {
public:
    void reset() {
        buffer_.clear();
        in_sync_ = false;
        cached_sample_rate_ = 0;
        cached_samples_per_frame_ = 0;
        cached_channels_ = 0;
        major_sync_count_ = 0;
        frames_emitted_ = 0;
        bytes_discarded_ = 0;
        partial_waits_ = 0;
        lost_sync_count_ = 0;
    }

    void append(const std::uint8_t* data, std::size_t size) {
        if (!data || size == 0) return;
        buffer_.insert(buffer_.end(), data, data + size);
    }

    void append(const std::vector<std::uint8_t>& data) {
        append(data.data(), data.size());
    }

    bool pop(std::vector<std::uint8_t>* frame, TrueHdFrameInfo* info) {
        if (!frame || !info) return false;
        frame->clear();
        *info = {};

        for (;;) {
            if (!in_sync_) {
                const std::size_t start = findMajorSyncFrameStart(buffer_);
                if (start == npos) {
                    // Keep enough tail for a major sync and its four-byte AU header
                    // to straddle the next PES boundary.
                    constexpr std::size_t kKeep = 11;
                    if (buffer_.size() > kKeep) {
                        const std::size_t discard = buffer_.size() - kKeep;
                        bytes_discarded_ += discard;
                        buffer_.erase(buffer_.begin(), buffer_.begin()
                                + static_cast<std::ptrdiff_t>(discard));
                    }
                    if (!buffer_.empty()) ++partial_waits_;
                    return false;
                }
                if (start > 0) {
                    bytes_discarded_ += start;
                    buffer_.erase(buffer_.begin(), buffer_.begin()
                            + static_cast<std::ptrdiff_t>(start));
                }
                in_sync_ = true;
            }

            if (buffer_.size() < 2) {
                ++partial_waits_;
                return false;
            }
            const std::size_t frame_size = frameSize(buffer_.data(), buffer_.size());
            if (frame_size < 8 || frame_size > kMaximumFrameBytes) {
                in_sync_ = false;
                ++lost_sync_count_;
                ++bytes_discarded_;
                buffer_.erase(buffer_.begin());
                continue;
            }
            if (buffer_.size() < frame_size) {
                ++partial_waits_;
                return false;
            }

            TrueHdFrameInfo parsed;
            parsed.frame_size = frame_size;
            parsed.major_sync = isMajorSync(buffer_.data(), frame_size);
            if (parsed.major_sync) {
                parseMajorSync(buffer_.data(), frame_size, &parsed);
                if (parsed.sample_rate > 0) cached_sample_rate_ = parsed.sample_rate;
                if (parsed.samples_per_frame > 0) {
                    cached_samples_per_frame_ = parsed.samples_per_frame;
                }
                if (parsed.channels > 0) cached_channels_ = parsed.channels;
                ++major_sync_count_;
            }
            parsed.sample_rate = parsed.sample_rate > 0
                    ? parsed.sample_rate : cached_sample_rate_;
            parsed.samples_per_frame = parsed.samples_per_frame > 0
                    ? parsed.samples_per_frame : cached_samples_per_frame_;
            parsed.channels = parsed.channels > 0 ? parsed.channels : cached_channels_;

            frame->assign(buffer_.begin(), buffer_.begin()
                    + static_cast<std::ptrdiff_t>(frame_size));
            buffer_.erase(buffer_.begin(), buffer_.begin()
                    + static_cast<std::ptrdiff_t>(frame_size));
            *info = parsed;
            ++frames_emitted_;
            return true;
        }
    }

    std::size_t bufferedBytes() const { return buffer_.size(); }
    std::uint64_t majorSyncCount() const { return major_sync_count_; }
    std::uint64_t framesEmitted() const { return frames_emitted_; }
    std::uint64_t bytesDiscarded() const { return bytes_discarded_; }
    std::uint64_t partialWaits() const { return partial_waits_; }
    std::uint64_t lostSyncCount() const { return lost_sync_count_; }
    int sampleRate() const { return cached_sample_rate_; }
    int samplesPerFrame() const { return cached_samples_per_frame_; }
    int channels() const { return cached_channels_; }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    static constexpr std::size_t kMaximumFrameBytes = 0x0fffU * 2U;

    static std::size_t frameSize(const std::uint8_t* data, std::size_t available) {
        if (!data || available < 2) return 0;
        return ((((static_cast<std::size_t>(data[0]) << 8U)
                | static_cast<std::size_t>(data[1])) & 0x0fffU) * 2U);
    }

    static bool isMajorSync(const std::uint8_t* frame, std::size_t available) {
        if (!frame || available < 8) return false;
        const std::uint32_t word = (static_cast<std::uint32_t>(frame[4]) << 24U)
                | (static_cast<std::uint32_t>(frame[5]) << 16U)
                | (static_cast<std::uint32_t>(frame[6]) << 8U)
                | static_cast<std::uint32_t>(frame[7]);
        return (word & 0xfffffffeU) == 0xf8726fbaU;
    }

    static std::size_t findMajorSyncFrameStart(const std::vector<std::uint8_t>& data) {
        if (data.size() < 8) return npos;
        for (std::size_t i = 0; i + 8 <= data.size(); ++i) {
            if (!isMajorSync(data.data() + i, data.size() - i)) continue;
            const std::size_t size = frameSize(data.data() + i, data.size() - i);
            if (size >= 8 && size <= kMaximumFrameBytes) return i;
        }
        return npos;
    }

    static bool parseMajorSync(const std::uint8_t* frame, std::size_t available,
                               TrueHdFrameInfo* info) {
        if (!info || !isMajorSync(frame, available) || available < 13) return false;
        // Major-sync payload begins at frame byte 4.  After the 24-bit sync word
        // and 8-bit stream type, the high nibble of byte 8 is the rate code.
        info->stream_type = frame[7];
        const int ratebits = (frame[8] >> 4U) & 0x0fU;
        if (ratebits != 0x0f) {
            info->sample_rate = ((ratebits & 8) ? 44100 : 48000) << (ratebits & 7);
            info->samples_per_frame = 40 << (ratebits & 7);
        }

        // TrueHD (0xba) carries a five-bit 6-channel presentation map and a
        // thirteen-bit 8-channel presentation map.  Prefer the latter when set.
        if (info->stream_type == 0xba && available >= 13) {
            BitCursor bits(frame + 8, (available - 8) * 8U);
            int ignored_rate = 0;
            int ignored = 0;
            int stream1 = 0;
            int modifier = 0;
            int stream2 = 0;
            if (bits.read(4, &ignored_rate) && bits.read(8, &ignored)
                    && bits.read(5, &stream1) && bits.read(2, &modifier)
                    && bits.read(13, &stream2)) {
                (void)modifier;
                info->channels = trueHdChannels(stream2 != 0 ? stream2 : stream1);
            }
        }
        return info->sample_rate > 0;
    }

private:
    struct BitCursor {
        const std::uint8_t* data = nullptr;
        std::size_t bit_count = 0;
        std::size_t position = 0;
        BitCursor(const std::uint8_t* input, std::size_t bits)
                : data(input), bit_count(bits) {}
        bool read(int count, int* value) {
            if (!value || count < 0 || position + static_cast<std::size_t>(count) > bit_count) {
                return false;
            }
            int result = 0;
            for (int i = 0; i < count; ++i) {
                const std::uint8_t byte = data[position >> 3U];
                const int shift = 7 - static_cast<int>(position & 7U);
                result = (result << 1) | ((byte >> shift) & 1U);
                ++position;
            }
            *value = result;
            return true;
        }
    };

    static int trueHdChannels(int channel_map) {
        static constexpr std::array<int, 13> kCounts = {
                2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};
        int channels = 0;
        for (std::size_t i = 0; i < kCounts.size(); ++i) {
            if ((channel_map >> i) & 1) channels += kCounts[i];
        }
        return channels;
    }

    std::vector<std::uint8_t> buffer_;
    bool in_sync_ = false;
    int cached_sample_rate_ = 0;
    int cached_samples_per_frame_ = 0;
    int cached_channels_ = 0;
    std::uint64_t major_sync_count_ = 0;
    std::uint64_t frames_emitted_ = 0;
    std::uint64_t bytes_discarded_ = 0;
    std::uint64_t partial_waits_ = 0;
    std::uint64_t lost_sync_count_ = 0;
};

}  // namespace sylc_audio
