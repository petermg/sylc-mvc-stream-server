# SyLC MVC Stream Server

**Current public alpha:** `0.7.0-alpha.3`

SyLC MVC Stream Server converts user-supplied MVC 3D video into ordinary H.264 + AC-3 HLS that can be played by inexpensive Android TV and Fire TV devices, VLC, PotPlayer, and other compatible clients.

It supports:

- MakeMKV MVC `.mkv` and `.mk3d` files marked `stereo_mode=block_lr`
- Unencrypted Blu-ray 3D `.iso` images containing AVC + MVC video
- Half/full side-by-side
- Half/full over-under
- Left-eye and right-eye output
- Color and Dubois red/cyan anaglyph
- Server-encoded 3840×2160 passive row interleaving with selectable top-row parity
- Eye swapping
- Server-side seeking with MVC recovery
- DTS, DTS-HD, AC-3, E-AC-3, and Dolby TrueHD source audio
- TrueHD/DTS-HD decoding and deterministic AC-3 5.1 output
- Intel VA-API H.264 encoding or software x264 fallback
- Multiple read-only media libraries configured from the web UI
- 3D-aware server-side burn-in for embedded MKV/MK3D text subtitles and PGS bitmap subtitles
- 3D-aware server-side burn-in for matching `.srt`, `.ass`, `.ssa`, `.vtt`, and `.sup` sidecars
- Blu-ray ISO PGS cataloging, selection, native M2TS/PES extraction, timeline restoration, and burn-in
- Zero-time transparent PGS anchoring so delayed first captions retain authored startup timing
- Subtitle selection in the web UI and API, preserved across seeks and session replacements

## Important boundaries

SyLC does **not** decrypt AACS or BD+, play Blu-ray menus/BD-J, or handle UHD/HEVC Blu-ray. It is intended for media the user is legally permitted to access and for unencrypted disc images.

The server is intended for a **trusted LAN or VPN**. Do not expose port `8097` directly to the public Internet.

The optional API token protects media catalog, settings, and playback-control endpoints. HLS session URLs remain bearer-by-URL for broad player compatibility.

## Supported platform

The public alpha is developed and tested primarily on Ubuntu Server 24.04 LTS with Intel VA-API. Other modern Debian-family distributions may work but are not yet extensively tested.

Recommended packages:

```bash
sudo apt update
sudo apt install -y \
  ffmpeg cmake ninja-build build-essential curl libbluray2 unzip
```

The installed FFmpeg build must include the `subtitles`/libass filter for text-subtitle burn-in and the standard PGS decoder/`overlay` filter for bitmap subtitle burn-in.

A readable VA-API render device such as `/dev/dri/renderD128` is recommended. Software H.264 encoding is available when VA-API is unavailable.

## Install

Extract the source release, enter the folder, and run:

```bash
chmod +x scripts/install.sh scripts/uninstall.sh engine/run-phase6-streaming-session.sh
sudo ./scripts/install.sh
```

The installer:

- preserves an existing SyLC service user when upgrading;
- otherwise uses the user who invoked `sudo`, or creates a dedicated `sylc` user when necessary;
- builds the native MVC and Blu-ray ISO components;
- runs native self-tests;
- installs the systemd service;
- does not modify Jellyfin or any Docker stack.

Useful installer options:

```text
--service-user USER
--bind-host ADDRESS
--port PORT
--vaapi-device PATH
--software-encoder
```

After installation, open:

```text
http://SERVER-IP:8097
```

A fresh installation displays a first-run wizard. Select at least one server folder containing `.mkv`, `.mk3d`, or `.iso` files. An API token is optional.

## Media Libraries

Media Libraries are configured under **Server Settings → Media Libraries**.

Each library has:

- a display name;
- an absolute server path;
- recursive or top-folder-only scanning;
- enabled/disabled state;
- read-access validation;
- indexed-file count;
- rescan, edit, and remove controls.

Removing a library from SyLC never deletes or changes source files.

NAS shares should be mounted by the operating system first, for example at `/mnt/nas/3d-movies`, and then selected as a SyLC media library. The first public alpha does not manage SMB/NFS credentials or mounts.

Runtime settings are stored in:

```text
/var/lib/sylc-mvc-stream/config.json
```

Machine-level settings remain in:

```text
/etc/sylc-mvc-stream.env
```

## Service management

```bash
sudo systemctl status sylc-mvc-stream --no-pager --full
sudo journalctl -u sylc-mvc-stream -f
curl -fsS http://127.0.0.1:8097/api/health | python3 -m json.tool
```

Uninstall the service while retaining application data and reports:

```bash
sudo ./scripts/uninstall.sh
```

## Architecture

```text
MVC MKV or unencrypted Blu-ray 3D ISO
  → Matroska or UDF/MPLS/CLPI/SSIF reader
  → base/dependent MVC access-unit synchronization
  → edge264 MVC decode
  → seek recovery and hidden stabilization pairs
  → stereo/anaglyph composition
  → 3D-aware per-eye text or PGS/SUP subtitle burn-in when selected
  → FFmpeg VA-API or x264 H.264 encode
  → source audio decode/rematrix to AC-3
  → live HLS session
```

See `docs/ARCHITECTURE.md`, `docs/API.md`, and `docs/SOURCE_PROVENANCE.md` for details.

## Known alpha limitations

- One active conversion session at a time
- Linux/systemd installer only
- No encrypted-disc decryption
- No menus, BD-J, or chapters
- No built-in SMB/NFS mount manager
- HLS URLs are not token-header protected
- Hardware support beyond the tested Intel VA-API environment needs broader community validation

## Licensing

The project is GPL-3.0. Included edge264 source is BSD-3-Clause. Bundled VideoLAN libudfread is LGPL-2.1-or-later. System FFmpeg and libbluray are not redistributed.

No proprietary 4XVR code, binary, resource, or decompiled implementation is included.


## Passive 4K row-interleaved output

The `passive-rows-left-top` and `passive-rows-right-top` modes create a normal progressive 3840×2160 H.264 stream whose physical rows alternate between the two MVC eyes. Use exact 4K output, disable overscan and TV-side SBS/OU conversion, and use the parity that matches the display. This mode uses substantially more network bandwidth than Full-SBS.


## Subtitles

Subtitles are burned into the server-generated video so they remain correct in SBS,
over-under, mono, anaglyph, and passive-row output. For stereo layouts the same caption is
rendered into both eyes at the same authored position, placing it at neutral screen depth.
The Android player therefore does not draw a separate 2D subtitle overlay.

Supported selectable subtitle sources:

- embedded MKV/MK3D SubRip/SRT, ASS, SSA, and WebVTT text tracks;
- embedded MKV/MK3D Blu-ray PGS bitmap tracks;
- Blu-ray ISO PGS tracks declared by the selected feature playlist;
- sidecar `.srt`, `.ass`, `.ssa`, `.vtt`, and `.sup` files beside an MKV, MK3D, or ISO;
- `Off`.

Sidecars must use the movie's complete filename stem, for example:

```text
Movie 3D.mkv
Movie 3D.eng.srt
Movie 3D.eng.forced.srt
Movie 3D.commentary.ass
Movie 3D.eng.sup
```

For ISO PGS, the native source adapter reads the selected presentation-graphics PID from
the base M2TS stream, reconstructs PES and PGS display sets, and supplies a timestamped SUP
stream to FFmpeg. A bounded subtitle preroll is replayed at timestamp zero after a nonzero
seek so cached palette, object, and window definitions are available before the active
composition. Source media and sidecars remain read-only. Changing subtitles creates a
replacement HLS session at the current source timestamp, just like changing mode or audio.

Current subtitle limitations:

- forced-display flags inside one PGS stream are not yet exposed as a separate “forced only” choice;
- authored Blu-ray 3D subtitle depth/offset metadata is not applied; captions are placed at neutral screen depth;
- subtitle size, vertical position, depth, and delay controls are not yet available.
