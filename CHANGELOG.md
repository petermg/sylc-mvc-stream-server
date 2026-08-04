# Changelog

## Unreleased

- Clarified in the README and web interface that SyLC accepts MVC input only and is not a general SBS/OU/anaglyph converter.
- Added pre-session validation for MakeMKV `stereo_mode=block_lr` and Blu-ray ISO MVC metadata.
- Unsupported SBS, over/under, anaglyph, and ordinary 2D sources now fail cleanly before a conversion session is created.

## 0.7.0-alpha.3

- Added 3D-aware server-side subtitle burn-in for embedded MKV/MK3D SRT, ASS, SSA, WebVTT, and PGS tracks.
- Added same-stem `.srt`, `.ass`, `.ssa`, `.vtt`, and `.sup` sidecar subtitle support.
- Added selectable Blu-ray ISO PGS tracks with native M2TS/PES extraction and bounded seek-state reconstruction.
- Added per-eye subtitle composition for SBS, OU, mono, anaglyph, and passive 4K row-interleaved output.
- Preserved subtitle selection across seeking, output-mode changes, audio changes, and eye swaps.
- Fixed delayed first ISO PGS captions being rebased to movie startup by inserting a transparent zero-time PGS timeline anchor.
- Added subtitle selection to the web UI and session API, plus expanded diagnostics and tests.

## 0.7.0-alpha.2

- Added server-encoded 3840×2160 passive row-interleaved output.
- Added left-top/even and right-top/even row parity modes.
- Added web UI, API, report, validation, and test coverage for both modes.
- Kept the normal direct HLS client path; no client-side OpenGL row weaving is required.

## 0.7.0-alpha.1

- First public-alpha server release.
- Generic installer and systemd configuration.
- Web-managed multiple Media Libraries.
- First-run setup wizard.
- Restricted server-directory browser.
- Optional API-token authentication.
- Stable library-based media IDs.
- Configurable VA-API render device and software-encoder fallback.
- Preserved MVC MKV, Blu-ray 3D ISO, seek recovery, TrueHD, DTS-HD, 3D modes, and anaglyph support.
