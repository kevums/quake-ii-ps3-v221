# Installation

## Before installing

- Use a PS3 environment capable of installing homebrew PKGs or mounting
  Cobra/webMAN-style PS3 ISO images.
- Back up `config.cfg`, saves, and multiplayer address-book settings if an older
  `QUAKE2000` installation matters to you.
- Start with a DualShock 3 and no camera/Move initialization requirement. Move
  is enabled and calibrated from the in-game menu after the title reaches the
  main menu.

## Full PKG

1. Copy `Quake-II-PS3-v2.21-full.gnpdrm.pkg` to FAT32/NTFS/USB storage or make
   it available through the console's normal package-install workflow.
2. Install it with Package Manager.
3. Launch **Quake II PS3 v2.21** from the XMB.

The package installs under title ID `QUAKE2000` and contains the tested v2.21
NPDRM executable, artwork, configuration seed, and supplied `baseq2` data.

## ISO

1. Copy `Quake-II-PS3-v2.21.iso` to the console's usual `PS3ISO` location.
2. Refresh the webMAN/manager game list.
3. Mount the image and launch the mounted disc entry.

The ISO reads the engine and game data from `/dev_bdvd/PS3_GAME`. Writable
configuration, saves, screenshots, and logs use:

```text
/dev_hdd0/tmp/q2ps3-v221/USRDIR
```

The ISO has been structurally verified by extracting it and comparing all 11
files to the build stage. It still requires a physical-console boot/gameplay
smoke test before it should replace the tested PKG as the primary copy.

## First-run settings

- Start with 720p Performance, mono output, linear framebuffer, and no display
  filter. Add stereo, filters, or higher resolution one at a time.
- Keep UI scale at `1x` to avoid v2.21's known scaled-text corruption.
- Enable PlayStation Move only after reaching the menu, then run its calibration
  action while facing the sphere toward the PlayStation Eye.
- Network servers use normal Quake II IPv4 UDP, normally port `27910`.

## Integrity check on Windows

```powershell
Get-FileHash -Algorithm SHA256 .\release-assets\Quake-II-PS3-v2.21.iso
Get-FileHash -Algorithm SHA256 .\release-assets\Quake-II-PS3-v2.21-full.gnpdrm.pkg
```

Compare the results with `release-assets/SHA256SUMS.txt` or the table on the
front page.
