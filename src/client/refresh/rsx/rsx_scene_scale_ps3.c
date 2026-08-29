/*
 * PS3 scene-resolution bridge.
 *
 * The proven GL1/RSX objects render the 3D view and the 2D UI in one display
 * surface.  A 720p frame-packed scene therefore shades two complete 1280x720
 * eyes, which can miss the 33.3 ms presentation slot and fall directly to the
 * next 50 ms VBlank.  This bridge keeps the display surface and all 2D work at
 * native resolution, but renders only the 3D refdef into a smaller centered
 * rectangle of a private RSX surface and linearly resolves that rectangle into
 * the eye before the existing EndWorldRenderpass/UI boundary.
 *
 * It is deliberately implemented around refexport_t instead of modifying the
 * accepted GL1 frontend.  Allocation, tiled-target, z-trick, or rectangle
 * validation failure returns to the byte-identical native path.
 */

#include "../gl1/header/local.h"
#include "header/rsx_gl.h"

#include <rsx/rsx.h>
#include <stdint.h>
#include <string.h>

extern refexport_t PS3_BaseGetRefAPI(refimport_t imp);

static refimport_t scene_ri;
static refexport_t scene_base;
static cvar_t *scene_scale_cvar;
static cvar_t *scene_profile_cvar;
static cvar_t *scene_ztrick_cvar;
static cvar_t *scene_adaptive_cvar;
static cvar_t *scene_sync_cvar;
static cvar_t *scene_tiled_resolve_cvar;

static gcmContextData *scene_context;
static gcmSurface scene_display_surface;
static gcmSurface scene_offscreen_surface;
static int scene_display_valid;
static void *scene_color;
static void *scene_depth;
static u32 scene_color_offset;
static u32 scene_depth_offset;
static int scene_allocation_failed;

static int scene_active;
static int scene_has_view;
static int scene_begin_count;
static int scene_eye;
static float scene_scale;
static int scene_traced_active;
static int scene_traced_fallback;
static int scene_traced_sync_mode;
static unsigned long long scene_last_begin_us;
static int scene_frame_had_view;
static int scene_previous_had_view;
static int scene_auto_profile = -1;
static int scene_auto_step;
static int scene_cadence_miss_streak;

static const float scene_auto_scales[] = {
	1.0f,
	0.875f,
	0.75f,
	2.0f / 3.0f,
	7.0f / 12.0f,
	0.5f
};

typedef struct
{
	int x;
	int y;
	int w;
	int h;
} scene_rect_t;

static scene_rect_t scene_source_rect;
static scene_rect_t scene_destination_rect;
static scene_rect_t scene_destination_logical;

/* rsx_gl.o is object-renamed to call this tracker.  The bridge itself calls
 * the real rsxSetSurface(), so private-surface binds never replace this copy. */
void
PS3_SceneTrackedSetSurface(gcmContextData *context,
	const gcmSurface *surface)
{
	if (context && surface)
	{
		scene_context = context;
		scene_display_surface = *surface;
		scene_display_valid = 1;
	}
	rsxSetSurface(context, surface);
}

static void
PS3_SceneResetAutoPolicy(int profile)
{
	scene_auto_profile = profile;
	scene_cadence_miss_streak = 0;
	if (profile == 1)
	{
		scene_auto_step = 2; /* 960x540 at 1280x720. */
	}
	else if (profile == 2)
	{
		scene_auto_step = 3; /* Approximately 1280x720 at 1080p. */
	}
	else
	{
		scene_auto_step = 0; /* Custom begins native and reacts only to misses. */
	}
}

static void
PS3_SceneSampleCadence(void)
{
	unsigned long long now = (unsigned long long)Sys_Microseconds();
	unsigned long long cadence = scene_last_begin_us && now >= scene_last_begin_us ?
		now - scene_last_begin_us : 0;
	int profile = scene_profile_cvar ? (int)scene_profile_cvar->value : 0;

	scene_last_begin_us = now;
	if (profile != scene_auto_profile)
	{
		PS3_SceneResetAutoPolicy(profile);
	}
	/* Explicit scales and the recovery toggle are deterministic; only zero's
	 * profile-aware policy responds to measured presentation tiers. */
	if (!scene_scale_cvar || scene_scale_cvar->value != 0.0f ||
		!scene_adaptive_cvar || scene_adaptive_cvar->value == 0.0f ||
		!scene_previous_had_view || cadence < 10000 || cadence > 200000)
	{
		scene_cadence_miss_streak = 0;
		return;
	}

	/* A 30 Hz frame arrives near 33,333 us. Repeated intervals above 40 ms
	 * prove that VSync has fallen to the 50 ms tier; two consecutive misses
	 * lower one bounded step. Quality never oscillates upward during gameplay. */
	if (cadence > 40000)
	{
		scene_cadence_miss_streak++;
		if (scene_cadence_miss_streak >= 2 &&
			scene_auto_step < (int)(sizeof(scene_auto_scales) /
				sizeof(scene_auto_scales[0])) - 1)
		{
			char trace[160];
			scene_auto_step++;
			scene_cadence_miss_streak = 0;
			Com_sprintf(trace, sizeof(trace),
				"RSX scene scale: cadence %llu us missed 30 FPS; adaptive scale %.3f",
				cadence, scene_auto_scales[scene_auto_step]);
			PS3_RUNTIME_TRACE(trace);
		}
	}
	else
	{
		scene_cadence_miss_streak = 0;
	}
}

static float
PS3_SceneEffectiveScale(void)
{
	float requested = scene_scale_cvar ? scene_scale_cvar->value : 1.0f;

	/* Zero is the profile-aware automatic policy.  720p Performance shades a
	 * 960x540 scene; 1080p Quality shades approximately 1280x720. */
	if (requested == 0.0f)
	{
		int profile = scene_profile_cvar ? (int)scene_profile_cvar->value : 0;
		if (profile != scene_auto_profile)
		{
			PS3_SceneResetAutoPolicy(profile);
		}
		requested = scene_auto_scales[scene_auto_step];
	}

	if (requested < 0.5f)
	{
		requested = 0.5f;
	}
	if (requested > 1.0f)
	{
		requested = 1.0f;
	}
	return requested;
}

static void
PS3_SceneTargetRect(int *target_y, int *target_height)
{
	int stereo = Cvar_VariableValue("ps3_stereo_enable") > 0.0f;
	int output = (int)Cvar_VariableValue("ps3_stereo_output");
	int height = scene_display_surface.height;

	*target_y = 0;
	*target_height = height;
	if (!stereo)
	{
		return;
	}

	if (output == 0)
	{
		/* HDMI frame packing: 720 rows, a 30-row blanking interval, then
		 * another 720 rows.  vid.height is the logical height of one eye. */
		*target_height = vid.height;
		*target_y = scene_eye == 0 ? 0 : height - *target_height;
	}
	else
	{
		*target_height = height / 2;
		*target_y = scene_eye == 0 ? 0 : height - *target_height;
	}
}

static scene_rect_t
PS3_ScenePhysicalRect(const refdef_t *fd)
{
	scene_rect_t result;
	int target_y;
	int target_height;
	float scale_x;
	float scale_y;

	PS3_SceneTargetRect(&target_y, &target_height);
	scale_x = (float)scene_display_surface.width / (float)vid.width;
	scale_y = (float)target_height / (float)vid.height;
	result.x = (int)((float)fd->x * scale_x);
	result.w = (int)((float)fd->width * scale_x);
	result.h = (int)((float)fd->height * scale_y);
	result.y = target_y + target_height -
		(int)((float)(fd->y + fd->height) * scale_y);
	if (result.w < 1) result.w = 1;
	if (result.h < 1) result.h = 1;
	return result;
}

static int
PS3_SceneAllocate(void)
{
	size_t color_bytes;
	size_t depth_bytes;

	/* This live recovery switch must take effect even after the private surface
	 * has already been allocated earlier in the same renderer session. */
	if (RSXGL_TiledTargetsActive() && scene_tiled_resolve_cvar &&
		scene_tiled_resolve_cvar->value == 0.0f)
	{
		return 0;
	}
	if (scene_color && scene_depth)
	{
		return 1;
	}
	if (scene_allocation_failed || !scene_display_valid || !scene_context)
	{
		return 0;
	}
	/* The private allocation lies outside every registered display tile and is
	 * therefore linear even when the selected back buffer is tiled. Tiled
	 * output is resolved by an ordinary textured draw rather than by the 2D
	 * transfer engine. */
	if (scene_display_surface.type != GCM_SURFACE_TYPE_LINEAR ||
		scene_display_surface.colorLocation[0] != GCM_LOCATION_RSX ||
		scene_display_surface.depthLocation != GCM_LOCATION_RSX)
	{
		return 0;
	}

	color_bytes = (size_t)scene_display_surface.colorPitch[0] *
		(size_t)scene_display_surface.height;
	depth_bytes = (size_t)scene_display_surface.depthPitch *
		(size_t)scene_display_surface.height;
	if (!color_bytes || !depth_bytes || color_bytes > UINT32_MAX ||
		depth_bytes > UINT32_MAX)
	{
		scene_allocation_failed = 1;
		return 0;
	}

	scene_color = rsxMemalign(128, (u32)color_bytes);
	scene_depth = rsxMemalign(128, (u32)depth_bytes);
	if (!scene_color || !scene_depth ||
		rsxAddressToOffset(scene_color, &scene_color_offset) != 0 ||
		rsxAddressToOffset(scene_depth, &scene_depth_offset) != 0)
	{
		if (scene_color) rsxFree(scene_color);
		if (scene_depth) rsxFree(scene_depth);
		scene_color = NULL;
		scene_depth = NULL;
		scene_allocation_failed = 1;
		if (!scene_traced_fallback)
		{
			PS3_RUNTIME_TRACE(
				"RSX scene scale: allocation unavailable; native rendering retained");
			scene_traced_fallback = 1;
		}
		return 0;
	}
	return 1;
}

static void
PS3_SceneBind(void)
{
	scene_offscreen_surface = scene_display_surface;
	scene_offscreen_surface.type = GCM_SURFACE_TYPE_LINEAR;
	scene_offscreen_surface.colorOffset[0] = scene_color_offset;
	scene_offscreen_surface.depthOffset = scene_depth_offset;
	rsxSetSurface(scene_context, &scene_offscreen_surface);
	scene_active = 1;
	scene_has_view = 0;
}

static void
PS3_SceneClearRect(const scene_rect_t *rect)
{
	u32 mask = GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
		GCM_CLEAR_Z | GCM_CLEAR_S;

	rsxSetScissor(scene_context, (u16)rect->x, (u16)rect->y,
		(u16)rect->w, (u16)rect->h);
	rsxSetColorMask(scene_context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
		GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetDepthWriteEnable(scene_context, GCM_TRUE);
	rsxSetStencilMask(scene_context, 0xff);
	rsxSetClearColor(scene_context, 0x00000000);
	rsxSetClearDepthStencil(scene_context, 0xffffff00u);
	rsxClearSurface(scene_context, mask);
}

static void
PS3_SceneRestoreEyeViewport(void)
{
	int target_y;
	int target_height;
	int width = scene_display_surface.width;
	float scale[4];
	float offset[4];

	PS3_SceneTargetRect(&target_y, &target_height);
	scale[0] = width * 0.5f;
	scale[1] = target_height * -0.5f;
	scale[2] = 0.5f;
	scale[3] = 0.0f;
	offset[0] = width * 0.5f;
	offset[1] = target_y + target_height * 0.5f;
	offset[2] = 0.5f;
	offset[3] = 0.0f;
	rsxSetViewport(scene_context, 0, (u16)target_y, (u16)width,
		(u16)target_height, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(scene_context, 0, (u16)target_y, (u16)width,
		(u16)target_height);
}

static void
PS3_SceneResolve(void)
{
	gcmTransferScale scale;
	gcmTransferSurface surface;
	int display_restored = 0;

	if (!scene_active)
	{
		return;
	}

	if (scene_has_view && scene_context && scene_display_valid)
	{
		int sync_mode = scene_sync_cvar ? (int)scene_sync_cvar->value : 1;
		int tiled_resolve = RSXGL_TiledTargetsActive();

		if (sync_mode < 0) sync_mode = 0;
		if (sync_mode > 2) sync_mode = 2;
		if (tiled_resolve)
		{
			/* A 2D transfer cannot address the tile metadata of the display
			 * target. Restore it and let the 3D engine sample the private linear
			 * scene into one isolated quad. Z-cull regions are address-ranged, so
			 * the unrelated private depth allocation does not need rebinding. */
			rsxSetSurface(scene_context, &scene_display_surface);
			display_restored = 1;
			PS3_SceneRestoreEyeViewport();
			/* Physical RSX compatibility comes before eliminating this barrier.
			 * The private scene has just been a color target and is about to be
			 * sampled as a texture after the display target is restored. Same-FIFO
			 * ordering plus texture-cache invalidation should be sufficient in
			 * theory, but the returned-machine builds never established that on
			 * hardware. Keep the producer fully retired for this first original-
			 * toolchain tiled-resolve package; a later physical A/B may relax it. */
			rsxSetWaitForIdle(scene_context);
			if (!RSXGL_ResolveLinearSurface(scene_color_offset,
				scene_offscreen_surface.colorPitch[0],
				scene_offscreen_surface.width,
				scene_offscreen_surface.height,
				scene_source_rect.x, scene_source_rect.y,
				scene_source_rect.w, scene_source_rect.h,
				(float)scene_destination_logical.x,
				(float)scene_destination_logical.y,
				(float)scene_destination_logical.w,
				(float)scene_destination_logical.h) && !scene_traced_fallback)
			{
				PS3_RUNTIME_TRACE(
					"RSX scene scale: tiled textured resolve rejected; direct recovery required");
				scene_traced_fallback = 1;
			}
			if (!scene_traced_sync_mode)
			{
				PS3_RUNTIME_TRACE(
					"RSX scene scale: tiled 3D textured resolve active (hardware-safe pre-idle)");
				scene_traced_sync_mode = 1;
			}
		}
		else
		{
		memset(&scale, 0, sizeof(scale));
		memset(&surface, 0, sizeof(surface));
		/* R_SetGL2D already closes the native triangle stream. PSL1GHT's
		 * reference blitting path orders a 3D clear directly into the scale
		 * engine, so the default does not add another full pre-blit drain.
		 * Mode 2 retains the former belt-and-suspenders barrier for recovery. */
		if (sync_mode >= 2)
		{
			rsxSetWaitForIdle(scene_context);
		}
		scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
		scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
		scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
		scale.clipX = 0;
		scale.clipY = 0;
		scale.clipW = scene_display_surface.width;
		scale.clipH = scene_display_surface.height;
		scale.outX = (s16)scene_destination_rect.x;
		scale.outY = (s16)scene_destination_rect.y;
		scale.outW = (u16)scene_destination_rect.w;
		scale.outH = (u16)scene_destination_rect.h;
		scale.ratioX = rsxGetFixedSint32(
			(float)scene_source_rect.w / (float)scene_destination_rect.w);
		scale.ratioY = rsxGetFixedSint32(
			(float)scene_source_rect.h / (float)scene_destination_rect.h);
		scale.inW = scene_offscreen_surface.width;
		scale.inH = scene_offscreen_surface.height;
		scale.pitch = (u16)scene_offscreen_surface.colorPitch[0];
		scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
		scale.interp = GCM_TRANSFER_INTERPOLATOR_LINEAR;
		scale.offset = scene_color_offset;
		scale.inX = rsxGetFixedUint16((float)scene_source_rect.x);
		scale.inY = rsxGetFixedUint16((float)scene_source_rect.y);
		surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
		surface.pitch = (u16)scene_display_surface.colorPitch[0];
		surface.offset = scene_display_surface.colorOffset[0];
		rsxSetTransferScaleMode(scene_context,
			GCM_TRANSFER_LOCAL_TO_LOCAL, GCM_TRANSFER_SURFACE);
		rsxSetTransferScaleSurface(scene_context, &scale, &surface);
		/* The following UI switches from the asynchronous 2D scale engine back
		 * to the 3D backend. Keep that cross-engine barrier by default. Mode 0
		 * is an explicit diagnostic for FIFO-only ordering; mode 2 additionally
		 * restores the former pre-blit barrier above. */
		if (sync_mode >= 1)
		{
			rsxSetWaitForIdle(scene_context);
		}
		if (!scene_traced_sync_mode)
		{
			char trace[128];
			Com_sprintf(trace, sizeof(trace),
				"RSX scene scale: synchronization mode %d (0 FIFO, 1 post, 2 full)",
				sync_mode);
			PS3_RUNTIME_TRACE(trace);
			scene_traced_sync_mode = 1;
		}
		}
	}

	if (!display_restored && scene_context && scene_display_valid)
	{
		rsxSetSurface(scene_context, &scene_display_surface);
		PS3_SceneRestoreEyeViewport();
	}
	scene_active = 0;
	scene_has_view = 0;
}

static void
PS3_SceneBeginFrame(float camera_separation)
{
	float requested;

	scene_eye = scene_begin_count;
	if (scene_begin_count++ == 0)
	{
		PS3_SceneSampleCadence();
		scene_frame_had_view = 0;
	}
	scene_base.BeginFrame(camera_separation);
	requested = PS3_SceneEffectiveScale();
	if (requested >= 0.999f ||
		(scene_ztrick_cvar && scene_ztrick_cvar->value != 0.0f) ||
		!PS3_SceneAllocate())
	{
		scene_active = 0;
		return;
	}

	scene_scale = requested;
	PS3_SceneBind();
	if (!scene_traced_active)
	{
		char trace[128];
		Com_sprintf(trace, sizeof(trace),
			"RSX scene scale: %.3f 3D resolve active; native-resolution UI retained",
			scene_scale);
		PS3_RUNTIME_TRACE(trace);
		scene_traced_active = 1;
	}
}

static void
PS3_SceneRenderFrame(refdef_t *fd)
{
	refdef_t scaled;

	if (fd)
	{
		scene_frame_had_view = 1;
	}
	if (!scene_active || !fd)
	{
		scene_base.RenderFrame(fd);
		return;
	}

	scaled = *fd;
	scaled.width = (int)((float)fd->width * scene_scale + 0.5f);
	scaled.height = (int)((float)fd->height * scene_scale + 0.5f);
	/* Even dimensions keep the scale engine and both stereo eyes on identical
	 * sample centers. */
	scaled.width &= ~1;
	scaled.height &= ~1;
	if (scaled.width < 2) scaled.width = 2;
	if (scaled.height < 2) scaled.height = 2;
	scaled.x = fd->x + (fd->width - scaled.width) / 2;
	scaled.y = fd->y + (fd->height - scaled.height) / 2;
	scene_destination_rect = PS3_ScenePhysicalRect(fd);
	scene_source_rect = PS3_ScenePhysicalRect(&scaled);
	scene_destination_logical.x = fd->x;
	scene_destination_logical.y = fd->y;
	scene_destination_logical.w = fd->width;
	scene_destination_logical.h = fd->height;
	PS3_SceneClearRect(&scene_source_rect);
	scene_has_view = 1;
	scene_base.RenderFrame(&scaled);
}

static qboolean
PS3_SceneEndWorldRenderpass(void)
{
	qboolean result = scene_base.EndWorldRenderpass();
	PS3_SceneResolve();
	return result;
}

static void
PS3_SceneEndFrame(void)
{
	/* Timerefresh and unusual callers may omit the explicit world/UI boundary. */
	PS3_SceneResolve();
	scene_base.EndFrame();
	scene_previous_had_view = scene_frame_had_view;
	scene_begin_count = 0;
	scene_eye = 0;
}

static void
PS3_SceneShutdownContext(void)
{
	int exiting = Sys_PS3_ExitRequested();

	/* The base shutdown supplies the GPU finish for normal teardown. */
	scene_base.ShutdownContext();
	if (!exiting)
	{
		if (scene_color) rsxFree(scene_color);
		if (scene_depth) rsxFree(scene_depth);
	}
	scene_color = NULL;
	scene_depth = NULL;
	scene_color_offset = 0;
	scene_depth_offset = 0;
	scene_context = NULL;
	scene_display_valid = 0;
	scene_allocation_failed = 0;
	scene_active = 0;
	scene_has_view = 0;
	scene_begin_count = 0;
	scene_eye = 0;
	scene_last_begin_us = 0;
	scene_frame_had_view = 0;
	scene_previous_had_view = 0;
	scene_auto_profile = -1;
	scene_auto_step = 0;
	scene_cadence_miss_streak = 0;
	scene_traced_sync_mode = 0;
}

Q2_DLL_EXPORTED refexport_t
GetRefAPI(refimport_t imp)
{
	refexport_t result;

	scene_ri = imp;
	result = PS3_BaseGetRefAPI(imp);
	scene_base = result;
	scene_scale_cvar = scene_ri.Cvar_Get("ps3_rsx_scene_scale", "0",
		CVAR_ARCHIVE);
	scene_profile_cvar = scene_ri.Cvar_Get("ps3_rsx_performance_profile", "0",
		CVAR_ARCHIVE);
	scene_ztrick_cvar = scene_ri.Cvar_Get("gl1_ztrick", "0", 0);
	scene_adaptive_cvar = scene_ri.Cvar_Get("ps3_rsx_scene_adaptive", "1",
		CVAR_ARCHIVE);
	scene_sync_cvar = scene_ri.Cvar_Get("ps3_rsx_scene_sync", "1",
		CVAR_ARCHIVE);
	scene_tiled_resolve_cvar = scene_ri.Cvar_Get(
		"ps3_rsx_scene_tiled_resolve", "1", 0);
	result.BeginFrame = PS3_SceneBeginFrame;
	result.RenderFrame = PS3_SceneRenderFrame;
	result.EndWorldRenderpass = PS3_SceneEndWorldRenderpass;
	result.EndFrame = PS3_SceneEndFrame;
	result.ShutdownContext = PS3_SceneShutdownContext;
	return result;
}
