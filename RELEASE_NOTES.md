# SyLC MVC Stream Server 0.7.0-alpha.2

This alpha adds server-encoded passive-display row interleaving while preserving all 0.7.0-alpha.1 media-library, authentication, MVC MKV, Blu-ray 3D ISO, seeking, audio, and HLS behavior.

## New output modes

- `passive-rows-left-top` — 3840×2160 progressive H.264; left eye on the top/even physical row.
- `passive-rows-right-top` — 3840×2160 progressive H.264; right eye on the top/even physical row.

The server decodes a full-resolution MVC stereo pair, produces a 3840×1080 Full-SBS intermediate, converts it to one-pixel alternating eye rows, and expands horizontally to 3840×2160 with nearest-neighbor scaling. The output remains a normal progressive HLS stream; the client must not apply another 3D conversion.

## Confirmed benchmark

On the project's Intel UHD 630 test server, the left-top mode encoded a validated sample at approximately 1.29× real time with no pairing or decoder errors. The actual 4K passive display visually confirmed the left-top sample as correct.

## Important requirements

- Exact 3840×2160 display output.
- Pixel-exact/Just Scan/1:1 display mapping with overscan disabled.
- TV-side SBS/OU conversion disabled.
- Higher bandwidth than Full-SBS; the first benchmark averaged about 61.7 Mb/s at QP 22.

This remains an alpha feature and should be tested on the intended player, network, and display before relying on it for a full movie.

## Confirmed display result

The complete server 0.7.0-alpha.2 and Android player 0.1.0-alpha.9 path was tested on a 4K passive-polarized display. `passive-rows-left-top` produced correct smooth 3D after the television's motion smoothing/frame interpolation was disabled. Motion smoothing is therefore documented as incompatible with reliable passive-row mapping.
