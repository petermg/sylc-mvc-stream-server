# Source Provenance

## Project lineage

The service pipeline began with the previously proven SyLC server Phase 5 package:

```text
sylc-server-phase5-audio-hls.zip
SHA-256: 01242b68dda2c759c0b7c62d8d17aba024c318cd056a99d1da1fcaa599bbc9d6
```

That line supplied the MVC edge264 integration, stereo-pair/POC validation, compositor, audio/HLS path, and known ShortMVC regression behavior. The server line added direct streaming, service/session control, seeking, cleanup, output modes, anaglyph support, web-managed Media Libraries, and optional API authentication.

## Version 0.6.7 ISO source

The Blu-ray ISO implementation is ported from the open-source SyLC Android physical-stream work in the existing SyLC MVC Player project. Relevant concepts and source families include:

- physical ISO/UDF random-access source handling;
- MPLS feature resolution and replay-loop-decoy rejection;
- physical duration fallback;
- CLPI entry-point/audio seek parsing;
- M2TS PAT/PMT/PES audio demux and complete codec access-unit reconstruction;
- SSIF interleave parsing and MVC access-unit assembly;
- common Blu-ray timeline normalization;
- base/dependent-eye recovery protections.

The Linux port replaces Android file-descriptor/Java access with POSIX file access and bundled libudfread callbacks, then connects the resulting Annex-B video and audio pipes to the already proven server runner.

Version 0.6.7.1 also ports the Android playback core's explicit rule that one- or two-byte NAL type-24 records in the SyLC SSIF access-unit stream are framing markers rather than H.264 picture NALs. The Linux compositor now excludes those markers before edge264 and reports them separately.

The package's original/adapted SyLC ISO files are under:

```text
engine/phase6-streaming/src/iso/
engine/phase6-streaming/src/sylc_m2ts/
```

## edge264

edge264 source is included under BSD-3-Clause and remains the native AVC/MVC decoder component. Its license is at:

```text
engine/phase6-streaming/src/edge264/LICENSE_BSD.txt
```

## libudfread

VideoLAN libudfread 1.2.0 source is included unmodified except for normal CMake integration. It is built statically into the ISO source adapter. Source and LGPL-2.1-or-later license are at:

```text
engine/phase6-streaming/src/videolan/libudfread-1.2.0/
engine/phase6-streaming/src/videolan/libudfread-1.2.0/COPYING
```

## libbluray

The Linux adapter dynamically loads the target system's `libbluray.so.2` for title/playlist metadata. The package does not redistribute libbluray binaries, source, or development headers. The ABI declarations used by the loader are limited to the stable functions and title/clip/stream metadata needed for feature selection. Physical data access remains through the bundled libudfread path.

## FFmpeg

FFmpeg and ffprobe are invoked as installed system binaries. They are used for Matroska demux, codec decoding/encoding, anaglyph filtering, VA-API upload/encoding, HLS muxing, and validation. The package does not redistribute FFmpeg.

## Clean-room/proprietary boundary

No proprietary 4XVR source, binary, resource, or decompiled implementation is copied or included. Historical 4XVR behavior was used only as an external behavioral comparison. The ISO implementation derives from SyLC's own open-source Android work and the open-source dependencies listed above.

The Android ARM64 alpha and experimental ARMv7/dual-ABI projects remain parallel deliverables; this package contains the Linux server port only.

## Media and secrets

No movie, Blu-ray ISO, password, API key, Samba credential, VPN profile, or private environment file is included. The package contains only source, scripts, web assets, tests, documentation, and licenses.

## Version 0.6.7.2 recovery port

The signed source-pair offset calibrator, zero/+1/-1-frame SSIF phase aligner,
6+18 hidden-pair policy, and bounded earlier-anchor retry policy are ported from
the open-source SyLC Android `0.20.0-alpha.5` codebase. The Linux adaptation is
implemented in the ISO source adapter and shell runner; no proprietary 4XVR
code or binary behavior was used.

## Native TrueHD source path

The native TrueHD path is an original SyLC implementation built from public Blu-ray transport behavior and the public FFmpeg parser/demuxer contracts. It does not copy proprietary Dolby code. The implementation uses the standard MLP/TrueHD access-unit length and major-sync structure, separates the optional AC-3 companion by Blu-ray PES extended stream ID, and hands raw TrueHD to the system FFmpeg decoder. The final HLS output remains AC-3 and does not claim Atmos passthrough.
