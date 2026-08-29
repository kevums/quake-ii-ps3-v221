/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

/* -----------------------------------------------------------------------
 *
 * GSM frontend for software renderer.
 * 
 * Notes on PS3 changes:
 *     See notes in sw_ps3_main.c
 * 
 * -----------------------------------------------------------------------
 */

#include "header/local_ps3.h"
#include <sysutil/video.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <sys/systime.h>

// -----------------------------------------------------------------------------
//	 GCM frontend implementation variables
// -----------------------------------------------------------------------------
static int rend_buffer_width;
static int rend_buffer_height;
static int rend_buffer_pitch;

typedef struct
{
	int height;
	int width;
	int id;
	uint32_t *ptr;
	// Internal stuff
	uint32_t offset;
} rsxBuffer;

#define PS3_DISPLAY_BUFFER_COUNT 2

static rsxBuffer render_buffers[PS3_DISPLAY_BUFFER_COUNT];
static int displayed_buffer;
static int traced_presentations;

static gcmContextData* gcmContext = NULL;
// -----------------------------------------------------------------------------

static void
RE_CopyScaledEye(const pixel_t *source, uint32_t *pixels, int pitch,
	int destination_x, int destination_y, int destination_width,
	int destination_height)
{
	uint32_t *palette = (uint32_t *)sw_state.currentpalette;
	int y;

	if (!source || destination_width <= 0 || destination_height <= 0)
	{
		return;
	}

	for (y = 0; y < destination_height; y++)
	{
		int source_y = (y * vid_buffer_height) / destination_height;
		uint32_t *destination = pixels +
			(destination_y + y) * pitch + destination_x;
		const pixel_t *source_row = source + source_y * vid_buffer_width;
		int x;

		for (x = 0; x < destination_width; x++)
		{
			int source_x = (x * vid_buffer_width) / destination_width;
			destination[x] = palette[source_row[source_x]];
		}
	}
}

static void
RE_CopyStereoFrame(uint32_t *pixels, int pitch)
{
	/* HDMI 1.4 720p frame packing: 720 lines, 30-line gap, 720 lines. */
	int eye_height = (rend_buffer_height >= 1470) ? 720 :
		(rend_buffer_height / 2);
	int right_y = rend_buffer_height - eye_height;

	memset(pixels, 0, rend_buffer_height * pitch * sizeof(uint32_t));
	RE_CopyScaledEye(stereo_left_frame, pixels, pitch, 0, 0,
		rend_buffer_width, eye_height);
	RE_CopyScaledEye(vid_buffer, pixels, pitch, 0, right_y,
		rend_buffer_width, eye_height);
}

static qboolean
RE_CopyStereoFrameSPU(uint32_t *pixels, int pitch)
{
	int eye_height = (rend_buffer_height >= 1470) ? 720 :
		(rend_buffer_height / 2);
	int right_y = rend_buffer_height - eye_height;
	const uint32_t *palette = (const uint32_t *)sw_state.currentpalette;

	memset(pixels, 0, rend_buffer_height * pitch * sizeof(uint32_t));
	if (!PS3SPU_ConvertFrame(stereo_left_frame,
		vid_buffer_width, vid_buffer_height,
		pixels, pitch, rend_buffer_width, eye_height, palette))
	{
		return false;
	}

	if (!PS3SPU_ConvertFrame(vid_buffer,
		vid_buffer_width, vid_buffer_height,
		pixels + right_y * pitch, pitch,
		rend_buffer_width, eye_height, palette))
	{
		return false;
	}

	return true;
}

// =============================================================================
//	 rsxutil functions
// =============================================================================

qboolean
RE_waitFlip(void)
{
	u32 wait_iterations = 0;

	while (gcmGetFlipStatus () != 0)
	{
		if (++wait_iterations >= 25000)
		{
			PS3_BOOT_TRACE("RE_waitFlip: timeout");
			return false;
		}

		/* Sleep, to not stress the cpu. */
		sysUsleep(200); 
	}
	gcmResetFlipStatus();
	return true;
}

qboolean
RE_flip(gcmContextData *context, s32 buffer)
{
	if (gcmSetFlip(context, buffer) == 0)
	{
		rsxFlushBuffer(context);
		// Prevent the RSX from continuing until the flip has finished.
		gcmSetWaitFlip(context);

		return true;
	}
	return false;
}

qboolean
RE_makeBuffer(rsxBuffer* buffer, u16 width, u16 height, int id)
{
	int depth = sizeof(u32);
	int pitch = depth * width;
	int size = depth * width * height;

	buffer->ptr = (uint32_t*)(rsxMemalign(64, size));

	if (buffer->ptr == NULL)
		goto error;

	if (rsxAddressToOffset (buffer->ptr, &buffer->offset) != 0)
		goto error;

	/* Register the display buffer with the RSX */
	if (gcmSetDisplayBuffer(id, buffer->offset, pitch, width, height) != 0)
		goto error;

	buffer->width = width;
	buffer->height = height;
	buffer->id = id;

	return true;

 error:
	if (buffer->ptr != NULL)
	{
		rsxFree(buffer->ptr);
		buffer->ptr = NULL;
	}

	return false;
}

// =============================================================================
//	 Internal software refresher functions
// =============================================================================

void
RE_FlushFrame(int vmin, int vmax)
{
	static qboolean first_flush = true;
	int next_buffer = displayed_buffer ^ 1;
	uint32_t *pixels;
	qboolean stereo_enabled =
		(Cvar_VariableValue("ps3_stereo_enable") > 0.0f);

	/*
	 * Never write into the framebuffer that may still be scanned out. Wait
	 * for the previous flip, then fill the other registered RSX buffer.
	 * This is the ordering required by PSL1GHT's double-buffering contract.
	 */
	if (!RE_waitFlip())
	{
		ri.Sys_Error(ERR_FATAL, "PS3 display flip timed out");
		return;
	}

	pixels = render_buffers[next_buffer].ptr;

	if (stereo_enabled && ps3_stereo_eye == 1)
	{
		if (!RE_CopyStereoFrameSPU(pixels,
			rend_buffer_pitch / sizeof(uint32_t)))
		{
			RE_CopyStereoFrame(pixels,
				rend_buffer_pitch / sizeof(uint32_t));
		}
	}
	else if (rend_buffer_width != vid_buffer_width ||
		rend_buffer_height != vid_buffer_height)
	{
		memset(pixels, 0, rend_buffer_height * rend_buffer_pitch);

		if (Cvar_VariableValue("ps3_output_scale") > 0.0f)
		{
			if (!PS3SPU_ConvertFrame(vid_buffer,
				vid_buffer_width, vid_buffer_height, pixels,
				rend_buffer_pitch / sizeof(uint32_t),
				rend_buffer_width, rend_buffer_height,
				(const uint32_t *)sw_state.currentpalette))
			{
				/* Optional full-output scaling is expensive on the PPU. */
				RE_CopyScaledEye(vid_buffer, pixels,
					rend_buffer_pitch / sizeof(uint32_t), 0, 0,
					rend_buffer_width, rend_buffer_height);
			}
		}
		else
		{
			if (!PS3SPU_ConvertFrame(vid_buffer,
				vid_buffer_width, vid_buffer_height, pixels,
				rend_buffer_pitch / sizeof(uint32_t),
				vid_buffer_width, vid_buffer_height,
				(const uint32_t *)sw_state.currentpalette))
			{
				/* Conservative baseline: no output scaling. */
				RE_CopyFrame(pixels,
					rend_buffer_pitch / sizeof(uint32_t),
					0, vid_buffer_height * vid_buffer_width);
			}
		}
	}
	else if (PS3SPU_ConvertFrame(vid_buffer,
		vid_buffer_width, vid_buffer_height, pixels,
		rend_buffer_pitch / sizeof(uint32_t),
		vid_buffer_width, vid_buffer_height,
		(const uint32_t *)sw_state.currentpalette))
	{
		/* The SPU path intentionally converts the complete damaged frame. */
	}
	else if (sw_partialrefresh->value)
	{
		// Update required part
		RE_CopyFrame(pixels, rend_buffer_pitch / sizeof(uint32_t),
			vmin, vmax);
	}
	else
	{
		RE_CopyFrame(pixels, rend_buffer_pitch / sizeof(uint32_t),
			0, vid_buffer_height * vid_buffer_width);
	}

	if (!stereo_enabled &&
		(sw_anisotropic->value > 0) && !fastmoving &&
		rend_buffer_width == vid_buffer_width &&
		rend_buffer_height == vid_buffer_height)
	{
		SmoothColorImage(pixels + vmin, vmax - vmin, sw_anisotropic->value);
	}

	if (first_flush)
	{
		PS3_BOOT_TRACE("RE_FlushFrame: first frame copied");
	}

	if (!RE_flip(gcmContext, render_buffers[next_buffer].id))
	{
		ri.Sys_Error(ERR_FATAL, "Couldn't queue PS3 display flip");
		return;
	}

	displayed_buffer = next_buffer;
	if (traced_presentations < 2)
	{
		PS3_BOOT_TRACE((displayed_buffer == 0) ?
			"RE_FlushFrame: presented RSX buffer 0" :
			"RE_FlushFrame: presented RSX buffer 1");
		traced_presentations++;
	}

	if (first_flush)
	{
		PS3_BOOT_TRACE("RE_FlushFrame: first frame presented");
		first_flush = false;
	}

	// replace use next buffer
	swap_current = swap_current + 1;
	vid_buffer = swap_frames[swap_current & 1];

	// All changes flushed
	VID_NoDamageBuffer();
}

void
RE_CleanFrame(void)
{
	int i;

	memset(swap_frames[0], 0,
		vid_buffer_height * vid_buffer_width * sizeof(pixel_t));
	memset(swap_frames[1], 0,
		vid_buffer_height * vid_buffer_width * sizeof(pixel_t));
	if (stereo_left_frame)
	{
		memset(stereo_left_frame, 0,
			vid_buffer_height * vid_buffer_width * sizeof(pixel_t));
	}

	/* Clear both scanout buffers so a later flip cannot reveal stale pixels. */
	for (i = 0; i < PS3_DISPLAY_BUFFER_COUNT; i++)
	{
		if (render_buffers[i].ptr)
		{
			memset(render_buffers[i].ptr, 0,
				rend_buffer_width * rend_buffer_height * sizeof(uint32_t));
		}
	}

	// All changes flushed
	VID_NoDamageBuffer();
}

/*
** R_GammaCorrectAndSetPalette
*/
/* It is crucial to had it here - different frontends had
	 different palettes types */
void
R_GammaCorrectAndSetPalette(const unsigned char *palette)
{
	int i;

	// Replace palette
	for ( i = 0; i < 256; i++ )
	{
		if (sw_state.currentpalette[i*4+0] != sw_state.gammatable[palette[i*4+2]] ||
			sw_state.currentpalette[i*4+1] != sw_state.gammatable[palette[i*4+1]] ||
			sw_state.currentpalette[i*4+2] != sw_state.gammatable[palette[i*4+0]])
		{
			// SDL BGRA
			// sw_state.currentpalette[i*4+0] = sw_state.gammatable[palette[i*4+2]]; // blue
			// sw_state.currentpalette[i*4+1] = sw_state.gammatable[palette[i*4+1]]; // green
			// sw_state.currentpalette[i*4+2] = sw_state.gammatable[palette[i*4+0]]; // red

			// sw_state.currentpalette[i*4+3] = 0xFF; // alpha

			// GCM ARGB
			sw_state.currentpalette[i*4+0] = 0xFF; // alpha
			sw_state.currentpalette[i*4+1] = sw_state.gammatable[palette[i*4+0]]; // red
			sw_state.currentpalette[i*4+2] = sw_state.gammatable[palette[i*4+1]]; // green
			sw_state.currentpalette[i*4+3] = sw_state.gammatable[palette[i*4+2]]; // blue

			palette_changed = true;
		}
	}
}

// =============================================================================
//	 Externaly called functions
// =============================================================================

// S.D.L. specifict function to get
// flags for window creation
int RE_PrepareForWindow(void)
{
	return 0;
}

int vid_buffer_height = 0;
int vid_buffer_width = 0;

static int
RE_InitContext(void *gcmCon)
{
	int i;
	qboolean stereo_enabled;

	PS3_BOOT_TRACE("RE_InitContext: entered");

	if (gcmCon == NULL)
	{
		Com_Printf("%s() must not be called with NULL argument!\n",
			__func__);
		ri.Sys_Error(ERR_FATAL,
			"%s() must not be called with NULL argument!", __func__);
		// FIXME unmapped memory on that
		return false;
	}

	gcmContext = (gcmContextData*)gcmCon;

	stereo_enabled =
		(Cvar_VariableValue("ps3_stereo_enable") > 0.0f);
	gcmSetFlipMode((r_vsync->value || stereo_enabled) ?
		GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);


	videoConfiguration vConfig;
	if (videoGetConfiguration(0, &vConfig, NULL) != 0)
	{
		Com_Printf("Can't get video configuration\n");
	}
	else
	{
		videoResolution res;
		videoGetResolution(vConfig.resolution, &res);
		rend_buffer_width	= res.width;
	 	rend_buffer_height = res.height;
		rend_buffer_pitch	= vConfig.pitch;
	}

	vid_buffer_height = vid.height;
	vid_buffer_width = vid.width;

	Com_Printf("vid_buffer_width : %d\n", vid_buffer_width);
	Com_Printf("vid_buffer_height : %d\n", vid_buffer_height);

	memset(render_buffers, 0, sizeof(render_buffers));
	for (i = 0; i < PS3_DISPLAY_BUFFER_COUNT; i++)
	{
		if (!RE_makeBuffer(&render_buffers[i], rend_buffer_width,
			rend_buffer_height, i))
		{
			while (--i >= 0)
			{
				rsxFree(render_buffers[i].ptr);
				render_buffers[i].ptr = NULL;
			}

			ri.Sys_Error(ERR_FATAL, "Couldn't allocate PS3 display buffers");
			return false;
		}

		memset(render_buffers[i].ptr, 0,
			rend_buffer_width * rend_buffer_height * sizeof(uint32_t));
	}
	PS3_BOOT_TRACE("RE_InitContext: display buffers allocated");

	displayed_buffer = 0;
	traced_presentations = 0;
	if (!RE_flip(gcmContext, render_buffers[displayed_buffer].id))
	{
		ri.Sys_Error(ERR_FATAL, "Couldn't queue initial PS3 display flip");
		return false;
	}
	PS3_BOOT_TRACE("RE_InitContext: initial flip queued");

	R_InitGraphics(vid_buffer_width, vid_buffer_height);
	PS3_BOOT_TRACE("RE_InitContext: R_InitGraphics complete");
	SWimp_CreateRender(vid_buffer_width, vid_buffer_height);
	PS3SPU_Init();
	PS3_BOOT_TRACE("RE_InitContext: complete");

	return true;
}

void
RE_ShutdownContext(void)
{
	int i;

	PS3SPU_Shutdown();

	if (!gcmContext)
	{
		return;
	}

	gcmSetWaitFlip(gcmContext);
	memset(render_buffers[displayed_buffer].ptr, 0,
		rend_buffer_width * rend_buffer_height * sizeof(uint32_t));
	RE_flip(gcmContext, render_buffers[displayed_buffer].id);
	(void)RE_waitFlip();

	for (i = 0; i < PS3_DISPLAY_BUFFER_COUNT; i++)
	{
		if (render_buffers[i].ptr)
		{
			rsxFree(render_buffers[i].ptr);
			render_buffers[i].ptr = NULL;
		}
	}
	displayed_buffer = 0;

	RE_ShutdownRenderer();

	gcmContext = NULL;
}

/*
===============
GetRefAPI
===============
*/

refexport_t
GetRefAPI(refimport_t imp)
{
	// struct for save refexport callbacks, copy of re struct from main file
	// used different variable name for prevent confusion and cppcheck warnings
	refexport_t	refexport;

	memset(&refexport, 0, sizeof(refexport_t));
	ri = imp;

	refexport.api_version = API_VERSION;

	refexport.BeginRegistration = RE_BeginRegistration;
	refexport.RegisterModel = RE_RegisterModel;
	refexport.RegisterSkin = RE_RegisterSkin;
	refexport.DrawFindPic = RE_Draw_FindPic;
	refexport.SetSky = RE_SetSky;
	refexport.EndRegistration = RE_EndRegistration;

	refexport.RenderFrame = RE_RenderFrame;

	refexport.DrawGetPicSize = RE_Draw_GetPicSize;

	refexport.DrawPicScaled = RE_Draw_PicScaled;
	refexport.DrawStretchPic = RE_Draw_StretchPic;
	refexport.DrawCharScaled = RE_Draw_CharScaled;
	refexport.DrawTileClear = RE_Draw_TileClear;
	refexport.DrawFill = RE_Draw_Fill;
	refexport.DrawFadeScreen = RE_Draw_FadeScreen;

	refexport.DrawStretchRaw = RE_Draw_StretchRaw;

	refexport.Init = RE_Init;
	refexport.IsVSyncActive = RE_IsVsyncActive;
	refexport.Shutdown = RE_Shutdown;
	refexport.InitContext = RE_InitContext;
	refexport.ShutdownContext = RE_ShutdownContext;
	refexport.PrepareForWindow = RE_PrepareForWindow;

	refexport.SetPalette = RE_SetPalette;
	refexport.BeginFrame = RE_BeginFrame;
	refexport.EndWorldRenderpass = RE_EndWorldRenderpass;
	refexport.EndFrame = RE_EndFrame;

	// Tell the client that we're unsing the
	// new renderer restart API.
	ri.Vid_RequestRestart(RESTART_NO);

	Swap_Init();

	return refexport;
}
