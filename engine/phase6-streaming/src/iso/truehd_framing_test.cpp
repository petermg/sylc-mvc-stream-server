#include "sylc_truehd_framer.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<std::uint8_t> makeFrame(std::size_t bytes, bool major, int ratebits,
                                    int stream2_map) {
    assert(bytes >= 32 && bytes <= sylc_audio::TrueHdAccessUnitFramer::kMaximumFrameBytes);
    assert((bytes & 1U) == 0);
    std::vector<std::uint8_t> frame(bytes, 0x55);
    const std::size_t words = bytes / 2U;
    frame[0] = static_cast<std::uint8_t>((words >> 8U) & 0x0fU);
    frame[1] = static_cast<std::uint8_t>(words & 0xffU);
    if (major) {
        frame[4] = 0xf8; frame[5] = 0x72; frame[6] = 0x6f; frame[7] = 0xba;
        // ratebits, then 8 ignored bits, 5-bit stream1 map, 2-bit modifier,
        // and 13-bit stream2 map.  Use stream2 LR+C+LFE+LRs+LRrs = 8ch.
        frame[8] = static_cast<std::uint8_t>((ratebits & 0x0f) << 4U);
        const int stream1 = 0x0f; // LR+C+LFE+LRs = 6ch
        std::uint64_t packed = (static_cast<std::uint64_t>(stream1) << 15U)
                | static_cast<std::uint64_t>(stream2_map & 0x1fff);
        // packed starts after 12 bits (rate + ignored byte), at frame bit 96.
        for (int bit = 0; bit < 20; ++bit) {
            const int value = static_cast<int>((packed >> (19 - bit)) & 1U);
            const std::size_t absolute = 12U + static_cast<std::size_t>(bit);
            const std::size_t byte = 8U + (absolute >> 3U);
            const int shift = 7 - static_cast<int>(absolute & 7U);
            frame[byte] = static_cast<std::uint8_t>((frame[byte] & ~(1U << shift))
                    | (static_cast<unsigned>(value) << shift));
        }
    }
    return frame;
}

}  // namespace

int main() {
    sylc_audio::TrueHdAccessUnitFramer framer;
    const auto major = makeFrame(160, true, 0, 0x4f); // 48 kHz, 8 channels
    const auto normal = makeFrame(96, false, 0, 0);

    std::vector<std::uint8_t> stream = {0xde, 0xad, 0xbe, 0xef, 0x00};
    stream.insert(stream.end(), major.begin(), major.end());
    stream.insert(stream.end(), normal.begin(), normal.end());

    // Deliberately split both the major sync word and the second frame.
    framer.append(stream.data(), 8);
    std::vector<std::uint8_t> frame;
    sylc_audio::TrueHdFrameInfo info;
    assert(!framer.pop(&frame, &info));
    framer.append(stream.data() + 8, 91);
    assert(!framer.pop(&frame, &info));
    framer.append(stream.data() + 99, stream.size() - 99);
    assert(framer.pop(&frame, &info));
    assert(frame == major);
    assert(info.major_sync);
    assert(info.sample_rate == 48000);
    assert(info.samples_per_frame == 40);
    assert(info.channels == 8);
    assert(framer.pop(&frame, &info));
    assert(frame == normal);
    assert(!info.major_sync);
    assert(info.sample_rate == 48000);
    assert(info.samples_per_frame == 40);
    assert(info.channels == 8);
    assert(!framer.pop(&frame, &info));
    assert(framer.framesEmitted() == 2);
    assert(framer.majorSyncCount() == 1);
    assert(framer.bytesDiscarded() == 5);
    std::cout << "TrueHD framing test PASS\n";
    return 0;
}
