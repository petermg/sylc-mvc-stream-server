#include "sylc_m2ts_audio_demuxer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace sylc_audio {
namespace {

constexpr std::size_t kCatalogPacketLimit = 350000;
constexpr std::uint64_t kCatalogByteLimit = 96ull << 20;
constexpr std::size_t kMaximumPesBytes = 8u << 20;
constexpr std::size_t kMaximumProbeBytes = 4u << 20;
constexpr int kProbeSamples = 8;

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24U)
            | (static_cast<std::uint32_t>(p[1]) << 16U)
            | (static_cast<std::uint32_t>(p[2]) << 8U)
            | static_cast<std::uint32_t>(p[3]);
}

std::int64_t parsePts(const std::uint8_t* p) {
    return (static_cast<std::int64_t>((p[0] >> 1U) & 0x07U) << 30U)
            | (static_cast<std::int64_t>(p[1]) << 22U)
            | (static_cast<std::int64_t>((p[2] >> 1U) & 0x7fU) << 15U)
            | (static_cast<std::int64_t>(p[3]) << 7U)
            | static_cast<std::int64_t>((p[4] >> 1U) & 0x7fU);
}

struct BitReader {
    const std::uint8_t* data = nullptr;
    std::size_t bits = 0;
    std::size_t pos = 0;

    bool skip(std::size_t count) {
        if (count > bits - std::min(bits, pos)) return false;
        pos += count;
        return true;
    }
    bool read(int count, int* value) {
        if (!value || count < 0 || static_cast<std::size_t>(count) > bits - std::min(bits, pos)) {
            return false;
        }
        int result = 0;
        for (int i = 0; i < count; ++i) {
            const std::uint8_t current = data[pos >> 3U];
            const int shift = 7 - static_cast<int>(pos & 7U);
            result = (result << 1) | ((current >> shift) & 1U);
            ++pos;
        }
        *value = result;
        return true;
    }
};

bool isDtsType(std::uint8_t stream_type) {
    return stream_type == 0x82 || stream_type == 0x85 || stream_type == 0x86
            || stream_type == 0xa2;
}

struct DtsCoreFrame {
    std::size_t size = 0;
    int samples = 0;
    int sample_rate = 0;
};

bool isCoreSync(const std::uint8_t* p) {
    const std::uint32_t word = be32(p);
    return word == 0x7ffe8001U || word == 0xfe7f0180U
            || word == 0x1fffe800U || word == 0xff1f00e8U;
}

bool parseDtsCoreFrame(const std::uint8_t* data, std::size_t available, DtsCoreFrame* frame) {
    if (!data || !frame || available < 12) return false;
    std::array<std::uint8_t, 16> converted{};
    const std::uint8_t* p = data;
    const std::uint32_t sync = be32(data);
    if (sync == 0xfe7f0180U) {
        const std::size_t copy = std::min<std::size_t>(converted.size(), available);
        for (std::size_t i = 0; i + 1 < copy; i += 2) {
            converted[i] = data[i + 1];
            converted[i + 1] = data[i];
        }
        p = converted.data();
    } else if (sync != 0x7ffe8001U) {
        // Blu-ray DTS and DTS-HD use the normal 16-bit representation. The 14-bit
        // markers remain recognized by catalog probing, but are not valid HDMV AUs.
        return false;
    }
    const int nblocks = (((p[4] & 0x01U) << 6U) | (p[5] >> 2U)) + 1;
    const std::size_t frame_size = (((static_cast<std::size_t>(p[5]) & 0x03U) << 12U)
            | (static_cast<std::size_t>(p[6]) << 4U)
            | ((static_cast<std::size_t>(p[7]) >> 4U) & 0x0fU)) + 1U;
    const int sr_code = (p[8] >> 2U) & 0x0fU;
    static constexpr std::array<int, 16> kSampleRates = {
            0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,96000,192000};
    if (frame_size < 96 || nblocks <= 0) return false;
    frame->size = frame_size;
    frame->samples = nblocks * 32;
    frame->sample_rate = kSampleRates[static_cast<std::size_t>(sr_code)];
    return frame->sample_rate > 0;
}

bool parseDtsExssSize(const std::uint8_t* data, std::size_t available, std::size_t* frame_size) {
    if (!data || !frame_size || available < 10 || be32(data) != 0x64582025U) return false;
    BitReader bits{data, available * 8U, 32U};
    int ignored = 0;
    int blown_up = 0;
    int header_minus_one = 0;
    int frame_minus_one = 0;
    if (!bits.read(8, &ignored) || !bits.read(2, &ignored) || !bits.read(1, &blown_up)
            || !bits.read(blown_up ? 12 : 8, &header_minus_one)
            || !bits.read(blown_up ? 20 : 16, &frame_minus_one)) {
        return false;
    }
    const std::size_t parsed = static_cast<std::size_t>(frame_minus_one) + 1U;
    const std::size_t header = static_cast<std::size_t>(header_minus_one) + 1U;
    if (parsed < header || parsed < 10U || parsed > (8U << 20U)) return false;
    *frame_size = parsed;
    return true;
}

std::size_t findValidDtsCore(const std::vector<std::uint8_t>& bytes, std::size_t start) {
    for (std::size_t i = start; i + 12 <= bytes.size(); ++i) {
        if (!isCoreSync(bytes.data() + i)) continue;
        DtsCoreFrame frame;
        if (parseDtsCoreFrame(bytes.data() + i, bytes.size() - i, &frame)) return i;
    }
    return std::string::npos;
}


void parseDtsCore(const std::vector<std::uint8_t>& data, int* channels, int* sample_rate,
                  int* amode_out, int* lfe_out) {
    if (channels) *channels = 0;
    if (sample_rate) *sample_rate = 0;
    if (amode_out) *amode_out = -1;
    if (lfe_out) *lfe_out = -1;
    for (std::size_t i = 0; i + 12 < data.size(); ++i) {
        if (be32(data.data() + i) != 0x7ffe8001U) continue;
        BitReader bits{data.data(), data.size() * 8U, i * 8U + 32U};
        int amode = -1;
        int sfreq = -1;
        int lfe = -1;
        if (!bits.skip(1 + 5 + 1 + 7 + 14) || !bits.read(6, &amode)
                || !bits.read(4, &sfreq) || !bits.skip(5 + 1 + 1 + 1 + 1 + 1 + 3 + 1 + 1)
                || !bits.read(2, &lfe)) {
            return;
        }
        static constexpr std::array<int, 10> kBaseChannels = {1,2,2,2,2,3,3,4,4,5};
        static constexpr std::array<int, 16> kSampleRates = {
                0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,96000,192000};
        int parsed_channels = amode >= 0 && amode < static_cast<int>(kBaseChannels.size())
                ? kBaseChannels[static_cast<std::size_t>(amode)] : 0;
        if (parsed_channels > 0 && lfe > 0 && lfe < 3) ++parsed_channels;
        const int parsed_rate = sfreq >= 0 && sfreq < static_cast<int>(kSampleRates.size())
                ? kSampleRates[static_cast<std::size_t>(sfreq)] : 0;
        if (channels) *channels = parsed_channels;
        if (sample_rate) *sample_rate = parsed_rate;
        if (amode_out) *amode_out = amode;
        if (lfe_out) *lfe_out = lfe;
        return;
    }
}

void parseAc3(const std::vector<std::uint8_t>& data, int* channels, int* sample_rate) {
    if (channels) *channels = 0;
    if (sample_rate) *sample_rate = 0;
    for (std::size_t i = 0; i + 8 < data.size(); ++i) {
        if (data[i] != 0x0b || data[i + 1] != 0x77) continue;
        const int fscod = (data[i + 4] >> 6U) & 0x03U;
        static constexpr std::array<int, 4> kRates = {48000,44100,32000,0};
        const int rate = kRates[static_cast<std::size_t>(fscod)];
        const int acmod = (data[i + 6] >> 5U) & 0x07U;
        static constexpr std::array<int, 8> kChannels = {2,1,2,3,3,4,4,5};
        int parsed_channels = kChannels[static_cast<std::size_t>(acmod)];
        // This compact parser intentionally treats the common LFE bit position used by
        // Blu-ray AC-3 as a best-effort hint. Decoder output format remains authoritative.
        if ((data[i + 6] & 0x10U) != 0 && parsed_channels < 6) ++parsed_channels;
        if (channels) *channels = parsed_channels;
        if (sample_rate) *sample_rate = rate;
        return;
    }
}

}  // namespace

M2TSAudioDemuxer::M2TSAudioDemuxer() = default;
M2TSAudioDemuxer::~M2TSAudioDemuxer() { close(); }

bool M2TSAudioDemuxer::openStream(std::unique_ptr<std::istream> stream,
                                  std::uint64_t file_size,
                                  const std::string& label,
                                  std::string* error) {
    return openStream(std::move(stream), file_size, label, {}, 0, error);
}

bool M2TSAudioDemuxer::openStream(std::unique_ptr<std::istream> stream,
                                  std::uint64_t file_size,
                                  const std::string& label,
                                  const std::vector<AudioTrackInfo>& declared_tracks,
                                  std::uint64_t catalog_probe_byte,
                                  std::string* error) {
    close();
    reader_ = std::make_unique<mvc_demux::M2TSReader>();
    label_ = label;
    if (!reader_->openStream(std::move(stream), file_size, label)) {
        if (error) *error = "SyLC M2TSReader could not open audio transport stream: " + label;
        close();
        return false;
    }
    if (!scanCatalog(declared_tracks, catalog_probe_byte, error)) {
        close();
        return false;
    }
    return true;
}

void M2TSAudioDemuxer::close() {
    if (reader_) reader_->close();
    reader_.reset();
    label_.clear();
    tracks_.clear();
    scan_pes_.clear();
    probes_.clear();
    selected_track_ = static_cast<std::size_t>(-1);
    selected_pid_ = 0;
    selected_stream_type_ = 0;
    selected_is_hd_ = false;
    selected_is_truehd_ = false;
    selected_truehd_core_fallback_ = false;
    selected_pes_ = {};
    elementary_buffer_.clear();
    truehd_framer_.reset();
    ready_samples_.clear();
    raw_origin_pts_us_ = -1;
    next_frame_pts_us_ = -1;
    last_pes_raw_pts_us_ = -1;
    eof_flushed_ = false;
    catalog_discovery_method_ = "not attempted";
    catalog_probe_byte_ = 0;
    declared_track_count_ = 0;
    pat_pmt_track_count_ = 0;
    directly_verified_track_count_ = 0;
}

bool M2TSAudioDemuxer::isAudioStreamType(std::uint8_t type) {
    switch (type) {
        case 0x80: // LPCM
        case 0x81: // AC-3
        case 0x82: // DTS core
        case 0x83: // TrueHD
        case 0x84: // E-AC-3
        case 0x85: // DTS-HD HRA
        case 0x86: // DTS-HD MA
        case 0xa1: // secondary AC-3
        case 0xa2: // secondary DTS
            return true;
        default:
            return false;
    }
}

std::string M2TSAudioDemuxer::defaultProfile(std::uint8_t type) {
    switch (type) {
        case 0x80: return "Blu-ray LPCM";
        case 0x81: return "AC-3";
        case 0x82: return "DTS core";
        case 0x83: return "Dolby TrueHD";
        case 0x84: return "E-AC-3";
        case 0x85: return "DTS-HD HRA";
        case 0x86: return "DTS-HD MA";
        case 0xa1: return "Secondary AC-3";
        case 0xa2: return "Secondary DTS";
        default: return "Blu-ray audio";
    }
}

std::string M2TSAudioDemuxer::mimeForStreamType(std::uint8_t type) {
    switch (type) {
        case 0x81:
        case 0xa1:
            return "audio/ac3";
        case 0x84:
            return "audio/eac3";
        case 0x82:
        case 0xa2:
            return "audio/vnd.dts";
        case 0x85:
        case 0x86:
            return "audio/vnd.dts.hd";
        case 0x83:
            return "audio/true-hd";
        case 0x80:
            return "audio/raw";
        default:
            return "audio/unknown";
    }
}

bool M2TSAudioDemuxer::isSupportedStreamType(std::uint8_t type) {
    return type == 0x81 || type == 0x82 || type == 0x84 || type == 0x85
            || type == 0x86 || type == 0xa1 || type == 0xa2;
}

void M2TSAudioDemuxer::refreshTracksFromPrograms() {
    std::map<std::uint16_t, std::uint8_t> discovered;
    for (const auto& program : reader_->getPrograms()) {
        for (const auto& entry : program.streamPids) {
            if (isAudioStreamType(entry.second)) discovered[entry.first] = entry.second;
        }
    }
    for (const auto& entry : discovered) {
        const auto found = std::find_if(tracks_.begin(), tracks_.end(), [&](const AudioTrackInfo& t) {
            return t.pid == entry.first;
        });
        if (found != tracks_.end()) continue;
        AudioTrackInfo track;
        track.index = static_cast<int>(tracks_.size());
        track.pid = entry.first;
        track.stream_type = entry.second;
        track.profile = defaultProfile(entry.second);
        track.mime = mimeForStreamType(entry.second);
        track.supported = isSupportedStreamType(entry.second);
        tracks_.push_back(std::move(track));
    }
}

bool M2TSAudioDemuxer::scanCatalog(const std::vector<AudioTrackInfo>& declared_tracks,
                                   std::uint64_t catalog_probe_byte,
                                   std::string* error) {
    scan_pes_.clear();
    probes_.clear();
    tracks_.clear();
    declared_track_count_ = 0;
    for (const auto& declared : declared_tracks) {
        if (declared.pid == 0 || !isAudioStreamType(declared.stream_type)) continue;
        const auto duplicate = std::find_if(tracks_.begin(), tracks_.end(),
                [&](const AudioTrackInfo& track) { return track.pid == declared.pid; });
        if (duplicate != tracks_.end()) continue;
        AudioTrackInfo track = declared;
        track.index = static_cast<int>(tracks_.size());
        if (track.profile.empty()) track.profile = defaultProfile(track.stream_type);
        if (track.mime.empty()) track.mime = mimeForStreamType(track.stream_type);
        track.supported = isSupportedStreamType(track.stream_type);
        if (track.probe.empty()) track.probe = "MPLS STN declared; PES verification pending";
        tracks_.push_back(std::move(track));
        ++declared_track_count_;
    }

    const std::uint64_t packet_size = static_cast<std::uint64_t>(
            std::max(1, reader_->getPacketSize()));
    catalog_probe_byte_ = 0;
    if (catalog_probe_byte > 0 && catalog_probe_byte < reader_->getFileSize()) {
        catalog_probe_byte_ = (catalog_probe_byte / packet_size) * packet_size;
    }
    if (!reader_->seek(catalog_probe_byte_)) {
        catalog_probe_byte_ = 0;
        reader_->seek(0);
    }
    const std::uint64_t scan_start = reader_->tell();
    mvc_demux::M2TSReader::TSPacket packet;
    std::size_t packets = 0;
    while (packets < kCatalogPacketLimit
            && reader_->tell() - std::min(reader_->tell(), scan_start) < kCatalogByteLimit
            && reader_->readPacket(packet)) {
        ++packets;
        refreshTracksFromPrograms();
        if (!tracks_.empty()) processProbePacket(packet);
        bool complete = !tracks_.empty();
        for (const auto& track : tracks_) {
            const auto p = probes_.find(track.pid);
            if (p == probes_.end() || p->second.samples < kProbeSamples) {
                complete = false;
                break;
            }
        }
        if (complete) break;
    }
    for (auto& state : scan_pes_) finalizeProbePES(state.first, state.second);
    {
        std::set<std::uint16_t> pat_pmt_audio_pids;
        for (const auto& program : reader_->getPrograms()) {
            for (const auto& entry : program.streamPids) {
                if (isAudioStreamType(entry.second)) pat_pmt_audio_pids.insert(entry.first);
            }
        }
        pat_pmt_track_count_ = pat_pmt_audio_pids.size();
    }
    if (tracks_.empty()) {
        if (error) {
            std::ostringstream out;
            out << "SyLC audio discovery found no Blu-ray audio PIDs in " << label_
                << " after MPLS STN hints and PAT/PMT scan of " << packets
                << " packets / " << (reader_->tell() - scan_start) << " bytes";
            *error = out.str();
        }
        return false;
    }
    for (auto& track : tracks_) {
        const auto probe = probes_.find(track.pid);
        if (probe != probes_.end() && probe->second.samples > 0) {
            inspectTrackProbe(track, probe->second);
            ++directly_verified_track_count_;
            if (declared_track_count_ > 0) {
                track.probe = "MPLS STN declared + direct PID/PES verified; " + track.probe;
            }
        } else if (declared_track_count_ > 0) {
            track.probe = "MPLS STN declared; no PES sample in bounded catalog window";
        }
    }

    if (declared_track_count_ > 0 && directly_verified_track_count_ > 0) {
        catalog_discovery_method_ = pat_pmt_track_count_ > 0
                ? "MPLS STN + direct PID/PES + PAT/PMT validation"
                : "MPLS STN + direct PID/PES (PAT/PMT not required)";
    } else if (declared_track_count_ > 0) {
        catalog_discovery_method_ = "MPLS STN declaration (bounded direct probe incomplete)";
    } else {
        catalog_discovery_method_ = "PAT/PMT scan";
    }

    auto default_it = std::find_if(tracks_.begin(), tracks_.end(), [](const AudioTrackInfo& t) {
        return t.stream_type == 0x86;
    });
    if (default_it == tracks_.end()) {
        default_it = std::find_if(tracks_.begin(), tracks_.end(), [](const AudioTrackInfo& t) {
            return t.stream_type == 0x85 || t.stream_type == 0x83;
        });
    }
    if (default_it == tracks_.end()) default_it = tracks_.begin();
    if (default_it != tracks_.end()) default_it->default_main = true;

    reader_->seek(0);
    scan_pes_.clear();
    return true;
}

void M2TSAudioDemuxer::processProbePacket(const mvc_demux::M2TSReader::TSPacket& packet) {
    const auto track = std::find_if(tracks_.begin(), tracks_.end(), [&](const AudioTrackInfo& t) {
        return t.pid == packet.pid;
    });
    if (track == tracks_.end() || !packet.payloadExists || packet.payload.empty()) return;
    PESState& state = scan_pes_[packet.pid];
    if (packet.payloadUnitStartIndicator) {
        if (state.started && !state.buffer.empty()) finalizeProbePES(packet.pid, state);
        state.buffer.clear();
        state.started = true;
    }
    if (!state.started) return;
    if (state.buffer.size() + packet.payload.size() > kMaximumPesBytes) {
        state = {};
        return;
    }
    state.buffer.insert(state.buffer.end(), packet.payload.begin(), packet.payload.end());
}

void M2TSAudioDemuxer::finalizeProbePES(std::uint16_t pid, PESState& state) {
    if (!state.started || state.buffer.empty()) { state = {}; return; }
    const auto track = std::find_if(tracks_.begin(), tracks_.end(), [&](const AudioTrackInfo& t) {
        return t.pid == pid;
    });
    if (track == tracks_.end()) { state = {}; return; }
    std::vector<std::uint8_t> payload;
    std::int64_t pts = -1;
    int extended_stream_id = -1;
    if (parsePES(state.buffer, &payload, &pts, &extended_stream_id) && !payload.empty()) {
        ProbeState& probe = probes_[pid];
        if (probe.first_pts90k < 0 && pts >= 0) probe.first_pts90k = pts;
        if (probe.samples < kProbeSamples && probe.bytes.size() < kMaximumProbeBytes) {
            const std::size_t wanted = std::min<std::size_t>(
                    payload.size(), kMaximumProbeBytes - probe.bytes.size());
            probe.bytes.insert(probe.bytes.end(), payload.begin(), payload.begin() + wanted);
            ++probe.samples;
        }
        if (track->stream_type == 0x83) {
            std::vector<std::uint8_t>& destination = extended_stream_id == 0x76
                    ? probe.ac3_core_bytes : probe.truehd_bytes;
            int& counter = extended_stream_id == 0x76
                    ? probe.ac3_core_pes : probe.truehd_pes;
            if (destination.size() < kMaximumProbeBytes) {
                const std::size_t wanted = std::min<std::size_t>(
                        payload.size(), kMaximumProbeBytes - destination.size());
                destination.insert(destination.end(), payload.begin(), payload.begin() + wanted);
            }
            ++counter;
        }
    }
    state = {};
}

void M2TSAudioDemuxer::inspectTrackProbe(AudioTrackInfo& track, const ProbeState& probe) {
    track.first_raw_pts_us = probe.first_pts90k >= 0
            ? (probe.first_pts90k * 1000000LL) / 90000LL : -1;
    bool core = false;
    bool exss = false;
    bool xll = false;
    for (std::size_t i = 0; i + 4 <= probe.bytes.size(); ++i) {
        const std::uint32_t word = be32(probe.bytes.data() + i);
        core = core || word == 0x7ffe8001U || word == 0xfe7f0180U
                || word == 0x1fffe800U || word == 0xff1f00e8U;
        exss = exss || word == 0x64582025U;
        xll = xll || word == 0x41a29547U;
    }
    int amode = -1;
    int lfe = -1;
    if (track.stream_type == 0x82 || track.stream_type == 0x85 || track.stream_type == 0x86
            || track.stream_type == 0xa2) {
        parseDtsCore(probe.bytes, &track.channels, &track.sample_rate, &amode, &lfe);
        if (xll || track.stream_type == 0x86) track.profile = "DTS-HD MA";
        else if (exss || track.stream_type == 0x85) track.profile = "DTS-HD HRA";
        else track.profile = "DTS core";
        track.mime = (track.profile == "DTS core") ? "audio/vnd.dts" : "audio/vnd.dts.hd";
        track.bridge_format = "dts";
        track.decode_path = track.profile == "DTS core"
                ? "native DTS decode" : "native DTS-HD decode";
    } else if (track.stream_type == 0x81 || track.stream_type == 0x84
            || track.stream_type == 0xa1) {
        parseAc3(probe.bytes, &track.channels, &track.sample_rate);
        track.bridge_format = track.stream_type == 0x84 ? "eac3" : "ac3";
        track.decode_path = track.stream_type == 0x84
                ? "native E-AC-3 decode" : "native AC-3 decode";
    } else if (track.stream_type == 0x83) {
        track.profile = "Dolby TrueHD";
        track.mime = "audio/true-hd";
        track.embedded_ac3_core = probe.ac3_core_pes > 0;

        TrueHdAccessUnitFramer framer;
        framer.append(probe.truehd_bytes);
        std::vector<std::uint8_t> frame;
        TrueHdFrameInfo info;
        while (framer.pop(&frame, &info)) {
            if (!info.major_sync) continue;
            track.truehd_major_sync = true;
            track.channels = info.channels;
            track.sample_rate = info.sample_rate;
            break;
        }
        if (track.truehd_major_sync) {
            track.supported = true;
            track.bridge_format = "truehd";
            track.decode_path = "native TrueHD extraction and FFmpeg lossless decode";
        } else if (track.embedded_ac3_core) {
            parseAc3(probe.ac3_core_bytes, &track.channels, &track.sample_rate);
            track.supported = true;
            track.bridge_format = "ac3";
            track.decode_path = "embedded AC-3 core fallback";
        } else {
            track.supported = false;
            track.bridge_format = "unsupported";
            track.decode_path = "no valid TrueHD major sync or embedded AC-3 core found";
        }
    }
    std::ostringstream out;
    out << "PMT stream_type=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(track.stream_type) << std::dec
        << "; PID=0x" << std::hex << std::setw(4) << std::setfill('0')
        << track.pid << std::dec
        << "; PES samples=" << probe.samples << " bytes=" << probe.bytes.size()
        << "; raw PTS origin=" << track.first_raw_pts_us << " us";
    if (track.mime.find("dts") != std::string::npos) {
        out << "; core=" << (core ? "true" : "false")
            << " EXSS=" << (exss ? "true" : "false")
            << " XLL=" << (xll ? "true" : "false")
            << "; amode=" << amode << " lfe=" << lfe;
    }
    if (track.stream_type == 0x83) {
        out << "; TrueHD PES=" << probe.truehd_pes
            << " bytes=" << probe.truehd_bytes.size()
            << "; AC-3 companion PES=" << probe.ac3_core_pes
            << " bytes=" << probe.ac3_core_bytes.size()
            << "; major-sync=" << (track.truehd_major_sync ? "true" : "false")
            << "; bridge=" << track.bridge_format
            << "; path=" << track.decode_path;
    }
    track.probe = out.str();
}

bool M2TSAudioDemuxer::resetSelectedTrack(std::size_t track_index,
                                            std::int64_t target_global_us,
                                            std::int64_t global_offset_us,
                                            std::string* error) {
    if (!reader_ || track_index >= tracks_.size()) {
        if (error) *error = "Invalid SyLC native Blu-ray audio track index";
        return false;
    }
    selected_track_ = track_index;
    selected_pid_ = tracks_[track_index].pid;
    selected_stream_type_ = tracks_[track_index].stream_type;
    selected_is_hd_ = tracks_[track_index].profile.find("DTS-HD") != std::string::npos;
    selected_is_truehd_ = tracks_[track_index].stream_type == 0x83;
    selected_truehd_core_fallback_ = selected_is_truehd_
            && tracks_[track_index].bridge_format == "ac3";
    selected_pes_ = {};
    elementary_buffer_.clear();
    truehd_framer_.reset();
    ready_samples_.clear();
    raw_origin_pts_us_ = -1;
    next_frame_pts_us_ = -1;
    last_pes_raw_pts_us_ = -1;
    eof_flushed_ = false;
    target_global_us_ = std::max<std::int64_t>(0, target_global_us);
    global_offset_us_ = std::max<std::int64_t>(0, global_offset_us);
    packet_count_ = 0;
    pes_count_ = 0;
    payload_bytes_ = 0;
    skipped_before_target_ = 0;
    access_units_emitted_ = 0;
    truehd_pes_seen_ = 0;
    truehd_ac3_core_pes_seen_ = 0;
    truehd_frames_emitted_ = 0;
    truehd_major_syncs_seen_ = 0;
    partial_frame_waits_ = 0;
    bytes_discarded_before_sync_ = 0;
    maximum_access_unit_bytes_ = 0;
    first_pts_us_ = -1;
    last_pts_us_ = -1;
    physical_seek_active_ = false;
    physical_seek_byte_ = 0;
    physical_seek_budget_bytes_ = 0;
    clip_in_raw_pts_us_ = -1;
    anchor_raw_pts_us_ = -1;
    anchor_global_pts_us_ = -1;
    physical_seek_bytes_before_first_sample_ = 0;
    physical_seek_budget_exceeded_ = false;
    return true;
}

bool M2TSAudioDemuxer::selectTrack(std::size_t track_index,
                                   std::int64_t target_global_us,
                                   std::int64_t global_offset_us,
                                   std::string* error) {
    if (!resetSelectedTrack(track_index, target_global_us, global_offset_us, error)) return false;
    if (!reader_->seek(0)) {
        if (error) *error = "Could not rewind SyLC M2TS audio reader";
        return false;
    }
    return true;
}

bool M2TSAudioDemuxer::selectTrackAt(std::size_t track_index,
                                     std::int64_t target_global_us,
                                     std::int64_t global_offset_us,
                                     std::uint64_t byte_offset,
                                     std::int64_t clip_in_raw_pts_us,
                                     std::int64_t anchor_raw_pts_us,
                                     std::uint64_t read_budget_bytes,
                                     std::string* error) {
    if (!resetSelectedTrack(track_index, target_global_us, global_offset_us, error)) return false;
    const std::uint64_t packet_size = static_cast<std::uint64_t>(
            std::max(1, reader_->getPacketSize()));
    const std::uint64_t aligned = (byte_offset / packet_size) * packet_size;
    if (aligned >= reader_->getFileSize()) {
        if (error) *error = "CLPI M2TS audio seek byte is outside the selected clip";
        return false;
    }
    if (!reader_->seek(aligned)) {
        if (error) *error = "Could not seek SyLC M2TS audio reader to CLPI entry point";
        return false;
    }
    physical_seek_active_ = true;
    physical_seek_byte_ = aligned;
    physical_seek_budget_bytes_ = std::max<std::uint64_t>(packet_size, read_budget_bytes);
    clip_in_raw_pts_us_ = std::max<std::int64_t>(0, clip_in_raw_pts_us);
    anchor_raw_pts_us_ = std::max<std::int64_t>(0, anchor_raw_pts_us);
    anchor_global_pts_us_ = global_offset_us_
            + std::max<std::int64_t>(0, anchor_raw_pts_us_ - clip_in_raw_pts_us_);
    return true;
}

std::int64_t M2TSAudioDemuxer::normalizeRawPts(std::int64_t raw_pts_us) const {
    if (raw_pts_us < 0) {
        if (last_pts_us_ >= 0) return last_pts_us_;
        if (anchor_global_pts_us_ >= 0) return anchor_global_pts_us_;
        return global_offset_us_;
    }
    if (clip_in_raw_pts_us_ >= 0) {
        return global_offset_us_
                + std::max<std::int64_t>(0, raw_pts_us - clip_in_raw_pts_us_);
    }
    if (raw_origin_pts_us_ >= 0) {
        return global_offset_us_
                + std::max<std::int64_t>(0, raw_pts_us - raw_origin_pts_us_);
    }
    return global_offset_us_;
}

void M2TSAudioDemuxer::processSelectedPacket(
        const mvc_demux::M2TSReader::TSPacket& packet) {
    if (packet.pid != selected_pid_ || !packet.payloadExists || packet.payload.empty()) return;
    if (packet.payloadUnitStartIndicator) {
        if (selected_pes_.started && !selected_pes_.buffer.empty()) finalizeSelectedPES();
        selected_pes_.buffer.clear();
        selected_pes_.started = true;
    }
    if (!selected_pes_.started) return;
    if (selected_pes_.buffer.size() + packet.payload.size() > kMaximumPesBytes) {
        selected_pes_ = {};
        return;
    }
    selected_pes_.buffer.insert(selected_pes_.buffer.end(),
                                packet.payload.begin(), packet.payload.end());
}

void M2TSAudioDemuxer::finalizeSelectedPES() {
    if (!selected_pes_.started || selected_pes_.buffer.empty()) {
        selected_pes_ = {};
        return;
    }
    std::vector<std::uint8_t> payload;
    std::int64_t pts90k = -1;
    int extended_stream_id = -1;
    const bool valid = parsePES(selected_pes_.buffer, &payload, &pts90k,
                                &extended_stream_id);
    selected_pes_ = {};
    if (!valid || payload.empty()) return;

    ++pes_count_;
    payload_bytes_ += payload.size();
    last_pes_raw_pts_us_ = pts90k >= 0 ? (pts90k * 1000000LL) / 90000LL : -1;
    if (raw_origin_pts_us_ < 0 && last_pes_raw_pts_us_ >= 0) {
        raw_origin_pts_us_ = last_pes_raw_pts_us_;
    }

    if (selected_is_truehd_) {
        const bool ac3_companion = extended_stream_id == 0x76;
        if (ac3_companion) ++truehd_ac3_core_pes_seen_;
        else ++truehd_pes_seen_;

        if (selected_truehd_core_fallback_) {
            if (!ac3_companion) return;
            const std::int64_t normalized = normalizeRawPts(last_pes_raw_pts_us_);
            if (normalized < target_global_us_) {
                ++skipped_before_target_;
                last_pts_us_ = normalized;
                return;
            }
            CompressedAudioSample sample;
            sample.data = std::move(payload);
            sample.pts_us = normalized;
            maximum_access_unit_bytes_ = std::max(maximum_access_unit_bytes_, sample.data.size());
            ready_samples_.push_back(std::move(sample));
            ++access_units_emitted_;
            if (first_pts_us_ < 0) first_pts_us_ = normalized;
            last_pts_us_ = normalized;
            return;
        }

        // FFmpeg identifies the companion AC-3 packets by PES extended stream ID
        // 0x76.  The full TrueHD path must omit those packets and concatenate only
        // the lossless MLP access units before framing them across PES boundaries.
        if (ac3_companion) return;
        if (next_frame_pts_us_ < 0 && last_pes_raw_pts_us_ >= 0) {
            next_frame_pts_us_ = normalizeRawPts(last_pes_raw_pts_us_);
        }
        truehd_framer_.append(payload);
        extractTrueHdAccessUnits(false);
        return;
    }

    if (isDtsType(selected_stream_type_)) {
        if (elementary_buffer_.size() + payload.size() > (16U << 20U)) {
            // Retain only enough tail to find a split sync/header rather than allowing
            // malformed input to grow without bound.
            const std::size_t keep = std::min<std::size_t>(elementary_buffer_.size(), 64U);
            bytes_discarded_before_sync_ += elementary_buffer_.size() - keep;
            if (keep > 0) {
                std::vector<std::uint8_t> tail(elementary_buffer_.end()
                        - static_cast<std::ptrdiff_t>(keep), elementary_buffer_.end());
                elementary_buffer_.swap(tail);
            } else {
                elementary_buffer_.clear();
            }
        }
        if (next_frame_pts_us_ < 0 && last_pes_raw_pts_us_ >= 0) {
            next_frame_pts_us_ = normalizeRawPts(last_pes_raw_pts_us_);
        }
        elementary_buffer_.insert(elementary_buffer_.end(), payload.begin(), payload.end());
        extractAccessUnits(false);
        return;
    }

    // AC-3/E-AC-3 are complete PES-delivered elementary payloads.
    const std::int64_t normalized = normalizeRawPts(last_pes_raw_pts_us_);
    if (normalized < target_global_us_) {
        ++skipped_before_target_;
        last_pts_us_ = normalized;
        return;
    }
    CompressedAudioSample sample;
    sample.data = std::move(payload);
    sample.pts_us = normalized;
    ready_samples_.push_back(std::move(sample));
    ++access_units_emitted_;
    maximum_access_unit_bytes_ = std::max(maximum_access_unit_bytes_, ready_samples_.back().data.size());
    if (first_pts_us_ < 0) first_pts_us_ = normalized;
    last_pts_us_ = normalized;
}

void M2TSAudioDemuxer::extractAccessUnits(bool end_of_stream) {
    if (!isDtsType(selected_stream_type_)) return;
    for (;;) {
        const std::size_t start = findValidDtsCore(elementary_buffer_, 0);
        if (start == std::string::npos) {
            if (elementary_buffer_.size() > 64U) {
                const std::size_t discard = elementary_buffer_.size() - 64U;
                bytes_discarded_before_sync_ += discard;
                elementary_buffer_.erase(elementary_buffer_.begin(),
                        elementary_buffer_.begin() + static_cast<std::ptrdiff_t>(discard));
            }
            if (!elementary_buffer_.empty()) ++partial_frame_waits_;
            return;
        }
        if (start > 0) {
            bytes_discarded_before_sync_ += start;
            elementary_buffer_.erase(elementary_buffer_.begin(),
                    elementary_buffer_.begin() + static_cast<std::ptrdiff_t>(start));
        }

        DtsCoreFrame core;
        if (!parseDtsCoreFrame(elementary_buffer_.data(), elementary_buffer_.size(), &core)) {
            elementary_buffer_.erase(elementary_buffer_.begin());
            ++bytes_discarded_before_sync_;
            continue;
        }
        if (elementary_buffer_.size() < core.size) {
            ++partial_frame_waits_;
            return;
        }

        std::size_t access_unit_size = core.size;
        if (selected_is_hd_) {
            const std::size_t aligned_core = (core.size + 3U) & ~std::size_t(3U);
            if (elementary_buffer_.size() < aligned_core + 4U) {
                if (!end_of_stream) ++partial_frame_waits_;
                return;
            }
            if (be32(elementary_buffer_.data() + aligned_core) == 0x64582025U) {
                if (elementary_buffer_.size() < aligned_core + 10U) {
                    if (!end_of_stream) ++partial_frame_waits_;
                    return;
                }
                std::size_t exss_size = 0;
                if (parseDtsExssSize(elementary_buffer_.data() + aligned_core,
                                     elementary_buffer_.size() - aligned_core, &exss_size)) {
                    access_unit_size = aligned_core + exss_size;
                    if (elementary_buffer_.size() < access_unit_size) {
                        if (!end_of_stream) ++partial_frame_waits_;
                        return;
                    }
                } else {
                    // Fall back to the next validated core marker. This mirrors the
                    // essential boundary behavior of FFmpeg's DCA parser without
                    // passing fragmented PES chunks to the decoder.
                    const std::size_t next = findValidDtsCore(elementary_buffer_, core.size);
                    if (next == std::string::npos) {
                        if (!end_of_stream) ++partial_frame_waits_;
                        return;
                    }
                    access_unit_size = next;
                }
            }
        }

        if (elementary_buffer_.size() < access_unit_size) {
            if (!end_of_stream) ++partial_frame_waits_;
            return;
        }

        if (next_frame_pts_us_ < 0) {
            next_frame_pts_us_ = anchor_global_pts_us_ >= 0
                    ? anchor_global_pts_us_ : global_offset_us_;
        }
        const std::int64_t frame_pts = next_frame_pts_us_;
        const std::int64_t duration_us = core.sample_rate > 0
                ? (static_cast<std::int64_t>(core.samples) * 1000000LL
                   + core.sample_rate / 2) / core.sample_rate
                : 10667LL;
        next_frame_pts_us_ += std::max<std::int64_t>(1, duration_us);

        CompressedAudioSample sample;
        sample.data.assign(elementary_buffer_.begin(), elementary_buffer_.begin()
                + static_cast<std::ptrdiff_t>(access_unit_size));
        sample.pts_us = frame_pts;
        elementary_buffer_.erase(elementary_buffer_.begin(), elementary_buffer_.begin()
                + static_cast<std::ptrdiff_t>(access_unit_size));

        if (frame_pts < target_global_us_) {
            ++skipped_before_target_;
            last_pts_us_ = frame_pts;
            continue;
        }
        maximum_access_unit_bytes_ = std::max(maximum_access_unit_bytes_, sample.data.size());
        ready_samples_.push_back(std::move(sample));
        ++access_units_emitted_;
        if (first_pts_us_ < 0) first_pts_us_ = frame_pts;
        last_pts_us_ = frame_pts;
    }
}

void M2TSAudioDemuxer::extractTrueHdAccessUnits(bool end_of_stream) {
    (void)end_of_stream;
    for (;;) {
        std::vector<std::uint8_t> frame;
        TrueHdFrameInfo info;
        if (!truehd_framer_.pop(&frame, &info)) {
            partial_frame_waits_ = truehd_framer_.partialWaits();
            bytes_discarded_before_sync_ = truehd_framer_.bytesDiscarded();
            return;
        }
        if (info.major_sync) {
            ++truehd_major_syncs_seen_;
        }
        if (next_frame_pts_us_ < 0) {
            next_frame_pts_us_ = anchor_global_pts_us_ >= 0
                    ? anchor_global_pts_us_ : global_offset_us_;
        }
        const std::int64_t frame_pts = next_frame_pts_us_;
        const std::int64_t duration_us = info.sample_rate > 0
                && info.samples_per_frame > 0
                ? (static_cast<std::int64_t>(info.samples_per_frame) * 1000000LL
                   + info.sample_rate / 2) / info.sample_rate
                : 833LL;
        next_frame_pts_us_ += std::max<std::int64_t>(1, duration_us);

        if (frame_pts < target_global_us_) {
            ++skipped_before_target_;
            last_pts_us_ = frame_pts;
            continue;
        }
        CompressedAudioSample sample;
        sample.data = std::move(frame);
        sample.pts_us = frame_pts;
        maximum_access_unit_bytes_ = std::max(maximum_access_unit_bytes_, sample.data.size());
        ready_samples_.push_back(std::move(sample));
        ++access_units_emitted_;
        ++truehd_frames_emitted_;
        if (first_pts_us_ < 0) first_pts_us_ = frame_pts;
        last_pts_us_ = frame_pts;
    }
}

bool M2TSAudioDemuxer::popReadySample(CompressedAudioSample& sample) {
    if (ready_samples_.empty()) return false;
    sample = std::move(ready_samples_.front());
    ready_samples_.pop_front();
    return true;
}

bool M2TSAudioDemuxer::readNextSample(CompressedAudioSample& sample, std::string* error) {
    sample = {};
    if (!reader_ || selected_track_ >= tracks_.size()) {
        if (error) *error = "SyLC Blu-ray audio track was not selected";
        return false;
    }
    if (popReadySample(sample)) return true;

    mvc_demux::M2TSReader::TSPacket packet;
    while (reader_->readPacket(packet)) {
        ++packet_count_;
        if (physical_seek_active_ && first_pts_us_ < 0) {
            const std::uint64_t current = reader_->tell();
            physical_seek_bytes_before_first_sample_ = current >= physical_seek_byte_
                    ? current - physical_seek_byte_ : 0;
            if (physical_seek_bytes_before_first_sample_ > physical_seek_budget_bytes_) {
                physical_seek_budget_exceeded_ = true;
                if (error) *error = "CLPI-aligned Blu-ray audio seek exceeded bounded read budget";
                return false;
            }
        }
        processSelectedPacket(packet);
        if (popReadySample(sample)) return true;
    }
    if (!eof_flushed_) {
        eof_flushed_ = true;
        finalizeSelectedPES();
        if (selected_is_truehd_ && !selected_truehd_core_fallback_) {
            extractTrueHdAccessUnits(true);
        } else {
            extractAccessUnits(true);
        }
        if (popReadySample(sample)) return true;
    }
    sample.end_of_stream = true;
    return false;
}

bool M2TSAudioDemuxer::parsePES(const std::vector<std::uint8_t>& pes,
                                std::vector<std::uint8_t>* payload,
                                std::int64_t* pts90k,
                                int* extended_stream_id) {
    if (!payload || !pts90k || pes.size() < 9 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1) {
        return false;
    }
    const std::size_t header = 9u + pes[8];
    if (header > pes.size()) return false;
    *pts90k = -1;
    if (extended_stream_id) *extended_stream_id = -1;
    const std::uint8_t flags = pes[7];
    std::size_t cursor = 9;
    if ((flags & 0xc0U) == 0x80U) {
        if (cursor + 5 > header) return false;
        *pts90k = parsePts(pes.data() + cursor);
        cursor += 5;
    } else if ((flags & 0xc0U) == 0xc0U) {
        if (cursor + 10 > header) return false;
        *pts90k = parsePts(pes.data() + cursor);
        cursor += 10;
    }
    if ((flags & 0x01U) != 0 && cursor < header) {
        const std::uint8_t pes_ext = pes[cursor++];
        std::size_t skip = (pes_ext >> 4U) & 0x0bU;
        skip += skip & 0x09U;
        if (cursor + skip <= header) cursor += skip;
        else cursor = header;
        if ((pes_ext & 0x41U) == 0x01U && cursor + 2 <= header) {
            if ((pes[cursor] & 0x7fU) > 0 && (pes[cursor + 1] & 0x80U) == 0) {
                if (extended_stream_id) *extended_stream_id = pes[cursor + 1];
            }
        }
    }
    payload->assign(pes.begin() + static_cast<std::ptrdiff_t>(header), pes.end());
    return !payload->empty();
}

std::string M2TSAudioDemuxer::diagnostics() const {
    std::ostringstream out;
    out << "SyLC native M2TS audio demuxer\n"
        << "Source: " << label_ << "\n"
        << "Catalog discovery: " << catalog_discovery_method_ << "\n"
        << "Catalog probe byte: " << catalog_probe_byte_ << "\n"
        << "Declared/PAT-added/directly-verified tracks: " << declared_track_count_
        << " / " << pat_pmt_track_count_ << " / " << directly_verified_track_count_ << "\n"
        << "Tracks: " << tracks_.size();
    for (const auto& track : tracks_) {
        out << "\n  #" << track.index << " PID=0x" << std::hex << track.pid << std::dec
            << " type=0x" << std::hex << static_cast<int>(track.stream_type) << std::dec
            << " " << track.profile << " " << track.channels << "ch/"
            << track.sample_rate << "Hz"
            << (track.default_main ? " DEFAULT/MAIN" : "")
            << (track.supported ? " supported" : " unsupported")
            << "\n    " << track.probe;
    }
    out << "\nSelected PID: 0x" << std::hex << selected_pid_ << std::dec
        << "\nPackets/PES/payload bytes: " << packet_count_ << " / " << pes_count_
        << " / " << payload_bytes_
        << "\nRaw transport PTS origin / last: " << raw_origin_pts_us_ << " / "
        << last_pes_raw_pts_us_ << " us"
        << "\nNormalized timeline offset / target: " << global_offset_us_ << " / "
        << target_global_us_ << " us"
        << "\nCLPI physical seek active/byte: " << (physical_seek_active_ ? "yes" : "no")
        << " / " << physical_seek_byte_
        << "\nCLPI raw clip-in/anchor PTS: " << clip_in_raw_pts_us_ << " / "
        << anchor_raw_pts_us_ << " us"
        << "\nCLPI normalized anchor global PTS: " << anchor_global_pts_us_ << " us"
        << "\nCLPI bounded bytes used/limit/exceeded: "
        << physical_seek_bytes_before_first_sample_ << " / "
        << physical_seek_budget_bytes_ << " / "
        << (physical_seek_budget_exceeded_ ? "yes" : "no")
        << "\nCompressed access units emitted: " << access_units_emitted_
        << "; partial-frame waits=" << partial_frame_waits_
        << "; bytes discarded before sync=" << bytes_discarded_before_sync_
        << "; max AU=" << maximum_access_unit_bytes_
        << "\nTrueHD path/PES/core-PES/frames/major-syncs: "
        << (selected_is_truehd_ ? (selected_truehd_core_fallback_ ? "AC-3-core-fallback" : "native") : "n/a")
        << " / " << truehd_pes_seen_ << " / " << truehd_ac3_core_pes_seen_
        << " / " << truehd_frames_emitted_ << " / " << truehd_major_syncs_seen_
        << "; framer lost-sync=" << truehd_framer_.lostSyncCount()
        << "\nSkipped before target: " << skipped_before_target_
        << "\nFirst/last normalized PTS: " << first_pts_us_ << " / " << last_pts_us_ << " us";
    return out.str();
}

}  // namespace sylc_audio
