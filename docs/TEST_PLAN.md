# Public alpha test plan

## Automated

Run:

```bash
./tests/run_all.sh
```

This covers:

- Python service/session behavior;
- Media Libraries persistence, migration, stable IDs, and path validation;
- first-run setup and API-token authentication;
- HTTP catalog/session/seek/HLS behavior;
- ISO selection logic;
- MVC recovery and TrueHD framing native tests;
- shell and JavaScript syntax checks;
- subtitle discovery, selection, runner environment, replacement preservation, and sidecar-cache invalidation.

## Manual installation

1. Install on a clean supported Linux host.
2. Confirm no username, IP address, or media path is requested by editing source files.
3. Complete first-run setup in a browser.
4. Add, edit, disable, enable, rescan, and remove multiple libraries.
5. Confirm source files remain unchanged.
6. Enable an API token and confirm unauthenticated API requests receive HTTP 401.
7. Confirm a valid token restores web-control access.

## Manual media acceptance

Test at least:

- MVC MKV from time zero and after a seek;
- ISO with exact SSIF extent maps;
- ISO requiring PTS-search fallback and eye-phase correction;
- DTS-HD MA 7.1 source;
- TrueHD/Atmos 7.1 source;
- half/full SBS and OU;
- Color and Dubois anaglyph;
- eye swap;
- stop, replacement seek, and stale-session cleanup;
- embedded SRT/ASS plus same-stem sidecar SRT/ASS;
- subtitle Off/on and track replacement at the current source position;
- subtitle timing from zero and after nonzero seek;
- per-eye subtitle duplication in half/full SBS and OU;
- subtitle rendering in Dubois anaglyph and passive rows;
- embedded MKV PGS selection and burn-in from zero and after nonzero seek;
- Blu-ray ISO PGS selection, startup, seek replacement, and track switching;
- sidecar SUP selection and burn-in;
- PGS duplication in SBS/OU, neutral-depth Dubois output, and passive-row output;
- full-length and multi-segment ISO PGS timing, including captions crossing clip boundaries.

Preserve the diagnostic report before starting another title.
