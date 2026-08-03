# Validation report — 0.7.0-alpha.2

This update retains the previously validated MVC/ISO/audio engine and adds two server-encoded passive-row output modes.

## Automated results for this package

- Python session/service and Media Libraries tests: 17/17 pass
- HTTP playback/replacement-seek integration: pass
- First-run setup/API-token HTTP integration: pass
- ISO source-logic smoke checks: pass
- JavaScript syntax check: pass
- Python compile check: pass
- Installer and runner shell syntax checks: pass
- FFmpeg passive-row filter smoke test: exact 3840×2160 frame size pass

The native MVC and TrueHD sources are unchanged from 0.7.0-alpha.1. A complete native rebuild was started in the packaging environment but exceeded its execution-time window; the new feature does not modify those native sources.

## Real passive-display validation

The standalone pre-integration benchmark produced:

- left-top passive rows: approximately 1.288× real time;
- right-top passive rows: approximately 1.295× real time;
- 330/330 stereo pairs, with no POC, pairing, or decoder errors;
- 3840×2160 H.264 output;
- approximately 61.7 Mb/s average video bitrate at VA-API QP 22.

The left-top sample was visually confirmed as correct on the project's 4K passive 3D display.

## Remaining test scope

The integrated HLS mode still needs full-movie testing on the intended network, Fire TV model, and passive display. The high 4K bitrate may be the limiting factor even though server throughput is sufficient.
