# Changelog

## 0.7.0-alpha.2

- Documented the confirmed 4K passive-display result and the requirement to disable motion smoothing/frame interpolation.
- Added passive-row setup, troubleshooting, repository-release guidance, and GitHub issue templates.

- Added server-encoded 3840×2160 passive row-interleaved output.
- Added left-top/even and right-top/even row parity modes.
- Added web UI, API, report, validation, and test coverage for both modes.
- Kept the normal direct HLS client path; no client-side OpenGL row weaving is required.

## 0.7.0-alpha.1

- First public-alpha server release
- Generic installer and systemd configuration
- Web-managed multiple Media Libraries
- First-run setup wizard
- Restricted server-directory browser
- Optional API-token authentication
- Stable library-based media IDs
- Configurable VA-API render device and software-encoder fallback
- Preserved MVC MKV, Blu-ray 3D ISO, seek recovery, TrueHD, DTS-HD, 3D modes, and anaglyph support
