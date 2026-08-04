# Validation report — 0.7.0-alpha.3

Validated August 4, 2026 in the source-build environment. The alpha.3 subtitle/media-path acceptance described below remains from the live Ubuntu Server 24.04 deployment.

## Automated and structural validation

- Python service/public-alpha tests: PASS
- HTTP integration and subtitle/session metadata: PASS
- Setup/API-token integration: PASS
- ISO selection and seek logic smoke suite: PASS
- Shell and browser JavaScript syntax checks: PASS
- Native CMake build: PASS
- Native MVC recovery and TrueHD framing tests: PASS
- PGS M2TS/PES extraction and segment reconstruction tests: PASS
- Clean-package extraction and checksum validation: PASS
- MVC-input validation regression tests: PASS for MakeMKV `block_lr`; Full-SBS `left_right` and ISO features without MVC are rejected before session creation
- Native CMake build without bundled `libudfread/config.h`: PASS

## Real media-path validation

The server pipeline was exercised with real MVC media for:

- duplicated text sidecars in Half-SBS;
- embedded text subtitles after a nonzero seek in Dubois anaglyph;
- 3840×2160 passive rows with subtitles;
- embedded MKV PGS;
- Blu-ray ISO PGS;
- sidecar SUP;
- nonzero PGS seek-state restoration;
- delayed-first-caption startup anchoring.

The tested MVC runs reported zero stereo-pairing or decoder errors.

## Live acceptance

The project owner confirmed on August 3, 2026 that:

- subtitles render as intended;
- delayed first ISO PGS captions no longer appear at movie startup;
- subtitles remain aligned after seeking;
- the matching Android/Fire TV player's controls and remote Menu-button toggle behave as requested.

## Remaining broader validation

Additional titles and multi-segment playlists remain useful for broader compatibility coverage, especially unusual authored PGS state reuse or clip-boundary events.
