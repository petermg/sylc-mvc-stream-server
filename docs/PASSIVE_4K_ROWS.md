# Passive 4K row-interleaved output

SyLC MVC Stream Server 0.7.0-alpha.2 adds server-encoded output for 4K passive-polarized 3D displays.

This is **spatial row interleaving**, not temporally interlaced video. The server creates a normal progressive 3840×2160 H.264 stream whose physical rows alternate between the two eyes:

```text
row 0: left eye
row 1: right eye
row 2: left eye
row 3: right eye
...
```

The alternate mode reverses the eye assignment.

## Output modes

- `passive-rows-left-top` — left eye on the top/even row
- `passive-rows-right-top` — right eye on the top/even row

Use left-top first. Select right-top, or use eye swapping, only when depth is reversed or the display uses the opposite row parity.

## Required display setup

For correct eye separation, every encoded row must reach the corresponding physical panel row unchanged:

1. Set the playback device to exact 3840×2160 output.
2. Use the display's pixel-exact option: **Just Scan**, **Screen Fit**, **1:1**, **Full Pixel**, or equivalent.
3. Disable overscan, zoom, aspect stretching, and vertical resizing.
4. Leave the display's SBS/OU 3D conversion off. The stream is already row-interleaved.
5. **Disable motion smoothing, frame interpolation, TruMotion, MotionFlow, Auto Motion Plus, or equivalent processing.** Interpolated frames disturb alternating-eye rows and can create motion artifacts or regions with incorrect eye separation.
6. Use ordinary progressive playback. Do not add another client-side row-interleaving shader.

## Confirmed result

The left-top mode was visually confirmed on a 4K passive display using SyLC Stream Player 0.1.0-alpha.9. The image became correct after television motion smoothing was disabled.

A benchmark on an Intel UHD 630 server encoded a 3840×2160 left-top sample at approximately 1.29× real time, with matched MVC pairs and no pairing or decoder errors. The sample averaged approximately 61.7 Mb/s at QP 22, so network and client decoder capability should be tested before relying on Wi-Fi for a complete movie.

## Troubleshooting

### Image looks 2D or both eyes appear mixed

- Confirm the playback device is outputting 3840×2160 rather than a 1080p application surface that is later upscaled.
- Confirm no player, AVR, or television stage is vertically resizing the image.
- Confirm the TV's SBS/OU mode is off.

### Motion produces blocks, tearing, halos, or different behavior in parts of the screen

Disable all motion interpolation and picture-processing features first. Passive row interleaving depends on stable one-to-one physical row mapping; synthesized frames can mix or shift the eye rows.

### Depth is reversed

Switch between left-top and right-top, or use eye swapping.

### Playback buffers or stutters

The 4K row mode can use substantially more bandwidth than Full SBS. Test wired Ethernet or strong 5 GHz/6 GHz Wi-Fi, and inspect HLS segment sizes before changing encoder quality.

### Desktop preview shows thick horizontal bands

A row-interleaved 2160p file cannot be judged reliably in a scaled window or screenshot. Downscaling merges one-pixel eye rows into visible bands or moiré. Evaluate it full-screen at exact 3840×2160 on the intended passive display.
