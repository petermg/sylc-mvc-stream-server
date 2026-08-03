# Architecture

SyLC MVC Stream Server is divided into four layers.

## 1. Control plane

`app/server.py` provides a dependency-free Python HTTP service for:

- first-run setup;
- optional API-token authentication;
- Media Libraries configuration;
- catalog scanning;
- title/audio probing;
- session creation, replacement seeking, stopping, and cleanup;
- HLS and diagnostic-report delivery.

Runtime settings are stored atomically in `config.json`. Source files are never modified.

## 2. Source adapters

### MVC MKV/MK3D

FFmpeg reads the Matroska container and supplies the H.264 MVC Annex-B stream and selected audio.

### Unencrypted Blu-ray 3D ISO

`sylc_iso_source` reads the ISO directly through bundled libudfread. It parses physical Blu-ray structures, selects the feature playlist, reads SSIF/base M2TS streams, exposes audio tracks, and plans safe seek anchors.

System libbluray is loaded dynamically when present for authoritative title metadata. A physical MPLS fallback remains available.

## 3. MVC recovery and composition

`sylc_hsbs_pipe` uses edge264 to decode base and dependent MVC views, validates POC pairing, and produces stereo pairs.

For nonzero ISO starts, the source path can:

1. seek to a preceding CLPI/SSIF anchor;
2. replay persistent SPS/PPS/subset-SPS bootstrap data;
3. measure dependent-minus-base timestamp offset;
4. correct a stable 0, +1, or -1 frame source phase;
5. hide structural and stabilization pairs;
6. release only a clean visible pair;
7. align audio to the clean release timestamp.

The compositor produces full-SBS raw video internally, then FFmpeg applies the selected output geometry or anaglyph filter.

## 4. Encode and delivery

FFmpeg performs:

- Intel VA-API or software x264 H.264 encoding;
- DTS/DTS-HD, AC-3/E-AC-3, or TrueHD decode;
- deterministic multichannel rematrix to AC-3-compatible 5.1(side);
- HLS segmentation.

The active HLS playlist grows while the pipeline runs. Replaced session HLS remains temporarily readable until cleanup.


### Passive 4K rows

For passive-row modes, the MVC compositor emits a full-SBS 3840×1080 intermediate. FFmpeg `stereo3d` converts this to single-row eye interleaving, then nearest-neighbor horizontal expansion creates an exact 3840×2160 progressive frame before VA-API or software H.264 encoding.
