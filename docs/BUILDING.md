# PS3 build notes

The PS3 build uses PSL1GHT and the PS3DEV PPU/SPU toolchains. Build serially on
the shared workstation (`-j1`) so this project does not compete with other PS3
build or test workloads.

```sh
export PS3DEV=/path/to/ps3dev
export PSL1GHT="$PS3DEV"
export PORTLIBS="$PS3DEV/portlibs/ppu"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH"

make -f Makefile.ps3 \
  BUILD=build-rsx \
  TARGET=q2ps3 \
  PS3_NATIVE_RSX=1 \
  PS3_CAMERA=0 \
  PS3_V198_OSK_RECOVERY=1 \
  PS3_FORCE_FONT_SAMPLER_PER_DRAW=1 \
  PS3_USB_INPUT_BRIDGE=0 \
  PS3_POLYBLEND_2D_OVERRIDE=0 \
  PS3_SCENE_SCALE_BRIDGE=0 \
  PS3_SAFE_VIDEO=0 \
  -j1
```

For a disc-oriented SELF, add `PS3_DISC_BUILD=1`. That compile-time switch
uses `/dev_bdvd/PS3_GAME` (with `/app_home` fallback) as the read-only data root
and `/dev_hdd0/tmp/q2ps3-v221` as the writable home root.

The packaged v2.21 HDD executable is a frozen accepted binary. Rebuilding the
current development snapshot is not claimed to reproduce that EBOOT bit for
bit; use the hashes in [Release provenance](RELEASE_PROVENANCE.md) when auditing
or preserving the accepted release.

Physical PS3 validation is authoritative for this port. Do not run RPCS3 as
part of the shared-machine workflow unless the project owner explicitly changes
that policy.
