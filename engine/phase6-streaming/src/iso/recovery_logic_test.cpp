#include "mvc_source_pair_offset_calibrator.h"
#include "mvc_ssif_phase_aligner.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Pair = mvc_demux::MVCSSIFDemuxer::FramePair;

Pair pair(std::uint64_t base_ts, std::uint64_t dep_ts,
          std::uint8_t base_marker, std::uint8_t dep_marker) {
    Pair value;
    value.baseData = {0, 0, 0, 1, base_marker};
    value.dependentData = {0, 0, 0, 1, dep_marker};
    value.timestamp = base_ts;
    value.depTimestamp = dep_ts;
    value.isKeyframe = false;
    return value;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        MvcSourcePairOffsetCalibrator calibrator;
        const std::int64_t samples[] = {42, 41, 42, 43, 42, 41, 120};
        for (std::int64_t sample : samples) {
            if (calibrator.calibrated()) break;
            calibrator.observe(sample);
        }
        require(calibrator.calibrated(), "stable offset did not calibrate");
        require(calibrator.calibratedOffsetMs() == 42,
                "robust median offset was not 42 ms");
        require(calibrator.accepts(50), "inlier near calibrated offset was rejected");
        require(!calibrator.accepts(70), "outlier was accepted");

        MvcSsifPhaseAligner ahead;
        require(ahead.configure(42, 42, 10), "dependent-ahead phase did not configure");
        require(ahead.phaseShiftFrames() == 1, "dependent-ahead shift is not +1");
        Pair corrected;
        require(!ahead.push(pair(1000, 1042, 0x11, 0x21), corrected),
                "first dependent-ahead pair should be retained");
        require(ahead.push(pair(1042, 1084, 0x12, 0x22), corrected),
                "second dependent-ahead pair should emit");
        require(corrected.timestamp == 1042 && corrected.depTimestamp == 1042,
                "dependent-ahead timestamps were not aligned");
        require(corrected.baseData.back() == 0x12 && corrected.dependentData.back() == 0x21,
                "dependent-ahead payloads were not shifted by one frame");

        MvcSsifPhaseAligner behind;
        require(behind.configure(-42, 42, 10), "dependent-behind phase did not configure");
        require(behind.phaseShiftFrames() == -1, "dependent-behind shift is not -1");
        require(!behind.push(pair(1042, 1000, 0x31, 0x41), corrected),
                "first dependent-behind pair should be retained");
        require(behind.push(pair(1084, 1042, 0x32, 0x42), corrected),
                "second dependent-behind pair should emit");
        require(corrected.timestamp == 1042 && corrected.depTimestamp == 1042,
                "dependent-behind timestamps were not aligned");
        require(corrected.baseData.back() == 0x31 && corrected.dependentData.back() == 0x42,
                "dependent-behind payloads were not shifted by one frame");

        MvcSsifPhaseAligner passthrough;
        require(passthrough.configure(2, 42, 10), "near-zero phase did not configure");
        require(passthrough.phaseShiftFrames() == 0, "near-zero phase is not passthrough");
        require(passthrough.push(pair(2000, 2002, 0x51, 0x61), corrected),
                "passthrough pair did not emit");
        require(corrected.baseData.back() == 0x51 && corrected.dependentData.back() == 0x61,
                "passthrough payload changed");

        MvcSsifPhaseAligner unsupported;
        require(!unsupported.configure(84, 42, 10),
                "two-frame phase should be rejected");

        std::cout << "MVC source-offset calibration: PASS\n"
                  << "MVC 0/+1/-1-frame phase correction: PASS\n"
                  << "Unsupported phase rejection: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
