# Review of the returned Quake II PS3 handover

Audited: 2026-08-28  
Reviewed document: `RETURN_TO_ORIGINAL_COMPUTER_HANDOVER.md`  
Reviewed document SHA-256: `47CAE66B8DD45AADC0882218450D90253081FE6A77D76B75F36830A772DDD966`  
Authoritative workspace: `F:\DEV\QuakeIIPS3\q2ps3`  
Validation policy: physical PS3 only; **do not use RPCS3**

## Executive conclusion

The returned handover contains useful negative evidence, but its proposed
recovery baseline is not present on this computer. The current filesystem has
no v2.29, v2.30, or v2.31 package, EBOOT, ELF, map, build directory, source
snapshot, validation directory, or archive. The source, changelog, build
objects, and top-level artifacts all end at v2.28.

The statement that an original, physically working v2.31 is retained here is
therefore a user-reported historical fact, not a recoverable artifact. It
cannot presently serve as a byte-exact golden baseline.

The last recoverable original-computer package with prior physical gameplay
evidence is v2.27:

```text
q2ps3/q2ps3-rsx-v227-update.gnpdrm.pkg
SHA-256 7D8AF4F1FC368D426E8CC4E4F7BEFAE2415EA6F4ED7E63A6EDB137969796DB47
```

The owner previously reported v2.27 as exhibiting identical approximately
20-FPS behavior, which means it entered gameplay rather than failing during
startup or map load. The returned machine did not test this untouched package.
It regenerated or repackaged EBOOTs and included `baseq2/autoexec.cfg` in its
recovery overlays. Those tests therefore do not invalidate the original v2.27
artifact.

v2.28 is the best next hardware-compatible optimization candidate already
available here. It is a narrow two-object change from v2.27 and is designed to
make the existing reduced-resolution scene path work when the display target
is tiled. It is built and packaged but has not been physically accepted.

The correct continuation is:

1. Back up and remove the PS3's recovery-test `autoexec.cfg`.
2. Re-prove the untouched original v2.27 package on the physical PS3.
3. Measure its fixed-scale linear path.
4. Test the untouched original v2.28 package first in its v2.27-equivalent
   linear/direct configuration, then enable its tiled resolve conservatively.
5. Use physical telemetry to decide whether the remaining limit is RSX fill,
   PPU submission, dynamic uploads, or presentation synchronization.

Do not port the returned v2.29-v2.31 experiments before this sequence.

## Evidence reconciliation

| Returned-handover claim | Current evidence | Assessment |
| --- | --- | --- |
| Original v2.31 is the last/best physical build | Owner report only; no matching artifact anywhere in this workspace | Plausible history, but unavailable and not usable as a golden binary |
| Mounted/original copy ended at v2.28 | Current tree ends at v2.28 and all shared v2.28 hashes match the handover | Verified |
| v2.29-v2.31 work exists on the returned derivative | No returned build/source/validation directory was transferred | Described but not independently auditable here |
| Every returned recovery package failed physically | Recorded as direct owner observations | Accept as physical outcomes, but causal attribution is limited |
| Returned recovery overlays had five entries including `autoexec.cfg` | Stated in the handover; the files themselves are absent here | Critical test contamination and persistence risk |
| Original updates are engine-only | Original v2.27 raw package lists exactly three entries | Verified |
| Local compact-target v2.31 is unsafe | It added surface/pitch/raster complexity without reducing the already shaded rectangle, and the returned tests failed | Reject this path |
| RPCS3 can be used for gross validation | Explicitly prohibited by the owner on this computer | Superseded; do not use it |
| Optimization remains incomplete | Last known physical result is about 20 FPS against a 30-FPS floor | Verified |

The returned handover's physical failure table should be read as a failure of
its reconstruction process, not a clean version bisect. The same derivative
environment produced failures from v2.30, regenerated v2.27, an “exact-EBOOT”
v2.27 package with configuration overrides, and a local v2.31 reconstruction.
That cross-version pattern points more strongly to artifact lineage,
configuration persistence, package construction, or toolchain differences
than to one renderer optimization.

## What the returned failures actually prove

### v2.30 autoboot matrix

All six 30/60 × linear/tiled/Z-cull packages froze at the end of a forced game
load. Because they shared one EBOOT and all issued `vid_restart` and `map
base1` automatically, they did not isolate surface mode or FPS target.

The later normal-menu v2.30 package also froze after a manual game load. That
shows autoboot was not the only problem, but it still does not isolate source
from regenerated EBOOT, signer/toolchain, installed configuration, or package
payload.

Conclusion: reject those packages; do not conclude that 30/60 target-aware
adaptation itself is incompatible.

### Reconstructed v2.27 recovery packages

The returned machine's regenerated v2.27 reached the menu and froze at manual
load. An “exact-EBOOT” variant with broad safety overrides black-screened, and
a regenerated menu-proven variant with narrower overrides also
black-screened.

The important missing test was the untouched original finalized v2.27 package
from this computer, with no added `baseq2` directory and no `autoexec.cfg`.
Because the original v2.27 previously entered gameplay here, the returned
failure is evidence against its reconstruction/configuration envelope, not
against v2.27 source.

### Returned local v2.31 compact-target experiment

The compact experiment shrank allocation, pitch, surface descriptor, and
raster geometry. It did not lower the number of world pixels relative to the
existing scaled rectangle. Its likely upside was memory footprint/locality;
its downside was a new set of hardware surface and viewport invariants.

The returned package also black-screened with `ps3_rsx_scene_compact 0`, so
compact mode was not the sole cause of that physical package's failure.
Nevertheless, the experiment has poor risk/reward and should not be recreated.

Conclusion: permanently exclude compact allocation/pitch and runtime raster
remapping from the present optimization line.

### Persistent configuration

Every returned recovery overlay reportedly included:

```text
/PARAM.SFO
/USRDIR
/USRDIR/EBOOT.BIN
/USRDIR/baseq2
/USRDIR/baseq2/autoexec.cfg
```

An update that later omits `autoexec.cfg` does not delete the installed file.
This means commands from an earlier failed test can continue affecting every
later engine package. Broad “safe” renderer overrides can also change the boot
path before the menu appears.

All future optimization packages must return to the original three-entry
format and contain no configuration file:

```text
/PARAM.SFO
/USRDIR
/USRDIR/EBOOT.BIN
```

Apply diagnostic settings manually after a normal menu boot.

## Current authoritative state

### Available compatibility ladder

| Version | Role | Finalized package SHA-256 |
| --- | --- | --- |
| v2.19 | Physically confirmed gameplay/online/OSK baseline; known scaled-text defect | `E9F272E91C8A5F5C66B219490B1DC37A2D7B8DFCC1B68EAD4DE9CEAC48F26803` |
| v2.23 | Adds corrected per-eye water/damage/power-up blend and USB input lineage | `BFB834F0C3BEE9DE50A221C35638D3EC08DFD5BF6C6978C35AE5076130FF92A2` |
| v2.24 | Restores the accepted viewport/scissor cache | `90B7D1A8BD56F4A66FFB21C604128EEB6DF60407E4D9BCF6CDFB098075C68152` |
| v2.27 | Last recoverable package with reported physical gameplay; approximately 20 FPS | `7D8AF4F1FC368D426E8CC4E4F7BEFAE2415EA6F4ED7E63A6EDB137969796DB47` |
| v2.28 | Current two-object tiled-resolve candidate; not physically accepted | `6C668B2A7348B5DC5B024F5F447EDF24DBB75FAB140D85D92DFC082B7A0F52AE` |

If an exact original v2.31 is later found, hash it and insert it above v2.27
only after it independently passes the complete physical safety test. Do not
relabel another artifact as v2.31.

### Exact original v2.27 manifest and hashes

The original raw package was inspected with `pkg -l` and contains:

```text
raw data   1,040 bytes  /PARAM.SFO
directory      0 bytes  /USRDIR
raw data 889,968 bytes  /USRDIR/EBOOT.BIN
```

| Artifact | SHA-256 |
| --- | --- |
| `build-rsx-v227/q2ps3.elf` | `09BF3B3162C1372801058E139075BFC39001678FB78651652EF56B87DD2F9FBB` |
| `build-rsx-v227-update-stage/PARAM.SFO` | `54995331E76663DC999C53DB9078BA2681CFB9E70EC500CF21F19CB4FDEFEA64` |
| `build-rsx-v227-update-stage/USRDIR/EBOOT.BIN` | `03A8081E13662C92A3FE665A9082B383372E997C3E136BFABE5E131A6F0AD79D` |
| `q2ps3-rsx-v227-update.pkg` | `0E1A357764E1E290B04BA9B787005057CE42C511C33F31CD1963A0E384BF9114` |
| `q2ps3-rsx-v227-update.gnpdrm.pkg` | `7D8AF4F1FC368D426E8CC4E4F7BEFAE2415EA6F4ED7E63A6EDB137969796DB47` |

### Exact original v2.28 hashes

| Artifact | SHA-256 |
| --- | --- |
| `build-rsx-v228/q2ps3.elf` | `3815F498148F80D2B1787667EF628ED030CA0FEA89D04C96E4BF46FA7C891252` |
| `build-rsx-v228-update-stage/PARAM.SFO` | `25AD68DB0E5CB90814F1179339DEB28A29247D469476F7C2B39DACEFB7CCF584` |
| `build-rsx-v228-update-stage/USRDIR/EBOOT.BIN` | `B9B6176EAC6ACF73357CD16E5AE2B1350379D0606EC0EF9829C4DFCE50D52CAE` |
| `q2ps3-rsx-v228-update.pkg` | `37BD4E80BF75A1C18256E83B74151E0314744BC117C35E01B28D05488E23F6B6` |
| `q2ps3-rsx-v228-update.gnpdrm.pkg` | `6C668B2A7348B5DC5B024F5F447EDF24DBB75FAB140D85D92DFC082B7A0F52AE` |

The v2.27 and v2.28 build directories each contain 141 object files. Exactly
two differ:

| Object | v2.27 SHA-256 | v2.28 SHA-256 |
| --- | --- | --- |
| `rsx_gl.o` | `7B29E2B8A812BB2E3148F961304209B0F6AFFD3905884808A85129BDD490CC46` | `5BA8C8C2DCC6B233AA89C6A07329FBAB90F62541C8975282DCE9EE585444546F` |
| `rsx_scene_scale_ps3.o` | `8CFD907CB01C1BE8E06799266DD4BB989C8960BB7DB09AFF1D81530B36400A60` | `65B78316CE838D3F35A536D789BD0C18FEBAE0EC2602744D30D90D6AF591CF4F` |

This is the narrowest useful optimization delta presently available.

### Original build toolchain identity

The v2.28 line was built with the prebuilt toolchain at:

```text
F:\DEV\GunConPS1\on\work\ps3dev-prebuilt
```

The compiler is `powerpc64-ps3-elf-gcc (GCC) 7.2.0`. Do not silently switch to
the unrelated `F:\DEV\QuakeIIPS3\ps3toolchain` tree or to the returned
machine's tools.

| Tool | SHA-256 |
| --- | --- |
| `bin/sfo` | `43FA0115C9A2FD524793ED552C2632C0AE668F948510F2AC9E6B356602661E05` |
| `bin/make_self_npdrm` | `F53D7B6F7360E0F1C60E19E9DAF0BA59849ABF19955A5C5550C90C026341F78D` |
| `bin/pkg` | `9301224E2C5DEE4456148CDD0A5614B61FEE13F311249B6594A34935AC48E9DD` |
| `bin/package_finalize` | `2269FD67D341B5E312C59EB459572D85155774CCA1E4B999F2CC2812EC763EA6` |
| `bin/sprxlinker` | `DC26F54D68E65B4F2D0F3C08354D0FF8C5CD6930E5089FBB3432080032FD25CA` |
| `ppu/bin/powerpc64-ps3-elf-gcc` | `6040BF6BB10062D46ED93049547F953B0E22460224F91F2774D3791B2FD5975A` |
| `ppu/bin/powerpc64-ps3-elf-objcopy` | `9C9AB58F02926BC0A6E8ED2A336E4EF2C2365CF3CB86E13398D151B03300C4B7` |
| `ppu/bin/powerpc64-ps3-elf-strip` | `FBCD67477F57DD0EF1A6D1B1137619AEE0F2A01350805ABC2564E2CB468B2F3C` |
| `ppu/bin/powerpc64-ps3-elf-ld` | `3F470273D1AE4151F6FDE6A0B061BA146F6A60A466EE7564519DDC701025B3BB` |

## Optimization assessment

### v2.29 private tiling/Z-cull

Private tiled color/depth registration and private Z-cull can reduce bandwidth
or improve depth rejection, but they do not reduce the primary shaded-pixel
count beyond the existing scene scale. They also add tile alignment, pitch,
compression/tag, address-range, and lifetime constraints that are sensitive on
physical RSX hardware.

The returned physical matrix did not isolate these features. Do not port them
until v2.28 proves that scaled tiled-display resolving is stable and physical
telemetry shows memory/depth pressure remains material.

Verdict: deferred, not accepted, not disproven in isolation.

### v2.30 target-aware adaptation

Target-aware adaptation is policy, not throughput. It can choose a lower scale
sooner for a 60-FPS target or wait for the 50-ms VBlank tier at 30 FPS, but it
does not itself reduce the cost of a given frame at a given scale.

It may be useful after a physical fixed-scale curve identifies working scales.
Reimplement the small policy in the original source only then; do not import a
returned object or EBOOT.

Verdict: conceptually useful later, irrelevant to initial compatibility and
insufficient as the main optimization.

### Returned v2.31 compact target

The compact target preserves the same scaled world rectangle and therefore
does not create a new pixel-count reduction. It changes several hardware
invariants simultaneously and already produced failure evidence.

Verdict: rejected permanently for this line.

### Original v2.28 textured tiled resolve

v2.27's reduced scene works only when its scale-engine destination is linear.
If the user's approximately 20-FPS test used tiled or tiled+Z-cull display
targets, v2.27 could have silently retained full-resolution scene rendering.
v2.28 addresses exactly that gap: keep the private scene linear and full-pitch,
restore the tiled display eye, invalidate texture cache, then sample the scene
with one ordinary 3D quad before native-resolution UI.

This keeps the established surface dimensions and avoids v2.29-v2.31 private
tile/compact changes. It is the strongest current optimization candidate.

Static review found one conservative hardware-test requirement: in the tiled
path, `ps3_rsx_scene_sync 1` does not issue a GPU-idle wait around the
render-target-to-texture resolve; `ps3_rsx_scene_sync 2` adds a pre-resolve
idle. Same-FIFO ordering plus texture-cache invalidation may be sufficient, but
the first physical tiled test should use mode 2. Only reduce it to mode 1 after
the complete safety gate passes.

Verdict: highest-priority physical candidate, but not yet accepted.

## Hardware-compatible continuation procedure

No step below uses RPCS3.

### Phase A: remove test-state contamination

Before installing anything, back up:

```text
/dev_hdd0/game/QUAKE2000/USRDIR/baseq2/config.cfg
/dev_hdd0/game/QUAKE2000/USRDIR/baseq2/autoexec.cfg
/dev_hdd0/game/QUAKE2000/USRDIR/boot-trace.log
/dev_hdd0/game/QUAKE2000/USRDIR/baseq2/qconsole.log
/dev_hdd0/game/QUAKE2000/USRDIR/baseq2/save/
```

After backup, remove or rename the recovery-test `autoexec.cfg`. Do not put a
replacement in an update package. Preserve the existing config separately;
if needed, perform one controlled run with clean defaults.

### Phase B: establish the exact v2.27 physical baseline

1. Verify the finalized v2.27 package hash is
   `7D8AF4F1FC368D426E8CC4E4F7BEFAE2415EA6F4ED7E63A6EDB137969796DB47`.
2. Install that untouched package over the existing full base installation.
3. Reboot the PS3.
4. Boot normally to the menu. Do not auto-restart video or auto-load a map.
5. Manually load `base1`, play, transition, save, load, return to menu, and
   exit through XMB.
6. Preserve logs and exact settings.

If this exact package fails after stale `autoexec.cfg` is removed and the
console is rebooted, repeat the same process with the exact v2.19 package as
the deeper known-good fallback. Do not rebuild either package.

### Phase C: obtain a physical fixed-scale curve on v2.27

Use mono 720p, filters off, 30-FPS target, `gl1_ztrick 0`, and linear targets
for the first curve. Enter settings manually after normal boot:

```text
set ps3_rsx_stats 1
set ps3_rsx_scene_adaptive 0
set ps3_rsx_tiled_targets 0
vid_restart
```

Measure explicit `ps3_rsx_scene_scale` values 1.000, 0.875, 0.750, 0.667,
0.583, and 0.500 on the same route. Collect at least five warmed 120-frame
telemetry blocks per scale.

- Strong cadence improvement as scale falls proves fill/bandwidth pressure.
- Little cadence response with high PPU stage time proves CPU/submission work.
- Low PPU time plus 33.3/50-ms quantization points to RSX/presentation or
  synchronization.
- A scale that reaches 30 FPS must still pass transitions, save/load, and XMB
  exit before acceptance.

### Phase D: isolate original v2.28 safely

Install the untouched original v2.28 finalized package; do not rebuild it and
do not include an `autoexec.cfg`.

Test in this order, manually:

1. Linear targets, `ps3_rsx_scene_scale 1`, normal menu and gameplay. This
   keeps the new resolve inactive and checks package/engine compatibility.
2. Linear targets at the best known v2.27 fixed scale. This should reproduce
   the established scale-engine path.
3. Tiled targets mode 1 at that scale with `ps3_rsx_scene_sync 2`.
4. If safe, repeat mode 1 with sync mode 1 and compare cadence/waits.
5. Only then test tiled+Z-cull display mode 2; keep the private scene linear.
6. Test stereo only after mono passes.

The tiled run must record:

```text
RSX scene scale: tiled 3D textured resolve active
```

If linear/full-resolution v2.28 fails, the tiled resolver was not executing;
investigate artifact/package/ABI differences rather than renderer tiling. If
linear works and the first tiled test fails, restore v2.27 and focus on the
render-target-to-texture transition and synchronization.

### Acceptance gate

A candidate is hardware-compatible only after all of these pass:

- Cold boot to the normal menu.
- Manual `base1` load and at least one level transition.
- Save and load.
- Correct world, HUD/UI, lighting, blend, and sound.
- No freeze or responsive black screen.
- Return to menu and clean XMB exit.
- Five consecutive warmed 120-frame blocks with zero `floor_misses` for the
  30-FPS package.
- Repetition on representative heavy maps after `base1`.

60 FPS is a separate branch after 30 FPS is frozen as a rollback package. It
requires zero physical `target_misses` at the 16,667-us budget; a menu counter
or brief visual observation is not enough.

## Decision tree after physical telemetry

- **Scale strongly improves cadence and v2.28 tiled resolve is stable:** select
  the highest scale that clears the 30-FPS gate. Adaptation can later start at
  that proven scale.
- **Scale improves cadence but 0.75 misses 30:** test 0.667 and 0.583 before
  adding private tiling or compact allocations.
- **Scale reaches 0.5 with little improvement:** stop scene-resolver work. Use
  `PPU stages` and RSX counters to choose the measured dominant stage.
- **High `world` PPU time:** inspect material/lightmap page transitions and BSP
  submission while preserving persistent static geometry.
- **High `entities` time:** inspect prepared MD2/lighting reuse and entity
  batching.
- **High `dlights`, texture updates, renames, or finishes:** isolate dynamic
  lightmap marking/upload retirement.
- **Low PPU time but high waits/cadence:** investigate flip pacing, resolve
  barriers, RSX fill/bandwidth, and command completion one variable at a time.
- **v2.28 tiled mode fails only at sync 1:** retain sync 2 for compatibility;
  performance is secondary to a functioning command stream.

## Final review status

The returned machine made progress by ruling out its own reconstructed
packages and by demonstrating that compact-target work has poor value. It did
not produce a hardware-compatible successor, did not preserve a deployable
accepted package, and did not transfer enough artifacts to audit v2.29-v2.31
locally.

This original computer does retain a coherent recovery and optimization path:
the exact original v2.27 package, its build/stage artifacts, the exact original
v2.28 two-object candidate, and the toolchain that produced them. That is the
authoritative path forward unless the missing genuine v2.31 artifact is found
and physically re-proven.

## Implemented after this review

v2.32 was built from the original v2.28 object set with one hardware-safety
change: the tiled render-target-to-texture resolve now always retires its
private color-target producer before sampling. Exactly one of 141 linked
objects changed. The finalized three-entry physical-test package is:

```text
q2ps3/q2ps3-rsx-v232-hwcompat-update.gnpdrm.pkg
SHA-256 14F3F2965363C4CC7BCC9202C94F46E2E8630FD44FAD7E947AB7EE79070B9B15
```

See `q2ps3/V232_PHYSICAL_TEST.md` for the no-autoexec, physical-PS3-only test
and rollback procedure.
