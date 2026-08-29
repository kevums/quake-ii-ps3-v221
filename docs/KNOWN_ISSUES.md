# Known issues in the frozen v2.21 baseline

## Scaled text

The native RSX font atlas is reliable at 1x but can duplicate or smear glyphs
when the combined UI scale selects 2x or 3x output. Recommended values:

```text
r_consolescale 1
r_menuscale 1
r_hudscale 1
```

This is presentation corruption, not duplicated console output. v2.20 and
v2.21 sampler experiments did not eliminate it, so the issue remains documented
instead of risking the known-good renderer with another broad UI rewrite.

## Stereo full-screen tints

In 720p frame-packed 3D, underwater, damage, and power-up color blends can
saturate one eye while the other eye remains correct. The problem is in the
stereo full-screen overlay path rather than the underlying world rendering.
The later attempted fix is not included because this release intentionally
freezes the accepted v2.21 compatibility baseline.

## USB mouse and keyboard

The native PS3 USB bridge first appeared after v2.21, so it is not present in
this exact release. DualShock 3, Navigation controller input, optional Move,
and native PS3 on-screen keyboard text entry are present.

## Performance

The native RSX renderer is considerably faster than the original software
renderer, but 720p can still dip below the selected 30 FPS cap in expensive
scenes. Start with the 720p Performance profile, mono, no display filter, and
linear targets. Stereo renders two eyes and therefore costs substantially more.

## Diagnostic messages

- `point parameters: failed` means the optional RSX point-sprite parameter
  state was unavailable. Normal Quake II geometry does not depend on that path.
- `paletted textures: disabled` means indexed textures are expanded before RSX
  upload instead of sampled through a hardware palette. It is informational,
  not a missing-texture error.
