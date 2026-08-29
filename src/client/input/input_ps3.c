/*
 * Copyright (C) 2010 Yamagi Burmeister
 * Copyright (C) 1997-2005 Id Software, Inc.
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
 * Joystick threshold code is partially based on http://ioquake3.org code.
 *
 * =======================================================================
 *
 * This is the Quake II input system backend for DualShock 3 on PS3.
 *
 * =======================================================================
 * 
 * Notes on PS3 changes:
 *     Native input system for DualShock 3. Works w/o SDL.
 *     It may be possible to configure to use the keyboard and mouse too.
 * 
 * -----------------------------------------------------------------------
 */

#include <io/pad.h>

#include "header/input.h"
#include "header/psmove_ps3.h"
#include "../../client/header/keyboard.h"
#include "../../client/header/client.h"

#ifdef PS3_USB_INPUT_BRIDGE
/* Keep the physically proven pad/Move backend intact and expose it beneath
 * the USB keyboard/mouse wrapper in input_ps3_usb.c. */
#define IN_Init In_PS3_BaseInit
#define IN_Update In_PS3_BaseUpdate
#define IN_Move In_PS3_BaseMove
#define IN_Shutdown In_PS3_BaseShutdown
#define In_FlushQueue In_PS3_BaseFlushQueue
#endif

// ----

// Maximal mouse move per frame
#define MOUSE_MAX 3000

// Minimal mouse move per frame
#define MOUSE_MIN 40

// ----

// These are used to communicate the events collected by
// IN_Update() called at the beginning of a frame to the
// actual movement functions called at a later time.
static float joystick_yaw, joystick_pitch;
static float joystick_forwardmove, joystick_sidemove;
static float joystick_up;

// The last time input events were processed.
// Used throughout the client.
int sys_frame_time;

// the joystick altselector that turns K_JOYX into K_JOYX_ALT
// is pressed
qboolean joy_altselector_pressed = false;

// Console Variables
cvar_t *freelook;
cvar_t *lookstrafe;
cvar_t *m_forward;
cvar_t *m_pitch;
cvar_t *m_side;
cvar_t *m_up;
cvar_t *m_yaw;
cvar_t *sensitivity;

static cvar_t *exponential_speedup;
static cvar_t *in_grab;
static cvar_t *m_filter;

static cvar_t *psmove_enable;
static cvar_t *psmove_deadzone;
static cvar_t *psmove_smoothing;
static cvar_t *psmove_yawsensitivity;
static cvar_t *psmove_pitchsensitivity;
static cvar_t *psmove_invertpitch;
static qboolean psmove_initialized = false;
static float psmove_yaw_rate;
static float psmove_pitch_rate;
static float psmove_filtered_yaw;
static float psmove_filtered_pitch;

// ----

struct hapric_effects_cache {
	int effect_volume;
	int effect_duration;
	int effect_begin;
	int effect_end;
	int effect_attack;
	int effect_fade;
	int effect_id;
	int effect_x;
	int effect_y;
	int effect_z;
};

qboolean show_haptic;

// Joystick sensitivity
cvar_t *joy_yawsensitivity;
cvar_t *joy_pitchsensitivity;
cvar_t *joy_forwardsensitivity;
cvar_t *joy_sidesensitivity;
cvar_t *joy_upsensitivity;
cvar_t *joy_expo;

// Joystick direction settings
static cvar_t *joy_axis_leftx;
static cvar_t *joy_axis_lefty;
static cvar_t *joy_axis_rightx;
static cvar_t *joy_axis_righty;
static cvar_t *joy_axis_triggerleft;
static cvar_t *joy_axis_triggerright;

// Joystick threshold settings
static cvar_t *joy_axis_leftx_threshold;
static cvar_t *joy_axis_lefty_threshold;
static cvar_t *joy_axis_rightx_threshold;
static cvar_t *joy_axis_righty_threshold;
static cvar_t *joy_axis_triggerleft_threshold;
static cvar_t *joy_axis_triggerright_threshold;

// Joystick haptic
static cvar_t *joy_haptic_magnitude;

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

static inline void
IN_adjustJoyDirection(const unsigned int rawAxisValue, const char* direction, const float _threshold)
{
	// Com_Printf("IN_adjustJoyDirection rawAxisValue: 0x%04X direction: %s threshold: %0.4f\n", rawAxisValue, direction, _threshold);
	int axis_value; // [-128, 127]
	//Com_Printf("IN_adjustJoyDirection base axis_value: %d\n", axis_value);
	float fix_value;
	float threshold;
	if (_threshold > 0.9)
	{
		threshold = 0.9;
	}
	else
	{
		threshold = _threshold;
	}
	// Com_Printf("IN_adjustJoyDirection threshold: %0.04f\n", threshold);


	float axe_range_value = rawAxisValue;
	axe_range_value  /= 255.0f;
	axe_range_value -= 0.5f;
	axe_range_value /= 0.5f;
	axe_range_value = axe_range_value > 0.0f ? axe_range_value : -axe_range_value;

	// Smoothly ramp from dead zone to maximum value (from ioquake)
	// https://github.com/ioquake/ioq3/blob/master/code/sdl/sdl_input.c
	// fix_value = ((float) abs(rawAxisValue) / 255.0f - threshold) / (1.0f - threshold);
	fix_value = (axe_range_value - threshold) / (1.0f - threshold);
	// Com_Printf("IN_adjustJoyDirection base fix_value: %0.04f\n", fix_value);

	if (fix_value < 0.0f)
	{
		fix_value = 0.0f;
	}
	// Com_Printf("IN_adjustJoyDirection adjust fix_value: %0.04f\n", fix_value);

	// Apply expo
	fix_value = pow(fix_value, joy_expo->value);
	// Com_Printf("IN_adjustJoyDirection final fix_value: %0.04f\n", fix_value);

	axis_value = (int) (32767 * ((rawAxisValue < 0x7F) ? -fix_value : fix_value));
	// Com_Printf("IN_adjustJoyDirection final axis_value: %d\n", axis_value);

	if (cls.key_dest == key_game && (int) cl_paused->value == 0)
	{
		     if (strcmp(direction, "sidemove") == 0)
		{
			joystick_sidemove = axis_value * joy_sidesensitivity->value;

			// We need to be twice faster because with joystic we run...
			joystick_sidemove *= cl_sidespeed->value * 2.0f;
		}
		else if (strcmp(direction, "forwardmove") == 0)
		{
			joystick_forwardmove = axis_value * joy_forwardsensitivity->value;

			// We need to be twice faster because with joystic we run...
			joystick_forwardmove *= cl_forwardspeed->value * 2.0f;
		}
		else if (strcmp(direction, "yaw") == 0)
		{
			joystick_yaw = axis_value * joy_yawsensitivity->value;
			joystick_yaw *= cl_yawspeed->value;
		}
		else if (strcmp(direction, "pitch") == 0)
		{
			joystick_pitch = axis_value * joy_pitchsensitivity->value;
			joystick_pitch *= cl_pitchspeed->value;
		}
		else if (strcmp(direction, "updown") == 0)
		{
			joystick_up = axis_value * joy_upsensitivity->value;
			joystick_up *= cl_upspeed->value;
		}
	}

	static qboolean left_trigger = false;
	static qboolean right_trigger = false;

	if (strcmp(direction, "triggerleft") == 0)
	{
		qboolean new_left_trigger = abs(axis_value) > (32767 / 4);

		if (new_left_trigger != left_trigger)
		{
			left_trigger = new_left_trigger;
			Key_Event(K_TRIG_LEFT, left_trigger, true);
		}
	}
	else if (strcmp(direction, "triggerright") == 0)
	{
		qboolean new_right_trigger = abs(axis_value) > (32767 / 4);

		if (new_right_trigger != right_trigger)
		{
			right_trigger = new_right_trigger;
			Key_Event(K_TRIG_RIGHT, right_trigger, true);
		}
	}
}

static inline void
IN_adjustAxes(padData paddata)
{
	char* direction_type;
	float threshold;
	// Left horizontal
	{
		direction_type = joy_axis_leftx->string;
		threshold = joy_axis_leftx_threshold->value;
		IN_adjustJoyDirection(paddata.ANA_L_H, direction_type, threshold);
	}

	// Left vertical
	{
		direction_type = joy_axis_lefty->string;
		threshold = joy_axis_lefty_threshold->value;
		IN_adjustJoyDirection(paddata.ANA_L_V, direction_type, threshold);
	}

	// Right horizontal
	{
		direction_type = joy_axis_rightx->string;
		threshold = joy_axis_rightx_threshold->value;
		IN_adjustJoyDirection(paddata.ANA_R_H, direction_type, threshold);
	}

	// Right vertical
	{
		direction_type = joy_axis_righty->string;
		threshold = joy_axis_righty_threshold->value;
		IN_adjustJoyDirection(paddata.ANA_R_V, direction_type, threshold);
	}

	// L2
	// {
	// 	Com_Printf("L2: 0x%04X\n", paddata.PRE_L2);
	// 	direction_type = joy_axis_triggerleft->string;
	// 	threshold = joy_axis_triggerleft_threshold->value;
	// 	IN_adjustJoyDirection(paddata.PRE_L2, direction_type, threshold);
	// }

	// R2
	// {
	// 	Com_Printf("R2: 0x%04X\n", paddata.PRE_R2);
	// 	direction_type = joy_axis_triggerright->string;
	// 	threshold = joy_axis_triggerright_threshold->value;
	// 	IN_adjustJoyDirection(paddata.PRE_R2, direction_type, threshold);
	// }

	// Other presure sensitive buttons could be added as where L2 and R2
	// If doing so make sure their MAP_DS3_TO_QKEY macros are removed in
	// IN_Update
}

#define MAP_DS3_TO_QKEY(PAD_KEY, QKEY) \
if (pad_archive[i].PAD_KEY != paddata.PAD_KEY)\
{\
	Key_Event(QKEY, paddata.PAD_KEY, true);\
}

/*
 * Updates the input queue state. Called every
 * frame by the client and does nearly all the
 * input magic.
 */
void
IN_Update(void)
{
	padInfo padinfo;
	padData paddata;

	static padData pad_archive[MAX_PADS];
	qboolean osk_active = Sys_PS3_OSKActive();

	ioPadGetInfo(&padinfo);
	for(int i = 0; i < MAX_PADS; ++i)
	{
		if(padinfo.status[i] == 0)
		{
			continue;
		}
		if (ioPadGetData(i, &paddata) != 0 )
		{
			continue;
		}

		if (paddata.len < 8)
		{
			break;
		}

		/* The system OSK owns the pad while visible. Still archive every sample
		 * so the confirm/cancel buttons cannot be replayed into the Quake menu
		 * when sysutil returns control. */
		if (osk_active)
		{
			pad_archive[i] = paddata;
			continue;
		}

		/* Keep Select in the regular controller-key range so it can be rebound.
		 * Its one-time default binding opens the menu, preserving the old
		 * K_JOY_BACK behavior without making the physical button special. */
		MAP_DS3_TO_QKEY(BTN_SELECT,   K_BTN_BACK);
		MAP_DS3_TO_QKEY(BTN_START,    K_BTN_START);

		MAP_DS3_TO_QKEY(BTN_LEFT,     K_DPAD_LEFT);
		MAP_DS3_TO_QKEY(BTN_DOWN,     K_DPAD_DOWN);
		MAP_DS3_TO_QKEY(BTN_RIGHT,    K_DPAD_RIGHT);
		MAP_DS3_TO_QKEY(BTN_UP,       K_DPAD_UP);

		MAP_DS3_TO_QKEY(BTN_SQUARE,   K_BTN_X);
		MAP_DS3_TO_QKEY(BTN_CROSS,    K_BTN_A);
		MAP_DS3_TO_QKEY(BTN_CIRCLE,   K_BTN_B);
		MAP_DS3_TO_QKEY(BTN_TRIANGLE, K_BTN_Y);

		MAP_DS3_TO_QKEY(BTN_R1,       K_SHOULDER_RIGHT);
		MAP_DS3_TO_QKEY(BTN_L1,       K_SHOULDER_LEFT);

		// L2/R2 are ignored b/c YQ2 threat them as axes
		// See end note in IN_adjustAxes above
		//
		// For some reason they are not working as
		// axes LOL
		MAP_DS3_TO_QKEY(BTN_R2,       K_TRIG_RIGHT);
		MAP_DS3_TO_QKEY(BTN_L2,       K_TRIG_LEFT);

		MAP_DS3_TO_QKEY(BTN_R3,       K_STICK_RIGHT);
		MAP_DS3_TO_QKEY(BTN_L3,       K_STICK_LEFT);

		IN_adjustAxes(paddata);
		
		pad_archive[i] = paddata;
	}

	psmove_yaw_rate = 0.0f;
	psmove_pitch_rate = 0.0f;

	if (!osk_active && psmove_enable->value && psmove_initialized)
	{
		PSMove_Update(&psmove_yaw_rate, &psmove_pitch_rate);
	}

	sys_frame_time = Sys_Milliseconds();
}

/*
 * Move handling
 */
void
IN_Move(usercmd_t *cmd)
{
	// To make the the viewangles changes independent of framerate we need to scale
	// with frametime (assuming the configured values are for 60hz)
	//
	// 1/32768 is to normalize the input values from SDL (they're between -32768 and
	// 32768 and we want -1 to 1) for movement this is not needed, as those are
	// absolute values independent of framerate
	float joyViewFactor = (1.0f/32768.0f) * (cls.rframetime/0.01666f);

	if (joystick_yaw)
	{
		cl.viewangles[YAW] -= (m_yaw->value * joystick_yaw) * joyViewFactor;
	}

	if(joystick_pitch)
	{
		cl.viewangles[PITCH] += (m_pitch->value * joystick_pitch) * joyViewFactor;
	}

	if (joystick_forwardmove)
	{
		cmd->forwardmove -= (m_forward->value * joystick_forwardmove) / 32768;
	}

	if (joystick_sidemove)
	{
		cmd->sidemove += (m_side->value * joystick_sidemove) / 32768;
	}

	if (joystick_up)
	{
		cmd->upmove -= (m_up->value * joystick_up) / 32768;
	}

	if (psmove_enable->value && psmove_initialized &&
		cls.key_dest == key_game && (int)cl_paused->value == 0)
	{
		float deadzone = psmove_deadzone->value;
		float smoothing = psmove_smoothing->value;
		float frame_time = cls.rframetime;
		float yaw = psmove_yaw_rate;
		float pitch = psmove_pitch_rate;

		if (deadzone < 0.0f)
		{
			deadzone = 0.0f;
		}

		if (fabsf(yaw) < deadzone)
		{
			yaw = 0.0f;
		}

		if (fabsf(pitch) < deadzone)
		{
			pitch = 0.0f;
		}

		if (smoothing < 0.0f)
		{
			smoothing = 0.0f;
		}
		else if (smoothing > 1.0f)
		{
			smoothing = 1.0f;
		}

		if (frame_time < 0.0f)
		{
			frame_time = 0.0f;
		}
		else if (frame_time > 0.1f)
		{
			frame_time = 0.1f;
		}

		/* 0 means no smoothing; larger values retain more of the prior sample. */
		if (smoothing > 0.95f)
		{
			smoothing = 0.95f;
		}
		psmove_filtered_yaw +=
			(yaw - psmove_filtered_yaw) * (1.0f - smoothing);
		psmove_filtered_pitch +=
			(pitch - psmove_filtered_pitch) * (1.0f - smoothing);

		/* Convert libgem radians/second to Quake view-angle degrees. */
		cl.viewangles[YAW] -= psmove_filtered_yaw *
			psmove_yawsensitivity->value * 57.2957795f * frame_time;
		cl.viewangles[PITCH] += psmove_filtered_pitch *
			psmove_pitchsensitivity->value * 57.2957795f * frame_time *
			(psmove_invertpitch->value ? -1.0f : 1.0f);
	}
}

/* ------------------------------------------------------------------ */

static void
IN_JoyAltSelectorDown(void)
{
	joy_altselector_pressed = true;
}

static void
IN_JoyAltSelectorUp(void)
{
	joy_altselector_pressed = false;
}

static void
IN_PSMoveRestart(void)
{
	if (psmove_initialized)
	{
		PSMove_Shutdown();
		psmove_initialized = false;
	}

	psmove_filtered_yaw = 0.0f;
	psmove_filtered_pitch = 0.0f;

	if (psmove_enable->value)
	{
		psmove_initialized = PSMove_Init();
	}
}

static void
IN_PSMoveCalibrate(void)
{
	if (!psmove_enable->value)
	{
		Com_Printf("PS Move aiming is disabled.\n");
		return;
	}

	if (!psmove_initialized)
	{
		IN_PSMoveRestart();
	}
	if (psmove_initialized)
	{
		PSMove_RequestCalibration();
	}
}

static void
IN_PSMoveStatus(void)
{
	if (!psmove_initialized)
	{
		Com_Printf("PS Move backend is not initialized. Use "
			"psmove_calibrate or psmove_reinit after boot.\n");
		return;
	}
	PSMove_PrintStatus();
}

void
In_FlushQueue(void)
{
	Key_MarkAllUp();
	IN_JoyAltSelectorUp();
}

/* ------------------------------------------------------------------ */

/*
 * Haptic Feedback:
 *    effect_volume=0..SHRT_MAX
 *    effect{x,y,z} - effect direction
 *    name - sound file name
 */
void
Haptic_Feedback(char *name, int effect_volume, int effect_duration,
			   int effect_begin, int effect_end,
			   int effect_attack, int effect_fade,
			   int effect_x, int effect_y, int effect_z)
{
}

/*
 * Initializes the backend
 */
void
IN_Init(void)
{
	cvar_t *binding_revision;

	PS3_BOOT_TRACE("IN_Init: before ioPadInit");
	ioPadInit(7);
	PS3_BOOT_TRACE("IN_Init: ioPadInit complete");

	Com_Printf("------- input initialization -------\n");

	joystick_yaw = joystick_pitch = joystick_forwardmove = joystick_sidemove = 0;

	exponential_speedup = Cvar_Get("exponential_speedup", "0", CVAR_ARCHIVE);
	freelook = Cvar_Get("freelook", "1", CVAR_ARCHIVE);
	in_grab = Cvar_Get("in_grab", "2", CVAR_ARCHIVE);
	lookstrafe = Cvar_Get("lookstrafe", "0", CVAR_ARCHIVE);
	m_filter = Cvar_Get("m_filter", "0", CVAR_ARCHIVE);
	m_up = Cvar_Get("m_up", "1", CVAR_ARCHIVE);
	m_forward = Cvar_Get("m_forward", "1", CVAR_ARCHIVE);
	m_pitch = Cvar_Get("m_pitch", "0.022", CVAR_ARCHIVE);
	m_side = Cvar_Get("m_side", "0.8", CVAR_ARCHIVE);
	m_yaw = Cvar_Get("m_yaw", "0.022", CVAR_ARCHIVE);
	sensitivity = Cvar_Get("sensitivity", "3", CVAR_ARCHIVE);

	psmove_enable = Cvar_Get("psmove_enable", "0", CVAR_ARCHIVE);
	psmove_deadzone = Cvar_Get("psmove_deadzone", "0.025", CVAR_ARCHIVE);
	psmove_smoothing = Cvar_Get("psmove_smoothing", "0.35", CVAR_ARCHIVE);
	psmove_yawsensitivity = Cvar_Get("psmove_yawsensitivity", "1.0", CVAR_ARCHIVE);
	psmove_pitchsensitivity = Cvar_Get("psmove_pitchsensitivity", "1.0", CVAR_ARCHIVE);
	psmove_invertpitch = Cvar_Get("psmove_invertpitch", "0", CVAR_ARCHIVE);

	joy_haptic_magnitude = Cvar_Get("joy_haptic_magnitude", "0.0", CVAR_ARCHIVE);

	joy_yawsensitivity = Cvar_Get("joy_yawsensitivity", "1.0", CVAR_ARCHIVE);
	joy_pitchsensitivity = Cvar_Get("joy_pitchsensitivity", "1.0", CVAR_ARCHIVE);
	joy_forwardsensitivity = Cvar_Get("joy_forwardsensitivity", "1.0", CVAR_ARCHIVE);
	joy_sidesensitivity = Cvar_Get("joy_sidesensitivity", "1.0", CVAR_ARCHIVE);
	joy_upsensitivity = Cvar_Get("joy_upsensitivity", "1.0", CVAR_ARCHIVE);
	joy_expo = Cvar_Get("joy_expo", "2.0", CVAR_ARCHIVE);

	joy_axis_leftx = Cvar_Get("joy_axis_leftx", "sidemove", CVAR_ARCHIVE);
	joy_axis_lefty = Cvar_Get("joy_axis_lefty", "forwardmove", CVAR_ARCHIVE);
	joy_axis_rightx = Cvar_Get("joy_axis_rightx", "yaw", CVAR_ARCHIVE);
	joy_axis_righty = Cvar_Get("joy_axis_righty", "pitch", CVAR_ARCHIVE);
	joy_axis_triggerleft = Cvar_Get("joy_axis_triggerleft", "triggerleft", CVAR_ARCHIVE);
	joy_axis_triggerright = Cvar_Get("joy_axis_triggerright", "triggerright", CVAR_ARCHIVE);

	joy_axis_leftx_threshold = Cvar_Get("joy_axis_leftx_threshold", "0.15", CVAR_ARCHIVE);
	joy_axis_lefty_threshold = Cvar_Get("joy_axis_lefty_threshold", "0.15", CVAR_ARCHIVE);
	joy_axis_rightx_threshold = Cvar_Get("joy_axis_rightx_threshold", "0.15", CVAR_ARCHIVE);
	joy_axis_righty_threshold = Cvar_Get("joy_axis_righty_threshold", "0.15", CVAR_ARCHIVE);
	joy_axis_triggerleft_threshold = Cvar_Get("joy_axis_triggerleft_threshold", "0.15", CVAR_ARCHIVE);
	joy_axis_triggerright_threshold = Cvar_Get("joy_axis_triggerright_threshold", "0.15", CVAR_ARCHIVE);

	/* Older PS3 configs never contained BTN_BACK because Select was emitted as
	 * the unbindable K_JOY_BACK escape key. Give the new regular key its old
	 * menu-opening behavior exactly once, without overwriting a later remap. */
	binding_revision = Cvar_Get("ps3_ds3_bindings_revision", "0", CVAR_ARCHIVE);
	if (binding_revision->value < 1.0f)
	{
		if (!keybindings[K_BTN_BACK] || !keybindings[K_BTN_BACK][0])
		{
			Key_SetBinding(K_BTN_BACK, "menu_main");
		}
		Cvar_Set("ps3_ds3_bindings_revision", "1");
	}

	Cmd_AddCommand("+joyaltselector", IN_JoyAltSelectorDown);
	Cmd_AddCommand("-joyaltselector", IN_JoyAltSelectorUp);
	Cmd_AddCommand("psmove_reinit", IN_PSMoveRestart);
	Cmd_AddCommand("psmove_calibrate", IN_PSMoveCalibrate);
	Cmd_AddCommand("psmove_status", IN_PSMoveStatus);

	/* Never enter libgem/libcamera from the title's boot path. Real Move and Eye
	 * devices can make those modules schedule SPURS work immediately; older
	 * builds consequently hung before the first visible frame when an archived
	 * psmove_enable=1 met an already-connected device. The menu toggle,
	 * calibrate action and psmove_reinit command remain explicit activation
	 * points after video, sysutil and input startup have all completed. */
	psmove_initialized = false;
	psmove_yaw_rate = 0.0f;
	psmove_pitch_rate = 0.0f;
	psmove_filtered_yaw = 0.0f;
	psmove_filtered_pitch = 0.0f;
	PS3_BOOT_TRACE("IN_Init: Move backend quarantined until explicit activation");
	PS3_BOOT_TRACE("IN_Init: complete");

	Com_Printf("------------------------------------\n\n");
}

void
IN_Shutdown(void)
{
	PS3_RUNTIME_TRACE("IN_Shutdown: entered");
	if (psmove_initialized)
	{
		PSMove_Shutdown();
		psmove_initialized = false;
	}

	Cmd_RemoveCommand("psmove_reinit");
	Cmd_RemoveCommand("psmove_calibrate");
	Cmd_RemoveCommand("psmove_status");
	Cmd_RemoveCommand("+joyaltselector");
	Cmd_RemoveCommand("-joyaltselector");
	Cmd_RemoveCommand("force_centerview");

	Com_Printf("Shutting down input.\n");
	PS3_RUNTIME_TRACE("IN_Shutdown: complete");
}

/* ------------------------------------------------------------------ */
