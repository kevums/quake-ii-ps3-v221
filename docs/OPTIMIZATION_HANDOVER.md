# Quake II PS3 Optimization Handover

Last audited: 2026-08-27  
Workspace on the originating computer: `F:\DEV\QuakeIIPS3`  
Project directory: `q2ps3/`  
PS3 title/application ID: `QUAKE2000`  
Content ID: `UP0001-QUAKE2000_00-0000000000000000`

## Read this first

This working tree, not Git history, is the authoritative project. It is heavily
modified and contains important untracked source, including the entire native
RSX renderer under `q2ps3/src/client/refresh/rsx/`. A fresh clone, `git clean`,
or transferring only committed files will destroy essential work.

Transfer the complete `QuakeIIPS3` directory to the other computer. Keep the
`.git` directory, every modified/untracked source file, versioned `param-*.xml`
files, update packages, and at least the important build directories listed
below. The safest method is a byte-for-byte copy or archive of the entire
directory. Do not normalize, reset, or clean the tree during handoff.

The project must be tested on a physical PS3. Do not use RPCS3. Another project
(KZ3) has priority over emulator and machine resources, and the owner explicitly
does not want RPCS3 used for this port. Build serially with `-j1` and avoid
running heavyweight projects at the same time.

The active target is a generally sustained minimum of 30 FPS, not a special
case tuned only for one favorite graphics configuration. The last physically
reported result was about 20 FPS in 720p Performance with a 30 FPS cap. v2.27
behaved identically to the preceding scaler build. v2.28 is built but has not
yet been tested on hardware.

## Status at handoff

| Item | Status |
| --- | --- |
| Gameplay | Working on physical PS3 |
| Native RSX renderer | Working and visually accepted before the latest optimization experiment |
| 720p performance | Improved substantially over the original software renderer, but still dips to/about 20 FPS in the reported case |
| 1080p | Supported; considerably more expensive |
| Frame-packed and top/bottom 3D | Working; v2.23 fixed the per-eye underwater/quad/damage color blend |
| Online play | Working |
| PS3 OSK and persistent address book | Working as of v2.19 |
| USB keyboard/mouse | Implemented as of v2.22; optional and hot-pluggable |
| DualShock 3 | Working, including in-game remapping work |
| PS Move | Implemented but not a completion priority; the fixed weapon/reticle design limits its value without a VR-style rework |
| Sound/Ogg | Working |
| Shadows/lighting/filters | Working in the accepted renderer baseline |
| Console/menu text at UI scale >1 | Still visually doubled/smeared/overlapping; accepted known defect |
| v2.28 tiled scene resolve | Compiled, linked, packaged, symbol-audited; **not physically tested** |

Do not reopen the UI text problem as part of general performance work unless it
is explicitly scoped. Numerous text/projection experiments produced upside-down,
backwards, clipped, or enormous menus and several boot-to-XMB/black-screen
regressions. v2.19 was deliberately selected as the behavioral baseline after
those experiments. The owner accepts the current text defect in exchange for
preserving the otherwise working port.

## Baselines and version history that matter

### v2.19: accepted behavioral baseline

v2.19 restored the physically proven v1.98 renderer/frontend/video/input/sound/
network/gameplay object set and retained only the multiplayer address-book fix.
The native OSK writes accepted server text into `adr#`, archives it, writes the
configuration immediately, and commits all fields when leaving the address
book. Online play was physically confirmed.

The on-disk artifact is:

`q2ps3/q2ps3-rsx-v219-update.gnpdrm.pkg`

There is no v2.19 full ISO in this workspace. Treat v2.19 as a behavioral
baseline, not as an ISO artifact. Older full-content packages exist (through
v1.50), but normal development must create update-only packages so installed
commercial PAK data is preserved.

### v2.20-v2.21: UI sampler tests

These attempted to correct scaled `conchars` sampling. Physical output remained
identical. Do not assume these solved the text issue.

### v2.22-v2.23: input and stereo blend

v2.22 added optional native USB keyboard/mouse input and a native 2D per-eye
polyblend path. v2.23 fixed the actual PPC call site so water, damage, and power
up tints are alpha-blended into each active stereo eye instead of saturating one
eye. Preserve the v2.23 `R_PolyBlend` relocation when relinking.

### v2.24: low-risk RSX state optimization

Restored only the independently validated physical viewport/scissor cache.
The problematic historical GL1 camera-matrix object was not restored.

### v2.25-v2.27: private scene-resolution bridge

The bridge wraps the accepted renderer exports. It renders only the 3D scene to
a lower-resolution private linear color/depth allocation, resolves it to the
display eye, then renders HUD, menu, console, OSK, text, and filters at native
output resolution.

- Custom starts at native scale.
- 720p Performance starts at 0.75 (normally 960x540 scene into 1280x720).
- 1080p Quality starts at 2/3 (roughly 1280x720 scene into 1920x1080).
- Adaptive mode can step down through 0.875, 0.75, 0.667, 0.583, and 0.5 after
  repeated gameplay cadence above 40 ms.
- An explicit nonzero `ps3_rsx_scene_scale` disables adaptive selection.
- v2.27 removed a redundant pre-resolve RSX idle wait. Physical behavior was
  reported identical, so do not infer that the removed wait was the bottleneck.

Before v2.28, tiled/tiled+Z-cull display targets fell back to full-resolution
scene rendering because the scale-engine destination could not be tiled.

### v2.28: current experimental candidate

v2.28 keeps the private linear scene surface but supports effective tiled and
tiled+Z-cull display targets by sampling the scene into the restored tiled eye
with one ordinary fixed-shader textured quad. Linear output retains the v2.27
scale-engine resolve.

Only these object files differ between `build-rsx-v227/` and
`build-rsx-v228/`:

| Object | v2.27 SHA-256 | v2.28 SHA-256 |
| --- | --- | --- |
| `rsx_gl.o` | `7B29E2B8A812BB2E3148F961304209B0F6AFFD3905884808A85129BDD490CC46` | `5BA8C8C2DCC6B233AA89C6A07329FBAB90F62541C8975282DCE9EE585444546F` |
| `rsx_scene_scale_ps3.o` | `8CFD907CB01C1BE8E06799266DD4BB989C8960BB7DB09AFF1D81530B36400A60` | `65B78316CE838D3F35A536D789BD0C18FEBAE0EC2602744D30D90D6AF591CF4F` |

Everything else in those two build directories is byte-identical. v2.28 was
compiled and linked, and its bridge symbols were audited, but it is not a
proven release until it passes physical testing.

## Important files and what they do

All paths below are relative to `q2ps3/`.

### Build, packaging, and project history

| Path | Purpose |
| --- | --- |
| `Makefile.ps3` | PS3 source selection, feature switches, shaders, link libraries, and the scene-bridge symbol rewrite rules |
| `CHANGELOG` | Detailed version-by-version experiment and physical-test history; read before reviving an old idea |
| `README_PS3.md` | User-facing setup, cvars, video profiles, 3D, controls, networking, telemetry, and build notes |
| `param-v228.xml` | Current 02.28 package metadata; copy to the next version and update the version field |
| `build-rsx-v219/` | Accepted behavioral baseline object set |
| `build-rsx-v223/` | Correct stereo polyblend build |
| `build-rsx-v224/` | Accepted viewport/scissor-cache build |
| `build-rsx-v227/` | Last physically tested scene-scaler candidate |
| `build-rsx-v228/` | Current compiled experimental candidate |
| `build-rsx-v228-update-stage/` | Exact three-entry update staging tree |
| `q2ps3-rsx-v228-update.gnpdrm.pkg` | Finalized deployable v2.28 update package |
| `q2ps3-rsx-v228-update.pkg` | Raw/unfinalized package; do not deploy this file |

The many other versioned build directories are useful object-level history.
They are not clean release branches. `CHANGELOG` records why each existed.

### Renderer architecture

The native renderer intentionally reuses the mature Quake II GL1 frontend for
asset loading, BSP traversal, visibility, lightmaps, entities, and draw-order
semantics. It does not use desktop OpenGL at runtime. The RSX backend consumes
the GL-like calls and emits native RSX command buffers.

| Path | Responsibility |
| --- | --- |
| `src/client/refresh/gl1/gl1_main.c` | GL1 frontend entry points, scene setup, stereo-eye rendering, refexport API |
| `src/client/refresh/gl1/gl1_surf.c` | BSP/world surface traversal and chains |
| `src/client/refresh/gl1/gl1_light.c` | Dynamic-light marking and light calculations |
| `src/client/refresh/gl1/gl1_lightmap.c` | Static/dynamic lightmap construction and updates |
| `src/client/refresh/gl1/gl1_mesh.c` / `gl1_md2.c` | Alias/MD2 entity rendering |
| `src/client/refresh/gl1/gl1_draw.c` | 2D pictures, text-facing GL1 draw calls, fills |
| `src/client/refresh/gl1/gl1_polyblend_ps3.c` | Stereo-safe per-eye water/damage/power-up full-screen blend |
| `src/client/refresh/rsx/rsx_backend.c` | Binds the GL1 frontend to the native RSX implementation |
| `src/client/refresh/rsx/rsx_gl.c` | RSX allocations, command emission, shaders, state caches, batches, tiled/Z-cull targets, stats, transfers, flips |
| `src/client/refresh/rsx/header/rsx_gl.h` | Native RSX bridge API |
| `src/client/refresh/rsx/rsx_scene_scale_ps3.c` | `refexport_t` wrapper for lower-resolution 3D plus native-resolution UI; adaptive scale and resolve policy |
| `src/client/refresh/rsx/shaders/rsx_fixed.vcg/.fcg` | Generic fixed-function-style textured/color path |
| `src/client/refresh/rsx/shaders/rsx_lightmap.vcg/.fcg` | Combined base-texture/lightmap path |
| `src/client/refresh/rsx/shaders/rsx_filter.fcg` | Output filter fragment program |
| `src/client/vid/glimp_ps3.c` | Video modes, scanout, frame-packed 3D setup, flips, presentation pacing |
| `src/client/vid/vid_ps3.c` | PS3 video integration with the client |
| `src/common/frame_ps3.c` | Frame-cap scheduling and target timing |
| `src/client/menu/videomenu.c` | Video profile and graphics-option UI |

The native RSX backend already contains substantial optimization work:

- Persistent static BSP geometry.
- Indexed and triangle batching.
- Combined base texture plus lightmap in one pass.
- State/raster caches and resolved viewport/scissor suppression.
- Prepared alias geometry.
- Second-eye particle expansion reuse.
- Second-eye dynamic-light marking and lightmap reuse where safe.
- Dynamic texture renaming to reduce synchronized uploads.
- Tiled color/depth targets and optional Z-cull.
- Per-eye filters and frame-packed rendering.
- Opt-in RSX and PPU-stage telemetry.

Check for an existing mechanism before adding another cache or upload path.

### Input, OSK, networking, and persistence

| Path | Responsibility |
| --- | --- |
| `src/client/input/input_ps3.c` | DualShock 3 input and shared PS3 input entry points |
| `src/client/input/input_ps3_usb.c` | Optional hot-pluggable USB keyboard and five-button/wheel mouse; suppressed while OSK owns input |
| `src/client/input/input_psmove_ps3.c` | Move/navigation implementation |
| `src/client/input/input_psmove_ps3_stub.c` | Link-time no-Move fallback |
| `src/client/input/header/psmove_ps3.h` | Move bridge declarations |
| `src/backends/ps3/system.c` | GameOS/system integration, asynchronous PS3 OSK lifecycle, boot trace, sysutil callbacks |
| `src/backends/ps3/network_ps3.c` | Native IPv4 UDP networking |
| `src/client/cl_network.c` | Client connection, server discovery/query, address book pings |
| `src/client/menu/qmenu.c` | Editable-field behavior and PS3 OSK field routing |
| `src/client/menu/menu.c` | Join-server and address-book menus, `adr0`-`adr8` commit/persistence |

Address-book persistence is specifically implemented in
`AddressBook_CommitField()`, `AddressBook_CommitAll()`, and the address-book menu
key handler near the end of `src/client/menu/menu.c`. Do not replace this with
the old behavior that only copied a temporary field buffer.

### Audio

| Path | Responsibility |
| --- | --- |
| `src/client/sound/sound_backend_ps3.c` | PS3 audio backend/output lifecycle |
| `src/client/sound/sound_ps3.c` | PS3 sound integration/mixing support |
| `src/client/sound/ogg.c` | Ogg track playback and track-range handling |
| `src/client/sound/wave.c` | WAV loading |

### Software/SPU fallback

`src/client/refresh/soft/` and `src/client/refresh/soft/spu/` contain the older
software renderer and optional SPU support. The current performance path is the
native RSX renderer (`PS3_NATIVE_RSX=1`). Do not confuse the SPU/software path
with the current RSX optimization target.

## Build environment

The originating computer used WSL plus this external prebuilt toolchain:

```text
PS3DEV=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt
PSL1GHT=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt
PORTLIBS=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/portlibs/ppu
PATH=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/bin:/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/ppu/bin:/usr/local/bin:/usr/bin:/bin
```

That toolchain is outside this workspace. Copy it separately or install a
compatible ps3dev/PSL1GHT toolchain with `libgem`, `libspurs`, `libnet`, RSX,
audio, IO, and sysutil libraries. Adjust `/mnt/f/...` if the new computer uses
a different drive/mount.

Set `PATH` exactly in the WSL shell. Do not append the Windows-expanded
PowerShell `$PATH`; it creates invalid Bash paths and can select the wrong
tools.

### Reproduce the v2.28 configuration

From Windows PowerShell:

```powershell
wsl.exe bash -lc 'export PS3DEV=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt; export PSL1GHT=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt; export PORTLIBS=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/portlibs/ppu; export PATH=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/bin:/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/ppu/bin:/usr/local/bin:/usr/bin:/bin; cd /mnt/f/DEV/QuakeIIPS3/q2ps3; make -f Makefile.ps3 BUILD=build-rsx-v228 TARGET=q2ps3 PS3_NATIVE_RSX=1 PS3_CAMERA=0 PS3_V198_OSK_RECOVERY=1 PS3_FORCE_FONT_SAMPLER_PER_DRAW=0 PS3_SCENE_SCALE_BRIDGE=1 -j1'
```

Relevant Makefile switches:

| Switch | Current meaning/default |
| --- | --- |
| `PS3_NATIVE_RSX=1` | Build native RSX instead of software renderer |
| `PS3_MOVE` | Defaults to 1; selects Move implementation and links GEM/SPURS |
| `PS3_CAMERA=0` | Keep camera library out of this build while retaining Move code |
| `PS3_USB_INPUT_BRIDGE=1` | Include USB keyboard/mouse bridge; default 1 |
| `PS3_V198_OSK_RECOVERY=1` | Preserve accepted OSK recovery behavior |
| `PS3_FORCE_FONT_SAMPLER_PER_DRAW=0` | Do not use ineffective v2.21 per-draw sampler diagnostic |
| `PS3_POLYBLEND_2D_OVERRIDE` | Defaults to native-RSX setting; preserves stereo-safe blend |
| `PS3_SCENE_SCALE_BRIDGE=1` | Wrap renderer exports and track display-surface binds for scene scaling |

When `PS3_SCENE_SCALE_BRIDGE=1`, `Makefile.ps3` deliberately rewrites two
symbols as part of the object recipes:

- `GetRefAPI` in `gl1_main.o` becomes `PS3_BaseGetRefAPI` so the wrapper exports
  the public `GetRefAPI`.
- `rsxSetSurface` in `rsx_gl.o` becomes `PS3_SceneTrackedSetSurface` so the
  wrapper observes every display-surface bind.

Do not bypass these recipes. A bridge can link successfully while doing
nothing if either rename is absent.

The raw top-level `q2ps3.elf` is not the ELF to feed directly to
`make_self_npdrm`. PSL1GHT strips and runs `sprxlinker` into the build copy,
which fills PS3 import-descriptor function counts. Use
`build-rsx-vXYZ/q2ps3.elf` for NPDRM packaging. Using the raw top-level ELF has
previously caused an immediate return to XMB before `main()`.

For a new version, either use a fresh build directory or copy the immediately
preceding build directory and remove only the exact object files meant to be
rebuilt. Verify timestamps and hashes: historical/future timestamps can make
`make` silently reuse an old object. Never solve this with `git clean`.

## Update-only packaging

Normal builds must produce an update package containing exactly:

```text
PARAM.SFO
USRDIR/EBOOT.BIN
```

This overlays the installed executable and metadata while preserving
`USRDIR/baseq2`, commercial PAKs, saves, and configuration. Do not repeatedly
build or install full-content packages unless the baseline data layout changes
or the owner explicitly requests one.

Example for a new `v229` after creating `param-v229.xml` and a successful
`build-rsx-v229/`:

```sh
export PS3DEV=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt
export PSL1GHT=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt
export PORTLIBS=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/portlibs/ppu
export PATH=/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/bin:/mnt/f/DEV/GunConPS1/on/work/ps3dev-prebuilt/ppu/bin:/usr/local/bin:/usr/bin:/bin
cd /mnt/f/DEV/QuakeIIPS3/q2ps3

mkdir -p build-rsx-v229-update-stage/USRDIR
sfo -f param-v229.xml build-rsx-v229-update-stage/PARAM.SFO
make_self_npdrm build-rsx-v229/q2ps3.elf build-rsx-v229-update-stage/USRDIR/EBOOT.BIN UP0001-QUAKE2000_00-0000000000000000
pkg --contentid UP0001-QUAKE2000_00-0000000000000000 build-rsx-v229-update-stage q2ps3-rsx-v229-update.pkg
cp q2ps3-rsx-v229-update.pkg q2ps3-rsx-v229-update.gnpdrm.pkg
package_finalize q2ps3-rsx-v229-update.gnpdrm.pkg
```

The exact `sfo` CLI shipped with a replacement toolchain may use a slightly
different output form; check `sfo --help` if necessary. Before distribution,
list/inspect the package and confirm it has only the three stage entries
(directory included). Deploy the finalized `.gnpdrm.pkg`, never the raw
`.pkg`.

## v2.28 artifact audit

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `build-rsx-v228/q2ps3.elf` (stripped/sprxlinked packaging ELF) | 2,502,544 | `3815F498148F80D2B1787667EF628ED030CA0FEA89D04C96E4BF46FA7C891252` |
| `q2ps3.elf` (raw top-level link ELF; do not package directly) | 4,195,159 | `D6FB03D821771475C34A59288391BCACB4A5434437919A4928E1D25AFD08F66A` |
| `q2ps3.self` (not the update EBOOT) | 892,304 | `CE60B6E0F4D072AD9670C8C3D813BA4D7EA19A72C8F94405249FA27E9BB4DB27` |
| `build-rsx-v228-update-stage/PARAM.SFO` | 1,040 | `25AD68DB0E5CB90814F1179339DEB28A29247D469476F7C2B39DACEFB7CCF584` |
| `build-rsx-v228-update-stage/USRDIR/EBOOT.BIN` | 881,920 | `B9B6176EAC6ACF73357CD16E5AE2B1350379D0606EC0EF9829C4DFCE50D52CAE` |
| `q2ps3-rsx-v228-update.pkg` (raw) | 883,536 | `37BD4E80BF75A1C18256E83B74151E0314744BC117C35E01B28D05488E23F6B6` |
| `q2ps3-rsx-v228-update.gnpdrm.pkg` (deploy this) | 883,536 | `6C668B2A7348B5DC5B024F5F447EDF24DBB75FAB140D85D92DFC082B7A0F52AE` |

The raw package was listed with the toolchain's `pkg -l` and contains exactly
three items: 1,040-byte `/PARAM.SFO`, directory `/USRDIR`, and 881,920-byte
`/USRDIR/EBOOT.BIN`. The finalized package has the same three-item metadata,
content ID, package size, and data size; this older `pkg` utility prints
`Unsupported Type` for the finalized NPDRM type and therefore lists entries
from the raw package instead. Raw and finalized hashes differ as expected.

The v2.28 executable symbol audit already confirmed:

- `rsx_gl.o` defines `RSXGL_ResolveLinearSurface` and
  `RSXGL_TiledTargetsActive`.
- `rsx_gl.o` references `PS3_SceneTrackedSetSurface` after rewrite.
- The scene wrapper defines `PS3_SceneTrackedSetSurface` and references
  `PS3_BaseGetRefAPI`.
- `gl1_main.o` defines the rewritten `PS3_BaseGetRefAPI`.
- The final ELF exposes the wrapper's public `GetRefAPI`.
- The v2.23 `RI_RenderFrame` call relocation still reaches `R_PolyBlend`.

## First physical test on the new computer

1. Install `q2ps3-rsx-v228-update.gnpdrm.pkg` on the physical PS3 over an
   existing full installation. Reboot the PS3 after package installation if a
   known-good build unexpectedly black-screens; this was required in some past
   update transitions.
2. Start with mono, 720p Performance, 30 FPS cap, display filters off, and
   `gl1_ztrick 0`.
3. Enable `ps3_rsx_stats 1`.
4. Set `ps3_rsx_scene_scale 0` and `ps3_rsx_scene_adaptive 1`.
5. Test `ps3_rsx_tiled_targets 0`, then `1`, then `2`, running `vid_restart`
   after each target-mode change. Use the same map, save, route, view direction,
   combat state, and test duration.
6. Under tiled modes, v2.28 should write this to the trace:

   ```text
   RSX scene scale: tiled 3D textured resolve active
   ```

7. Copy this file from the console after each controlled run:

   ```text
   /dev_hdd0/game/QUAKE2000/USRDIR/boot-trace.log
   ```

8. Record exact output resolution, profile, cap, stereo mode, tiled mode,
   Z-cull, scene scale, adaptive state, filters, map/location, and observed FPS.

Do not compare unrelated rooms or settings. A two-FPS anecdote is less useful
than one consistent 120-frame stats interval from the same scene.

### Recovery switches for v2.28

These allow a bad scene-resolve mode to be bypassed without removing the rest
of the accepted renderer:

| Command/cvar | Effect |
| --- | --- |
| `ps3_rsx_scene_tiled_resolve 0` | Session-only bypass of the new tiled textured resolve |
| `ps3_rsx_scene_scale 1` | Direct full-resolution renderer; bypasses scene scaling |
| `ps3_rsx_scene_sync 2` | Conservative pre- and post-resolve idle diagnostics |
| `ps3_rsx_scene_sync 1` | Default conservative post-resolve barrier for the linear path |
| `ps3_rsx_scene_sync 0` | FIFO-only diagnostic; not a general safe default |
| `ps3_rsx_tiled_targets 0` then `vid_restart` | Linear display targets |

If v2.28 fails before a usable menu/console, install the known prior update
package rather than changing many variables at once. Preserve the failed
`boot-trace.log` first.

## Telemetry and how to use it

`ps3_rsx_stats 1` emits one report every 120 displayed frames. Normal play has
stats disabled, so the instrumentation is not a default performance cost.

The `RSX stats:` line includes API calls, GPU draws, batches/flushes, vertices,
indices, state skips, raster hits, UI strings/glyphs/duplicates/merges, texture
updates, in-place uploads, renames and renamed KiB, lightmap draws/vertices,
static lightmap vertices, stereo lightmap reuse, lightmap texture-set counts,
world clip tests/skips, GPU finishes, flip-wait average/maximum, render and
physical cadence average/maximum, 16.667/20/33.334/34 ms over-budget counts,
30-FPS floor samples/misses/streak, selected target FPS/budget/samples/misses,
and total-frame maxima.

The companion `PPU stages:` line attributes average/maximum PPU command-build
time to the complete scene and to dynamic lights, setup/PVS, world, entities,
effects, alpha/flash, other work, and alias source/reference counts. In stereo,
both eyes accumulate into one displayed-frame sample.

Use evidence to pick the next change:

- High flip/GPU wait or cadence with low PPU-stage times means RSX/GPU or
  presentation bound. Compare tiled modes, scene scale, filters, and stereo.
- A dominant `scene_avg_us` or one large PPU stage means CPU/frontend bound.
  Optimize that stage rather than lowering resolution again.
- High `finishes`, `updates`, `renames`, or `rename_kib` points toward dynamic
  lightmap/texture upload retirement pressure. Consider a dedicated or
  double-buffered dynamic lightmap strategy only if the trace proves it.
- GPU draws/flushes much higher than useful batches suggests texture/page/state
  transitions. Inspect material/lightmap page sorting and batch breaks.
- If adaptive scale reaches 0.5 and cadence/FPS does not materially improve,
  fill rate is not the limiting factor. Stop iterating on the resolver.
- If mono meets the floor and frame-packed 3D does not, profile eye-reuse paths
  and two-eye submission separately; do not weaken mono behavior.

The next optimization should begin with the controlled v2.28 A/B trace. Do not
continue blind scene-scaler iterations if the numbers show a PPU, dynamic
lightmap, command-submission, or presentation bottleneck.

## Expected initialization messages that are not failures

`Point parameters: Failed` is harmless in this backend. The GL1 frontend probes
for the desktop ARB point-parameter extension, but the native RSX backend does
not advertise it or return that procedure. Native particles are already
expanded into triangles/quads, so this is neither a missing rendering feature
nor the likely 20-FPS cause.

`Paletted texture: Disabled` is also expected. `gl1_palettedtexture` defaults
off and the RSX path uploads ordinary 32-bit textures/mip levels. Adding palette
textures would require a native palette/shader implementation and is not
automatically a performance win.

## Known constraints and traps

- Never use RPCS3 for this task.
- Build serially (`-j1`) and avoid overlapping KZ3/resource-heavy work.
- Make update-only packages unless explicitly asked for a full baseline.
- Do not package the raw top-level ELF.
- Do not deploy the unfinalized `.pkg` when a `.gnpdrm.pkg` is expected.
- Do not assume Git describes the current source; important directories are
  untracked.
- Do not reset/clean the working tree.
- Do not treat v2.28 as proven until physical hardware confirms it.
- Do not optimize only the owner's favorite settings; 30 FPS is the general
  acceptance floor, with higher stable selectable caps where actually viable.
- Do not mix renderer, UI projection, input, OSK, and packaging changes into
  one physical test package. Keep object-level deltas small and auditable.
- Preserve a known-good update package on both computer and USB storage before
  installing an experiment.
- When a package black-screens, distinguish a live black screen from a total
  system freeze, preserve the trace, and try one console reboot before
  concluding that a previously proven package is bad.

## Recommended continuation sequence

1. Copy the complete workspace and external PS3 toolchain.
2. Verify the v2.28 finalized package hash.
3. Re-run package-entry inspection on the destination toolchain.
4. Physically test v2.28 with the controlled tiled-mode matrix above.
5. Preserve one trace per run with settings in the filename or adjacent notes.
6. Choose exactly one optimization based on the dominant telemetry field.
7. Create `param-v229.xml`, a separate `build-rsx-v229/`, and an update-only
   package.
8. Audit object differences, symbol rewrites, package contents, and hashes.
9. Test on the physical PS3 and update both `CHANGELOG` and this handover with
   the observed result, including negative/identical results.

The port is functionally near completion. The remaining work is disciplined
performance measurement and narrow optimization—not feature expansion or a
renderer/UI rewrite.
