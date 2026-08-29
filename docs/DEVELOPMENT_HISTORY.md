# Quake II for PlayStation 3

This tree updates the original PSL1GHT/Yamagi Quake II port with native PS3
UDP networking, optional stereoscopic output, PlayStation Move gyro aiming,
optional SPU frame acceleration, DualShock 3 controls, sound, and the existing
software renderer.

Only the GPL engine and game-code sources are included. Quake II's commercial
game data is not redistributable; use `pak0.pak` from a legally owned copy.

v2.37 is the prebuilt higher-version escape hatch for the v2.36 physical test.
It restores the exact v2.21/v2.35 EBOOT.BIN and should be installed only if the
one-object v2.36 raster-state experiment loses visible output.

v2.36 starts conservative optimization from the exact v2.21 hardware baseline.
It changes only `rsx_gl.o`, importing the isolated v2.24 physical viewport and
scissor state filter. Repeated same-target raster methods are omitted, while
real mono/stereo eye transitions are preserved. No video, GL1 camera/UI,
stereo-blend, input, sound, online/OSK, gameplay, or scene-scaling object is
changed. This is deliberately a small CPU/submission optimization before any
fill-rate mechanism is reconsidered.

v2.35 is the new physical-console compatibility baseline. It packages the
owner-confirmed working v2.21 EBOOT.BIN byte-for-byte under 02.35 metadata, with
no rebuild, relink, resign, or configuration payload. Later optimization work
must branch from this exact executable and change one auditable object at a
time; the post-v2.21 video/renderer combination is no longer presumed safe.

v2.34 is a byte-identical reissue of the physically accepted v2.27 EBOOT.BIN
under 02.34 metadata. It exists because v2.33 remained black even while audio
and blind menu navigation worked with ordinary 720p forced and archived stereo
disabled. No object is rebuilt or relinked, so this package cleanly removes the
v2.33 video selector and the later v2.28/v2.32 scene-resolve branch without
altering installed configuration, game data, saves, bindings, or addresses.

v2.33 is a physical-console visible-output recovery update. A v2.32 system that
still plays audio and accepts blind menu input is running the engine, so this
build restores the accepted v2.27 executable object set and changes only the PS3
video object. It forces ordinary 1280x720 output and disables archived stereo
for that launch, using the same selector that previously recovered visible
output in v2.03. The separate RGBW diagnostic framebuffer is not compiled in.
The package contains no configuration or startup-command payload, so installed
game data, saves, bindings, address-book entries, and other user files remain
untouched.

v2.32 is the original-computer hardware-compatibility continuation of v2.28.
It keeps the private reduced scene linear and the display target unchanged,
but always retires the private color-target producer before the tiled
render-target-to-texture resolve. This conservative wait deliberately trades
some possible throughput for the first trustworthy physical test; it can be
relaxed only after the tiled path boots, loads, plays, transitions, saves,
loads, and exits cleanly on PS3 hardware. No returned-machine compact target,
private tiling/Z-cull, adaptive-policy object, startup command, or config file
is included. Linear output and all unrelated renderer objects remain v2.28.

v2.28 lets the adaptive scene-resolution bridge remain active with effective
tiled and tiled-plus-Z-cull display targets. Those layouts resolve the private
linear scene through one ordinary fixed-shader quad into the restored tiled
eye; linear output retains the v2.27 scale-engine resolve. The session-only
`ps3_rsx_scene_tiled_resolve 0` switch restores the direct full-resolution
renderer for tiled-layout recovery testing.

v2.27 defaults `ps3_rsx_scene_sync` to `1`. Native triangle submission is
already closed at the world/UI boundary, so the redundant full RSX drain before
the local-memory scale blit is omitted. The post-blit barrier remains before
native-resolution UI rendering. Value `2` restores both barriers and `0` is a
FIFO-only diagnostic. The setting has no effect when `ps3_rsx_scene_scale 1`
selects the proven direct renderer.

v2.25 keeps the physically accepted feature baseline but gives the two named
performance profiles a lower-resolution 3D scene surface. The 720p Performance
profile normally shades the scene at 960x540 and resolves it into the unchanged
1280x720 output; HUD, menus, console, text, OSK and display filters remain at
native output resolution. The 1080p Quality profile shades approximately
1280x720. Custom remains native. `ps3_rsx_scene_scale 0` selects this automatic
policy, an explicit `0.50` through `1.00` overrides it, and `1.00` restores
native scene rendering. `gl1_ztrick 0` remains required; v2.28 supports both
linear and effective tiled framebuffer layouts. For a physical performance
capture, enable `ps3_rsx_stats 1`; `render_avg`, `cadence_avg`, `floor_misses`,
and the PPU-stage line distinguish submission cost from remaining fill limits.
v2.26 makes zero's automatic policy cadence-aware: two consecutive gameplay
intervals above 40 ms lower one bounded scene-scale step, directly responding
to the PS3's observed 50 ms VBlank tier. `ps3_rsx_scene_adaptive 0` disables
that feedback while retaining each profile's initial scale. Explicit nonzero
scene scales are never modified.

## Feature status

| Feature | Status |
| --- | --- |
| Single-player and local game | Implemented |
| DualShock 3 | Implemented |
| PlayStation Move | Optional Move wand + PlayStation Eye tracking, calibration overlay, gyro fallback, and buttons |
| LAN/Internet play | IPv4 UDP client/server, DNS names, and LAN broadcast |
| Stereoscopic 3D | Optional HDMI 720p frame packing with runtime depth scaling |
| Video output | XMB-configured mode or selectable 720p performance output |
| RSX presentation | Double-buffered scanout with selectable 2D VSync |
| Native RSX renderer | Preview build; hardware geometry, textures, depth/stencil, and blending |
| SPU acceleration | Optional 1-5 worker palette conversion and output scaling |
| Native sound | 48 kHz stereo effects backend; Ogg music when tracks are installed |

On PS3, selecting a difficulty starts `base1` directly. This keeps campaign
startup independent of the legacy intro cinematic decoder; cinematics remain
available to the engine and their large work buffers are stored outside the
constrained PPU main-thread stack.

The savegame copier likewise uses a persistent transfer buffer instead of a
64 KiB local array, allowing the initial `save0` autosave to complete before
the local client joins the newly spawned map.

The sphere-topped PlayStation Move wand is handled by libgem, separately from
the Navigation controller's ordinary gamepad path. Enable it under
`gamepad / Move settings`, then choose `calibrate / recenter PS Move`. The
on-screen alignment prompt reports camera/controller state and accepts the MOVE
button to begin calibration while the sphere faces the PlayStation Eye. Camera-
corrected angular velocity is used while tracked, with inertial fallback during
brief occlusion. `psmove_status`, `psmove_calibrate`, and `psmove_reinit` are
available at the console for diagnostics and recovery. Move/Eye modules are
never initialized during title boot, even if `psmove_enable` was archived as
enabled. Use one of those in-menu actions after reaching the menu; this prevents
already-connected hardware from blocking startup.

The default renderer is CPU-based. A 1080p display buffer is supported, and
`r_mode 21` requests a native 1920x1080 software framebuffer, but 1080p/60 is
not a realistic guaranteed target for that path. The recommended software mode
is a 640x480 internal framebuffer scaled to the configured 720p or 1080p output.
The optional native RSX build described below renders geometry and textures on
the GPU. Stereo renders the scene twice in either renderer.
The scanout path uses two RSX buffers so the PPU/SPU never writes into the
surface currently being displayed. The 2D VSync setting selects synchronized
or immediate flips; stereoscopic frame packing always uses synchronized flips.

### Native RSX preview

Set `PS3_NATIVE_RSX=1` to build the native renderer instead of the software
renderer. It reuses the engine's GL1 BSP/model/lightmap frontend but implements
the drawing API with direct PSL1GHT RSX commands and compiled Cg programs; no
OpenGL library or software world rasterizer is linked into that build.

```sh
export LD_LIBRARY_PATH="$PS3DEV/cg-runtime/usr/lib/x86_64-linux-gnu"
make -f Makefile.ps3 PS3_NATIVE_RSX=1
```

The native path is currently a hardware-test preview. The software build stays
the default until world rendering, UI, campaign maps, stereo, and effects have
all passed physical-PS3 validation. v1.13 passed the initial physical-console
2D gameplay test; v1.14 adds native 1280x1470 HDMI frame-packed stereo for its
next hardware-validation milestone. v1.15 adds native RSX mip chains and the
corresponding nearest, bilinear, and trilinear minification modes; it requires
a physical-console regression pass before replacing the v1.13 baseline. v1.16
enables fenced texture renaming for dynamic lightmaps by default, removing a
full GPU synchronization from each in-frame update. Retired allocations are
bounded to 16 MiB and released only after the ordered display flip completes.
It passes an RPCS3 boot-to-`base1` smoke test with both native mip sampling and
dynamic texture renaming active; physical-console regression is still required.
v1.17 exposes RSX NPOT textures and optional 1x-16x anisotropic sampling to GL1,
uses the native CPU mip generator instead of the legacy texture rescale path,
and keeps repeated cinematic uploads on the fenced update path. Stereo
screenshots now read the currently selected eye instead of the full HDMI frame.
v1.18 keeps the native shader pair resident and caches unchanged MVP and sampler
state across GL1 draws. Real uploads and renamed texture offsets still trigger
the required cache invalidation or rebind, while repeated world polygons avoid
redundant RSX state methods. v1.19 makes the Brightness control functional in
the native build by applying a PS3-only gamma curve while textures are uploaded.
Changing brightness performs one full renderer reload so existing textures are
rebuilt; it adds no per-fragment cost during gameplay. v1.20 converts compatible
triangle fans, strips, quads, and lists into contiguous RSX triangle batches.
World and lightmap surfaces sharing state can therefore use one hardware draw;
all matrix, texture, blend, depth/stencil, raster, stereo-eye, upload, clear,
readback, and frame transitions remain explicit ordering boundaries. Set
`ps3_rsx_batch 0` to disable batching at the next frame boundary for diagnosis.
v1.21 mirrors fixed-function state on the RSX side and suppresses only setters
whose effective hardware value is already active. This prevents repeated GL1
setup calls from prematurely flushing v1.20's batches. For measurement, set
`ps3_rsx_stats 1`; the boot trace records aggregate API-draw, hardware-draw,
batch, flush, vertex, filtered-state, texture-update, rename-byte, and forced-
finish counts every 120 completed frames. v1.31 also records completed-flip wait
and PPU-side renderer intervals with microsecond timestamps.
Set `ps3_rsx_state_filter 0` to restore repeated state emission at the next
frame boundary without restarting the renderer.
v1.22 registers and pumps the PS3 system utility callback so an XMB Exit Game
request can interrupt a pending flip wait. It saves configuration and stops
audio and input normally, but leaves final RSX/GCM resource reclamation to LV2
on that process-exit path because XMB may already own the display queue. Normal
console `quit` and renderer restarts continue to perform the full RSX teardown.
On physical v1.22 hardware, returning to Quake II's main menu before choosing
XMB **Quit Game** is the confirmed safe shutdown sequence. Direct XMB exit from
active gameplay remains a v1.23 physical-validation target.
v1.23 replaces expanded fan/strip/quad geometry with bounded 16-bit indexed
RSX batches and checks the exit callback again before queuing a flip. Indexed
submission is initially off pending its physical-console pass; set
`ps3_rsx_indexed_batch 1` to enable it at the next frame boundary, or `0` to
return to expanded batching. Its frame-packed path also acquires by pass order
and clears each eye, preventing stale weapon, sprite, and console imagery from
accumulating when `r_clear` is disabled.
v1.24 adds optional RSX tiled color targets, compressed tiled Z24S8 storage,
and hardware Z-cull metadata. Set `ps3_rsx_tiled_targets 1`, then run
`vid_restart`; use `0` plus another restart to return to the linear allocation
path. The backend also stays linear if legacy `gl1_ztrick` is enabled because
that option alternates depth direction while one Z-cull region does not. Tiled
targets remain off by default and the switch is deliberately not archived, so
a complete relaunch always recovers the linear path during physical testing.
Identical warm-cache RPCS3 Vulkan `base1` runs completed 120 frames with the same 91,810
GL1 draws and 3,331 hardware submissions in either mode.
v1.25 specializes vertex packing for GL1's float client arrays, avoiding the
per-component generic type/normalization decoder while preserving identical
RSX vertex data. Unusual, unsupported, or four-byte-unaligned arrays continue
through the generic decoder, and `ps3_rsx_fast_arrays 0` forces that fallback
at the next frame boundary. RPCS3 exercised the fast path for every streamed
vertex in a 120-frame `base1` run. Fast, generic, and fast-plus-indexed paused
tests completed the same draw workload; the combined indexed run packed 406,272
source vertices and emitted 668,514 indices in 3,298 RSX submissions. Disassembly also
confirms that source components use individual PPU float loads rather than the
alignment-sensitive paired copies that affected the earlier preview.
v1.26 fixes the remaining ownership gap in direct XMB shutdown: the renderer
itself now avoids `rsxFinish()`, unmapping, and allocation frees after the
sysutil exit callback fires, matching the later GCM host-context guard. Normal
console `quit` and renderer restarts retain complete teardown. This targets the
physical result where menu-first XMB exit is safe but direct gameplay exit can
freeze; the direct path remains unconfirmed until a hardware test passes.
v1.26 also adds an optional PPU-friendly dynamic stream location. Set
`ps3_rsx_stream_main_memory 1`, then run `vid_restart`, to map the 8 MiB vertex
and 2 MiB index rings in main XDR memory instead of writing them through the
RSX-local aperture. Since v1.32, dirty PPU cache lines are explicitly stored to
XDR before one ordering barrier and RSX vertex-cache invalidation per hardware
submission; mapping failure automatically uses local memory.
Set `0` plus another restart to return to local streaming. The switch defaults
off and is not archived, so a full relaunch always recovers the established
path. Matched RPCS3 indexed `base1` tests completed 120 frames in both modes,
and cleanly unmapped the XDR ring on normal shutdown.
v1.27 avoids copying an entire RSX texture for updates made before that exact
allocation's first sample in the current frame. BeginFrame's completed flip is
the fence for prior readers; after a draw samples the allocation, the existing
copy-on-update path still renames it and retires the old storage through the
next flip. `ps3_rsx_texture_frame_reuse` defaults to `1`; set it to `0` to
restore v1.26's always-rename behavior without restarting. Matched 120-frame
RPCS3 `base1` runs produced the same draw workload and no GPU-wide finishes.
Reuse-on performed 87 of 131 updates in place and renamed 2,816 KiB,
while reuse-off renamed 8,576 KiB across 134 updates, reducing copied rename
traffic by about 67 percent in that scene.
v1.28 adds a coherent local-to-main RSX screenshot path and corrects the world
corruption it revealed. RSX hardware face culling is suppressed because it
rejects Quake's streamed fans, while the GL1 BSP, surface, and frustum culling
paths remain active. Frame-packed stereo now expands the CPU frustum for each
asymmetric eye, clears unused pixels to black, and reads one 720-line eye at a
time. RPCS3 produced complete, distinct top and bottom `base1` views with the
full optimized linear-renderer configuration and no normal-frame GPU finishes.
The optional tiled-target path still defaults off: it runs with Z-cull active,
but screenshot readback does not currently detile that surface reliably.
v1.29 resolves the remaining RSX face-culling convention instead of leaving
the hardware unit suppressed. It maps Quake GL1's FRONT/BACK selection directly
and selects a CCW RSX front face. RPCS3 framebuffer tests covered all four
mapping/winding combinations; the default retained the full BSP and passed
right- and left-handed weapon rendering, a moved gameplay view, and two
complete frame-packed stereo eyes. `ps3_rsx_hw_cull_mode 0` restores v1.28's
suppressed-hardware-cull fallback for the current session. Modes 2 through 4
are retained only for renderer diagnosis.
v1.30 completes the tiled-target screenshot path used for native validation.
Readback now preserves the tile allocation base, supplies packed-eye selection
as a source coordinate, and waits for the RSX 3D backend to make the active
surface visible before starting the 2D local-to-main blit. Clean RPCS3 captures
passed for linear targets, tiled color/depth plus Z-cull, and both eyes of a
tiled 1280x1470 frame-packed surface. The wait occurs only when a screenshot is
requested, so normal rendering retains the zero-finish frame path. Tiled
targets remain opt-in with `ps3_rsx_tiled_targets 1` plus `vid_restart` until
they pass on physical hardware.
v1.31 adds the timing evidence needed for that hardware pass. With
`ps3_rsx_stats 1`, each 120-frame record includes `wait_avg`/`wait_max` for time
blocked on the preceding display flip, `render_avg`/`render_max` for CPU time
from flip release through GL1-to-RSX command generation and final batch flush,
and `total_max`, `over20`, and `over34` for their sum. These are renderer-owned
intervals, not complete engine frame time and not a direct RSX GPU-duration
query. Statistics remain off by default, so normal play has no timing-call or
trace-I/O overhead. The exact signed v1.31 EBOOT completed a 240-frame linear
RPCS3 smoke test with a clean capture and normal full teardown. Additional
packaged-EBOOT runs captured complete `base1`, `city1`, `waste1`, `space`, and
`boss2` scenes while exercising repeated map registration, dynamic lightmaps,
warp and sky surfaces, entities, weapons, and UI. The identical ordered sweep
also passed with tiled color/depth targets and Z-cull active. Linear/tiled
framebuffer differences stayed confined to time-dependent animation and weapon
bob; world geometry remained complete in both. Every telemetry interval
completed without a normal-frame GPU-wide finish, flip timeout, or renderer
fault. RPCS3 timing is not evidence of physical RSX performance, so the tiled
path remains opt-in until the console A/B below.

v1.32 fixes cache visibility for the optional mapped-main-memory vertex/index
stream. Mapping XDR and ordering PPU stores do not themselves publish dirty PPU
cache lines for RSX reads. The renderer now stores each completed 128-byte cache
line to memory, orders the whole vertex/index pair once, then invalidates RSX's
prior vertex-cache view before emitting the draw. GPU-to-PPU screenshot DMA
keeps a separate writeback-and-invalidate path. The exact signed v1.32 EBOOT
completed five-map RPCS3 sweeps with main-memory streaming on both linear
targets and tiled targets plus Z-cull; all ten map captures were complete, both
runs shut down normally, and normal rendering retained zero GPU-wide finishes.
An additional exact packaged-EBOOT run enabled mapped streams, tiled targets,
Z-cull, and 1280x1470 frame-packed stereo simultaneously. Its separately read
top and bottom eyes were complete and distinct, and the run retained normal
teardown with no normal-frame finish. The stream switch remains session-only
and defaults off until its physical-PS3 performance pass.

The same release narrows direct gameplay-to-XMB shutdown to the operation not
present in the physically safe menu-first sequence. After a sysutil Exit Game
request, it bypasses active local-server/game-module teardown and leaves those
process-owned resources, along with guarded RSX/GCM allocations, to LV2. Normal
console `quit` still performs full shutdown. Until direct Exit Game passes on a
physical PS3, returning to the main menu before using XMB **Quit Game** remains
the safe fallback.

Physical v1.36 testing confirmed native audio and projected shadows, but also
proved that synchronizing the recycled vertex arena was not sufficient to fix
the reported doubled/stale text. v1.37 separated orthographic UI geometry from
the world renderer's indexed path: console characters, menu panels, HUD
elements, fills, and fades expand into a deterministic non-indexed triangle
stream, with a fresh 2D vertex-fetch invalidation at every submitted batch.
Charset coordinates are also inset by half a texel inside each 8x8 atlas cell.
Indexed batching remains enabled for world and model geometry, where its
bandwidth reduction matters.

Physical v1.37 photographs and its trace showed the entire UI and console once
in the upper stereo eye and again in the lower eye with the HDMI frame-packing
gap between them. v1.38 attempted a blocking GameOS mode confirmation to test
whether the raw 1280x1470 surface raced the HDMI transition, but that EBOOT
returned to XMB with `No usable renderer found`. The later exported trace proved
that it had reached `main()`, selected 0x81/1280x1470 successfully, and received
video state 1 from the completed blocking call. The inherited backend then
mistakenly rejected that documented `VIDEO_STATE_ENABLED` value because older
asynchronous physical runs had exposed zero at the same checks. v1.40 accepts
both observed usable states, retains busy/unknown rejection, and restores the
blocking transition so the stereo/text fix can actually initialize RSX. v1.39
remains the exact v1.37 rollback package.

v1.41 adds a normal 1280x720 top-and-bottom stereo output option for display
chains that expose the raw 1280x1470 frame-packed surface, and sends ordinary
console, HUD, menu, fill, fade, and cinematic quads directly to the native RSX
stream. v1.42 extends the typed native stream to BSP base faces, lightmap
chains, flowing and turbulent surfaces, and sky faces. Those high-volume paths
retain the existing indexed/expanded batching and recovery cvars, but no longer
repeat generic client-array state changes and format validation for every
visible fan. This is a PPU command-generation optimization; it does not remove
lighting, shadows, shaders, texture filtering, or depth testing.

v1.43 restores PSL1GHT's required import-table link pass after the withdrawn
v1.41/v1.42 packages exposed a pre-main XMB crash. v1.44 batches complete
console rows, HUD lines, inventory labels, and ordinary menu strings into one
contiguous native glyph submission instead of reserving one quad per character,
cutting PPU command-generation overhead. UI vertices also move to a dedicated
cache-coherent mapped-XDR arena by default, so console/menu updates no longer
rewrite the RSX-local world stream that produced stale physical fetches. Set
`ps3_rsx_ui_main_memory 0` followed by `vid_restart` only to A/B the previous
shared stream. With `ps3_rsx_stats 1`, `ui_strings` and `ui_glyphs` confirm use
of the batched frontend path. The same release specializes fan-index generation
for typed BSP/lightmap/water/sky draws. Physical-console validation remains
required before treating either change as the final text or pacing fix.

v1.45 routes the remaining right-aligned qmenu labels, center-print messages,
and credits through the same complete-string path. It also reduces the PS3
main loop's no-frame polling rate from one LV2 sleep syscall every 5
microseconds to one every 50 microseconds. The established render/network delta
reset is unchanged; a residual-cadence experiment was removed after validation
exposed horizontally incomplete world frames.

v1.46 is a boot-safe repack of that same executable with newer install metadata.
A controlled A/B rejected both a desktop-style 72 Hz scheduling target and a
different default eye for stereo screenshots after diagnostic captures became
nondeterministic. Neither experiment is in the release. The proven 60 Hz PS3
cadence, zero-residual timing, complete-string text path, and XDR UI stream remain
unchanged.

v1.47 keeps those proven paths and makes the XDR UI stream rotate across a
16 MiB mapping instead of rewriting offset zero on every vblank. A flip-fenced
boundary wrap always leaves the prior 8 MiB worst-case frame budget available,
so console/menu/HUD geometry and draw ordering are unchanged while physical RSX
sees fresh fetch addresses for many frames. If the larger mapping is unavailable,
the renderer automatically retries the v1.46 8 MiB coherent stream. Set
`ps3_rsx_ui_vertex_ring 0` followed by `vid_restart` only to force that fallback
for comparison.

v1.50 adds a dedicated native two-sampler Cg program for compatible opaque BSP
surfaces. It samples the base texture and baked lightmap in one draw, restoring
the darkness that the native equal-depth lightmap replay failed to apply and
removing the second fill pass for most static world geometry. A repeatable 720p
`base1` run reduced frontend draws from 225,664 to 179,170, packed vertices from
1,165,644 to 942,926, and indices from 2,094,840 to 1,703,376 per 120 frames.
Material-pair sorting raises hardware submissions from 8,284 to 13,992 in that
same interval, so the net benefit on resolution-bound physical RSX must be
measured on the console rather than inferred from RPCS3. The feature defaults
on and is archived; `ps3_rsx_combined_lightmaps 0` restores the established
two-pass fallback immediately. The new shader reuses COLOR0 storage as its
TEXCOORD1 input, preserving v1.47's exact 40-byte console/menu/HUD vertex layout
and 16 MiB rotating XDR lifetime fix.

v1.51 moves the existing native VSync wait ahead of the main loop's time sample,
keeping input, simulation, RSX command generation, and presentation on the same
hardware cadence. This avoids first meeting a separate software 60 Hz threshold
and then waiting again for the flip, which could turn a small wake-up overrun
into a missed refresh and visible movement jerk. `ps3_vsync_prewait 1` is the
archived default; set it to `0` to restore the bounded polling fallback. The RSX
statistics line now includes average/maximum completed-frame cadence and counts
intervals above 20 and 34 ms. The v1.50 text vertex bytes, rotating coherent UI
arena, full acquired-surface clear, lighting path, and game assets are unchanged
so the physical pass can isolate pacing and the outstanding stale-text report.

v1.52 keeps that pacing and text path intact while splitting the combined
lighting pair's sampler dirty state. World texture chains usually retain one
base image across several lightmap pages; only the changed lightmap descriptor
is now emitted at those page boundaries. Program switches, texture replacement,
filter/wrap changes, and allocation renames still invalidate the affected unit,
and pixel updates retain the existing texture-cache fence. With
`ps3_rsx_stats 1`, `lm_base_sets` and `lm_page_sets` report the descriptors
actually sent during each 120-frame interval.

v1.53 also removes redundant PPU setup from that combined world path. The
feature gate, base texture binding, and overbright scale are resolved once per
opaque world base-texture chain. One reusable lightmap bucket table is cleared
as its occupied buckets are consumed, and only the first through last occupied
lightmap pages are scanned. Surface eligibility, material pairing, ordering,
and lighting results are unchanged. The RSX backend and all console/menu/HUD
objects are byte-identical to v1.52, preserving the text diagnostic unchanged
for a physical-console A/B test.

v1.54 changes the memory path behind that text diagnostic without changing its
glyphs or frontend calls. Console, menu, HUD, and other 2D vertices are still
batched in the rotating XDR construction arena, but completed ranges are fully
evicted from the PPU cache and copied by RSX into an eight-MiB local staging
arena before drawing. Vertex fetches bind only the immutable local destination,
which removes direct non-coherent XDR reads and CPU-rewritten RSX addresses from
the physical stale/doubled-text path. The local arena is reset only after the
previous flip completes. If its allocation fails, the renderer logs the event
and retains the v1.53 direct-XDR fallback; `ps3_rsx_ui_local_stage 0` forces
that fallback for comparison. The v1.53 world optimization remains unchanged.

v1.55 preserves that staged UI implementation exactly and tightens the
combined-world fallback. The texture-chain walk already knows the animation
frame used to form each base-image chain and already classifies lightmap state
before deciding whether a surface can use the combined program. Ordinary
fallback surfaces now reuse both results rather than repeating texture
animation and scanning their light styles a second time. All draw decisions,
dynamic updates, material bindings, and visual results remain unchanged.

v1.56 keeps the staged UI construction, cache publication, RSX-local copy,
40-byte vertex format, and shaders unchanged. It reduces PPU work in the
combined world path by preparing the shader, sampler pair, stream/index mode,
and overbright scale once for each occupied base-texture/lightmap-page bucket.
Every BSP fan in that bucket then takes a prepared append path. Sparse empty
pages are skipped, material transitions remain hard batch boundaries, and
inline brush models continue through the fully validated standalone entry
point. This changes submission overhead without changing lighting math,
geometry, texture coordinates, or draw order.

v1.57 adds a dedicated procedural RSX fragment program for optional display
filters. **scanlines**, **RGB aperture grille**, **vignette**, **CRT soft**, and
**CRT strong** are each one multiplicative eye-sized pass after the world and
all 2D overlays. They require no offscreen framebuffer and no mask texture;
**off** returns before submitting any filter work. Frame-packed and top/bottom
stereo close each eye separately, so both eyes receive exactly one pass. Video
Options exposes live filter strength, texture filtering, dynamic lighting,
damage/pickup/underwater color blends, and lightmapped/model overbright.

The same release adds real 720p/1080p performance presets. The 720p preset
forces 1280x720 scanout, caps presentation at 60 FPS, and enables all proven
native batching, state filtering, texture lifetime, combined-lightmap, and
GPU-local staged-UI paths. The 1080p preset requests 1920x1080 scanout and caps
at 30 FPS for stable pacing; unavailable output modes return safely to the XMB
timing. These are hardware targets, not guarantees: use `ps3_rsx_stats 1` on a
physical console to confirm sustained cadence. Experimental tiled/Z-cull
targets are deliberately not enabled by either preset.

v1.58 closes the remaining same-address reuse hole in v1.54's text staging.
The source XDR ring rotated, but its RSX-local copy destination still restarted
at offset zero every frame. The local stage now prefers a 16 MiB rotating ring
and wraps only after the prior flip has fenced its contents while reserving a
complete 8 MiB worst-case frame budget. If that larger local allocation cannot
be made, initialization automatically retries the established 8 MiB staged
path before allowing the direct-XDR diagnostic fallback. This costs at most an
additional 8 MiB of RSX local memory and does not add a normal-frame GPU finish.
The same build culls completely offscreen 2D rectangles and glyph runs before
packing or transfer and stops scaled consoles from traversing invisible rows.
The expected result is lower UI submission cost and fresh physical fetch
addresses across ordinary frames; final confirmation still requires the
physical-console menu/console test.

v1.59 reduces fixed PPU work in every native world pass without changing the
rendered result. The BSP walk records only base textures that actually receive
visible opaque surfaces; the texture-chain draw then visits those entries in
the same ascending material order rather than scanning all 1,024 image slots.
Within each visible material, the combined-lightmap path similarly consumes
only occupied lightmap pages in ascending order. This matters twice per scene
in stereoscopic modes. With `ps3_rsx_stats 1`, each 120-frame report now also
includes `target_fps`, `target_samples`, and `target_misses`: the 720p profile
counts cadence samples above 20 ms against its 60 FPS target, while the 1080p
profile counts samples above 34 ms against its paced 30 FPS target. Statistics
remain completely disabled during normal play unless explicitly enabled.

v1.60 makes the rotating local UI ring the final PPU write destination instead
of filling it with an asynchronous RSX transfer. Console, menu, HUD, fills, and
other orthographic batches are packed once at fresh GPU-local addresses and
retain the existing cache invalidate before vertex fetch. The flip wait fences
the ring before its boundary wrap, and the allocation is the same 16 MiB used
by v1.59, so this neither adds memory nor changes glyph geometry, texture
coordinates, shaders, draw order, or stereo routing. It also removes XDR cache
publication plus one full vertex copy from the normal UI path.

`ps3_rsx_ui_local_stage` now has three diagnostic modes that take effect after
`vid_restart`: `2` is the preferred direct rotating local ring, `1` restores
the v1.59 XDR-to-local DMA stage for physical A/B testing, and `0` restores
direct XDR fetch. If the 16 MiB local ring cannot be allocated, initialization
automatically retains the established staged or direct-XDR fallback and records
the selected path in `boot-trace.log`.

v1.61 reduces per-frame PPU and RSX submission overhead around dynamic
entities. The native path gathers translucent entities during the existing
solid pass, retaining their original order but avoiding a second full entity
array scan. Frames with no translucent entities avoid the otherwise redundant
depth-write state pair. Sprite geometry is constructed before state changes so
fully off-frustum sprites can be rejected early; visible sprites use the native
direct textured-fan submission helper. Beam endpoint/radius bounds receive the
same conservative early rejection before normalization and ring construction.
These paths preserve every visible entity's geometry and order, and the v1.60
direct local text path is unchanged. Use `ps3_rsx_stats 1` on a physical PS3 to
evaluate `target_misses` for the 720p/60 and 1080p/30 profiles.

v1.62 routes every editable PS3 menu field through the native system
on-screen keyboard. Highlight a field and press Cross: address-book entries
open the URL layout for host/IP and port strings, numeric server settings open
the 10-key panel, and player or hostname fields open the full keyboard with
their current value prefilled. The engine remains live while the asynchronous
dialog is open, but consumes its controller samples so confirm/cancel cannot
leak into the menu. A four-MiB sysutil memory container exists only for the
dialog lifetime and is released after `SYSUTIL_OSK_UNLOADED`; a failed
reservation safely falls back to the process container. Accepted input is
restricted to Quake II's existing printable byte-string contract.

v1.63 keeps both native stereo visibility passes because Quake II offsets the
CPU view origin for each eye; that can change its PVS, BSP-facing decisions,
and translucent ordering even when the projection frustum is conservatively
expanded. It instead separates the dynamic-light generation from the surface
visibility generation. The paired second eye reuses the first eye's identical
world and inline brush-model light marks, while still advancing an independent
visibility generation and performing the normal eye-specific BSP/frustum walk.
This removes repeated recursive light marking without reusing geometry or
ordering decisions that depend on the eye origin. Mono rendering is unchanged,
and the inert 256-KiB v1.62 replay cache is gone. When a frame contains dynamic
lights, `boot-trace.log` records `RSX renderer: stereo dynamic-light marks
reused on second eye` the first time the path is exercised.

v1.64 adds a companion `PPU stages` line to each opt-in 120-frame RSX report.
Its `*_avg_us` and `*_max_us` fields attribute command-generation time to
dynamic-light marking, setup/PVS, the world, entities, effects, alpha/flash,
the complete measured scene, and all remaining renderer/UI work. Both stereo
eyes accumulate into the same displayed-frame sample. Compare these fields with
the existing cadence and `target_misses` values to distinguish a PPU scene
bottleneck from flip/GPU pressure. No stage timestamps are read unless
`ps3_rsx_stats 1` is active.

v1.65 reduces the arithmetic inside every native frustum-box test. Quake II's
general classifier projects both the maximum and minimum box corners so it can
return front, back, or spanning. Rendering only needs to know whether the
maximum support corner lies behind a frustum plane, so the PS3 path projects
that corner alone. This preserves the original strict comparison—including
boxes exactly on the plane—while removing the second dot product and general
classifier call from BSP-node and entity culling. A deterministic test covering
one million valid bounds, planes, and all eight normal sign masks matched the
original result in every case.

v1.66 removes repeated linked-list walks when native world traversal resolves
animated opaque textures. Each texinfo caches its resolved image for the current
entity animation frame and renderer registration generation, so all surfaces
using it—and the paired stereo eye—reuse the same answer. A loader-time walk
marks every chain containing any translucent frame as ineligible because the
legacy alpha path intentionally rewrites its texinfo image while establishing
draw order. Static textures bypass the cache as before. The native-only fields
consume 16 bytes per texinfo on PS3, bounded to 128 KiB by the BSP format, and a
first real cache hit records `RSX renderer: opaque animated-texture resolution
cache active` in `boot-trace.log`.

v1.67 retains eye-independent MD2 results for the paired native stereo pass.
For ordinary lit alias entities visible to both eyes, the second eye reuses the
first eye's BSP/dynamic-light sample, shade orientation, projected-shadow light
spot, and model-space frame interpolation. Frustum culling, model/view matrices,
depth state, vertex packing, shadow geometry submission, and RSX draws remain
independent for each eye. A one-MiB native-only arena is filled using each
visible model's actual vertex count; scenes that exceed it, models visible to
only one eye, mono output, and maps lacking BSP light data automatically use the
original path. First reuse records `stereo alias lighting reused on second eye`
and `stereo alias interpolation reused on second eye` in `boot-trace.log`.

v1.68 uses the same bounded arena to retain the final MD2 primitive streams
expanded by the first eye: primitive type, positions, texture coordinates, and
lit vertex colors. A matching second-eye entity therefore skips the GL-command
walk and per-command color/vertex assembly, but still performs its eye-specific
culling and matrices and submits independent RSX draws and projected shadows.
No second one-MiB allocation is introduced; the larger descriptors add about
three KiB of BSS. If an entity cannot fit, its partial command cache is reclaimed
and that entity immediately follows the original scratch-buffer path. First use
records `stereo alias command expansion reused on second eye` in
`boot-trace.log`. Mono rendering is unchanged.

v1.69 retains the projected-shadow primitive stream as well when the entity has
a real BSP light sample and therefore an exact paired-eye `lightspot` and
`shadevector`. The second eye skips the MD2 shadow-command walk and projection
arithmetic while retaining its independent shadow matrix, blend/depth state,
and RSX submissions. Cases whose legacy shadow inputs can be order-dependent
(fullbright, shells, and maps without BSP light data) deliberately use the
original path. Shadow blocks share the existing one-MiB arena and roll back per
entity on overflow; their descriptor fields add exactly three KiB of BSS. MD2
model and shadow client arrays are now enabled once around the complete entity
stream rather than toggled around every primitive. First reuse records `stereo
alias shadow expansion reused on second eye` in `boot-trace.log`.

v1.70 applies the same paired-eye principle to particles without retaining any
RSX address. The invariant three-vertex particle atlas coordinates initialize
only when a frame first reaches a higher particle count. In stereo, the first
eye's completed world-space position/color arrays remain in the existing static
workspaces and are decoded into a separate append-only RSX range for the second
eye under that eye's own matrix and viewport. Both eyes use the same simulation
snapshot and view axes, and this renderer shifts stereo projection rather than
particle geometry, so the reused CPU arrays are exact. Eye pairing now follows
the two-BeginFrame/one-EndFrame lifecycle and therefore continues to work at a
stereo depth of zero. First reuse records `stereo particle expansion reused on
second eye` in `boot-trace.log`. The v1.69 UI and glyph objects are unchanged.

v1.71 specializes the remaining backend half of that path. Native GL1 particle
triangles now bypass retained client-array state and generic type/stride/source
decoding. One float4 color is stored and resolved per particle rather than
writing and resolving the same value for all three corners, reducing persistent
PPU workspace by 128 KiB. The final vertices, texture coordinates, effective
colors, state, draw ordering, matrices, and separate stereo-eye RSX ranges are
identical. No glyph expansion, console/menu frontend, UI stream selection,
cache invalidation, or flip-fenced ring code changed.

Frame pacing is now independently selectable in Video Options as
**Profile/Display**, **30 FPS cap**, or **60 FPS cap**, so optimization and
measurement are not tied to one visual preset. The cap is a pacing request—not
a promise that overloaded settings can sustain it. Every `ps3_rsx_stats 1`
record now applies a universal 30 FPS acceptance floor to custom settings and
presets alike through `floor_fps=30` and `floor_misses`; an explicit 60 FPS
target additionally uses the existing stricter 20 ms miss threshold.

v1.72 removes the final addressable vertex stream from native text. Console,
menu, HUD, and isolated glyph calls now emit four-vertex RSX quads directly in
bounded command-buffer chunks, so neither XDR nor local-memory arena reuse can
return stale glyph data. That release initially retained a diagnostic arena
comparison switch; v1.88 removes the fallback and switch entirely. The
implementation-level
batch, indexed, state-filter, fast-array, texture-lifetime, combined-lightmap,
and inline-text accelerators are now enabled whenever Video Options is applied,
including Custom. This does not force a resolution, filter, stereo format, or
framebuffer choice: the 30 FPS acceptance floor covers all of them.

v1.73 applies that same configuration-independent policy to static world
geometry. Eligible opaque lightmapped BSP fans are packed once into persistent
RSX-local pages; normal visibility submits only compact dynamic indices, and a
second stereo eye reuses the same vertex payload. Dynamic lighting, water,
flowing or translucent materials, alpha surfaces, renderer-restart-stale
handles, and arena pressure automatically use the prior streaming path. The
overbright multiplier is now a lightmap vertex-program uniform, so changing it
does not invalidate stored vertices. `ps3_rsx_static_world 1` is the default;
set it to `0` before loading a map for a streaming comparison. With
`ps3_rsx_stats 1`, `static_lm_vertices` reports how much combined-lightmap
geometry used the persistent path. The optimization does not select or assume
a resolution, performance preset, stereo mode, filter, tiled target mode, or
quality setting.

v1.74 reduces the remaining CPU cost unique to stereoscopic world lighting.
The first eye retains a bounded 512 KiB collection of final dynamic-lightmap
tiles; matching second-eye surfaces copy those texels into their own freshly
packed atlas instead of repeating lightstyle accumulation and dynamic-light
falloff math. Visibility, atlas packing and uploads remain per-eye, and cache
pressure or an unmatched surface falls back independently. Mono rendering does
not retain or copy tiles and enters the original build function directly. Use
`ps3_rsx_stereo_lightmap_reuse 0` for comparison. With statistics enabled,
`stereo_lm_reuse_pixels` reports the number of texels whose lighting arithmetic
was reused over the 120-frame interval.

v1.75 separates the two eyes' dynamic RSX lightmap allocations. The first eye
continues to use texture zero, while the second uses the otherwise-unoccupied
last lightmap namespace slot. This preserves separate packing and uploads but
keeps eye two's first update on the pre-sample in-place path instead of copying
and renaming eye one's already-sampled 64 KiB atlas. Mono allocates the reserve
during map registration but never selects or updates it. Set
`ps3_rsx_stereo_lightmap_atlas 0` for an immediate comparison; the existing
`inplace`, `renames`, and `rename_kib` counters show the result. Dynamic atlas
uploads in every mode also avoid a redundant cached bind and two unchanged
filter calls. These implementation accelerators are applied independently of
resolution, frame cap, presentation filter, framebuffer layout, and visual
quality settings.

v1.76 makes the explicit 30 FPS selection a hardware presentation divider on
normal ~60 Hz PS3 output. The completed frame flip supplies the first VBlank;
the main loop waits for one additional VBlank before sampling input and running
the next simulation/render pass. Previously that second interval consisted of
roughly 330 short sleeps and complete no-frame scheduler passes, allowing the
software timer to drift around the display boundary. The new wait remains
bounded, checks XMB/sysutil callbacks, and automatically falls back if the
counter does not advance. It is used only for a 29--31 FPS request on a reported
58--61 Hz timing. The 60 FPS selection, VSync-off operation, timedemo, 50 Hz
output, and non-divisible custom caps are unchanged. v1.92 moves the additional
wait to the flip-queue boundary after render submission and replaces the old
trace with `PS3 pacing: exact 30 Hz render-ahead VBlank budget active`.
`cadence_avg`, `cadence_max`, `floor_misses`, and `target_misses` provide the
physical result.

v1.85 removes a second, contradictory software eligibility check after that
hardware wait. Once the native-refresh flip fence or exact 60-to-30 VBlank
divider completes successfully, the corresponding frame is rendered without
requiring the integer microsecond sample to equal a fractional display period.
This prevents a valid 16,666 us or 33,332 us sample from slipping a refresh due
only to rounding. Non-divisible custom caps still use software timing, and a
VBlank timeout or sysutil interruption cannot authorize a frame. The trace
reports `completed VBlank directly authorizes exact presentation frames` when
the hardware-authoritative route is live. This is a scheduler correction for
all resolutions, filters, layouts, quality settings, and mono/stereo output;
it does not tune around a preferred visual preset.

v1.77 reduces MD2 model CPU work before any resolution- or filter-dependent
rasterization begins. Normal client entities already provide one interpolated
world origin in both `origin` fields, so the alias renderer now skips the angle
basis and zero-delta transform that it previously repeated for every visible
model. Exact-current-frame and repeated-frame models also expand only the live
vertex source. Actual two-frame animation, moving `RF_FRAMELERP` origins,
power-screen shells, lighting, projected shadows, and the paired-eye cache keep
their established results. Because this is upstream of RSX submission, it is
enabled equally for Custom, 720p, 1080p, mono, stereo, all presentation filters,
and both linear and tiled framebuffer layouts; no favorite preset is assumed.

v1.78 closes an update-install loophole in the text correction. Because an
update PKG preserves the installed configuration, an earlier diagnostic
`ps3_rsx_inline_text 0` could continue selecting the obsolete addressable
vertex path. `ps3_rsx_inline_text_revision` now performs one archived migration
to the command-buffer glyph stream. v1.88 subsequently removes that comparison
path and switch, making explicit command-inline triangles unconditional even
when the obsolete cvar remains in an installed configuration. The route audit
confirms that console,
menu, HUD/layout, inventory, chat/input, and individual cursor characters all
reach this same renderer function.

The same release caches successful stationary-entity BSP light-point hits.
Only the surface, sample pointer, impact point, and shadow plane are retained;
lightstyle colors, modulation, dynamic lights, final model scaling, and shadow
use remain live every frame. A moved entity or map-registration change follows
the original recursive BSP query and refreshes the cache. This is another
pre-raster CPU reduction shared by Custom, 720p, 1080p, mono, stereo, filters,
and either framebuffer layout.

v1.79 reduces alias-model culling arithmetic in every mode. The old routine
rotated eight bounding-box corners and tested each corner against four frustum
planes. The replacement transforms the box center once and uses the exact
projection radius of its three oriented half-axes for each plane; models with
zero rotation also bypass the trigonometric basis calculation. A tiny
conservative boundary can draw an edge model rather than incorrectly reject
one due to floating-point rounding. Debug `gl_showbbox` output still constructs
the same eight corners on demand. Moving/rotating models and the union of old
and current animation frames remain covered, while stereo eyes keep separate
frustum tests.

v1.80 removes identity matrix work throughout the native renderer. Entity
setup now omits zero translation and each zero Euler rotation before entering
the compatibility layer. The backend also rejects identity translate/rotate/
scale calls from other paths, and recognizes already-normalized unit rotation
axes without calculating their length. Nonzero operations retain the original
ordering and arithmetic. This matters especially for the common yaw-only model:
pitch and roll no longer execute trigonometry or 4x4 matrix multiplies, while
unrotated models avoid all three rotations.

v1.81 specializes the native submission of animated MD2 alias models and their
projected shadows. Those draws are numerous short triangle fans and strips
whose input layouts are fixed by the GL1 frontend. They now bypass retained
client-array enable/pointer state plus generic type, stride, alignment, and
source-layout discovery, and write the same final RSX vertices directly.
Indexed batching still preserves source-vertex reuse; expanded batching and
the original unbatched hardware primitives remain valid fallbacks. Strip
winding, model lighting, shell colors, alpha, depth/stencil behavior, and
shadow projection are unchanged. Stereo's cached second-eye model and shadow
commands enter the same path, so the reduction applies equally to Custom,
720p, 1080p, all filters, framebuffer layouts, and quality settings.

v1.82 retains the final pre-glow entity-light result for stationary models,
building on the cached BSP sample introduced in v1.78. Reuse requires exact
agreement among the entity origin, map registration, sampled surface, each
relevant lightstyle RGB triplet, `r_modulate`, and the complete dynamic-light
array. Dynamic-light inputs are compared once per renderer pass and represented
by a monotonic revision, so each cached entity needs only one revision check
instead of repeating every light distance. Any input change uses the original
calculation immediately. The `final_light_hits` field in `ps3_rsx_stats 1`
quantifies successful reuse over each 120-frame interval.

v1.83 makes every command-inline font glyph an explicit two-triangle list.
Text therefore depends on neither a recycled vertex/index address nor RSX quad
decomposition state. The bounded emitter reserves its complete begin, color,
six TEX0/POS pairs per glyph, and end sequence before writing any command. Its
maximum reservation remains 5,385 words by reducing the chunk from 192 quads
to 128 six-vertex glyphs. Revision 2 of the archived migration selects this
route once for existing update installs; the trace must report `explicit-
triangle command-buffer inline text active` when physical text is displayed.

v1.84 carries a four-bit frustum mask down the world BSP traversal. A node
still uses its positive support corner for the exact established outside test,
but its negative support corner can prove that every enclosed descendant lies
inside a plane. Such descendants no longer repeat that plane's arithmetic.
`world_clip_tests` counts executed node/plane tests and `world_clip_skips`
counts tests eliminated by inherited masks in each 120-frame statistics block.
The masks are independent per stereo eye because each eye retains its own
frustum. Randomized outside-equivalence and contained-child validation covered
500,000 boxes without a mismatch.

v1.86 removes the work left after that mask becomes zero: an enclosed
descendant no longer enters the clip helper merely to inspect four cleared
bits. Statistics still add four `world_clip_skips` for that node, keeping
before/after captures comparable while normal gameplay avoids the function and
loop entirely. This applies to every visual configuration because the saving
occurs before RSX rasterization.

The same update makes the original popup/menu text printer line-atomic. It was
the remaining green/white string path that called the native glyph function
once per character; its complete line now reaches the explicit-triangle inline
emitter in one ordered run. Fixed-width transparent spaces at the end of
console rows are also removed before refresher submission. Console, notify,
input, qmenu, popup, HUD, inventory, chat, loading, and cursor text therefore
all converge on the deterministic native glyph implementation, while isolated
border/cursor glyphs retain their deliberate one-character draws.

v1.87 moves the immutable MD2 strip/fan parse from the per-entity render and
projected-shadow loops to model load. Compact native records retain each
primitive's original order/type and every corner's exact texture coordinates
and source-vertex index; animation interpolation, frame-normal lighting,
shells, alpha, shadows, native triangle batching, and stereo's paired-eye cache
remain unchanged. Command bounds, termination, counts, and source indices are
validated before a model becomes renderable, and the prepared records live in
the model's existing lifetime hunk. This removes repeated PPU file-format work
for visible monsters, pickups, players, and weapons in every Custom/720p/1080p,
mono/stereo, filter, framebuffer, and quality combination rather than
optimizing one preferred Video Options screenshot.

v1.88 passes those records directly to a native RSX packer. GL1 now creates one
lit color per unique MD2 source vertex instead of writing position, UV, and
color to three temporary per-corner arrays which the backend immediately read
and repacked. Projected shadows are similarly evaluated once per unique source
vertex. The paired stereo eye retains these compact source results rather than
expanded strip/fan command payloads. Fan/strip order and winding, texture-seam
coordinates, frame interpolation and normals, shells, alpha, projection,
indexed/expanded batching, and draw state remain exact. `ps3_rsx_stats 1`
reports `alias_sources` and `alias_refs` in the PPU-stage record so physical
captures show the unique-source work relative to the eliminated corner pass.

The same release makes explicit-triangle command-buffer glyphs the sole native
font renderer. The previous addressable vertex-arena diagnostic branch and its
live switch are gone; no retained update configuration can select it. Console,
notify, input, qmenu, popup, HUD, inventory, chat, loading, cursor, and isolated
glyph calls therefore have no stale-address fallback. This hardening is
independent of the selected resolution, stereo mode, display filter,
framebuffer layout, or quality settings.

v1.89 also prepares the final MD2 triangle index list at model load. Every
validated fan and strip is flattened once with its original winding, allowing
normal models and projected shadows to reserve one indexed RSX block instead
of rebuilding indices and reserving space primitive by primitive every frame.
Oversized custom models retain the v1.88 fallback. A randomized equivalence
test covered 250,000 topologies, 1,749,176 fans/strips, and 334,453,752 indices
with zero winding or batch-base mismatches. This CPU reduction is upstream of
resolution, stereo, filters, framebuffer layout, and quality choices, so it is
not tied to a preferred settings profile. The sole command-buffer text path is
unchanged.

v1.90 applies the same load-time principle to persistent BSP geometry. Each
eligible opaque lightmapped world fan stores its final absolute 16-bit triangle
list beside the polygon in the map hunk. Visible material chains copy those
indices into their existing batch rather than reconstructing three values per
triangle every eye and frame. Dynamic lightmaps, transparent/flowing/warped
surfaces, static-arena overflow, and the diagnostic fallback are unchanged. A
one-million-case test covered 3,071,210,712 indices with zero sequence or range
mismatches. This work is shared by every visual profile and option combination;
it does not select or silently alter a preferred configuration.

v1.91 prepares two more immutable BSP facts: each face's lightstyle count and
whether its geometry can enter the combined-lightmap path. The frame loop keeps
all live decisions—lightstyle values, dynamic lights, animated texture alpha,
translucency, fullbright, lightmap debugging, saturation, and world light data—
but no longer repeats fixed terminator, polygon, lightmap-range, turbulent, and
flowing tests per surface. Global combined-lightmap conditions are hoisted once
per world pass. Two million randomized old/new predicate cases produced zero
decision mismatches. This is another settings-independent PPU reduction.

v1.92 corrects the exact 60-to-30 Hz divider's placement. The older schedule
waited through the intermediate VBlank before beginning the frame, leaving only
one 16.7 ms interval of actual work time despite a 33.3 ms presentation target.
The completed prior flip now starts input, simulation, and rendering immediately.
EndFrame flushes the completed draw workload so RSX can execute it, waits only
the flip command until the intermediate queue boundary, and then appends that
flip in FIFO order. A deterministic 501-point schedule sweep found no target
miss from 16.7 through 33.3 ms; work above 33.3 ms remains a genuine miss. This
path is selected only by the explicit 30 FPS cap on normal 58--61 Hz output and
does not inspect or alter resolution, mono/stereo, filter, framebuffer layout,
or quality choices. Look for `PS3 pacing: exact 30 Hz render-ahead VBlank budget
active` in `boot-trace.log`.

v1.93 keeps that performance policy independent of the user's preferred
profile and hardens two shared renderer lifetimes. Direct native 2D glyph and
quad submission is accepted only while the current render surface is acquired,
active, and writable; callbacks can no longer append UI commands after the
surface has been queued for display. The command-inline text path stages each
visible glyph once in bounded chunks and verifies the exact emitted RSX word
count at runtime. Tiled depth allocation now uses Z-cull's 64-row registration
alignment when Z-cull is enabled, which prevents its registered region from
extending beyond 480p and 720p depth surfaces; color retains the smaller valid
32-row tiled allocation. Persistent zero-on-consume lightmap heads also remove
a repeated 1 KiB table clear from each world pass and recover automatically if
a render abort interrupts cleanup. None of these paths inspects or changes the
selected resolution, frame cap, mono/stereo mode, filter, framebuffer layout,
or visual quality. The 30 FPS option remains a universal acceptance floor, and
`ps3_rsx_stats 1` is the physical measurement needed to identify any remaining
workload that genuinely runs beyond its 33.3 ms frame budget.

v1.94 extends the command-inline lifetime from glyphs to every ordinary 2D
layer which surrounds them. Console backgrounds, menu artwork and bars, HUD
icons, fills, fades, tiled borders, cinematics, and per-eye display-filter
rectangles no longer write or fetch a rotating addressable vertex generation.
Each uses one explicit six-vertex triangle command sequence, with the same
exact word-count invariant as text. This prevents a current text row from being
placed over a stale console/menu window and removes 240 bytes of addressable
vertex traffic for every such rectangle; enabled filters also avoid that work
once per eye. State-changing draws are still ordered by an explicit batch
flush, and offscreen rejection is unchanged. The implementation does not
inspect or force resolution, frame cap, stereo mode, filter choice,
framebuffer layout, or quality settings.

v1.95 removes redundant full draw-state preparation between adjacent inline
text runs and ordinary 2D layers. The shortcut is accepted only when the
generic shader is already bound, the uploaded MVP revision is current, and
both sampler and texture-cache state are clean; lightmap, display-filter, and
every changed state use the full established path. Accepted textured draws
still update allocation-sampling hazard tracking. With `ps3_rsx_stats 1`, the
new `ui_state_hits` field reports the number of preparations avoided. An
exhaustive 512-state predicate check admitted only the one completely prepared
state. This is shared renderer work across Custom, 720p, and 1080p output,
mono and stereo, every filter, either framebuffer layout, and every quality
choice. It neither recognizes nor tunes for a favorite settings profile; the
30 FPS minimum remains the general physical-console acceptance floor.

v1.96 keeps those unchanged inline draws inside one bounded RSX triangle
stream instead of opening and closing a hardware primitive for every console
row, menu label, HUD string, and surrounding 2D layer. Any program, texture,
matrix, fixed-state, viewport/scissor, render-stream, filter, eye, frame, fence,
or shutdown boundary closes the stream first; generic draw preparation also
performs a defensive close. Each append reserves the future STOP before
writing vertices, command-buffer rollover occurs only after closing, and one
stream is capped at 16,384 vertices. `ps3_rsx_stats 1` reports `ui_batches` and
`ui_merges` so physical logs expose the draw reduction directly. A 248,201-
append deterministic stress pass exercised 91,608 command-buffer rollovers
without issuing a callback inside Begin/End or losing a STOP reservation. This
shared path applies to every resolution, mono/stereo mode, filter, framebuffer
layout, and quality setting; it does not special-case a preferred profile.

v1.97 removes repeated native matrix construction from frame-start, post-world
2D setup, and display-filter resolve. One exact helper creates the same Quake
II orthographic projection, identity modelview, UI-stream selection, and final
GL_MODELVIEW selection while advancing matrix state only once. If those final
matrices are already resident, state filtering converts the matrix setup into
a complete no-op; `ps3_rsx_stats 1` reports these saves as `ortho_hits`.
Disabling state filtering retains the forced non-cached diagnostic path. A
Float32 bit-level comparison covered the five physical output shapes plus
250,000 randomized dimensions with no differences. This is shared renderer
work for Custom, 720p, and 1080p, mono and stereo, every display filter, both
framebuffer layouts, all quality settings, and both selectable frame caps. It
does not inspect or optimize for a favorite settings profile, and the 30 FPS
minimum remains the general physical-console acceptance floor.

v1.98 removes full-table work from the compatibility lightmap pass. Native
world and brush rendering track the exact occupied pages, traverse them in the
same ascending order, retain their chains through show-triangle diagnostics,
and clear only those heads when the next pass begins. When the combined path
handles every surface, the empty fallback now returns before changing depth,
blend, texture, or dynamic-atlas state. Dynamic surfaces and all diagnostic
fallbacks remain intact. `ps3_rsx_stats 1` also reports `ui_dupes`: a stats-only
same-eye signature of completed string submissions, reset at each eye boundary.
It does not suppress draws; a nonzero count proves that identical text was
submitted twice by the engine, while zero directs the stale-text investigation
back to raster/presentation lifetime. Randomized lightmap testing covered
200,000 passes and 4,804,914 insertions with no mismatches, and UI-signature
testing detected all 320,000 injected duplicates across 10,000 eye passes with
no false positives. These are shared paths across every resolution, stereo
mode, filter, framebuffer layout, quality setting, and 30/60 FPS selection.

v1.99 removes redundant native camera and raster-state construction below all
of those choices. The renderer now derives the world-view matrix directly from
the forward/right/up basis which `R_SetupFrame` already calculated, replacing
five repeated trigonometric rotations, six generic matrix multiplies, and a
compatibility matrix readback for every eye. It also filters viewport and
scissor methods by their resolved physical RSX rectangle, so an identical mono
rectangle is skipped while a real frame-packed/top-and-bottom eye change still
emits. `ps3_rsx_stats 1` reports these omissions as `raster_hits`.

Frame-floor telemetry is intentionally independent of the performance-profile
selector. `floor_fps=30` now uses `floor_budget_us=33334`, and only cadence
samples above that rounded 30 Hz interval increment `floor_misses`;
`floor_streak_max` records the longest consecutive miss run. A selected 60 FPS
target uses `target_budget_us=16667`. The older `cadence_over20` and
`cadence_over34` fields remain solely for comparison with earlier logs, while
`cadence_over16667` and `cadence_over33334` are the exact acceptance counters.
These measurements and optimizations cover Custom, both named presets, every
output resolution, mono/stereo, every filter, tiled or linear targets, all
quality settings, and either cap. No favorite settings combination is inferred
or favored. The v1.98 `ui_dupes` field remains the physical discriminator for
the duplicated/stale green-white text report.


v1.37 also removes recurring PPU heap allocations from particles, alias models,
projected shadows, flowing surfaces, offset lightmaps, and water warps by using
workspaces sized to Quake II's existing format limits. Flip completion polling
uses 50-microsecond granularity instead of 200 microseconds to reduce wake-up
jitter near a completed vblank, without changing the 60 Hz engine cadence. The
lighting audit confirms that native RSX still applies Quake II's
destination-times-lightmap blend and SDL-compatible upload gamma. Each startup
now logs gamma, base-texture intensity, lightmap modulation, fullbright, and
overbright values, allowing a physical trace to distinguish an archived
brightness setting from a missing pass without forcing a different look.

The v1.38 combined vertex-color packing experiment was removed by the v1.39
rollback and restored in v1.40 after the log isolated video-state rejection as
the boot failure. Texture-environment changes are already batch boundaries, so
v1.40 finalizes replacement/scaled color on the initial vertex write and avoids
a redundant full-batch PPU rewrite.

Projected shadows use explicit alpha, cull, and depth-write state and retain
Quake's 0.1-unit geometric lift without artifact-prone RSX polygon offset. The
exact v1.36 EBOOT submitted a 602-vertex projected shadow in `base1`, and the
physical pass confirmed both the shadow and 48 kHz native audio paths.

The same build uses LV2's monotonic timer and schedules VSync gameplay at the
actual 60 Hz output cadence instead of requesting 72 engine frames against a
60 Hz flip queue. Its first native-audio slice repairs the old permanent mixer
lock and hardware-block cursor ordering, initializes silent hardware buffers,
and makes port, event-queue, and audio-thread shutdown staged and repeatable.
`soundinfo` exposes copied, nonzero, and mixer-zero block counters for hardware
testing. Effects are mixed as signed 16-bit stereo into a 48 kHz PS3 port; Ogg
music additionally requires tracks under `baseq2/music`.

v1.33 enables indexed triangle batching by default after the native renderer
passed physical-console gameplay. In a matched 120-frame RPCS3 `base1` test,
that path streamed 406,272 source vertices instead of the expanded fallback's
669,342, a 39.2 percent reduction; existing archived safety defaults migrate
once, and `ps3_rsx_indexed_batch 0` remains available without a restart. Every
native target is cleared to stable black before rendering; this removes stale
mono scanlines and provides the same guarantee independently for both packed
stereo eyes through an ordered GPU surface clear.

The console now defaults to a PS3-specific 1x scale. Yamagi's inherited
automatic scale enlarged every console-font texel 2x at 720p and 3x at 1080p,
which made dense startup output look doubled even though controlled indexed,
expanded, unbatched, and unfiltered captures all contained one draw. Existing
automatic configurations migrate once; an explicitly selected console scale,
plus the independent menu and HUD scales, remains unchanged.

The native renderer has not lost its shaders or lighting. Its compiled Cg
vertex/fragment pair implements the fixed-function operations expected by the
GL1 frontend. Quake II supplies most world lighting as baked lightmap textures,
then adds dynamic-light blend passes; it does not use modern normal maps or
per-pixel material shaders. Base textures, lightmaps, vertex color modulation,
dynamic lights, entities, weapons, particles, and UI all remain on the RSX
path.

v1.34 attempted to fix the remaining doubled text in the green/white
console-style menus.
Those screens use Quake II's 8x8 charset rather than the picture-based main
menu. On PS3, an inherited automatic `r_menuscale` now migrates once to 1x,
matching the proven console correction; explicit user-selected scales remain
unchanged. New installs also start with `r_menuscale 1` in `yq2.cfg`.

Physical v1.34 testing disproved that diagnosis: stale/doubled rows survived at
1x. v1.35 removes that workaround and restores automatic menu scaling only for
configurations changed by v1.34. The native backend now clears the complete
newly acquired color/depth/stencil target under a known full scissor and write
masks before any eye or GL1 pass can inherit old raster state. That one ordered
clear covers both regions of a frame-packed target and replaces redundant
per-eye color/depth clears during ordinary rendering.

Projected entity shadows are enabled by default again and have a live
**entity shadows** switch in Video Options. Native face culling is disabled only
while drawing the flattened shadow mesh, then restored immediately. This fixes
the regression introduced when hardware culling became the native default;
`gl1_stencilshadow` remains optional.

Video Options includes **PS3 output**. **system / XMB mode** retains the
resolution selected in PS3 Settings, **720p native target** changes the actual
HDMI scanout and native color/depth targets to 1280x720, and **1080p native
target** requests 1920x1080. This is distinct from `r_mode`: 720p reduces RSX
framebuffer work by 55.6 percent relative to 1080p. If a requested timing is
unavailable the game safely restores the system mode; stereoscopic frame
packing continues to select its required 720p output automatically.

The same screen now separates **tiled targets** from **tiled + Z-cull (3D
proven)** for physical A/B testing. v1.34 showed a possible improvement with
the combined path in frame-packed 3D but did not produce a working mono result.
The tiled-only choice isolates mono target layout from Z-cull metadata. Both
remain experimental, session-only, and linear by default after every relaunch;
if either fails, restart the console. v1.34 also caches the native backend's
live cvar objects, avoiding repeated string-table lookups at frame boundaries
and during dynamic-lightmap updates.

For a physical tiled-target A/B, use the same map, view, and movement for each
run. First collect the linear baseline:

```text
set ps3_rsx_stats 1
set gl1_ztrick 0
set ps3_rsx_stream_main_memory 0
set ps3_rsx_tiled_targets 0
vid_restart
map base1
```

Then test tiled targets without Z-cull and repeat the same route:

```text
set ps3_rsx_tiled_targets 1
vid_restart
map base1
```

The combined mode physically proven only in 3D is value 2:

```text
set ps3_rsx_tiled_targets 2
vid_restart
map base1
```

The optional main-memory stream can be isolated in a fourth run with
`set ps3_rsx_stream_main_memory 1` followed by `vid_restart`, while keeping the
target setting otherwise unchanged. The resulting timing records are written
to `/dev_hdd0/game/QUAKE2000/USRDIR/boot-trace.log`. Both experimental switches
are session-only and return to their conservative local/linear defaults after a
full relaunch.

Linear RSX textures use one hardware pitch across every mip level. The native
backend therefore pads lower-level rows to the level-0 pitch, stores the levels
contiguously, advertises only levels Quake has uploaded in sequence, and sets
the sampler's maximum LOD to the final available level. This restores the
world-texture minification path that the earlier baseline intentionally
collapsed to level zero.

The v1.13 vertex stream uses a naturally
aligned 40-byte layout and explicit 32-bit PPU stores, fixing the first-draw
alignment fault isolated by v1.12's trace. It also decodes client arrays by
their declared component type. The v1.11 safety behavior remains: a branchless
fragment program and bounded display-flip waits. v1.16 defaults
`ps3_rsx_texture_rename` to `1`; set it to `0` to restore the v1.13 synchronized
upload path. A one-time settings revision upgrades the archived v1.13 default;
after that migration, an explicit `0` remains respected. A failed run records
native initialization, first draw, flip, GPU finish, and dynamic lightmap stages in:

```text
/dev_hdd0/game/QUAKE2000/USRDIR/boot-trace.log
```

## Build

Install a current ps3dev toolchain and PSL1GHT with `libgem`, `libspurs`, and
`libnet`, then export the standard paths. For a default ps3dev installation:

```sh
export PS3DEV=/usr/local/ps3dev
export PSL1GHT="$PS3DEV/psl1ght"
export PORTLIBS="$PS3DEV/portlibs/ppu"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH"
make -f Makefile.ps3
```

This produces `q2ps3.elf` and `q2ps3.self`. The application/title ID is
`QUAKE2000` and the runtime data directory is:

```text
/dev_hdd0/game/QUAKE2000/USRDIR/baseq2
```

Copy `pak0.pak` and `stuff/yq2.cfg` into that directory. Expansion packs and
mods use their normal Quake II game directories beside `baseq2`.

Do not pass the raw top-level ELF directly to `make_self_npdrm`. PSL1GHT's
`.self`/`.pkg` rules first strip a build copy and run `sprxlinker`, which fills
the PS3 import-descriptor function counts. Skipping that pass produces an EBOOT
that fails in newlib's allocator constructor before `main()`.

To create a package, point PSL1GHT's package rule at a staging directory whose
`USRDIR/baseq2` contains the legally owned game data:

```sh
make -f Makefile.ps3 q2ps3.pkg \
  PKGFILES=/path/to/pkgfiles \
  ICON0=/path/to/ICON0.PNG \
  SFOXML=/path/to/param.xml
```

That full package is the baseline install. Later engine releases should use an
overlay update package with the same title/content ID and only these files:

```text
PARAM.SFO
USRDIR/EBOOT.BIN
```

Installing the update replaces the executable and version metadata while
preserving `USRDIR/baseq2`, including the commercial PAK files and user data.
Create a new full package only when the baseline assets or directory layout
actually change.

## Recommended video profiles

Select a **performance profile** in Video Options and Apply:

| Profile | Physical output | Frame cap | Notes |
| --- | --- | --- | --- |
| 720p performance preset | 1280x720 | 60 by default | Recommended sustained-performance path |
| 1080p quality preset | 1920x1080 | 30 by default | Higher fill cost; display support required |
| Custom | User-selected | Display by default | Manual output, mode, VSync, and diagnostics |

The presets select matching 16:9 `r_mode` values. Proven native submission
optimizations are enabled for Custom and both presets, so changing visual
settings does not fall back to a slower renderer path. The separate frame-rate target can retain the preset
default or request a 30/60 cap with `ps3_rsx_fps_target 0`, `1`, or `2`.
Display filters remain optional because their single full-output pass consumes
additional RSX fill bandwidth when enabled. Thirty FPS remains the acceptance
floor across all combinations, not only these presets.

## Stereoscopic 3D

Stereoscopic output supports native HDMI 720p frame packing and a standard
1280x720 top-and-bottom compatibility signal. Select the format in Video
Options, or set it explicitly and run `vid_restart`:

```text
set ps3_stereo_enable 1
set ps3_stereo_output 0
vid_restart
```

`ps3_stereo_output 0` selects native frame packing and requires the connected
display chain to advertise the PS3 720p 3D mode. `ps3_stereo_output 1` selects
top-and-bottom compatibility output. Use that mode if a projector, receiver, or
capture device shows two complete menu/console images separated vertically;
then select Top/Bottom 3D decoding on the display. It uses ordinary 720p HDMI
timing and maps each full eye into a 1280x360 half, avoiding the raw 1280x1470
frame-packed scanout and its 30-line gap.

`ps3_stereo_depth` is a live depth scaler from `0.0` to `2.0`; it does not need
a video restart. `0.0` removes eye separation, `1.0` is the default, and `2.0`
doubles it. Start low and increase it gradually. The underlying eye-distance
setting remains available as `gl1_stereo_separation` (default `1`). Start with
`r_mode 4` for stereo performance. Native frame packing clears the top and
bottom eye regions independently every presentation; the normal `r_clear`
setting does not disable this required stereo clear.

## SPU acceleration

The standard GameOS application limit is six SPUs. This port can assign one to
five persistent workers to framebuffer palette conversion, output scaling, and
the two 720p images used for stereoscopic frame packing. One application SPU is
always left available so PlayStation Move can be enabled without rebuilding the
worker group.

SPU acceleration is disabled by default. It can be enabled from the Video menu
or with:

```text
set ps3_spu_accel 1
set ps3_spu_workers 4
vid_restart
```

`ps3_spu_workers` accepts `1` through `5`. The implementation uses aligned
main-memory DMA staging and a bounded completion wait. If allocation, startup,
signalling, or a worker job fails, acceleration disables itself and the frame is
converted by the existing PPU path instead.

Examples:

```text
set ps3_stereo_enable 1
set ps3_stereo_output 0
set ps3_stereo_depth 0.7
vid_restart

// Compatibility mode for a display chain that exposes raw frame packing
set ps3_stereo_output 1
vid_restart

// Adjust while playing
set ps3_stereo_depth 1.0

// Return to 2D
set ps3_stereo_enable 0
vid_restart
```

## PlayStation Move

The sphere-topped wand uses PlayStation Eye correction when the camera is
available and falls back to inertial aiming while optical tracking is absent.
The Navigation controller stays on the ordinary gamepad path. For boot safety,
libgem and libcamera are not started automatically from an archived setting.
Reach the main menu first, then use `gamepad / Move settings` to enable and
calibrate the wand, or use:

```text
set psmove_enable 1
psmove_reinit
```

The trigger emits `TRIG_RIGHT`, the Move button emits `BTN_GUIDE`, and the face
buttons use the corresponding controller bindings. Defaults bind the trigger to
attack and Move/Cross to jump. Useful tuning cvars are:

```text
psmove_deadzone 0.025
psmove_smoothing 0.35
psmove_yawsensitivity 1.0
psmove_pitchsensitivity 1.0
psmove_invertpitch 0
```

Point the illuminated sphere toward the Eye and follow the alignment overlay.
The native backend submits the Eye's timestamped frames to libgem for corrected
angular velocity; brief tracking loss retains raw-gyro aiming. A Navigation
controller or DualShock 3 can supply movement while the Move wand aims.

## Network play

The port uses Quake II's normal IPv4 UDP protocol. The default server port is
UDP `27910`.

```text
// Host a deathmatch game
deathmatch 1
map q2dm1

// Join by address or DNS name
connect 192.168.1.25:27910
connect quake.example.net:27910
```

LAN discovery uses IPv4 broadcast. Internet hosting requires UDP 27910 to be
allowed through the host network/NAT. There is no PSN matchmaking, relay, UPnP,
IPv6, or encrypted transport in this port.

## Default controls

- Left stick: move; right stick: look
- R2 / Move trigger: attack
- Cross / Move: jump
- Circle: crouch
- Square and shoulder buttons: cycle weapons
- Select: open the main menu

DualShock 3 buttons can be changed while the game is running under **Options >
remap DualShock 3**. Cross selects an action, the next pressed controller
button assigns it, Triangle clears the selected binding, and Circle leaves the
screen. The normal archived `config.cfg` stores the result. Select is exposed
as `BTN_BACK` and can be reassigned like the other controller buttons; existing
installations receive its original `menu_main` binding once during migration.

The PS3 default for `r_consolescale` is `1`. This keeps console text crisp at
720p and 1080p instead of applying Yamagi's automatic 2x/3x integer scaling.
Set another positive value if larger console text is preferred; menu and HUD
scaling are independent.
- Triangle: inventory
- Start: pause
- D-pad: inventory navigation/use/drop

Put personal overrides in `baseq2/autoexec.cfg`; it is loaded after `yq2.cfg`.
