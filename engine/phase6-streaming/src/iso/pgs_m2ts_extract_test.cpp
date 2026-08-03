#include "sylc_m2ts_pgs_demuxer.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

void writeBe32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>((value >> 24U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>(value & 0xffU),
    };
    out.write(bytes, sizeof(bytes));
}

void writeBe16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>(value & 0xffU),
    };
    out.write(bytes, sizeof(bytes));
}

void writeSup(std::ostream& out, const sylc_pgs::PgsSegment& segment) {
    out.put('P');
    out.put('G');
    writeBe32(out, static_cast<std::uint32_t>(segment.pts90k));
    writeBe32(out, static_cast<std::uint32_t>(segment.dts90k));
    out.put(static_cast<char>(segment.type));
    writeBe16(out, static_cast<std::uint16_t>(segment.payload.size()));
    if (!segment.payload.empty()) {
        out.write(reinterpret_cast<const char*>(segment.payload.data()),
                  static_cast<std::streamsize>(segment.payload.size()));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " input.m2ts pid output.sup\n";
        return 2;
    }
    auto input = std::make_unique<std::ifstream>(argv[1], std::ios::binary);
    if (!*input) {
        std::cerr << "could not open input\n";
        return 2;
    }
    input->seekg(0, std::ios::end);
    const auto size = input->tellg();
    input->clear();
    input->seekg(0, std::ios::beg);
    const auto pid = static_cast<std::uint16_t>(std::stoul(argv[2], nullptr, 0));
    sylc_pgs::M2TSPgsDemuxer demuxer;
    std::string error;
    if (!demuxer.openStream(std::move(input), static_cast<std::uint64_t>(size),
                            argv[1], pid, &error)
            || !demuxer.selectAt(0, 0, -1, -1, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
    if (!output) return 2;
    sylc_pgs::PgsSegment segment;
    while (demuxer.readNextSegment(segment, &error)) writeSup(output, segment);
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cerr << demuxer.diagnostics() << '\n';
    return demuxer.segmentCount() == 0 ? 1 : 0;
}
