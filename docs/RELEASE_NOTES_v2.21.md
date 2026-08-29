# Quake II PS3 v2.21

This private preservation release makes the known-good v2.21 physical-console
baseline available as both a full HDD-install PKG and a standalone PS3 ISO.

## Included

- Native RSX renderer and the accepted v2.21 compatibility object set.
- Single-player, save/load, DualShock 3 remapping, sound, network play, native
  on-screen keyboard address entry, optional Move, optional SPU work, filters,
  and stereoscopic 3D.
- `ICON0.PNG`, `PIC0.PNG`, and `PIC1.PNG` from the project `ICONS` folder.
- Supplied Quake II `baseq2` game data in both private binary containers.

## Validation

- Full PKG uses the exact tested v2.21 NPDRM EBOOT.
- ISO retains 136 of 138 v2.21 objects byte-for-byte and changes only disc/HDD
  filesystem selection.
- Both containers were extracted and compared against their staging roots.

## Known limitations

- UI scale above 1x can produce doubled or smeared text.
- Frame-packed 3D full-screen color blends can affect one eye incorrectly.
- The post-v2.21 native USB mouse/keyboard bridge is not included.
- The newly assembled full PKG and ISO still need final physical-console smoke
  tests as containers, even though the PKG EBOOT itself is already accepted.

The release assets contain commercial game data and must remain private.
