#include "sylc_m2ts_audio_demuxer.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: sylc_truehd_m2ts_extract_test INPUT.m2ts OUTPUT.thd\n";
        return 2;
    }
    auto input = std::make_unique<std::ifstream>(argv[1], std::ios::binary);
    if (!*input) {
        std::cerr << "Could not open input: " << argv[1] << "\n";
        return 2;
    }
    input->seekg(0, std::ios::end);
    const auto end = input->tellg();
    if (end <= 0) {
        std::cerr << "Input is empty\n";
        return 2;
    }
    const auto size = static_cast<std::uint64_t>(end);
    input->seekg(0, std::ios::beg);

    sylc_audio::M2TSAudioDemuxer demuxer;
    std::string error;
    if (!demuxer.openStream(std::move(input), size, argv[1], &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto& tracks = demuxer.tracks();
    std::size_t selected = tracks.size();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].stream_type == 0x83 && tracks[i].supported
                && tracks[i].bridge_format == "truehd") {
            selected = i;
            break;
        }
    }
    if (selected >= tracks.size()) {
        std::cerr << "No native TrueHD track was discovered\n"
                  << demuxer.diagnostics() << "\n";
        return 1;
    }
    if (!demuxer.selectTrack(selected, 0, 0, &error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not create output: " << argv[2] << "\n";
        return 2;
    }
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    for (;;) {
        sylc_audio::CompressedAudioSample sample;
        error.clear();
        if (demuxer.readNextSample(sample, &error)) {
            output.write(reinterpret_cast<const char*>(sample.data.data()),
                         static_cast<std::streamsize>(sample.data.size()));
            if (!output) {
                std::cerr << "Output write failed\n";
                return 1;
            }
            ++samples;
            bytes += sample.data.size();
            continue;
        }
        if (!error.empty()) {
            std::cerr << error << "\n";
            return 1;
        }
        break;
    }
    output.close();
    std::cerr << demuxer.diagnostics() << "\n"
              << "TRUEHD_M2TS_TEST samples=" << samples
              << " bytes=" << bytes
              << " channels=" << tracks[selected].channels
              << " sample_rate=" << tracks[selected].sample_rate
              << " status=" << (samples > 0 ? "PASS" : "FAIL") << "\n";
    return samples > 0 ? 0 : 1;
}
