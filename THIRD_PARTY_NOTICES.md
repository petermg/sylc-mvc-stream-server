# Third-Party Notices

## edge264

The native MVC decoding engine includes edge264 source under the BSD 3-Clause License. The complete license text is included at:

```text
engine/phase6-streaming/src/edge264/LICENSE_BSD.txt
```

## libudfread 1.2.0

This package includes VideoLAN libudfread 1.2.0 source for direct read-only UDF access to Blu-ray ISO images. libudfread is distributed under the GNU Lesser General Public License, version 2.1 or later. The included source and license are located at:

```text
engine/phase6-streaming/src/videolan/libudfread-1.2.0/
engine/phase6-streaming/src/videolan/libudfread-1.2.0/COPYING
```

The installer builds libudfread as part of the native `sylc_iso_source` executable.

## libbluray

The ISO source adapter dynamically loads the target server's system-installed `libbluray.so.2` for Blu-ray title/playlist metadata when it is available. This package does not redistribute a libbluray binary or its source. libbluray is an LGPL-licensed VideoLAN project. When it is unavailable, SyLC uses its bundled physical MPLS fallback selector.

## FFmpeg / ffprobe

The service invokes the host system's `ffmpeg` and `ffprobe` executables for Matroska demuxing, audio decoding/encoding, the `stereo3d` Color/Dubois filter, VA-API or software H.264 encoding, HLS muxing, and media inspection. This package does not redistribute FFmpeg binaries or source.

## CMake and Ninja

CMake and Ninja are build tools used on the target server and are not redistributed in this package.

## Proprietary-code boundary

No proprietary 4XVR implementation code, binary, resource, or decompiled source is included. The ISO implementation is based on the open-source SyLC Android physical-stream work and the included/open-source dependencies documented in `docs/SOURCE_PROVENANCE.md`.
