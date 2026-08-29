/*
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
 * This is the refresher dependend video menu. If you add a new
 * refresher this menu must be altered.
 *
 * =======================================================================
 */

#include "../../client/header/client.h"
#include "../../client/menu/header/qmenu.h"
#include "header/qmenu.h"

extern void M_ForceMenuOff(void);

static cvar_t *r_mode;
static cvar_t *vid_displayindex;
static cvar_t *r_hudscale;
static cvar_t *r_consolescale;
static cvar_t *r_menuscale;
static cvar_t *crosshair_scale;
static cvar_t *fov;
extern cvar_t *scr_viewsize;
extern cvar_t *vid_gamma;
extern cvar_t *vid_fullscreen;
extern cvar_t *vid_renderer;
static cvar_t *r_vsync;
static cvar_t *gl_anisotropic;
static cvar_t *gl_msaa_samples;
#ifdef __PSL1GHT__
static cvar_t *ps3_stereo_enable;
static cvar_t *ps3_stereo_output;
static cvar_t *ps3_stereo_depth;
static cvar_t *ps3_output_scale;
static cvar_t *ps3_rsx_tiled_targets;
static cvar_t *ps3_entity_shadows;
static cvar_t *ps3_spu_accel;
static cvar_t *ps3_spu_workers;
static cvar_t *ps3_rsx_performance_profile;
static cvar_t *ps3_rsx_fps_target;
static cvar_t *ps3_rsx_filter;
static cvar_t *ps3_rsx_filter_strength;
static cvar_t *ps3_texture_filter;
static cvar_t *ps3_dynamic_lighting;
static cvar_t *ps3_screen_blends;
static cvar_t *ps3_overbright;
#endif

static menuframework_s s_opengl_menu;

static menulist_s s_renderer_list;
static menulist_s s_mode_list;
static menulist_s s_display_list;
static menulist_s s_uiscale_list;
static menuslider_s s_brightness_slider;
static menuslider_s s_fov_slider;
static menulist_s s_fs_box;
static menulist_s s_vsync_list;
static menulist_s s_af_list;
static menulist_s s_msaa_list;
#ifdef __PSL1GHT__
static menulist_s s_ps3_stereo_list;
static menuslider_s s_ps3_stereo_depth_slider;
static menulist_s s_ps3_output_list;
static menulist_s s_ps3_tiled_targets_list;
static menulist_s s_ps3_entity_shadows_list;
static menulist_s s_ps3_spu_accel_list;
static menulist_s s_ps3_spu_workers_list;
static menulist_s s_ps3_performance_profile_list;
static menulist_s s_ps3_fps_target_list;
static menulist_s s_ps3_filter_list;
static menuslider_s s_ps3_filter_strength_slider;
static menulist_s s_ps3_texture_filter_list;
static menulist_s s_ps3_dynamic_lighting_list;
static menulist_s s_ps3_screen_blends_list;
static menulist_s s_ps3_overbright_list;
#endif
static menuaction_s s_defaults_action;
static menuaction_s s_apply_action;

// --------

// gl1, gl3, gles3, vk, soft
#define MAXRENDERERS 5

typedef struct
{
	const char *boxstr;
	const char *cvarstr;
} renderer;

renderer rendererlist[MAXRENDERERS];
int numrenderer;

static void
Renderer_FillRenderdef(void)
{
	numrenderer = -1;

	if (VID_HasRenderer("gl1"))
	{
		numrenderer++;
		rendererlist[numrenderer].boxstr = "[OpenGL 1.4]";
		rendererlist[numrenderer].cvarstr = "gl1";
	}

	if (VID_HasRenderer("gl3"))
	{
		numrenderer++;
		rendererlist[numrenderer].boxstr = "[OpenGL 3.2]";
		rendererlist[numrenderer].cvarstr = "gl3";
	}

	if (VID_HasRenderer("gles3"))
	{
		numrenderer++;
		rendererlist[numrenderer].boxstr = "[OpenGL ES3]";
		rendererlist[numrenderer].cvarstr = "gles3";
	}

	if (VID_HasRenderer("vk"))
	{
		numrenderer++;
		rendererlist[numrenderer].boxstr = "[Vulkan    ]";
		rendererlist[numrenderer].cvarstr = "vk";
	}

	if (VID_HasRenderer("soft"))
	{
		numrenderer++;
		rendererlist[numrenderer].boxstr = "[Software  ]";
		rendererlist[numrenderer].cvarstr = "soft";
	}

	// The custom renderer. Must be known to the menu,
	// but nothing more. The display string is hard
	// coded below, the cvar is unknown.
	numrenderer++;
}

static int
Renderer_GetRenderer(void)
{
	for (int i = 0; i < numrenderer; i++)
	{
		if (strcmp(vid_renderer->string, rendererlist[i].cvarstr) == 0)
		{
			return i;
		}
	}

	// Unknown renderer.
	return numrenderer;
}

// --------

static int
GetCustomValue(menulist_s *list)
{
	static menulist_s *last;
	static int i;

	if (list != last)
	{
		last = list;
		i = list->curvalue;
		do
		{
			i++;
		}
		while (list->itemnames[i]);
		i--;
	}

	return i;
}

static void
BrightnessCallback(void *s)
{
	menuslider_s *slider = (menuslider_s *)s;

	float gamma = slider->curvalue / 10.0;
	Cvar_SetValue("vid_gamma", gamma);
}

static void
FOVCallback(void *s) {
	menuslider_s *slider = (menuslider_s *)s;
	Cvar_SetValue("fov", slider->curvalue);
}

#ifdef __PSL1GHT__
static void
PS3CustomProfileCallback(void *unused)
{
	(void)unused;
	/* Any manual timing or resolution adjustment leaves a named preset. The
	 * renderer switches remain enabled; only the preset selector becomes
	 * Custom so Apply cannot silently overwrite the user's choice. */
	s_ps3_performance_profile_list.curvalue = 0;
}
#endif

static void
ResetDefaults(void *unused)
{
	VID_MenuInit();
}

#define CUSTOM_MODE_NAME "[Custom    ]"
#define AUTO_MODE_NAME   "[Auto      ]"

static void
ApplyChanges(void *unused)
{
	qboolean restart = false;

	/* Renderer */
	if (s_renderer_list.curvalue != Renderer_GetRenderer())
	{
		// The custom renderer (the last known renderer) cannot be
		// set, because the menu doesn't know it's cvar value. TODO:
		// Hack something that it cannot be selected.
		if (s_renderer_list.curvalue != numrenderer)
		{
			Cvar_Set("vid_renderer", (char *)rendererlist[s_renderer_list.curvalue].cvarstr);
			restart = true;
		}
	}

	/* auto mode */
	int requested_mode;
	if (!strcmp(s_mode_list.itemnames[s_mode_list.curvalue],
		AUTO_MODE_NAME))
	{
		requested_mode = -2;
	}
	else if (!strcmp(s_mode_list.itemnames[s_mode_list.curvalue],
		CUSTOM_MODE_NAME))
	{
		requested_mode = -1;
	}
	else
	{
		requested_mode = s_mode_list.curvalue;
	}
	if ((int)r_mode->value != requested_mode)
	{
		Cvar_SetValue("r_mode", requested_mode);
		restart = true;
	}

	if (s_display_list.curvalue != GLimp_GetWindowDisplayIndex() )
	{
		Cvar_SetValue( "vid_displayindex", s_display_list.curvalue );
		restart = true;
	}

	/* UI scaling */
	if (s_uiscale_list.curvalue == 0)
	{
		Cvar_SetValue("r_hudscale", -1);
	}
	else if (s_uiscale_list.curvalue < GetCustomValue(&s_uiscale_list))
	{
		Cvar_SetValue("r_hudscale", s_uiscale_list.curvalue);
	}

	if (s_uiscale_list.curvalue != GetCustomValue(&s_uiscale_list))
	{
		Cvar_SetValue("r_consolescale", r_hudscale->value);
		Cvar_SetValue("r_menuscale", r_hudscale->value);
		Cvar_SetValue("crosshair_scale", r_hudscale->value);
	}

	/* Restarts automatically */
	if (vid_fullscreen->value != s_fs_box.curvalue)
	{
		Cvar_SetValue("vid_fullscreen", s_fs_box.curvalue);
		restart = true;
	}

	if (s_fs_box.curvalue == 2)
	{
		Cvar_SetValue("r_mode", -2.0f);
	}

	/* vertical sync */
	if (r_vsync->value != s_vsync_list.curvalue)
	{
		Cvar_SetValue("r_vsync", s_vsync_list.curvalue);
		restart = true;
	}

#ifdef __PSL1GHT__
	Cvar_SetValue("ps3_rsx_performance_profile",
		s_ps3_performance_profile_list.curvalue);
	Cvar_SetValue("ps3_rsx_fps_target", s_ps3_fps_target_list.curvalue);
	/* These are renderer implementation accelerators, not quality choices.
	 * Keep the proven native path enabled for Custom and both named profiles so
	 * the 30-FPS floor work applies to every resolution, filter, and stereo
	 * combination instead of only the preset which happened to be tested. */
	Cvar_SetValue("ps3_rsx_batch", 1);
	Cvar_SetValue("ps3_rsx_indexed_batch", 1);
	Cvar_SetValue("ps3_rsx_state_filter", 1);
	Cvar_SetValue("ps3_rsx_fast_arrays", 1);
	Cvar_SetValue("ps3_rsx_texture_rename", 1);
	Cvar_SetValue("ps3_rsx_texture_frame_reuse", 1);
	Cvar_SetValue("ps3_rsx_combined_lightmaps", 1);
	Cvar_SetValue("ps3_rsx_static_world", 1);
	Cvar_SetValue("ps3_rsx_stereo_lightmap_reuse", 1);
	Cvar_SetValue("ps3_rsx_stereo_lightmap_atlas", 1);
	if (s_ps3_performance_profile_list.curvalue == 0)
	{
		/* Zero preserves XMB timing; one and two select real 720p/1080p
		 * scanout targets rather than only changing Quake's coordinates. */
		if ((int)ps3_output_scale->value != s_ps3_output_list.curvalue)
		{
			Cvar_SetValue("ps3_output_scale", s_ps3_output_list.curvalue);
			restart = true;
		}
	}
	else
	{
		const qboolean target_1080 =
			s_ps3_performance_profile_list.curvalue == 2;
		const int target_output = target_1080 ? 2 : 1;
		const int target_mode = target_1080 ? 21 : 14;
		if ((int)ps3_output_scale->value != target_output ||
			(int)r_mode->value != target_mode ||
			r_vsync->value == 0.0f ||
			Cvar_VariableValue("ps3_rsx_ui_main_memory") == 0.0f ||
			Cvar_VariableValue("ps3_rsx_ui_vertex_ring") == 0.0f ||
			Cvar_VariableValue("ps3_rsx_ui_local_stage") < 2.0f)
		{
			restart = true;
		}
		Cvar_SetValue("ps3_output_scale", target_output);
		Cvar_SetValue("r_mode", target_mode);
		Cvar_SetValue("r_vsync", 1);
		Cvar_SetValue("ps3_rsx_ui_main_memory", 1);
		Cvar_SetValue("ps3_rsx_ui_vertex_ring", 1);
		Cvar_SetValue("ps3_rsx_ui_local_stage", 2);
	}
	/* Keep presentation pacing independent from the visual preset. Automatic
	 * follows the two named presets and otherwise follows the display; explicit
	 * caps let any custom resolution/quality combination select an even 30 or
	 * 60 Hz cadence. A cap is not advertised as a performance guarantee. */
	if (s_ps3_fps_target_list.curvalue == 1)
	{
		Cvar_SetValue("vid_maxfps", 30);
	}
	else if (s_ps3_fps_target_list.curvalue == 2)
	{
		Cvar_SetValue("vid_maxfps", 60);
	}
	else if (s_ps3_performance_profile_list.curvalue == 2)
	{
		Cvar_SetValue("vid_maxfps", 30);
	}
	else if (s_ps3_performance_profile_list.curvalue == 1)
	{
		Cvar_SetValue("vid_maxfps", 60);
	}
	else
	{
		Cvar_SetValue("vid_maxfps", 300);
	}

	/* Off, native HDMI frame packing, or standard top-and-bottom. Both stereo
	 * formats preserve two complete eyes; the compatibility signal bypasses
	 * HDMI frame-packing negotiation on problematic display chains. */
	if ((int)(ps3_stereo_enable->value > 0.0f ?
		ps3_stereo_output->value + 1.0f : 0.0f) !=
		s_ps3_stereo_list.curvalue)
	{
		Cvar_SetValue("ps3_stereo_enable",
			s_ps3_stereo_list.curvalue != 0);
		if (s_ps3_stereo_list.curvalue != 0)
		{
			Cvar_SetValue("ps3_stereo_output",
				s_ps3_stereo_list.curvalue - 1);
		}
		restart = true;
	}
	Cvar_SetValue("ps3_stereo_depth",
		s_ps3_stereo_depth_slider.curvalue / 10.0f);

	/* Presentation filters and the GL1 quality controls are all live. The
	 * procedural filter's Off mode submits no extra pass. */
	Cvar_SetValue("ps3_rsx_filter", s_ps3_filter_list.curvalue);
	Cvar_SetValue("ps3_rsx_filter_strength",
		s_ps3_filter_strength_slider.curvalue / 10.0f);
	switch (s_ps3_texture_filter_list.curvalue)
	{
		case 0:
			Cvar_Set("gl_texturemode", "GL_NEAREST_MIPMAP_NEAREST");
			break;
		case 2:
			Cvar_Set("gl_texturemode", "GL_LINEAR_MIPMAP_LINEAR");
			break;
		default:
			Cvar_Set("gl_texturemode", "GL_LINEAR_MIPMAP_NEAREST");
			break;
	}
	Cvar_SetValue("gl1_dynamic", s_ps3_dynamic_lighting_list.curvalue);
	Cvar_SetValue("gl1_polyblend", s_ps3_screen_blends_list.curvalue);
	Cvar_SetValue("gl1_overbrightbits",
		s_ps3_overbright_list.curvalue == 0 ? 0 :
		(s_ps3_overbright_list.curvalue == 1 ? 2 : 4));

	/* Keep this recovery-safe cvar session-only. If a physical RSX rejects the
	 * tiled layout, rebooting returns to linear surfaces automatically. */
	if ((int)ps3_rsx_tiled_targets->value !=
		s_ps3_tiled_targets_list.curvalue)
	{
		Cvar_SetValue("ps3_rsx_tiled_targets",
			s_ps3_tiled_targets_list.curvalue);
		restart = true;
	}

	/* Projected entity shadows are a live GL1 frontend option and do not need a
	 * renderer restart. */
	Cvar_SetValue("r_shadows", s_ps3_entity_shadows_list.curvalue);

	if ((ps3_spu_accel->value != 0.0f) !=
		s_ps3_spu_accel_list.curvalue)
	{
		Cvar_SetValue("ps3_spu_accel", s_ps3_spu_accel_list.curvalue);
		restart = true;
	}

	if ((int)ps3_spu_workers->value !=
		s_ps3_spu_workers_list.curvalue + 1)
	{
		Cvar_SetValue("ps3_spu_workers",
			s_ps3_spu_workers_list.curvalue + 1);
		restart = true;
	}
#endif

	/* anisotropic filtering */
	if (s_af_list.curvalue == 0)
	{
		if (gl_anisotropic->value != 0)
		{
			Cvar_SetValue("r_anisotropic", 0);
			restart = true;
		}
	}
	else
	{
		if (gl_anisotropic->value != pow(2, s_af_list.curvalue))
		{
			Cvar_SetValue("r_anisotropic", pow(2, s_af_list.curvalue));
			restart = true;
		}
	}

	#ifndef __PSL1GHT__
	/* multisample anti-aliasing */
	if (s_msaa_list.curvalue == 0)
	{
		if (gl_msaa_samples->value != 0)
		{
			Cvar_SetValue("r_msaa_samples", 0);
			restart = true;
		}
	}
	else
	{
		if (gl_msaa_samples->value != pow(2, s_msaa_list.curvalue))
		{
			Cvar_SetValue("r_msaa_samples", pow(2, s_msaa_list.curvalue));
			restart = true;
		}
	}
	#endif

	if (restart)
	{
		Cbuf_AddText("vid_restart\n");
	}

	M_ForceMenuOff();
}

void
VID_MenuInit(void)
{
	int y = 0;

    // Renderer selection box.
	// MAXRENDERERS + Custom + NULL.
	static const char *renderers[MAXRENDERERS + 2] = { NULL };
    Renderer_FillRenderdef();

	for (int i = 0; i < numrenderer; i++)
	{
		renderers[i] = rendererlist[i].boxstr;
	}

	renderers[numrenderer] = CUSTOM_MODE_NAME;

	// must be kept in sync with vid_modes[] in vid.c
	static const char *resolutions[] = {
		"[320 240   ]",
		"[400 300   ]",
		"[512 384   ]",
		"[640 400   ]",
		"[640 480   ]",
		"[800 500   ]",
		"[800 600   ]",
		"[960 720   ]",
		"[1024 480  ]",
		"[1024 640  ]",
		"[1024 768  ]",
		"[1152 768  ]",
		"[1152 864  ]",
		"[1280 800  ]",
		"[1280 720  ]",
		"[1280 960  ]",
		"[1280 1024 ]",
		"[1366 768  ]",
		"[1440 900  ]",
		"[1600 1200 ]",
		"[1680 1050 ]",
		"[1920 1080 ]",
		"[1920 1200 ]",
		"[2048 1536 ]",
		"[2560 1080 ]",
		"[2560 1440 ]",
		"[2560 1600 ]",
		"[3440 1440 ]",
		"[3840 1600 ]",
		"[3840 2160 ]",
		"[4096 2160 ]",
		"[5120 2880 ]",
		AUTO_MODE_NAME,
		CUSTOM_MODE_NAME,
		0
	};

	static const char *uiscale_names[] = {
		"auto",
		"1x",
		"2x",
		"3x",
		"4x",
		"5x",
		"6x",
		"custom",
		0
	};

	static const char *yesno_names[] = {
		"no",
		"yes",
		0
	};

#ifdef __PSL1GHT__
	static const char *ps3_output_names[] = {
		"system / XMB mode",
		"720p native target",
		"1080p native target",
		0
	};

	static const char *ps3_performance_profile_names[] = {
		"custom",
		"720p performance preset",
		"1080p quality preset",
		0
	};

	static const char *ps3_fps_target_names[] = {
		"profile / display",
		"30 FPS cap",
		"60 FPS cap",
		0
	};

	static const char *ps3_filter_names[] = {
		"off",
		"scanlines",
		"RGB aperture grille",
		"vignette",
		"CRT soft",
		"CRT strong",
		0
	};

	static const char *ps3_texture_filter_names[] = {
		"nearest mip",
		"bilinear mip",
		"trilinear",
		0
	};

	static const char *ps3_overbright_names[] = {
		"off",
		"2x",
		"4x",
		0
	};

	static const char *ps3_stereo_names[] = {
		"off",
		"720p frame-packed",
		"720p top/bottom compat",
		0
	};

	static const char *ps3_target_names[] = {
		"linear (safe)",
		"tiled targets",
		"tiled + Z-cull (3D proven)",
		0
	};

	static const char *ps3_spu_worker_names[] = {
		"1", "2", "3", "4", "5", 0
	};
#endif

	static const char *fullscreen_names[] = {
			"no",
			"native fullscreen",
			"fullscreen window",
			0
	};

	static const char *pow2_names[] = {
		"off",
		"2x",
		"4x",
		"8x",
		"16x",
		0
	};

	if (!r_mode)
	{
		r_mode = Cvar_Get("r_mode", "4", 0);
	}

	if (!vid_displayindex)
	{
		vid_displayindex = Cvar_Get("vid_displayindex", "0", CVAR_ARCHIVE);
	}

	if (!r_hudscale)
	{
		r_hudscale = Cvar_Get("r_hudscale", "-1", CVAR_ARCHIVE);
	}

	if (!r_consolescale)
	{
		r_consolescale = Cvar_Get("r_consolescale", "-1", CVAR_ARCHIVE);
	}

	if (!r_menuscale)
	{
		r_menuscale = Cvar_Get("r_menuscale", "-1", CVAR_ARCHIVE);
	}

	if (!crosshair_scale)
	{
		crosshair_scale = Cvar_Get("crosshair_scale", "-1", CVAR_ARCHIVE);
	}

	if (!fov)
	{
		fov = Cvar_Get("fov", "90",  CVAR_USERINFO | CVAR_ARCHIVE);
	}

	if (!vid_gamma)
	{
		vid_gamma = Cvar_Get("vid_gamma", "1.2", CVAR_ARCHIVE);
	}

	if (!vid_renderer)
	{
		vid_renderer = Cvar_Get("vid_renderer", "gl1", CVAR_ARCHIVE);
	}

	if (!r_vsync)
	{
		r_vsync = Cvar_Get("r_vsync", "1", CVAR_ARCHIVE);
	}

	if (!gl_anisotropic)
	{
		gl_anisotropic = Cvar_Get("r_anisotropic", "0", CVAR_ARCHIVE);
	}

	if (!gl_msaa_samples)
	{
		gl_msaa_samples = Cvar_Get("r_msaa_samples", "0", CVAR_ARCHIVE);
	}

#ifdef __PSL1GHT__
	if (!ps3_stereo_enable)
	{
		ps3_stereo_enable = Cvar_Get("ps3_stereo_enable", "0", CVAR_ARCHIVE);
	}

	if (!ps3_stereo_output)
	{
		ps3_stereo_output = Cvar_Get("ps3_stereo_output", "0", CVAR_ARCHIVE);
	}

	if (!ps3_stereo_depth)
	{
		ps3_stereo_depth = Cvar_Get("ps3_stereo_depth", "1.0", CVAR_ARCHIVE);
	}

	if (!ps3_output_scale)
	{
		ps3_output_scale = Cvar_Get("ps3_output_scale", "0", CVAR_ARCHIVE);
	}

	if (!ps3_rsx_tiled_targets)
	{
		ps3_rsx_tiled_targets = Cvar_Get("ps3_rsx_tiled_targets", "0", 0);
	}

	if (!ps3_entity_shadows)
	{
		ps3_entity_shadows = Cvar_Get("r_shadows", "1", CVAR_ARCHIVE);
	}

	if (!ps3_spu_accel)
	{
		ps3_spu_accel = Cvar_Get("ps3_spu_accel", "0", CVAR_ARCHIVE);
	}

	if (!ps3_spu_workers)
	{
		ps3_spu_workers = Cvar_Get("ps3_spu_workers", "4", CVAR_ARCHIVE);
	}

	if (!ps3_rsx_performance_profile)
	{
		ps3_rsx_performance_profile =
			Cvar_Get("ps3_rsx_performance_profile", "0", CVAR_ARCHIVE);
	}

	if (!ps3_rsx_fps_target)
	{
		ps3_rsx_fps_target =
			Cvar_Get("ps3_rsx_fps_target", "0", CVAR_ARCHIVE);
	}

	if (!ps3_rsx_filter)
	{
		ps3_rsx_filter = Cvar_Get("ps3_rsx_filter", "0", CVAR_ARCHIVE);
	}

	if (!ps3_rsx_filter_strength)
	{
		ps3_rsx_filter_strength =
			Cvar_Get("ps3_rsx_filter_strength", "1.0", CVAR_ARCHIVE);
	}

	if (!ps3_texture_filter)
	{
		ps3_texture_filter =
			Cvar_Get("gl_texturemode", "GL_LINEAR_MIPMAP_NEAREST", CVAR_ARCHIVE);
	}

	if (!ps3_dynamic_lighting)
	{
		ps3_dynamic_lighting = Cvar_Get("gl1_dynamic", "1", CVAR_ARCHIVE);
	}

	if (!ps3_screen_blends)
	{
		ps3_screen_blends = Cvar_Get("gl1_polyblend", "1", CVAR_ARCHIVE);
	}

	if (!ps3_overbright)
	{
		ps3_overbright = Cvar_Get("gl1_overbrightbits", "0", CVAR_ARCHIVE);
	}
#endif

	s_opengl_menu.x = viddef.width * 0.50;
	s_opengl_menu.nitems = 0;

	s_renderer_list.generic.type = MTYPE_SPINCONTROL;
	s_renderer_list.generic.name = "renderer";
	s_renderer_list.generic.x = 0;
	s_renderer_list.generic.y = (y = 0);
	s_renderer_list.itemnames = renderers;
	s_renderer_list.curvalue = Renderer_GetRenderer();

	s_mode_list.generic.type = MTYPE_SPINCONTROL;
	s_mode_list.generic.name = "video mode";
	s_mode_list.generic.x = 0;
	s_mode_list.generic.y = (y += 10);
	#ifdef __PSL1GHT__
	s_mode_list.generic.callback = PS3CustomProfileCallback;
	#endif
	s_mode_list.itemnames = resolutions;

	if (r_mode->value >= 0)
	{
		s_mode_list.curvalue = r_mode->value;
	}
	else if (r_mode->value == -2)
	{
		// 'auto' is before 'custom'
		s_mode_list.curvalue = GetCustomValue(&s_mode_list) - 1;
	}
	else
	{
		// 'custom'
		s_mode_list.curvalue = GetCustomValue(&s_mode_list);
	}

	if (GLimp_GetNumVideoDisplays() > 1)
	{
		s_display_list.generic.type = MTYPE_SPINCONTROL;
		s_display_list.generic.name = "display index";
		s_display_list.generic.x = 0;
		s_display_list.generic.y = (y += 10);
		s_display_list.itemnames = GLimp_GetDisplayIndices();
		s_display_list.curvalue = GLimp_GetWindowDisplayIndex();
	}

	s_brightness_slider.generic.type = MTYPE_SLIDER;
	s_brightness_slider.generic.name = "brightness";
	s_brightness_slider.generic.x = 0;
	s_brightness_slider.generic.y = (y += 20);
	s_brightness_slider.generic.callback = BrightnessCallback;
	s_brightness_slider.minvalue = 1;
	s_brightness_slider.maxvalue = 20;
	s_brightness_slider.curvalue = vid_gamma->value * 10;

	s_fov_slider.generic.type = MTYPE_SLIDER;
	s_fov_slider.generic.x = 0;
	s_fov_slider.generic.y = (y += 10);
	s_fov_slider.generic.name = "field of view";
	s_fov_slider.generic.callback = FOVCallback;
	s_fov_slider.minvalue = 60;
	s_fov_slider.maxvalue = 120;
	s_fov_slider.curvalue = fov->value;

	s_uiscale_list.generic.type = MTYPE_SPINCONTROL;
	s_uiscale_list.generic.name = "ui scale";
	s_uiscale_list.generic.x = 0;
	s_uiscale_list.generic.y = (y += 10);
	s_uiscale_list.itemnames = uiscale_names;
	if (r_hudscale->value != r_consolescale->value ||
		r_hudscale->value != r_menuscale->value ||
		r_hudscale->value != crosshair_scale->value)
	{
		s_uiscale_list.curvalue = GetCustomValue(&s_uiscale_list);
	}
	else if (r_hudscale->value < 0)
	{
		s_uiscale_list.curvalue = 0;
	}
	else if (r_hudscale->value > 0 &&
			r_hudscale->value < GetCustomValue(&s_uiscale_list) &&
			r_hudscale->value == (int)r_hudscale->value)
	{
		s_uiscale_list.curvalue = r_hudscale->value;
	}
	else
	{
		s_uiscale_list.curvalue = GetCustomValue(&s_uiscale_list);
	}

	s_fs_box.generic.type = MTYPE_SPINCONTROL;
	s_fs_box.generic.name = "fullscreen";
	s_fs_box.generic.x = 0;
	s_fs_box.generic.y = (y += 10);
	s_fs_box.itemnames = fullscreen_names;
	s_fs_box.curvalue = (int)vid_fullscreen->value;

	s_vsync_list.generic.type = MTYPE_SPINCONTROL;
	s_vsync_list.generic.name = "vertical sync";
	s_vsync_list.generic.x = 0;
	s_vsync_list.generic.y = (y += 10);
	#ifdef __PSL1GHT__
	s_vsync_list.generic.callback = PS3CustomProfileCallback;
	#endif
	s_vsync_list.itemnames = yesno_names;
	s_vsync_list.curvalue = (r_vsync->value != 0);

#ifdef __PSL1GHT__
	s_ps3_performance_profile_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_performance_profile_list.generic.name = "performance profile";
	s_ps3_performance_profile_list.generic.x = 0;
	s_ps3_performance_profile_list.generic.y = (y += 10);
	s_ps3_performance_profile_list.generic.statusbar =
		"visual preset only; frame target can be overridden below";
	s_ps3_performance_profile_list.itemnames =
		ps3_performance_profile_names;
	s_ps3_performance_profile_list.curvalue =
		(int)ps3_rsx_performance_profile->value;
	if (s_ps3_performance_profile_list.curvalue < 0 ||
		s_ps3_performance_profile_list.curvalue > 2)
	{
		s_ps3_performance_profile_list.curvalue = 0;
	}

	s_ps3_fps_target_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_fps_target_list.generic.name = "frame rate target";
	s_ps3_fps_target_list.generic.x = 0;
	s_ps3_fps_target_list.generic.y = (y += 10);
	s_ps3_fps_target_list.generic.statusbar =
		"30 is the universal floor; 60 is preferred when sustained";
	s_ps3_fps_target_list.itemnames = ps3_fps_target_names;
	s_ps3_fps_target_list.curvalue = (int)ps3_rsx_fps_target->value;
	if (s_ps3_fps_target_list.curvalue < 0 ||
		s_ps3_fps_target_list.curvalue > 2)
	{
		s_ps3_fps_target_list.curvalue = 0;
	}

	s_ps3_output_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_output_list.generic.name = "PS3 output";
	s_ps3_output_list.generic.x = 0;
	s_ps3_output_list.generic.y = (y += 10);
	s_ps3_output_list.generic.statusbar =
		"native scanout size controls real RSX fill rate";
	s_ps3_output_list.generic.callback = PS3CustomProfileCallback;
	s_ps3_output_list.itemnames = ps3_output_names;
	s_ps3_output_list.curvalue = (int)ps3_output_scale->value;
	if (s_ps3_output_list.curvalue < 0 || s_ps3_output_list.curvalue > 2)
	{
		s_ps3_output_list.curvalue = 0;
	}

	s_ps3_stereo_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_stereo_list.generic.name = "stereoscopic 3D";
	s_ps3_stereo_list.generic.x = 0;
	s_ps3_stereo_list.generic.y = (y += 10);
	s_ps3_stereo_list.generic.statusbar =
		"top/bottom avoids raw doubled HDMI frame-packed output";
	s_ps3_stereo_list.itemnames = ps3_stereo_names;
	s_ps3_stereo_list.curvalue = ps3_stereo_enable->value > 0.0f ?
		(int)ps3_stereo_output->value + 1 : 0;
	if (s_ps3_stereo_list.curvalue < 0 ||
		s_ps3_stereo_list.curvalue > 2)
	{
		s_ps3_stereo_list.curvalue = 0;
	}

	s_ps3_stereo_depth_slider.generic.type = MTYPE_SLIDER;
	s_ps3_stereo_depth_slider.generic.name = "3D depth";
	s_ps3_stereo_depth_slider.generic.x = 0;
	s_ps3_stereo_depth_slider.generic.y = (y += 10);
	s_ps3_stereo_depth_slider.generic.statusbar =
		"stereo separation: 0.0 to 2.0";
	s_ps3_stereo_depth_slider.minvalue = 0;
	s_ps3_stereo_depth_slider.maxvalue = 20;
	s_ps3_stereo_depth_slider.curvalue = ps3_stereo_depth->value * 10.0f;

	s_ps3_tiled_targets_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_tiled_targets_list.generic.name = "RSX framebuffer";
	s_ps3_tiled_targets_list.generic.x = 0;
	s_ps3_tiled_targets_list.generic.y = (y += 10);
	s_ps3_tiled_targets_list.generic.statusbar =
		"experimental; tiled-only isolates mono from Z-cull";
	s_ps3_tiled_targets_list.itemnames = ps3_target_names;
	s_ps3_tiled_targets_list.curvalue = (int)ps3_rsx_tiled_targets->value;
	if (s_ps3_tiled_targets_list.curvalue < 0 ||
		s_ps3_tiled_targets_list.curvalue > 2)
	{
		s_ps3_tiled_targets_list.curvalue = 0;
	}

	s_ps3_entity_shadows_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_entity_shadows_list.generic.name = "entity shadows";
	s_ps3_entity_shadows_list.generic.x = 0;
	s_ps3_entity_shadows_list.generic.y = (y += 10);
	s_ps3_entity_shadows_list.generic.statusbar =
		"projected Quake II model shadows";
	s_ps3_entity_shadows_list.itemnames = yesno_names;
	s_ps3_entity_shadows_list.curvalue =
		(ps3_entity_shadows->value != 0.0f);

	s_ps3_filter_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_filter_list.generic.name = "display filter";
	s_ps3_filter_list.generic.x = 0;
	s_ps3_filter_list.generic.y = (y += 10);
	s_ps3_filter_list.generic.statusbar =
		"one optional native RSX pass per eye; Off has zero cost";
	s_ps3_filter_list.itemnames = ps3_filter_names;
	s_ps3_filter_list.curvalue = (int)ps3_rsx_filter->value;
	if (s_ps3_filter_list.curvalue < 0 || s_ps3_filter_list.curvalue > 5)
	{
		s_ps3_filter_list.curvalue = 0;
	}

	s_ps3_filter_strength_slider.generic.type = MTYPE_SLIDER;
	s_ps3_filter_strength_slider.generic.name = "filter strength";
	s_ps3_filter_strength_slider.generic.x = 0;
	s_ps3_filter_strength_slider.generic.y = (y += 10);
	s_ps3_filter_strength_slider.generic.statusbar =
		"0.0 to 2.0; one is the authored intensity";
	s_ps3_filter_strength_slider.minvalue = 0;
	s_ps3_filter_strength_slider.maxvalue = 20;
	s_ps3_filter_strength_slider.curvalue =
		ps3_rsx_filter_strength->value * 10.0f;
	if (s_ps3_filter_strength_slider.curvalue < 0)
		s_ps3_filter_strength_slider.curvalue = 0;
	else if (s_ps3_filter_strength_slider.curvalue > 20)
		s_ps3_filter_strength_slider.curvalue = 20;

	s_ps3_texture_filter_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_texture_filter_list.generic.name = "texture filtering";
	s_ps3_texture_filter_list.generic.x = 0;
	s_ps3_texture_filter_list.generic.y = (y += 10);
	s_ps3_texture_filter_list.itemnames = ps3_texture_filter_names;
	if (!Q_stricmp(ps3_texture_filter->string, "GL_NEAREST") ||
		!Q_stricmp(ps3_texture_filter->string,
			"GL_NEAREST_MIPMAP_NEAREST"))
	{
		s_ps3_texture_filter_list.curvalue = 0;
	}
	else if (!Q_stricmp(ps3_texture_filter->string,
		"GL_LINEAR_MIPMAP_LINEAR"))
	{
		s_ps3_texture_filter_list.curvalue = 2;
	}
	else
	{
		s_ps3_texture_filter_list.curvalue = 1;
	}

	s_ps3_dynamic_lighting_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_dynamic_lighting_list.generic.name = "dynamic lighting";
	s_ps3_dynamic_lighting_list.generic.x = 0;
	s_ps3_dynamic_lighting_list.generic.y = (y += 10);
	s_ps3_dynamic_lighting_list.itemnames = yesno_names;
	s_ps3_dynamic_lighting_list.curvalue =
		ps3_dynamic_lighting->value != 0.0f;

	s_ps3_screen_blends_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_screen_blends_list.generic.name = "screen color blends";
	s_ps3_screen_blends_list.generic.x = 0;
	s_ps3_screen_blends_list.generic.y = (y += 10);
	s_ps3_screen_blends_list.generic.statusbar =
		"damage, pickup, and underwater color overlays";
	s_ps3_screen_blends_list.itemnames = yesno_names;
	s_ps3_screen_blends_list.curvalue = ps3_screen_blends->value != 0.0f;

	s_ps3_overbright_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_overbright_list.generic.name = "lighting boost";
	s_ps3_overbright_list.generic.x = 0;
	s_ps3_overbright_list.generic.y = (y += 10);
	s_ps3_overbright_list.generic.statusbar =
		"brightens lightmapped worlds and models";
	s_ps3_overbright_list.itemnames = ps3_overbright_names;
	s_ps3_overbright_list.curvalue = ps3_overbright->value >= 4.0f ? 2 :
		(ps3_overbright->value >= 2.0f ? 1 : 0);

	s_ps3_spu_accel_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_spu_accel_list.generic.name = "SPU acceleration";
	s_ps3_spu_accel_list.generic.x = 0;
	s_ps3_spu_accel_list.generic.y = (y += 20);
	s_ps3_spu_accel_list.generic.statusbar =
		"offloads frame conversion and scaling; Apply restarts video";
	s_ps3_spu_accel_list.itemnames = yesno_names;
	s_ps3_spu_accel_list.curvalue = (ps3_spu_accel->value != 0.0f);

	s_ps3_spu_workers_list.generic.type = MTYPE_SPINCONTROL;
	s_ps3_spu_workers_list.generic.name = "SPU workers";
	s_ps3_spu_workers_list.generic.x = 0;
	s_ps3_spu_workers_list.generic.y = (y += 10);
	s_ps3_spu_workers_list.generic.statusbar =
		"one application SPU remains reserved for PS Move";
	s_ps3_spu_workers_list.itemnames = ps3_spu_worker_names;
	s_ps3_spu_workers_list.curvalue = (int)ps3_spu_workers->value - 1;
	if (s_ps3_spu_workers_list.curvalue < 0)
	{
		s_ps3_spu_workers_list.curvalue = 0;
	}
	else if (s_ps3_spu_workers_list.curvalue > 4)
	{
		s_ps3_spu_workers_list.curvalue = 4;
	}
#endif

	s_af_list.generic.type = MTYPE_SPINCONTROL;
	s_af_list.generic.name = "aniso filtering";
	s_af_list.generic.x = 0;
	s_af_list.generic.y = (y += 10);
	s_af_list.itemnames = pow2_names;
	s_af_list.curvalue = 0;
	if (gl_anisotropic->value)
	{
		do
		{
			s_af_list.curvalue++;
		} while (pow2_names[s_af_list.curvalue] &&
				pow(2, s_af_list.curvalue) <= gl_anisotropic->value);
		s_af_list.curvalue--;
	}

	s_msaa_list.generic.type = MTYPE_SPINCONTROL;
	s_msaa_list.generic.name = "multisampling";
	s_msaa_list.generic.x = 0;
	s_msaa_list.generic.y = (y += 10);
	s_msaa_list.itemnames = pow2_names;
	s_msaa_list.curvalue = 0;
	if (gl_msaa_samples->value)
	{
		do
		{
			s_msaa_list.curvalue++;
		} while (pow2_names[s_msaa_list.curvalue] &&
				pow(2, s_msaa_list.curvalue) <= gl_msaa_samples->value);
		s_msaa_list.curvalue--;
	}

	s_defaults_action.generic.type = MTYPE_ACTION;
	s_defaults_action.generic.name = "reset to default";
	s_defaults_action.generic.x = 0;
	s_defaults_action.generic.y = (y += 20);
	s_defaults_action.generic.callback = ResetDefaults;

	s_apply_action.generic.type = MTYPE_ACTION;
	s_apply_action.generic.name = "apply";
	s_apply_action.generic.x = 0;
	s_apply_action.generic.y = (y += 10);
	s_apply_action.generic.callback = ApplyChanges;

	Menu_AddItem(&s_opengl_menu, (void *)&s_renderer_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_mode_list);

	// only show this option if we have multiple displays
	if (GLimp_GetNumVideoDisplays() > 1)
	{
		Menu_AddItem(&s_opengl_menu, (void *)&s_display_list);
	}

	Menu_AddItem(&s_opengl_menu, (void *)&s_brightness_slider);
	Menu_AddItem(&s_opengl_menu, (void *)&s_fov_slider);
	Menu_AddItem(&s_opengl_menu, (void *)&s_uiscale_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_fs_box);
	Menu_AddItem(&s_opengl_menu, (void *)&s_vsync_list);
#ifdef __PSL1GHT__
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_performance_profile_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_fps_target_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_output_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_stereo_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_stereo_depth_slider);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_tiled_targets_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_entity_shadows_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_filter_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_filter_strength_slider);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_texture_filter_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_dynamic_lighting_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_screen_blends_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_overbright_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_spu_accel_list);
	Menu_AddItem(&s_opengl_menu, (void *)&s_ps3_spu_workers_list);
#endif
	Menu_AddItem(&s_opengl_menu, (void *)&s_af_list);
	#ifndef __PSL1GHT__
	Menu_AddItem(&s_opengl_menu, (void *)&s_msaa_list);
	#endif
	Menu_AddItem(&s_opengl_menu, (void *)&s_defaults_action);
	Menu_AddItem(&s_opengl_menu, (void *)&s_apply_action);

	Menu_Center(&s_opengl_menu);
	s_opengl_menu.x -= 8;
}

void
VID_MenuDraw(void)
{
	int w, h;
	float scale = SCR_GetMenuScale();

	/* draw the banner */
	Draw_GetPicSize(&w, &h, "m_banner_video");
	Draw_PicScaled(viddef.width / 2 - (w * scale) / 2, viddef.height / 2 - (110 * scale),
			"m_banner_video", scale);

	/* move cursor to a reasonable starting position */
	Menu_AdjustCursor(&s_opengl_menu, 1);

	/* draw the menu */
	Menu_Draw(&s_opengl_menu);
}

const char *
VID_MenuKey(int key)
{
	extern void M_PopMenu(void);

	menuframework_s *m = &s_opengl_menu;
	static const char *sound = "misc/menu1.wav";
	int menu_key = Key_GetMenuKey(key);

	switch (menu_key)
	{
		case K_ESCAPE:
			M_PopMenu();
			return NULL;
		case K_UPARROW:
			m->cursor--;
			Menu_AdjustCursor(m, -1);
			break;
		case K_DOWNARROW:
			m->cursor++;
			Menu_AdjustCursor(m, 1);
			break;
		case K_LEFTARROW:
			Menu_SlideItem(m, -1);
			break;
		case K_RIGHTARROW:
			Menu_SlideItem(m, 1);
			break;
		case K_ENTER:
			Menu_SelectItem(m);
			break;
	}

	return sound;
}
