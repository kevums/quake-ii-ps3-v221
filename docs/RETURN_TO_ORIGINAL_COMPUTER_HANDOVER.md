# Quake II PS3 return-to-original-computer handover

Date: 2026-08-28  
Title/content identity: `QUAKE2000` / `UP0001-QUAKE2000_00-0000000000000000`

## Read this first

Stop using every PS3 package produced during this local-machine recovery
attempt. None is an accepted build. Every finalized package is now stored only
inside a directory whose name begins with `rejected-`; there is no recovery
package left in the top level of `q2ps3/`.

The owner supplied the decisive correction after the failed recovery series:

> v2.31 was the last and best working iteration on the first computer.

The first computer retains the original work. Its exact working v2.31 package,
EBOOT, source tree, build objects, toolchain, and configuration are therefore
the only valid recovery baseline. Do not substitute any artifact with a
similar `v231` name from this local derivative. The local
`build-rsx-v231/q2ps3.elf` belongs to a later compact-target experiment and the
package reconstructed from it produced the same physical black-screen result.

This document supersedes any statement in `OPTIMIZATION_HANDOVER.md`,
`validation/v231/README.md`, or the changelog that says v2.30 was the last
candidate or that all of v2.31 was rejected. The compact-target experiment was
rejected; the owner reports that an earlier/original v2.31 iteration was the
last and best physical build.

## Goal and acceptance criteria

The game itself is complete. Work is limited to general performance
optimization for physical PS3 hardware.

- Recover the exact physically working v2.31 baseline first.
- Improve the approximately 20 FPS physical result toward a generally
  sustained 30 FPS minimum.
- Preserve selectable 30 and 60 FPS targets and make them lock to their VBlank
  cadence whenever the hardware workload fits the selected budget.
- RPCS3 is for installation, boot, renderer-path, and gross-correctness checks.
  RPCS3 timing is never physical RSX performance evidence.
- Packages must update the existing `QUAKE2000` base installation.
- Never include or replace PAKs, saves, `ICON0.PNG`, `PIC1.PNG`, or other
  artwork in an update.
- Never autoboot a map, load a game, or issue `vid_restart` at startup. The
  title must boot normally to its menu.
- Build serially with `-j1`.

## Workspace locations

This local-machine derivative is at:

```text
/Users/kevin/Documents/Codex/QII/QuakeIIPS3
```

The mounted original directory used as a read-mostly source during this work
was:

```text
/Volumes/QII/QuakeIIPS3
```

All edits and generated artifacts from this recovery attempt were intentionally
kept in the local path. No changes were intentionally written back to the NTFS
original. Bring this handover to the first computer and treat that computer's
actual files as authoritative.

### Mounted-drive v2.31 search result

An explicit audit of `/Volumes/QII/QuakeIIPS3/q2ps3` found no directory,
package, EBOOT, ELF, source snapshot, Git ref, reflog entry, or dangling commit
named v2.29, v2.30, or v2.31. The labeled build/package sequence on this mounted
copy ends at v2.28. The repository has only the original upstream grafted commit
(`38b05abcb4bbef29f4e04d649a6baab6c2f85d51`); the local version history was
never committed to Git, so Git object recovery cannot supply the missing v2.31.

The mounted v2.28 artifacts were compared with the local copies. Every artifact
present in both locations is byte-identical:

```text
3815f498148f80d2b1787667ef628ed030ca0fea89d04c96e4bf46fa7c891252  build-rsx-v228/q2ps3.elf
3499ca16f8b92174cf18671f0f585891d46f96080d3bbd7e901e9546115cc759  build-rsx-v228/q2ps3.elf.map
b9b6176eac6acf73357cd16e5ae2b1350379d0606ec0ef9829c4dfce50d52cae  build-rsx-v228-update-stage/USRDIR/EBOOT.BIN (drive only)
37bd4e80bf75a1c18256e83b74151e0314744bc117c35e01b28d05488e23f6b6  q2ps3-rsx-v228-update.pkg
6c668b2a7348b5dc5b024f5f447edf24dbb75fab140d85d92dfc082b7a0f52ae  q2ps3-rsx-v228-update.gnpdrm.pkg
```

Therefore the mounted drive is useful as an immutable pre-v2.29 comparison,
but it does **not** contain the owner's reported last/best original v2.31. That
artifact still has to be recovered from the first computer or its backups. Do
not relabel v2.28, local v2.30, or the rejected compact experiment as that
missing build.

## Physical PS3 results recorded during this attempt

The following outcomes came directly from the owner. A package passing RPCS3
does not override these physical results.

| Order | Physical package/test | Physical result | Status |
| --- | --- | --- | --- |
| 1 | Six v2.30 auto-loading 30/60 × linear/tiled/Z-cull overlays | Every package froze the console at the end of the forced game load | Rejected |
| 2 | v2.30 normal-menu package, same v2.30 EBOOT and comment-only autoexec | Reached normal startup, but a manual game load still froze at the end | Rejected |
| 3 | First v2.27 recovery, regenerated NPDRM EBOOT and comment-only autoexec | Reached the menu, then froze at the end of a manual game load | Rejected |
| 4 | Exact-EBOOT v2.27 recovery with broad safe renderer overrides | Black screen; console/XMB remained responsive | Rejected |
| 5 | Menu-proven regenerated v2.27 EBOOT with only native-scene safety overrides | Black screen; console/XMB remained responsive | Rejected |
| 6 | Locally reconstructed v2.31 package with only `ps3_rsx_scene_compact 0` | Owner reported “same shit” after the preceding responsive black-screen result | Rejected |

No package after the initial approximately 20 FPS working baseline entered
physical gameplay. There is therefore no valid later FPS comparison, surface
mode winner, or 30/60 lock result.

### Rejected v2.30 autoboot package hashes

All are under `validation/hardware-v230/rejected-autoboot-packages/`:

```text
47b5a6d7d1365f2bec19be48aa3f9b629323636971d97757f6d19c13c5458cb5  q2ps3-rsx-v230-hw-30-linear-update.gnpdrm.pkg
4f0a1bcaf17270050fc148c29f81b2051076ba2651b6740c1c6b98b57d10832c  q2ps3-rsx-v230-hw-30-tiled-update.gnpdrm.pkg
aa7266bdfe53e6a211553391191eebb9e7879af8d02fd29b9994cbc62370d518  q2ps3-rsx-v230-hw-30-zcull-update.gnpdrm.pkg
9655bc3b9e3eb9e662f55194246cfa3478bb15e3261bdabc387c0dc5fbb42705  q2ps3-rsx-v230-hw-60-linear-update.gnpdrm.pkg
0b8f33a1c680e8ea02c22e1ce74d3683816350788b0de84b862125403aa15b6a  q2ps3-rsx-v230-hw-60-tiled-update.gnpdrm.pkg
b64d71460b72bd092c7601d8f88be64b57f8392e1d230e97030507ddd3ed2bf3  q2ps3-rsx-v230-hw-60-zcull-update.gnpdrm.pkg
```

These packages all used the same v2.30 EBOOT and differed only by startup
configuration. Each autoexec issued `vid_restart` and then `map base1`. Their
shared end-of-load freeze meant the matrix did not isolate surface mode, and
the forced startup flow made the test invalid for normal use.

### Rejected normal/recovery package hashes

All v2.30/v2.27 packages are under
`validation/hardware-v230/rejected-recovery-packages/`:

```text
bbc998518ced4902499726f5e03f067ad6daa8155981dcd702610f19b068e5f8  engine-only-v230/q2ps3-rsx-v230-update.gnpdrm.pkg
836c31f1816b9114b4e741656874f38262cfbb9a158cb1dc31bfb443a164aa4f  q2ps3-rsx-v230-normal-start-update.gnpdrm.pkg
27686e43af60ad9c6f1eb0131e328620b0ad4b28bbeb9e49cc51c4571943b40c  q2ps3-rsx-v227-normal-recovery-update.gnpdrm.pkg
e3e05c345bdda8cffa381ca4f7775e545e3f4012196b9a5f07d8d1eda7bc0d8a  q2ps3-rsx-v227-exact-safe-recovery-update.gnpdrm.pkg
69b845e3f9f283008253920b85677cd878de8f24fa86d08127b18be65d9ab907  q2ps3-rsx-v227-menu-safe-recovery-update.gnpdrm.pkg
```

The locally reconstructed v2.31 package is under
`validation/hardware-v231/rejected-local-reconstruction/`:

```text
d3876dbf97aa0e5db06e76f30eac9e207ef52217693c8edfc04e2cc1491ae2c4  q2ps3-rsx-v231-hardware-recovery-update.gnpdrm.pkg
```

Do not install any of these files.

## What was changed or investigated locally

### v2.28-v2.30 optimization line

The existing detailed handoff records the implementation history. The most
relevant later changes were:

- v2.28 added a textured 3D resolve for tiled display output while retaining
  the v2.27 linear scale-engine path.
- v2.29 registered private scaled color/depth memory as tiled RSX regions and
  added a private Z-cull region for tiled-plus-Z-cull mode.
- v2.30 made adaptive scene scaling target-aware for selectable 30/60 FPS
  behavior. It used different cadence thresholds for the 30 and 60 targets,
  accumulated bounded miss pressure, ignored implausibly early samples, and
  reset policy state when the target/profile changed.
- The local v2.30 ELF is
  `q2ps3/build-rsx-v230/q2ps3.elf`, SHA-256
  `6e4ca34376d0e8baf120d397cfaeb6ff4579964a35d9b717dd2426a988f7ba94`.
- RPCS3 exercised the v2.30 target policy and renderer paths, but every
  physical package using that engine ultimately froze at game-load completion.

### Local v2.31 compact-target experiment

The local `build-rsx-v231/q2ps3.elf` attempted to allocate the private scaled
scene at its reduced dimensions and remap raster geometry to it.

- ELF SHA-256:
  `4b1d8d7eb200cfe9fe9816d30efd737edebb08cccd406df9234e4e5de076650a`
- `ps3_rsx_scene_compact 1` failed in RPCS3 on the first world frame with a
  dead FIFO/invalid RSX method in both tiled and linear tests.
- The identical ELF ran through repeated RPCS3 world/statistics intervals with
  `ps3_rsx_scene_compact 0` and the original full-size private target.
- Evidence is under `validation/v231/`; failed source snapshots are under
  `validation/v231/failed-source/`.
- Source was later restored to the v2.30 implementation. A revert-check build
  reproduced the v2.30 renderer objects and ELF byte-for-byte.

This local experimental ELF must not be assumed to equal the first computer's
physically working v2.31 iteration merely because both use the `v231` label.
The package reconstructed from it produced a responsive physical black screen.

### Exact late-lineage object isolation

Object hashing shows that the late local experiments were much narrower than
their version numbers suggest:

| Transition | Changed linked objects | Meaning |
| --- | --- | --- |
| v2.27 -> v2.28 | `rsx_gl.o`, `rsx_scene_scale_ps3.o` | Adds the textured resolve needed when the display target is tiled |
| v2.28 -> v2.29 | `rsx_scene_scale_ps3.o` only | Adds private tiled color/depth registration and optional private Z-cull |
| v2.29 -> v2.30 | `rsx_scene_scale_ps3.o` only | Makes cadence adaptation aware of the selected 30/60 target |
| local v2.30 -> local v2.31 | `rsx_gl.o`, `rsx_scene_scale_ps3.o` | Changes private target dimensions/pitch and dynamically remaps viewport/scissor geometry |

Relevant object SHA-256 values are:

```text
v2.27 rsx_scene_scale_ps3.o  8cfd907cb01c1be8e06799266dd4bb989c8960bb7db09aff1d81530b36400a60
v2.28 rsx_gl.o               5ba8c8c2dcc6b233aa89c6a07329fbab90f62541c8975282dce9ee585444546f
v2.28 rsx_scene_scale_ps3.o  65b78316ce838d3f35a536d789bd0c18febae0ec2602744d30d90d6af591cf4f
v2.29 rsx_scene_scale_ps3.o  0bd4af6aca09cb7ddda21c93f6ebd7c4c1ee2e4cef4bf91da22d583e5d49c389
v2.30 rsx_scene_scale_ps3.o  4c5fa861cdd4c5a5c222213b475d393726ac2c734d4748241208bd752555d4d5
local v2.31 rsx_gl.o         13fd2e23cb23aef0acef742fb86692945bb03d20efc2259a3c7fbc5892ca0dc9
local v2.31 scene bridge     2b8ec72f1afa70fb69cd8e3eb5ec9247398dc73982cb67773c8ba3707dcfdacd
```

The rejected compact code did not reduce the number of world pixels relative
to the already-scaled rectangle. Both paths render the same 960x540 world at a
0.75 scale; compact mode additionally shrank the allocation, pitch, surface
descriptor, and raster geometry. Its likely benefit was memory footprint or
locality, not the primary 56.25-percent pixel workload that the safe full-size
descriptor already achieved. Since both compact linear and compact tiled modes
failed while the same ELF's full-size control ran in RPCS3, compact allocation
is a poor risk/reward route for the next hardware build. Do not reintroduce it
while pursuing frame rate.

### Packaging work

All generated packages were update overlays using title ID `QUAKE2000` and
content ID `UP0001-QUAKE2000_00-0000000000000000`. Raw packages were copied and
finalized with PSL1GHT's `package_finalize`; only `.gnpdrm.pkg` files were used
for physical tests.

The normal/recovery overlays contained only these five entries:

```text
/PARAM.SFO
/USRDIR
/USRDIR/EBOOT.BIN
/USRDIR/baseq2
/USRDIR/baseq2/autoexec.cfg
```

No test package included PAKs, saves, `config.cfg`, `ICON0.PNG`, `PIC1.PNG`, or
other artwork. RPCS3 before/after hashes remained unchanged for `ICON0.PNG` and
`pak0.pak` through `pak3.pak`.

## RPCS3 evidence and its limits

RPCS3 successfully installed the finalized overlays and verified their
installed EBOOT/autoexec bytes. It also reached `base1` and rendered world
frames for configurations that failed or black-screened physical hardware.

Important evidence directories:

- `validation/hardware-v230/`: six v2.30 configuration/log sets and package
  installation traces.
- `validation/v231/`: local compact-target failures and the full-size-target
  control.
- `validation/hardware-v231/`: package-installed local v2.31 reconstruction
  logs. The log reaches BSP registration and adaptive scale 0.500 without a
  compact-target trace, yet the same package failed physically.

Conclusion: RPCS3 is useful for catching gross errors, but it is not predictive
of the physical startup/load failures encountered here. Do not treat another
RPCS3-successful package as a recovery until the physical PS3 passes it.

## Unresolved causes

No single cause was proven. The first computer should preserve evidence and
test these possibilities in order rather than guessing:

1. **Wrong artifact lineage.** The local `v231` label does not identify the
   exact original working EBOOT/package. Recreated NPDRM EBOOTs and similarly
   named ELFs repeatedly behaved differently on hardware.
2. **Persistent PS3 configuration.** Every recovery package deliberately
   preserved `config.cfg`. Update packages also do not delete an existing
   `autoexec.cfg` when they omit one. The PS3 may still carry settings written
   by previous tests. The last installed local reconstruction leaves an
   autoexec containing `set ps3_rsx_scene_compact 0`.
3. **Installed-base state.** PAKs, saves, and artwork were preserved as
   requested, but the current installation has received many overlays. A clean
   comparison against the original working base was never performed.
4. **Physical RSX synchronization or memory-layout behavior.** RPCS3 rendered
   paths that froze the real console. A physical-only command-stream,
   allocation, or synchronization defect remains possible.
5. **Package/toolchain mismatch.** The Mac PSL1GHT tools produced structurally
   valid packages accepted by both RPCS3 and PS3, but they did not reproduce
   the behavior of the first computer's known working artifact.

## Hardware-compatible 30/60 FPS optimization plan

This plan deliberately starts from the genuine first-computer v2.31. It does
not propose building on the local v2.30 or rejected local v2.31 executable.

### Frame budgets and the strongest existing lever

The reported approximately 20 FPS result corresponds to a roughly 50,000 us
presentation interval. Sustained 30 FPS requires no more than 33,334 us, a
one-third frame-time reduction. Sustained 60 FPS requires no more than 16,667
us, roughly a two-thirds reduction. Those are end-to-end cadence budgets, not
just PPU command-generation budgets.

If it is present in the genuine v2.31—or after it is ported as one isolated
golden-tree change—the full-size-descriptor scene bridge is the largest known
low-intrusion RSX fill-rate lever: shade a smaller 3D rectangle, resolve it to
the display eye, then render HUD/UI at native resolution. Its scale ladder has
these pixel costs:

| Scene scale | 720p world rectangle | 1080p world rectangle | World pixels vs native |
| --- | ---: | ---: | ---: |
| 1.000 | 1280x720 | 1920x1080 | 100.0% |
| 0.875 | 1120x630 | 1680x944 | 76.6% |
| 0.750 | 960x540 | 1440x810 | 56.3% |
| 0.667 | 852x480 | 1280x720 | 44.4% |
| 0.583 | 746x420 | 1120x630 | 34.0% |
| 0.500 | 640x360 | 960x540 | 25.0% |

If the known 20 FPS result was native-resolution and entirely fill-bound, a
0.75 scale has a theoretical ceiling near 35.6 FPS, 0.583 near 58.8 FPS, and
0.50 near 80 FPS. Those are only upper-bound inferences from pixel ratios;
fixed RSX costs, PPU submission, simulation, resolves, VBlank quantization,
stereo, and overdraw reduce the real result. If the reported 20 FPS was already
measured at a reduced scale, this calculation must be redone from the captured
starting scale.

### Phase 0: establish the compatibility envelope

1. Prove the exact original v2.31 package on physical hardware before changing
   source, configuration, signing, or packaging. It must cold boot to the
   normal menu, manually load `base1`, complete at least one level transition,
   load a save, return to the menu, and exit through XMB without freezing or a
   responsive black screen.
2. Hash its package/EBOOT/ELF/map and save its complete `boot-trace.log`,
   `qconsole.log`, `config.cfg`, and `autoexec.cfg`. Extract the package
   manifest. These become immutable golden evidence.
3. Compare its ELF map and object hashes with the mounted v2.28 and local
   v2.29-v2.31 values above. This reveals whether the real v2.31 contains the
   full-size scene bridge, private tiling, target-aware adaptation, neither, or
   a different implementation.
4. Reproduce the golden EBOOT byte-for-byte with the first computer's original
   compiler, `sprxlinker`, NPDRM signer, libraries, link order, and serial
   `-j1` build. Do not optimize until the reproduced EBOOT also passes the same
   physical test.

No later result is attributable if this phase is skipped.

### Phase 1: measure a fixed-scale curve before writing code

Use the exact golden build, normal menu startup, one repeatable `base1` route,
mono output, and `ps3_rsx_stats 1`. Disable adaptive scaling for the sweep so
each capture has one known scale. Test 1.000, 0.875, 0.750, 0.667, 0.583, and
0.500 separately, recording at least five complete 120-frame telemetry blocks
after warm-up. If the genuine scene bridge requires `gl1_ztrick 0`, verify and
record that value rather than silently changing it. Do not put map or restart
commands in `autoexec.cfg`; apply the setting interactively, restart video only
when the setting requires it, and load the map manually.

Capture these fields for every run:

- `cadence_avg`, `cadence_max`, `cadence_over16667`,
  `cadence_over33334`, `floor_misses`, `floor_streak_max`, and
  `target_misses` prove actual 30/60 delivery.
- `wait_avg`/`wait_max` report time blocked on the preceding display flip.
- `render_avg`/`render_max` cover PPU time from flip release through GL1/RSX
  command generation and final batch flush. They are not GPU-duration queries.
- The `PPU stages` line separates `dlights`, `setup`, `world`, `entities`,
  `effects`, `alpha`, and `other` CPU time.
- `api`, `gpu`, `flushes`, `vertices`, `indices`, `state_skips`, texture
  update/rename counters, lightmap counters, and `finishes` identify command,
  upload, and synchronization pressure.

Interpret the curve rather than guessing:

- Large cadence improvement as scale squared falls, with similar PPU stage
  time, proves RSX fill/bandwidth pressure. Scene scale is the correct lever.
- High `render_avg` and a dominant PPU stage with little scale response proves
  command-generation or game/render CPU pressure. Lower resolution alone
  cannot reach the target.
- Low PPU time but cadence locked to 33.3 or 50 ms indicates RSX/presentation
  pressure or a synchronization stall. Inspect `finishes`, resolve mode, and
  framebuffer layout one variable at a time.
- A 16.7 ms average with many `target_misses` is not sustained 60; use the
  exact miss counters and maxima, not a visually sampled FPS number.

Do not compare RPCS3 timing with these results. RPCS3 may validate the command
path and package installation, but only the physical telemetry curve can
choose the next optimization.

### Phase 2: reach and lock 30 FPS first

1. Retain the golden renderer's surface descriptor, pitch, allocation layout,
   viewport/scissor ownership, UI boundary, and normal startup behavior.
2. If Phase 1 proves fill pressure, select the highest fixed scale whose five
   warmed telemetry blocks have `floor_misses=0`. A native-20-FPS result makes
   0.75 the first rational 30 FPS candidate, followed by 0.667 and 0.583 only
   if required.
3. Once a fixed scale passes, enable adaptation around that known-safe ladder.
   Keep the v2.30 cadence-tier idea—30 FPS reacts only when cadence enters the
   50 ms tier—but port the policy into the exact golden source instead of
   transplanting the local object or EBOOT. Start the 30 target at the proven
   fixed scale rather than spending initial gameplay at native resolution.
4. Keep the exact 60-to-30 VBlank render-ahead divider already developed in
   the earlier line. Confirm the golden trace contains `PS3 pacing: exact 30 Hz
   render-ahead VBlank budget active`; preserve that object unless a physical
   A/B proves a change.
5. If Phase 1 proves PPU pressure, change only the dominant measured stage.
   Preserve the settings-independent accelerators already present in the
   lineage: persistent static BSP vertices/indices, combined lightmaps,
   occupied-page lightmap tracking, prepared MD2 records and indices, indexed
   native batches, frustum-mask propagation, final entity-light caching,
   inline UI batching, state filtering, and raster-state filtering. First
   verify each feature exists in the real v2.31 rather than assuming the local
   source represents it.

The 30 FPS acceptance gate is five consecutive warmed 120-frame blocks with
zero `floor_misses`, no load/transition freeze, no black screen, correct world
and UI, successful save/load, and clean XMB exit. One clean average is not a
pass. After tuning on `base1`, repeat the gate on `city1`, `waste1`, `space`,
and `boss2` so the result covers dynamic lightmaps, water/warp, sky, entities,
weapons, alpha/effects, and a heavy encounter rather than one favorable map.

### Phase 3: pursue 60 FPS without breaking the 30 FPS build

Freeze the accepted 30 FPS package and branch from its exact source/artifacts.
At a native 20 FPS starting point, 60 FPS likely needs a 0.583 or 0.500 3D
scale even in a strongly fill-bound scene; fixed CPU and resolve costs can make
that insufficient. Use the fixed-scale curve to decide before implementing.

For the 60 FPS branch:

1. Start at the lowest visually acceptable scale that already demonstrated
   the smallest physical `target_misses`; do not ramp down from native during
   every gameplay session. Keep native-resolution UI.
2. Require the existing exact 16,667 us target counters. The adaptation tier
   may use the robust 25,000 us discriminator to detect a fall from 60 to the
   30 Hz VBlank tier, but acceptance remains `target_misses=0` at 16,667 us.
3. Test linear display/private targets first because they minimize metadata and
   use the simpler existing scale-engine code path. That path is not considered
   physically accepted until a golden-derived package passes. Test tiled
   display resolve, then private tiling, then private Z-cull only if those
   mechanisms are already in the proven golden v2.31 or are introduced as three
   separate one-object packages. Never combine all three in the first physical
   test.
4. If the resolve or its global post-transfer idle is a measured limiter, an
   isolated same-3D-engine textured resolve for linear output is a more
   relevant experiment than compact allocation: it targets cross-engine
   synchronization while keeping full-size surface geometry. It must first
   pass command-stream and image tests in RPCS3, then the full physical safety
   gate. Do not remove a barrier merely because FIFO order looks sufficient.
5. If a PPU stage remains above budget, optimize that stage alone and compare
   object hashes. Candidate work must be driven by counters: world-chain and
   material submission if `world` dominates; prepared/cached MD2 and lighting
   if `entities` dominates; dynamic-light marking/atlas work if `dlights`
   dominates; particle/beam packing if `effects` dominates; UI batching and
   texture churn if `other` dominates.
6. Treat stereo as a separate performance class. Frame-packed stereo renders
   two eyes and can change PVS/visibility; it cannot inherit a mono 60 FPS
   claim. First prove mono 60, then collect the same five-block curve for each
   supported stereo output and accept 30 where the two-eye workload cannot
   satisfy 16.7 ms.

The 60 FPS acceptance gate is five consecutive warmed blocks with zero
`target_misses`, plus the complete hardware-safety gate used for 30 FPS. Keep
30 FPS as the universal fallback and never degrade its accepted package while
searching for 60.

### Changes specifically prohibited by this evidence

- Do not use the local v2.31 compact allocation, compact pitch, compact
  `gcmSurface.width/height`, or runtime raster-geometry override.
- Do not transplant the local v2.29/v2.30 scene object into the original v2.31
  EBOOT. Re-implement one source-level policy change in the golden tree and
  retain every other golden object byte-for-byte.
- Do not infer that private tiling or Z-cull is physically safe from RPCS3.
- Do not use automatic `vid_restart`, `map`, `load`, or game-autoboot commands.
- Do not overwrite or package `config.cfg`, PAKs, saves, icons, or artwork.
- Do not use a regenerated or re-signed EBOOT as the baseline until it has
  independently passed hardware; package structure and lineage are part of the
  compatibility envelope.
- Do not claim 30/60 from `render_avg`, RPCS3 timing, a menu counter, or a short
  visual observation. Only physical cadence blocks and the complete safety
  sequence establish acceptance.

## Required restart procedure on the first computer

Do not begin with a rebuild.

1. Make a byte-for-byte archive of the first computer's complete original
   working directory and toolchain metadata. Preserve timestamps and all
   untracked build/package files.
2. Locate the exact v2.31 package that was physically installed when the game
   last entered gameplay and performed best. Also locate its stage EBOOT and
   ELF if present.
3. Record SHA-256 hashes for the finalized package, raw package, EBOOT, ELF,
   `PARAM.SFO`, and any accompanying config. Add those hashes to this handover.
4. Inspect the original package manifest with `pkg -l`. Record every included
   path. Do not rebuild or re-finalize it before the first confirmation test.
5. On the PS3, back up these files before changing anything:

   ```text
   /dev_hdd0/game/QUAKE2000/USRDIR/baseq2/config.cfg
   /dev_hdd0/game/QUAKE2000/USRDIR/baseq2/autoexec.cfg
   /dev_hdd0/game/QUAKE2000/USRDIR/boot-trace.log
   /dev_hdd0/game/QUAKE2000/USRDIR/baseq2/qconsole.log
   /dev_hdd0/game/QUAKE2000/USRDIR/baseq2/save/
   ```

6. After backup, remove or rename the recovery-test `autoexec.cfg`; reinstalling
   an older engine-only update will not delete it. Preserve `config.cfg` first,
   then use a clean/default copy for one controlled baseline test if the exact
   original package still fails.
7. Reinstall the exact original working v2.31 package. Reboot the PS3. Boot to
   the normal menu and manually load the same known scene without changing
   video settings.
8. If it works, capture the package/EBOOT hashes, `boot-trace.log`,
   `qconsole.log`, exact config files, resolution/profile/target, scene, route,
   and observed FPS. This becomes the immutable golden baseline.
9. Only after that proof, reproduce the original package with the original
   source/toolchain. Compare the rebuilt ELF/EBOOT/package and physical result
   before adding any optimization.
10. Resume optimization with one source change per package. Every package must
    boot normally; select maps and settings manually after startup. Preserve a
    known-good rollback package after every accepted physical step.

## Suggested first technical comparison

Once the genuine v2.31 artifact is found, compare it against the local
experimental files by hashes and object maps rather than version names:

```sh
shasum -a 256 ORIGINAL_V231.pkg ORIGINAL_EBOOT.BIN ORIGINAL_Q2PS3.elf
shasum -a 256 q2ps3/build-rsx-v231/q2ps3.elf
pkg -l ORIGINAL_V231.pkg
```

If the original ELF exists, compare its linker map and the renderer objects
(`rsx_gl.o`, `rsx_backend.o`, `rsx_scene_scale_ps3.o`, `frame_ps3.o`, and
`gl1_*.o`) with the local build. The first meaningful question is not “what
version number is this?” but “which exact code and toolchain produced the EBOOT
that physically entered gameplay?”

## Final state of this local derivative

- No locally generated recovery package is approved or left in the top-level
  deployable location.
- Rejected v2.30/v2.27 packages are archived under
  `validation/hardware-v230/rejected-*`.
- The rejected local v2.31 reconstruction is archived under
  `validation/hardware-v231/rejected-local-reconstruction/`.
- The local authoritative source was restored to the v2.30 implementation
  after the compact-target experiment; the experimental source is retained
  only under `validation/v231/failed-source/`.
- The physical optimization objective is not complete. The work must restart
  from the first computer's exact, physically working v2.31 artifact.
