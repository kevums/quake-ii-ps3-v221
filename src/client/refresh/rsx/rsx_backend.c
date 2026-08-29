/* Platform hooks connecting Yamagi's GL1 scene frontend to native RSX. */
#include "../gl1/header/local.h"

static qboolean rsx_context_active;
static float rsx_context_gamma = 1.0f;

void
RI_EndFrame(void)
{
	RSXGL_EndFrame();
	R_PS3_EndVisualFrame();
}

void *
RI_GetProcAddress(const char *proc)
{
	(void)proc;
	return NULL;
}

qboolean
RI_IsVSyncActive(void)
{
	return RSXGL_IsVSync() ? true : false;
}

int
RI_PrepareForWindow(void)
{
	/* GLimp owns the already-created GCM command context on PS3. */
	gl_state.stencil = true;
	return 0;
}

void
RI_SetVsync(void)
{
	qboolean stereo =
		(Cvar_VariableValue("ps3_stereo_enable") > 0.0f);
	RSXGL_SetVSync(r_vsync->value != 0.0f || stereo);
}

void
RI_UpdateGamma(void)
{
	float gamma = R_GetUploadGamma();

	/* Native RSX has no arbitrary hardware display-gamma ramp. Gamma is folded
	 * into texture uploads instead, so changing it requires one full renderer
	 * reload. Avoid an unnecessary startup restart when the current context was
	 * already initialized with this value. */
	if (fabsf(gamma - rsx_context_gamma) < 0.0001f)
	{
		return;
	}

	PS3_BOOT_TRACE("RSX renderer: gamma change requests full texture reload");
	ri.Vid_RequestRestart(RESTART_FULL);
}

int
RI_InitContext(void *gcm_context)
{
	cvar_t *rename_revision;
	cvar_t *indexed_revision;

	if (!gcm_context)
	{
		ri.Sys_Error(ERR_FATAL,
			"RSX renderer InitContext called without a GCM context");
		return false;
	}

	/* Dynamic lightmaps use copy-on-update while a frame is active. Their old
	 * RSX allocations remain live until the ordered display flip completes.
	 * Keep the conservative synchronized path available as a recovery toggle. */
	ri.Cvar_Get("ps3_rsx_texture_rename", "1", CVAR_ARCHIVE);
	/* Before the first draw that samples an allocation in a frame, the previous
	 * flip is already complete and dynamic subimages may update it in place.
	 * Retain the always-rename v1.26 behavior as a live diagnostic fallback. */
	ri.Cvar_Get("ps3_rsx_texture_frame_reuse", "1", CVAR_ARCHIVE);
	/* Batch compatible GL1 triangle draws by default. The recovery toggle is
	 * sampled at frame boundaries, so it can be changed without a restart. */
	ri.Cvar_Get("ps3_rsx_batch", "1", CVAR_ARCHIVE);
	/* Fans, strips, and quads share streamed vertices through bounded 16-bit
	 * RSX index batches. The expanded v1.20 path remains a live fallback. */
	ri.Cvar_Get("ps3_rsx_indexed_batch", "1", CVAR_ARCHIVE);
	/* Suppress setters whose effective fixed-function state is already active.
	 * Like batching, this can be disabled at the next frame for diagnosis. */
	ri.Cvar_Get("ps3_rsx_state_filter", "1", CVAR_ARCHIVE);
	/* Specialize the client-array packer for the float layouts emitted by the
	 * GL1 frontend. The generic typed decoder remains a frame-boundary fallback. */
	ri.Cvar_Get("ps3_rsx_fast_arrays", "1", CVAR_ARCHIVE);
	/* Pack invariant opaque BSP vertices into persistent RSX-local pages during
	 * map registration. Visibility still uses the compact dynamic index stream;
	 * unsupported surfaces and allocation pressure retain the streaming path. */
	ri.Cvar_Get("ps3_rsx_static_world", "1", CVAR_ARCHIVE);
	/* Optionally put the dynamic vertex/index rings in 1 MiB-aligned mapped
	 * main memory so the PPU writes cached XDR instead of RSX local memory.
	 * Allocation location changes at renderer startup, and the first hardware
	 * pass must always recover to local memory after a full relaunch. */
	ri.Cvar_Get("ps3_rsx_stream_main_memory", "0", 0);
	/* Native tiled color/depth targets plus compressed Z-cull can reduce RSX
	 * framebuffer bandwidth. This changes allocation metadata, so it is sampled
	 * only when the renderer starts and requires vid_restart after a change. */
	/* Keep the first hardware-test switch session-only: if a physical console
	 * rejects the layout, a reset must return to linear targets automatically
	 * instead of persisting a boot-looping experimental allocation. */
	ri.Cvar_Get("ps3_rsx_tiled_targets", "0", 0);
	/* Native face culling uses Quake GL1's FRONT/BACK selection directly with a
	 * CCW front face. Keep suppression and the three alternate mappings as
	 * session-only recovery/diagnostic choices (0 and 2..4 respectively). */
	ri.Cvar_Get("ps3_rsx_hw_cull_mode", "1", 0);
	/* Optional low-rate diagnostics compare GL1 draw calls with actual RSX
	 * submissions. Disabled by default so normal play performs no trace I/O. */
	ri.Cvar_Get("ps3_rsx_stats", "0", CVAR_ARCHIVE);
	/* v1.11-v1.15 archived the safety-baseline default of zero. Migrate that
	 * inherited value exactly once; after revision 1 is archived, an explicit
	 * user choice to disable renaming remains untouched on later launches. */
	rename_revision = ri.Cvar_Get("ps3_rsx_texture_rename_revision", "0",
		CVAR_ARCHIVE);
	if (rename_revision->value < 1.0f)
	{
		ri.Cvar_Set("ps3_rsx_texture_rename", "1");
		ri.Cvar_Set("ps3_rsx_texture_rename_revision", "1");
	}
	/* v1.32 kept indexed triangle reuse disabled for its first physical pass.
	 * That renderer passed gameplay on hardware and the indexed path completed
	 * the full RPCS3 map/stereo/state matrix while streaming 39 percent fewer
	 * vertices. Migrate the old archived safety value once; users can still set
	 * the live diagnostic toggle back to zero afterward. */
	indexed_revision = ri.Cvar_Get("ps3_rsx_indexed_batch_revision", "0",
		CVAR_ARCHIVE);
	if (indexed_revision->value < 1.0f)
	{
		ri.Cvar_Set("ps3_rsx_indexed_batch", "1");
		ri.Cvar_Set("ps3_rsx_indexed_batch_revision", "1");
	}
	PS3_BOOT_TRACE("RSX renderer: initializing native context");
	if (!RSXGL_Init((gcmContextData *)gcm_context, vid.width, vid.height))
	{
		R_Printf(PRINT_ALL, "Native RSX context initialization failed.\n");
		return false;
	}
	rsx_context_gamma = R_GetUploadGamma();
	rsx_context_active = true;
	gl_state.stencil = true;
	RI_SetVsync();
	PS3_RUNTIME_TRACE("RSX renderer: native context ready");
	return true;
}

void
RI_ShutdownContext(void)
{
	if (!rsx_context_active)
	{
		return;
	}
	RSXGL_Shutdown();
	rsx_context_active = false;
}

void
GL_BeginRendering(int *x, int *y, int *width, int *height)
{
	*x = 0;
	*y = 0;
	*width = vid.width;
	*height = vid.height;
}

void
GL_EndRendering(void)
{
}
