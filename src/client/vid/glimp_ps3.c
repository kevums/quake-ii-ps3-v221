/*
 * Copyright (C) 2010 Yamagi Burmeister
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This is the client side of the render backend, implemented trough
 * PS3's GCM. Screen manipulation implemented here, everything else is
 * in the renderers.
 *
 * =======================================================================
 * 
 * Notes on PS3 changes:
 *     This used do link screen functions to gcm based renderers
 * 
 * -----------------------------------------------------------------------
 */

#include "../../common/header/common.h"
#include "header/ref.h"

#include <stdio.h>

static cvar_t *vid_displayrefreshrate;
static cvar_t *vid_displayindex;
static cvar_t *vid_rate;
static cvar_t *ps3_stereo_enable;
static cvar_t *ps3_stereo_output;
static cvar_t *ps3_stereo_depth;
static cvar_t *ps3_output_scale;

static qboolean initSuccessful = false;
static char **displayindices = NULL;
static int num_displays = 0;

// --- for compatibility with gcm
#include <sysutil/video.h>
#include <malloc.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sys/systime.h>

static gcmContextData *context = NULL;;
static void *host_addr = NULL;
static videoDeviceInfo vDeviceInfo;
int n_num_displays;
static char** n_displayindices;

#define CB_SIZE 0x100000
#define HOST_SIZE (32*1024*1024)
#define GCM_LABEL_INDEX 255
// ----

int glimp_refreshRate = -1;
static int last_display = 0;
static u8 standard_resolution = VIDEO_RESOLUTION_UNDEFINED;
static u8 standard_aspect = VIDEO_ASPECT_AUTO;
static u8 confirmed_resolution = VIDEO_RESOLUTION_UNDEFINED;
static qboolean traced_vsync_prewait = false;
static qboolean traced_vsync_prewait_timeout = false;
static qboolean traced_vblank_divisor = false;
static qboolean traced_vblank_divisor_timeout = false;
static qboolean scheduled_flip_queue_active = false;
static u64 scheduled_flip_queue_vblank = 0;

#ifdef PS3_DIAGNOSTIC_SCANOUT
/* Intentionally retained until process teardown: display scanout may still be
 * reading this allocation while the regular renderer replaces buffer zero. */
static void *recovery_scanout_buffer;

static void
GLimp_ShowRecoveryScanoutPattern(gcmContextData *screen_context,
	const videoResolution *resolution)
{
	u32 offset;
	u32 pitch;
	u32 *pixels;
	u32 y;
	u32 x;
	u32 wait_iterations = 0;
	size_t bytes;
	static const u32 bars[4] = {
		0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu
	};

	if (!screen_context || !resolution || recovery_scanout_buffer)
	{
		return;
	}

	pitch = resolution->width * sizeof(u32);
	bytes = (size_t)pitch * resolution->height;
	recovery_scanout_buffer = rsxMemalign(64, bytes);
	if (!recovery_scanout_buffer ||
		rsxAddressToOffset(recovery_scanout_buffer, &offset) != 0)
	{
		PS3_BOOT_TRACE("PS3 recovery scanout: allocation failed");
		return;
	}

	pixels = (u32 *)recovery_scanout_buffer;
	for (y = 0; y < resolution->height; y++)
	{
		u32 color = bars[(y * 4u) / resolution->height];
		for (x = 0; x < resolution->width; x++)
		{
			pixels[(size_t)y * resolution->width + x] = color;
		}
	}

	PS3_BOOT_TRACE("PS3 recovery scanout: queueing RGBW pattern");
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	gcmResetFlipStatus();
	if (gcmSetDisplayBuffer(0, offset, pitch, resolution->width,
		resolution->height) != 0 || gcmSetFlip(screen_context, 0) != 0)
	{
		PS3_BOOT_TRACE("PS3 recovery scanout: display buffer or flip rejected");
		return;
	}
	rsxFlushBuffer(screen_context);
	while (gcmGetFlipStatus() != 0 && wait_iterations++ < 100000u)
	{
		sysUsleep(30);
	}
	if (gcmGetFlipStatus() != 0)
	{
		PS3_BOOT_TRACE("PS3 recovery scanout: flip completion timeout");
		return;
	}
	PS3_BOOT_TRACE("PS3 recovery scanout: RGBW pattern visible window started");
	sysUsleep(2000000);
	PS3_BOOT_TRACE("PS3 recovery scanout: RGBW pattern visible window complete");
}
#endif

/* ps3_output_scale predates the native renderer but was previously a no-op.
 * Zero keeps the XMB timing, one forces a real 720p performance target, and
 * two forces a real 1080p quality target when the display advertises it. */
static u8
GLimp_Select2DResolution(void)
{
#ifdef PS3_SAFE_VIDEO_720
	/* Recovery builds must not inherit a broken title/XMB presentation mode.
	 * Select the universally supported 720p path before consulting archived
	 * output scaling. Leave the normal build completely unchanged. */
	if (videoGetResolutionAvailability(VIDEO_PRIMARY,
		VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9, 0) > 0)
	{
		Com_Printf("PS3 recovery video: forcing standard 720p output.\n");
		Cvar_Set("ps3_output_scale", "1");
		Cvar_Set("ps3_stereo_enable", "0");
		return VIDEO_RESOLUTION_720;
	}
	Com_Printf("PS3 recovery video: 720p unavailable; using the XMB mode.\n");
#endif

	if (ps3_output_scale && ps3_output_scale->value >= 1.0f &&
		ps3_output_scale->value < 2.0f)
	{
		if (videoGetResolutionAvailability(VIDEO_PRIMARY,
			VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9, 0) > 0)
		{
			return VIDEO_RESOLUTION_720;
		}

		Com_Printf("720p output is unavailable; restoring the XMB video mode.\n");
		Cvar_Set("ps3_output_scale", "0");
	}
	else if (ps3_output_scale && ps3_output_scale->value >= 2.0f &&
		ps3_output_scale->value < 3.0f)
	{
		if (videoGetResolutionAvailability(VIDEO_PRIMARY,
			VIDEO_RESOLUTION_1080, VIDEO_ASPECT_16_9, 0) > 0)
		{
			return VIDEO_RESOLUTION_1080;
		}

		Com_Printf("1080p output is unavailable; restoring the XMB video mode.\n");
		Cvar_Set("ps3_output_scale", "0");
	}

	return standard_resolution;
}

/* Zero is the native HDMI 720p frame-packed timing. One is a standard 720p
 * top-and-bottom compatibility signal for displays, projectors, capture
 * devices, or receivers that expose the raw 1470-line frame-packed buffer. */
static qboolean
GLimp_UsesFramePackedStereo(void)
{
	return ps3_stereo_enable && ps3_stereo_enable->value > 0.0f &&
		(!ps3_stereo_output || ps3_stereo_output->value < 1.0f);
}

static u8
GLimp_SelectOutputResolution(void)
{
	u8 output_resolution = GLimp_Select2DResolution();

#ifdef PS3_SAFE_VIDEO_720
	/* Never let an archived 3D setting replace the recovery timing. */
	return output_resolution;
#endif

	if (!ps3_stereo_enable || ps3_stereo_enable->value <= 0.0f)
	{
		return output_resolution;
	}

	if (!GLimp_UsesFramePackedStereo())
	{
		if (videoGetResolutionAvailability(VIDEO_PRIMARY,
			VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9, 0) > 0)
		{
			return VIDEO_RESOLUTION_720;
		}

		Com_Printf("720p top-and-bottom output isn't available; disabling stereo.\n");
		Cvar_Set("ps3_stereo_enable", "0");
		return output_resolution;
	}

	if (videoGetResolutionAvailability(VIDEO_PRIMARY,
		VIDEO_RESOLUTION_720_3D_FRAME_PACKING,
		VIDEO_ASPECT_16_9, 0) > 0)
	{
		return VIDEO_RESOLUTION_720_3D_FRAME_PACKING;
	}

	Com_Printf("HDMI 720p frame packing isn't available; disabling stereo.\n");
	Cvar_Set("ps3_stereo_enable", "0");
	return output_resolution;
}

static u8
GLimp_AspectForResolution(u8 resolution)
{
	if (resolution == VIDEO_RESOLUTION_720 ||
		resolution == VIDEO_RESOLUTION_720_3D_FRAME_PACKING)
	{
		return VIDEO_ASPECT_16_9;
	}

	return standard_aspect;
}

/* Existing physical-console runs report zero before/asynchronously during a
 * mode change, while a completed blocking videoConfigure reports the SDK's
 * documented VIDEO_STATE_ENABLED value of one. Both states have a usable
 * primary output on this backend; only BUSY/unknown values are rejected. */
static qboolean
GLimp_VideoStateUsable(const videoState *state)
{
	return state && (state->state == 0 ||
		state->state == VIDEO_STATE_ENABLED);
}

static qboolean
GLimp_ConfirmOutputMode(u8 requested_resolution, const char *phase)
{
	videoState state;
	videoResolution res;
	videoDeviceInfo device;
	char trace[192];
	int attempt;
	int port_type = -1;

	memset(&state, 0, sizeof(state));
	memset(&res, 0, sizeof(res));
	for (attempt = 0; attempt < 500; attempt++)
	{
		if (videoGetState(VIDEO_PRIMARY, 0, &state) == 0 &&
			GLimp_VideoStateUsable(&state) &&
			state.displayMode.resolution == requested_resolution &&
			videoGetResolution(state.displayMode.resolution, &res) == 0)
		{
			break;
		}

		sysUsleep(1000);
	}

	if (attempt == 500)
	{
		confirmed_resolution = VIDEO_RESOLUTION_UNDEFINED;
		Com_Printf("PS3 video mode 0x%02x did not become active.\n",
			(unsigned)requested_resolution);
		snprintf(trace, sizeof(trace),
			"PS3 video: %s mode confirmation timed out (requested 0x%02x, last 0x%02x, state %u)",
			phase, (unsigned)requested_resolution,
			(unsigned)state.displayMode.resolution, (unsigned)state.state);
		PS3_RUNTIME_TRACE(trace);
		return false;
	}

	if (videoGetDeviceInfo(VIDEO_PRIMARY, 0, &device) == 0)
	{
		port_type = device.portType;
	}

	confirmed_resolution = requested_resolution;
	Com_Printf("PS3 video mode 0x%02x confirmed at %ux%u.\n",
		(unsigned)requested_resolution, (unsigned)res.width,
		(unsigned)res.height);
	snprintf(trace, sizeof(trace),
		"PS3 video: %s requested 0x%02x confirmed 0x%02x %ux%u state %u port 0x%02x",
		phase, (unsigned)requested_resolution,
		(unsigned)state.displayMode.resolution, (unsigned)res.width,
		(unsigned)res.height, (unsigned)state.state,
		(unsigned)(port_type & 0xff));
	PS3_RUNTIME_TRACE(trace);

	return true;
}

/*
 * Resets the display index Cvar if out of bounds
 */
static void
ClampDisplayIndexCvar(void)
{
	if (!vid_displayindex)
	{
		// uninitialized render?
		return;
	}

	if (vid_displayindex->value < 0 || vid_displayindex->value >= n_num_displays)
	{
		Cvar_SetValue("vid_displayindex", 0);
	}
}

static void
ClearDisplayIndices(void)
{
	if (displayindices)
	{
		for (int i = 0; i < num_displays; ++i)
		{
			free(displayindices[i]);
		}

		free(displayindices);
		displayindices = NULL;
	}
}

void
GLimp_initDisplayIndices()
{
	n_displayindices = malloc((n_num_displays + 1) * sizeof(char*));

	for (int i = 0; i < n_num_displays; ++i)
	{
		/* There are a maximum of 10 digits in 32 bit int + 1 for the NULL terminator. */
		n_displayindices[i] = malloc(11 * sizeof( char ));
		YQ2_COM_CHECK_OOM(n_displayindices[i], "malloc()", 11 * sizeof(char))

		snprintf(n_displayindices[i], 11, "%d", i);
	}

	/* The last entry is NULL to indicate the list of strings ends. */
	n_displayindices[n_num_displays] = 0;
}

#define TRY_REFRESH(rates, mode) if ((rates & mode) != 0) return mode

u16
maxAvailableRate(u16 refreshRates)
{
	TRY_REFRESH(refreshRates, VIDEO_REFRESH_60HZ);
	TRY_REFRESH(refreshRates, VIDEO_REFRESH_59_94HZ);
	TRY_REFRESH(refreshRates, VIDEO_REFRESH_50HZ);
	TRY_REFRESH(refreshRates, VIDEO_REFRESH_30HZ);

	return VIDEO_REFRESH_AUTO;
}

const char*
refreshRateName(u16 refreshRate)
{
	if (refreshRate == VIDEO_REFRESH_60HZ) return "60";
	if (refreshRate == VIDEO_REFRESH_59_94HZ) return "59.94";
	if (refreshRate == VIDEO_REFRESH_50HZ) return "50";
	if (refreshRate == VIDEO_REFRESH_30HZ) return "30";

	return "0";
}

int
refreshRateToI(u16 refreshRate)
{
	if (refreshRate == VIDEO_REFRESH_60HZ) return 60;
	if (refreshRate == VIDEO_REFRESH_59_94HZ) return 60;
	if (refreshRate == VIDEO_REFRESH_50HZ) return 50;
	if (refreshRate == VIDEO_REFRESH_30HZ) return 30;

	return -1;
}

/*
 * Lists all available display modes.
 */
static void
GLimp_printDisplayModes(void)
{
	videoResolution res;
	u16 rate;
	for (size_t i = 0; i < vDeviceInfo.availableModeCount; ++i)
	{
		videoGetResolution(vDeviceInfo.availableModes[i].resolution, &res);
		rate = maxAvailableRate(vDeviceInfo.availableModes[i].refreshRates);

		Com_Printf(" - Mode %ld: %dx%d@%s\n",
			i,
			res.width,
			res.height,
			refreshRateName(rate)
		);
	}
}

qboolean
updateConfig()
{
	videoDeviceInfo vDevInfo;

	/* PS3 had 2 'videoOut' types: primary and secondary.
	   Don't know in which situations seconday video might
	   be used. videoGetNumberOfDevice returns number of
	   devices on <videoOut> (can't imagine what it could mean)
	   So each [<videoOut>][<videoOutDisplayIndex>] had it's
	   own 'videoDeviceInfo' structure and it's technically
	   posible to switch between all of them at runtime.

	   To keep things simple let's stay with PRIMARY
	   videoOut and use only first device on it. */

	if (videoGetDeviceInfo(VIDEO_PRIMARY, 0, &vDevInfo) != 0)
	{
		return false;
	}

	vDeviceInfo = vDevInfo;

	return true;
}

/*
 * A regular vid_restart keeps the RSX context alive.  Reconfigure the PS3
 * video output here so changing ps3_stereo_enable from the video menu can
 * switch between the user's normal mode and HDMI 720p frame packing.
 */
static qboolean
GLimp_ReconfigureOutput(void)
{
	videoState state;
	videoConfiguration vconfig;
	videoResolution res;
	u8 output_resolution;
	u32 sLabelVal = 1;
	u32 wait_iterations = 0;

	if (!context || videoGetState(VIDEO_PRIMARY, 0, &state) != 0)
	{
		Com_Printf("Couldn't query PS3 video output for mode switch.\n");
		return false;
	}

	if (!GLimp_VideoStateUsable(&state))
	{
		Com_Printf("PS3 video output is disabled or busy.\n");
		return false;
	}

	/* Remember the user's original 2D mode so disabling stereo or performance
	 * output restores it. Do not replace it with our own forced 720p mode on a
	 * later vid_restart. */
	if (standard_resolution == VIDEO_RESOLUTION_UNDEFINED &&
		state.displayMode.resolution < VIDEO_RESOLUTION_720_3D_FRAME_PACKING)
	{
		standard_resolution = state.displayMode.resolution;
		standard_aspect = state.displayMode.aspect;
	}

	if (standard_resolution == VIDEO_RESOLUTION_UNDEFINED)
	{
		standard_resolution = VIDEO_RESOLUTION_720;
		standard_aspect = VIDEO_ASPECT_16_9;
	}

	output_resolution = GLimp_SelectOutputResolution();

	if (state.displayMode.resolution == output_resolution &&
		confirmed_resolution == output_resolution)
	{
		return true;
	}

	if (videoGetResolution(output_resolution, &res) != 0)
	{
		Com_Printf("Couldn't resolve requested PS3 video mode.\n");
		return false;
	}

	/* Make sure no RSX commands still refer to the old display buffer. */
	rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
	rsxSetWaitLabel(context, GCM_LABEL_INDEX, sLabelVal);
	sLabelVal++;
	rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
	rsxFlushBuffer(context);

	while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != sLabelVal)
	{
		if (++wait_iterations >= 100000)
		{
			Com_Printf("Timed out waiting for RSX before PS3 video mode switch.\n");
			return false;
		}

		sysUsleep(30);
	}

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = output_resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width * sizeof(u32);
	vconfig.aspect = GLimp_AspectForResolution(output_resolution);

	/* The renderer consumes the new dimensions immediately after this returns;
	 * complete the HDMI timing/InfoFrame transition before its first flip. */
	if (videoConfigure(VIDEO_PRIMARY, &vconfig, NULL, 1) != 0)
	{
		Com_Printf("PS3 video output rejected the requested mode.\n");
		return false;
	}

	if (!GLimp_ConfirmOutputMode(output_resolution, "restart"))
	{
		return false;
	}

	gcmSetFlipMode(GCM_FLIP_VSYNC);
	gcmResetFlipStatus();
	Com_Printf("PS3 output switched to %s.\n",
		(output_resolution == VIDEO_RESOLUTION_720_3D_FRAME_PACKING) ?
		"720p stereoscopic frame packing" :
		((ps3_stereo_enable && ps3_stereo_enable->value > 0.0f) ?
		"720p stereoscopic top-and-bottom" : "standard 2D"));

	return true;
}

gcmContextData *
GLimp_initScreen(void *screen_host_addr, u32 size)
{
	gcmContextData *context = NULL; /* Context to keep track of the RSX buffer. */
	videoState state;
	videoConfiguration vconfig;
	videoResolution res; /* Screen Resolution */

	/* Initilise Reality, which sets up the command buffer and shared IO memory */
	PS3_BOOT_TRACE("GLimp_initScreen: before rsxInit");
	rsxInit(&context, CB_SIZE, size, screen_host_addr);
	if (context == NULL)
		goto error;
	PS3_BOOT_TRACE("GLimp_initScreen: rsxInit complete");

	/* Get the state of the display */
	if (videoGetState (0, 0, &state) != 0)
		goto error;
	PS3_BOOT_TRACE("GLimp_initScreen: videoGetState complete");

	/*
	 * PSL1GHT exposes the native video-out state here. The original PS3
	 * backend and real hardware use zero for an active primary display.
	 * Keep this in sync with GLimp_InitGraphics() below.
	 */
	if (!GLimp_VideoStateUsable(&state))
		goto error;

	if (state.displayMode.resolution < VIDEO_RESOLUTION_720_3D_FRAME_PACKING)
	{
		standard_resolution = state.displayMode.resolution;
		standard_aspect = state.displayMode.aspect;
	}

	if (standard_resolution == VIDEO_RESOLUTION_UNDEFINED)
	{
		standard_resolution = VIDEO_RESOLUTION_720;
	}

	u8 output_resolution = GLimp_SelectOutputResolution();

	if (ps3_stereo_enable && ps3_stereo_enable->value > 0.0f)
	{
		Com_Printf("Using 720p stereoscopic %s output.\n",
			GLimp_UsesFramePackedStereo() ? "frame-packed" : "top-and-bottom");
	}

	/* Get the selected standard or stereoscopic resolution. */
	if (videoGetResolution(output_resolution, &res) != 0)
		goto error;
	PS3_BOOT_TRACE("GLimp_initScreen: resolution selected");

	/* Configure the buffer format to xRGB */
	memset (&vconfig, 0, sizeof(videoConfiguration));
	vconfig.resolution = output_resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width * sizeof(u32);
	vconfig.aspect = GLimp_AspectForResolution(output_resolution);

	// waitRSXIdle
	{
		u32 sLabelVal = 1;
		u32 wait_iterations = 0;

		rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
		rsxSetWaitLabel(context, GCM_LABEL_INDEX, sLabelVal);

		sLabelVal++;

		// waitFinish
		{
			rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);

			rsxFlushBuffer(context);

			while( *(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != sLabelVal)
			{
				if (++wait_iterations >= 100000)
				{
					PS3_BOOT_TRACE("GLimp_initScreen: RSX idle timeout");
					goto error;
				}

				sysUsleep(30);
			}
		}
	}
	PS3_BOOT_TRACE("GLimp_initScreen: RSX idle complete");

	if (videoConfigure (VIDEO_PRIMARY, &vconfig, NULL, 1) != 0)
		goto error;
	PS3_BOOT_TRACE("GLimp_initScreen: videoConfigure complete");

	if (!GLimp_ConfirmOutputMode(output_resolution, "startup"))
		goto error;

#ifdef PS3_DIAGNOSTIC_SCANOUT
	GLimp_ShowRecoveryScanoutPattern(context, &res);
#endif

	gcmSetFlipMode(GCM_FLIP_VSYNC); // Wait for VSYNC to flip

	gcmResetFlipStatus();
	PS3_BOOT_TRACE("GLimp_initScreen: complete");

	return context;

error:
	PS3_BOOT_TRACE("GLimp_initScreen: error");
	if (context)
		rsxFinish(context, 0);

	return NULL;
}

/*
 * Initializes the video subsystem. Must
 * be called before anything else.
 */
qboolean
GLimp_Init(void)
{
	PS3_BOOT_TRACE("GLimp_Init: entered");

	vid_displayrefreshrate = Cvar_Get("vid_displayrefreshrate", "-1", CVAR_ARCHIVE);
	vid_displayindex = Cvar_Get("vid_displayindex", "0", CVAR_ARCHIVE);
	vid_rate = Cvar_Get("vid_rate", "-1", CVAR_ARCHIVE);
	ps3_stereo_enable = Cvar_Get("ps3_stereo_enable", "0", CVAR_ARCHIVE);
	ps3_stereo_output = Cvar_Get("ps3_stereo_output", "0", CVAR_ARCHIVE);
	ps3_stereo_depth = Cvar_Get("ps3_stereo_depth", "1.0", CVAR_ARCHIVE);
	ps3_output_scale = Cvar_Get("ps3_output_scale", "0", CVAR_ARCHIVE);
	if (ps3_output_scale->value < 0.0f || ps3_output_scale->value >= 3.0f)
	{
		Com_Printf("Invalid ps3_output_scale; restoring the XMB video mode.\n");
		Cvar_Set("ps3_output_scale", "0");
	}

	if (ps3_stereo_enable->value < 0.0f || ps3_stereo_enable->value > 1.0f)
	{
		Com_Printf("Invalid ps3_stereo_enable; disabling stereoscopic output.\n");
		Cvar_Set("ps3_stereo_enable", "0");
	}

	if (ps3_stereo_output->value < 0.0f || ps3_stereo_output->value > 1.0f)
	{
		Com_Printf("Invalid ps3_stereo_output; restoring HDMI frame packing.\n");
		Cvar_Set("ps3_stereo_output", "0");
	}

	if (ps3_stereo_depth->value < 0.0f || ps3_stereo_depth->value > 2.0f)
	{
		Com_Printf("ps3_stereo_depth must be in the range 0.0 to 2.0.\n");
		Cvar_Set("ps3_stereo_depth", "1.0");
	}

	if (!context)
	{
		Com_Printf("-------- vid initialization --------\n");

		/* Allocate a 1Mb buffer, alligned to a 1Mb boundary
		* to be our shared IO memory with the RSX. */
		host_addr = memalign(1024*1024, HOST_SIZE);
		if (host_addr == NULL)
		{
			Com_Printf("%s couldn't allocate the RSX host buffer\n", __func__);
			return false;
		}

		context = GLimp_initScreen(host_addr, HOST_SIZE);
		if (context == NULL)
		{
			Com_Printf("%s initScreen returned NULL\n", __func__);
			free(host_addr);
			host_addr = NULL;
			return false;
		}

		if (updateConfig() == false)
		{
			Com_Printf("Couldn't get display info\n");
			if (context)
			{
				rsxFinish(context, 0);
				context = NULL;
			}

			if (host_addr)
			{
				free(host_addr);
				host_addr = NULL;
			}

			context = NULL;
			return false;
		}
		PS3_BOOT_TRACE("GLimp_Init: display config complete");
		Com_Printf("Using native gcm...\n");
		n_num_displays = 1;
		GLimp_initDisplayIndices();
		ClampDisplayIndexCvar();
		Com_Printf("Display modes:\n");
		GLimp_printDisplayModes();

		Com_Printf("------------------------------------\n\n");
	}

	PS3_BOOT_TRACE("GLimp_Init: complete");
	return true;
}

/*
 * Shuts the render backend down
 */
static void
ShutdownGraphics(void)
{
	ClampDisplayIndexCvar();

	// make sure that after vid_restart the refreshrate will be queried from S.D.L.2 again.
	glimp_refreshRate = -1;
	scheduled_flip_queue_active = false;
	scheduled_flip_queue_vblank = 0;

	initSuccessful = false; // not initialized anymore
}

/*
 * Shuts the video subsystem down. Must
 * be called after evrything's finished and
 * clean up.
 */
void
GLimp_Shutdown(void)
{
	ShutdownGraphics();
	PS3_RUNTIME_TRACE("GLimp_Shutdown: entered");

	if (Sys_PS3_ExitRequested())
	{
		/* The XMB may already have stopped the flip queue. Do not enqueue a
		 * wait or finish behind it; LV2 reclaims GCM/host allocations when the
		 * process exits. Clearing local ownership prevents later reuse. */
		PS3_RUNTIME_TRACE("GLimp_Shutdown: XMB exit leaves GCM resources to process teardown");
		context = NULL;
		host_addr = NULL;
		ClearDisplayIndices();
		standard_resolution = VIDEO_RESOLUTION_UNDEFINED;
		standard_aspect = VIDEO_ASPECT_AUTO;
		confirmed_resolution = VIDEO_RESOLUTION_UNDEFINED;
		return;
	}

	if (context)
	{
		gcmSetWaitFlip(context);
		Com_Printf("rsxFinish(context, 1)\n");
		rsxFinish(context, 1);
		context = NULL;
	}

	if (host_addr)
	{
		Com_Printf("free(host_addr)\n");
		free(host_addr);
		host_addr = NULL;
	}

	ClearDisplayIndices();
	standard_resolution = VIDEO_RESOLUTION_UNDEFINED;
	standard_aspect = VIDEO_ASPECT_AUTO;
	confirmed_resolution = VIDEO_RESOLUTION_UNDEFINED;
	PS3_RUNTIME_TRACE("GLimp_Shutdown: complete");
}

qboolean
GLimp_InitGraphics(int fullscreen, int *pwidth, int *pheight)
{
	int width = *pwidth;
	int height = *pheight;
	PS3_BOOT_TRACE("GLimp_InitGraphics: entered");

	/* HDMI frame packing is a 1280x720-per-eye output mode. */
	if (ps3_stereo_enable && ps3_stereo_enable->value > 0.0f &&
		(width > 1280 || height > 720))
	{
		Com_Printf("Clamping the internal video mode to 1280x720 for PS3 3D.\n");
		width = *pwidth = 1280;
		height = *pheight = 720;
		Cvar_SetValue("r_mode", 14);
	}

	static qboolean contextInited = false;

	if (contextInited)
	{
		Com_Printf("re: %p\n", re.ShutdownContext);
		re.ShutdownContext();
		ShutdownGraphics();
		contextInited = false;
	}

	if (!GLimp_ReconfigureOutput())
	{
		return false;
	}

	/* Resolve the render canvas against the newly selected output mode. */
	{
		videoResolution res;
		videoState state;
		char trace[160];
		if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0)
		{
			Com_Printf("Can't get video state for default display.\n");
			return false;
		}

		if (!GLimp_VideoStateUsable(&state))
		{
			Com_Printf("Default display is disabled or busy.\n");
			return false;
		}

		if (videoGetResolution(state.displayMode.resolution, &res) != 0)
		{
			Com_Printf("The PS3 reported an unknown output resolution.\n");
			return false;
		}

		#ifdef PS3_NATIVE_RSX
		/* Native RSX rasterizes directly into the scanout-sized color target.
		 * Retaining Yamagi's default 640x480 desktop canvas did not lower fill
		 * cost: rsxgl_apply_viewport() expanded that canvas over every output
		 * pixel. It only stretched each 8x8 console glyph to 24x18 at 1080p,
		 * producing the long-standing oversized/overprinted-looking text.
		 *
		 * Make the engine, UI, projection, viewport and physical target agree.
		 * Frame-packed and top/bottom stereo retain a 1280x720 logical eye;
		 * the RSX stereo target code performs the required physical placement. */
		if (ps3_stereo_enable && ps3_stereo_enable->value > 0.0f)
		{
			width = 1280;
			height = 720;
		}
		else
		{
			width = res.width;
			height = res.height;
		}
		*pwidth = width;
		*pheight = height;
		snprintf(trace, sizeof(trace),
			"PS3 native RSX: render/UI canvas matched to %dx%d output (physical %ux%u)",
			width, height, (unsigned)res.width, (unsigned)res.height);
		PS3_RUNTIME_TRACE(trace);
		#else
		if ((width > res.width) || (height > res.height))
		{
			if (res.width == 1280 && res.height == 720)
			{
				Com_Printf("Clamping the internal video mode to PS3 720p output.\n");
				width = *pwidth = 1280;
				height = *pheight = 720;
				Cvar_SetValue("r_mode", 14);
			}
			else
			{
				Com_Printf("Internal mode %dx%d exceeds PS3 output %dx%d.\n",
					width, height, res.width, res.height);
				return false;
			}
		}
		#endif
	}

	/* We need the window size for the menu, the HUD, etc. */
	viddef.width = width;
	viddef.height = height;

	/* Mkay, now the hard work. Let's create the window. */
	// This one unused so force 0 to not confuse user at video menu
	// cvar_t *gl_msaa_samples = Cvar_Get("r_msaa_samples", "0", CVAR_ARCHIVE);
	Cvar_Set("r_msaa_samples", "0");

	/* Now that we've got a working window print it's mode. */
	int curdisplay = 0;

	if (curdisplay < 0) {
		curdisplay = 0;
	}

	/* Initialize rendering context. */
	PS3_BOOT_TRACE("GLimp_InitGraphics: before renderer InitContext");
	if (!re.InitContext(context))
	{
		/* InitContext() should have logged an error. */
		return false;
	}
	PS3_BOOT_TRACE("GLimp_InitGraphics: renderer InitContext complete");

	scheduled_flip_queue_active = false;
	scheduled_flip_queue_vblank = 0;
	initSuccessful = true;
	contextInited = true;

	PS3_BOOT_TRACE("GLimp_InitGraphics: complete");
	return true;
}

/*
 * Shuts the graphics down.
 */
void
GLimp_ShutdownGraphics(void)
{
	ShutdownGraphics();
}

/*
 * Returns the current display refresh rate. There're 2 limitations:
 *
 * * The timing code in frame.c only understands full integers, so
 *   values given by vid_displayrefreshrate are always round up. For
 *   example 59.95 become 60. Rounding up is the better choice for
 *   most users because assuming a too high display refresh rate
 *   avoids micro stuttering caused by missed frames if the vsync
 *   is enabled. The price are small and hard to notice timing
 *   problems.
 *
 * * S.D.L. returns only full integers. In most cases they're rounded
 *   up, but in some cases - likely depending on the GPU driver -
 *   they're rounded down. If the value is rounded up, we'll see
 *   some small and nard to notice timing problems. If the value
 *   is rounded down frames will be missed. Both is only relevant
 *   if the vsync is enabled.
 */
int
GLimp_GetRefreshRate(void)
{
	if (vid_displayrefreshrate->value > 0 ||
			vid_displayrefreshrate->modified)
	{
		glimp_refreshRate = ceil(vid_displayrefreshrate->value);
		vid_displayrefreshrate->modified = false;
	}

	if (glimp_refreshRate > 0)
	{
		return glimp_refreshRate;
	}

	// Initialization
	videoState vState;
	if (videoGetState(VIDEO_PRIMARY, 0, &vState) == 0)
	{
		u16 rate = maxAvailableRate(vState.displayMode.refreshRates);
		glimp_refreshRate = refreshRateToI(rate);
	}

	if (glimp_refreshRate <= 0)
	{
		// Something went wrong, use default.
		glimp_refreshRate = 60;
	}

	return glimp_refreshRate;
}

/* Wait for the native flip before the main loop samples its next time delta.
 * Do not reset the status here: RSXGL_BeginFrame consumes the completed flip,
 * acquires the back buffer, and performs the ordered full-surface clear. For an
 * exact refresh divisor, record the earliest flip-queue VBlank but begin the
 * next frame immediately. GLimp_WaitForScheduledFlipQueue() applies the rest of
 * the divisor after submitting that frame's completed render workload. */
qboolean
GLimp_PrewaitForFlip(int vblank_divisor)
{
#ifdef PS3_NATIVE_RSX
	u32 iterations = 0;
	qboolean pending_flip;

	scheduled_flip_queue_active = false;
	scheduled_flip_queue_vblank = 0;

	if (!initSuccessful || !context)
	{
		return false;
	}

	pending_flip = gcmGetFlipStatus() != 0;
	while (gcmGetFlipStatus() != 0 && iterations++ < 1000)
	{
		if ((iterations % 200) == 0 && Sys_PS3_CheckCallbacks())
		{
			return false;
		}
		sysUsleep(50);
	}

	if (gcmGetFlipStatus() != 0)
	{
		if (!traced_vsync_prewait_timeout)
		{
			PS3_RUNTIME_TRACE("PS3 pacing: early VSync wait exceeded 50 ms; using renderer recovery path");
			traced_vsync_prewait_timeout = true;
		}
		return false;
	}

	if (pending_flip && vblank_divisor > 1)
	{
		scheduled_flip_queue_vblank = gcmGetVBlankCount() +
			(u64)(vblank_divisor - 1);
		scheduled_flip_queue_active = true;
		if (!traced_vblank_divisor)
		{
			PS3_RUNTIME_TRACE(
				"PS3 pacing: exact 30 Hz render-ahead VBlank budget active");
			traced_vblank_divisor = true;
		}
	}

	if (pending_flip)
	{
		if (!traced_vsync_prewait)
		{
			PS3_RUNTIME_TRACE("PS3 pacing: input and simulation phase-locked to completed VSync flips");
			traced_vsync_prewait = true;
		}
		return true;
	}
#endif

	return false;
}

/* The exact 60-to-30 path must not consume its second VBlank before rendering:
 * doing so gives a nominal 30 Hz frame only one 60 Hz interval of actual work
 * time. Submit the completed workload first, let RSX process it concurrently
 * with this bounded wait, then append the flip command. Since the flip follows
 * the submitted draws in the same FIFO, it still cannot become ready until the
 * render is complete. */
qboolean
GLimp_WaitForScheduledFlipQueue(void)
{
#ifdef PS3_NATIVE_RSX
	u32 iterations = 0;
	u64 target_vblank;

	if (!scheduled_flip_queue_active)
	{
		return true;
	}

	target_vblank = scheduled_flip_queue_vblank;
	scheduled_flip_queue_active = false;
	scheduled_flip_queue_vblank = 0;

	if (!initSuccessful || !context)
	{
		return false;
	}

	/* Make the RSX work available before spending the remaining divisor wait. */
	rsxFlushBuffer(context);
	while (gcmGetVBlankCount() < target_vblank && iterations++ < 1000)
	{
		if ((iterations % 200) == 0 && Sys_PS3_CheckCallbacks())
		{
			return false;
		}
		sysUsleep(50);
	}

	if (gcmGetVBlankCount() < target_vblank)
	{
		if (!traced_vblank_divisor_timeout)
		{
			PS3_RUNTIME_TRACE(
				"PS3 pacing: post-render flip-queue wait exceeded 50 ms; queueing recovery flip");
			traced_vblank_divisor_timeout = true;
		}
		return false;
	}
#endif

	return true;
}

/*
 * Detect current desktop mode
 */
qboolean
GLimp_GetDesktopMode(int *pwidth, int *pheight)
{
	videoState vState;
	if (videoGetState(VIDEO_PRIMARY, 0, &vState) != 0)
	{
		// uncreachable posibly called then screen does not init yet
		return false;
	}
	videoResolution resolution;
	videoGetResolution(vState.displayMode.resolution, &resolution);

	*pwidth = resolution.width;
	*pheight = resolution.height;

	return true;
}

const char**
GLimp_GetDisplayIndices(void)
{
	return (const char**)n_displayindices;
}

int
GLimp_GetNumVideoDisplays(void)
{
	return n_num_displays;
}

int
GLimp_GetWindowDisplayIndex(void)
{
	return last_display;
}
