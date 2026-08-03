# SyLC MVC Stream Server 0.7.0-alpha.3

This public alpha adds complete server-side subtitle selection and 3D-aware burn-in while preserving the established MVC, audio, seeking, passive-row, and HLS paths from `0.7.0-alpha.2`.

## Highlights

- Selectable embedded MKV/MK3D SubRip/SRT, ASS, SSA, WebVTT, and Blu-ray PGS tracks.
- Selectable Blu-ray 3D ISO PGS tracks discovered from the selected feature playlist.
- Same-stem `.srt`, `.ass`, `.ssa`, `.vtt`, and `.sup` sidecars.
- Per-eye subtitle composition before SBS/OU packing, passive-row weaving, or anaglyph conversion, placing captions at neutral screen depth.
- Subtitle selection in the web UI and HTTP API.
- Subtitle selection retained across seeks, output-mode changes, audio changes, and eye swaps.
- Bounded PGS display-state restoration for nonzero ISO seeks.
- Transparent zero-time PGS anchoring so a delayed first caption retains its authored startup time instead of appearing at movie time zero.

## Matching Android/Fire TV player

Use SyLC Stream Player `0.1.0-alpha.10` for subtitle controls and remote Menu-button overlay toggling. Older player `0.1.0-alpha.9` can still play non-subtitle sessions from this server.

## Preserved behavior

- MVC MKV/MK3D and unencrypted Blu-ray 3D ISO sources.
- Half/Full SBS and OU, mono, Color/Dubois anaglyph, eye swap, and passive 4K rows.
- Intel VA-API H.264 or software x264 fallback.
- DTS, DTS-HD, AC-3, E-AC-3, and TrueHD input with deterministic AC-3 5.1 HLS output.
- Web-managed read-only Media Libraries, optional API token, session replacement, reports, and source-timeline seeking.

## Known limitations

- No AACS/BD+ decryption, Blu-ray menus/BD-J, chapters, or UHD/HEVC Blu-ray support.
- One active conversion session at a time.
- Forced-display flags within a PGS stream are not yet exposed as a separate forced-only option.
- Blu-ray 3D subtitle depth metadata is not yet applied; captions use neutral screen depth.
- Subtitle size, vertical position, depth, and delay controls are not yet available.

## Validation

Automated service/API, authentication, ISO logic, native MVC recovery, TrueHD framing, PGS extraction, shell, JavaScript, and clean-package checks pass. Real MVC media validation covered text, embedded PGS, ISO PGS, sidecar SUP, nonzero seeking, Dubois anaglyph, SBS/OU, and passive rows. The project owner subsequently confirmed that startup subtitle timing, seek timing, and the overall subtitle behavior work as intended on the live server.
