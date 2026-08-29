# v2.21 release provenance

## Accepted HDD executable

The full PKG contains the exact NPDRM EBOOT from the physically accepted v2.21
update stage:

```text
EBOOT.BIN size:   881,520 bytes
EBOOT.BIN SHA256: 11D761D899A14F7111C9F67105D781C7D9994B963BC086506BE13DFD82386DDA
```

The v2.21 ELF retained in the working directory is:

```text
q2ps3.elf size:   2,502,512 bytes
q2ps3.elf SHA256: EBA9F16777438698F5B6E82DD3A35D36877222A665BB2107FE124A158BE49A1E
```

## ISO executable

The v2.21 ISO object directory contains 138 PPU objects. A recursive SHA-256
comparison against the accepted v2.21 object directory found exactly two
differences:

```text
main.o
system.o
```

Those two objects only select the disc data root and a writable HDD home/log
root. The remaining 136 objects—including RSX, GL1, input, Move, audio,
networking, OSK, game, and server code—match the v2.21 build byte-for-byte.

```text
Disc q2ps3.elf SHA256: F32EB0D3F0908E0DCBB30A7C9FC1D9BA32E75FDC0816EEDCDCAB1BC26478B91C
Disc EBOOT.BIN SHA256: E05C13C269A18953D339CB419288ABF9DF11FA0FB02263AE3F579FCD72203B31
```

## Container verification

- The completed ISO was extracted with `extractps3iso`; all 11 extracted files
  matched the staging hashes.
- The raw full PKG was listed and extracted with PSL1GHT `pkg`; all 10 files
  matched the staging hashes.
- The finalized package header changed from type `0x00000001` to
  `0x80000001`, as expected after `package_finalize`.
- The package SFO uses category `HG`; the ISO SFO uses category `DG`. Both use
  title ID `QUAKE2000` and version/app version `02.21`.

## Release-container hashes

```text
090FDA26DA9D721B48173A91EB124ACADCE203A1B9D650DA09EAA57C9315E121  Quake-II-PS3-v2.21.iso
03E47A91FCC2754621FBDC06294039CE3AAEDAF2C07564BBC3E92EEAFE5E14A2  Quake-II-PS3-v2.21-full.gnpdrm.pkg
```
