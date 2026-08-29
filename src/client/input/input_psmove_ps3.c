/*
 * PlayStation Move support for the PSL1GHT build.
 *
 * libgem needs both its inertial stream and PlayStation Eye frames for a
 * fully corrected Move pose.  Keep the Navigation controller on the normal
 * ioPad path; this module owns only the sphere-topped Move wand.
 */

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include <io/camera.h>
#include <io/move.h>
#include <spurs/spurs.h>
#include <sys/memory.h>
#include <sys/spu.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>

#include "header/psmove_ps3.h"
#include "../header/keyboard.h"

#define PSMOVE_SELECT   0x0001
#define PSMOVE_TRIGGER  0x0002
#define PSMOVE_MOVE     0x0004
#define PSMOVE_START    0x0008
#define PSMOVE_TRIANGLE 0x0010
#define PSMOVE_CIRCLE   0x0020
#define PSMOVE_CROSS    0x0040
#define PSMOVE_SQUARE   0x0080

/* The public PSL1GHT move header omits libgem's state return values. */
#define PSMOVE_GEM_SPHERE_NOT_CALIBRATED       2
#define PSMOVE_GEM_SPHERE_CALIBRATING           3
#define PSMOVE_GEM_COMPUTING_AVAILABLE_COLORS   4
#define PSMOVE_GEM_HUE_NOT_SET                   5
#define PSMOVE_GEM_NO_VIDEO                      6
#define PSMOVE_GEM_NOT_CALIBRATED                8
#define PSMOVE_GEM_INFO_READY                    1
#define PSMOVE_GEM_DONT_TRACK             (2u << 24)
#define PSMOVE_GEM_DONT_CARE              (4u << 24)

#define PSMOVE_CAMERA_CONTAINER_SIZE (2u * 1024u * 1024u)
#define PSMOVE_CAMERA_EXPOSURE 128
#define PSMOVE_CAMERA_QUALITY 0.5f
#define PSMOVE_GEM_SPUS 5
#define PSMOVE_CONNECTION_POLL_MS 250
#define PSMOVE_HUE_RETRY_MS 2000

typedef struct
{
	u16 mask;
	int key;
} psmove_button_map_t;

typedef enum
{
	PSMOVE_BACKEND_OFF = 0,
	PSMOVE_BACKEND_WAITING_CONTROLLER,
	PSMOVE_BACKEND_WAITING_CAMERA,
	PSMOVE_BACKEND_FINDING_COLOR,
	PSMOVE_BACKEND_NEEDS_CALIBRATION,
	PSMOVE_BACKEND_CALIBRATING,
	PSMOVE_BACKEND_READY,
	PSMOVE_BACKEND_TRACKING_LOST,
	PSMOVE_BACKEND_INERTIAL_ONLY,
	PSMOVE_BACKEND_ERROR
} psmove_backend_status_t;

static const psmove_button_map_t button_map[] = {
	{PSMOVE_SELECT, K_JOY_BACK},
	{PSMOVE_TRIGGER, K_TRIG_RIGHT},
	{PSMOVE_MOVE, K_BTN_GUIDE},
	{PSMOVE_START, K_BTN_START},
	{PSMOVE_TRIANGLE, K_BTN_Y},
	{PSMOVE_CIRCLE, K_BTN_B},
	{PSMOVE_CROSS, K_BTN_A},
	{PSMOVE_SQUARE, K_BTN_X}
};

static Spurs *move_spurs ATTRIBUTE_PRXPTR = NULL;
static void *move_memory ATTRIBUTE_PRXPTR = NULL;
static qboolean gem_module_loaded = false;
static qboolean spurs_initialized = false;
static qboolean gem_initialized = false;

static qboolean camera_module_loaded = false;
static qboolean camera_container_created = false;
static qboolean camera_initialized = false;
static qboolean camera_initialized_owned = false;
static qboolean camera_opened = false;
static qboolean camera_started = false;
static sys_mem_container_t camera_container;
static cameraInfoEx camera_info;
static cameraReadInfo camera_read;
static u32 last_camera_frame;

static psmove_backend_status_t backend_status = PSMOVE_BACKEND_OFF;
static qboolean move_connected = false;
static qboolean calibration_requested = false;
static u16 previous_buttons = 0;
static int last_connection_poll;
static int last_hue_attempt;
static int status_overlay_until;
static int last_camera_error;

static float
PSMove_VectorElement(vec_float4 vector, unsigned int index)
{
	union
	{
		vec_float4 vector;
		float values[4];
	} value;

	value.vector = vector;
	return value.values[index & 3];
}

const char *
PSMove_GetStatusText(void)
{
	switch (backend_status)
	{
		case PSMOVE_BACKEND_WAITING_CONTROLLER:
			return "PS Move: sphere controller not detected";
		case PSMOVE_BACKEND_WAITING_CAMERA:
			return "PS Move: waiting for PlayStation Eye";
		case PSMOVE_BACKEND_FINDING_COLOR:
			return "PS Move: selecting a trackable sphere color";
		case PSMOVE_BACKEND_NEEDS_CALIBRATION:
			return "PS Move: aim at the Eye and press MOVE";
		case PSMOVE_BACKEND_CALIBRATING:
			return "PS Move: calibrating - keep the sphere visible";
		case PSMOVE_BACKEND_READY:
			return "PS Move: camera tracking ready";
		case PSMOVE_BACKEND_TRACKING_LOST:
			return "PS Move: sphere lost - face it toward the Eye";
		case PSMOVE_BACKEND_INERTIAL_ONLY:
			return "PS Move: inertial fallback active";
		case PSMOVE_BACKEND_ERROR:
			return "PS Move: initialization error";
		default:
			return "PS Move: disabled";
	}
}

const char *
PSMove_GetStatusDetail(void)
{
	switch (backend_status)
	{
		case PSMOVE_BACKEND_WAITING_CONTROLLER:
			return "Power on the wand with the glowing sphere; Navigation is separate.";
		case PSMOVE_BACKEND_WAITING_CAMERA:
			return "Connect a PlayStation Eye, then choose calibrate / recenter.";
		case PSMOVE_BACKEND_FINDING_COLOR:
			return "Keep the sphere in a clear view of the camera.";
		case PSMOVE_BACKEND_NEEDS_CALIBRATION:
			return "Hold the wand naturally, point at screen center, then press MOVE.";
		case PSMOVE_BACKEND_CALIBRATING:
			return "Hold steady until the sphere color locks and this message clears.";
		case PSMOVE_BACKEND_READY:
			return "Optical correction and Move buttons are active.";
		case PSMOVE_BACKEND_TRACKING_LOST:
			return "Aiming continues inertially until optical tracking returns.";
		case PSMOVE_BACKEND_INERTIAL_ONLY:
			return "Buttons and gyro work; connect the Eye for drift-corrected tracking.";
		case PSMOVE_BACKEND_ERROR:
			return "Run psmove_status for details, then use reinitialize PS Move.";
		default:
			return "Enable PS Move aiming in gamepad / Move settings.";
	}
}

static void
PSMove_SetStatus(psmove_backend_status_t status, int overlay_ms)
{
	if (backend_status != status)
	{
		backend_status = status;
		Com_Printf("%s\n", PSMove_GetStatusText());
	}

	if (overlay_ms > 0)
	{
		status_overlay_until = Sys_Milliseconds() + overlay_ms;
	}
}

qboolean
PSMove_ShouldDrawStatus(void)
{
	return gem_initialized &&
		(int)(status_overlay_until - Sys_Milliseconds()) > 0;
}

void
PSMove_PrintStatus(void)
{
	Com_Printf("%s\n%s\n", PSMove_GetStatusText(),
		PSMove_GetStatusDetail());
	Com_Printf("PS Move backend: wand=%s camera=%s started=%s frame=%u\n",
		move_connected ? "connected" : "missing",
		camera_opened ? "open" : "unavailable",
		camera_started ? "yes" : "no", last_camera_frame);
}

static void
PSMove_DispatchButtons(u16 buttons)
{
	unsigned int index;
	u16 changed = buttons ^ previous_buttons;

	for (index = 0; index < sizeof(button_map) / sizeof(button_map[0]); index++)
	{
		if (changed & button_map[index].mask)
		{
			Key_Event(button_map[index].key,
				(buttons & button_map[index].mask) != 0, true);
		}
	}

	previous_buttons = buttons;
}

static void
PSMove_ShutdownCamera(void)
{
	if (camera_started)
	{
		cameraStop(0);
		camera_started = false;
	}
	if (camera_opened)
	{
		cameraClose(0);
		camera_opened = false;
	}
	if (camera_initialized && camera_initialized_owned)
	{
		cameraEnd();
	}
	camera_initialized = false;
	camera_initialized_owned = false;
	if (camera_container_created)
	{
		sysMemContainerDestroy(camera_container);
		camera_container_created = false;
	}
	if (camera_module_loaded)
	{
		sysModuleUnload(SYSMODULE_CAMERA);
		camera_module_loaded = false;
	}
	last_camera_frame = 0;
}

static qboolean
PSMove_InitCamera(void)
{
	cameraType type = CAM_TYPE_UNKNOWN;
	int result;

	result = sysModuleLoad(SYSMODULE_CAMERA);
	if (result != 0 && result != (int)SYSMODULE_ERR_DUPLICATE)
	{
		Com_Printf("PS Move: couldn't load camera module (0x%x).\n", result);
		return false;
	}
	camera_module_loaded = (result == 0);

	result = sysMemContainerCreate(&camera_container,
		PSMOVE_CAMERA_CONTAINER_SIZE);
	if (result != 0)
	{
		Com_Printf("PS Move: couldn't create Eye memory container (0x%x).\n",
			result);
		PSMove_ShutdownCamera();
		return false;
	}
	camera_container_created = true;

	result = cameraInit();
	if (result != 0 && result != (int)CAMERA_ERRO_DOUBLE_INIT)
	{
		Com_Printf("PS Move: cameraInit failed (0x%x).\n", result);
		PSMove_ShutdownCamera();
		return false;
	}
	camera_initialized = true;
	camera_initialized_owned = (result == 0);

	result = cameraGetType(0, &type);
	if (result != 0 || type != CAM_TYPE_PLAYSTATION_EYE)
	{
		Com_Printf("PS Move: no PlayStation Eye detected (result 0x%x, type %d).\n",
			result, (int)type);
		PSMove_ShutdownCamera();
		return false;
	}

	memset(&camera_info, 0, sizeof(camera_info));
	camera_info.format = CAM_FORM_RAW8;
	camera_info.resolution = CAM_RESO_VGA;
	camera_info.framerate = 60;
	camera_info.info_ver = 0x0101;
	camera_info.container = camera_container;

	result = cameraOpenEx(0, &camera_info);
	if (result != 0)
	{
		Com_Printf("PS Move: couldn't open PlayStation Eye (0x%x).\n", result);
		PSMove_ShutdownCamera();
		return false;
	}
	camera_opened = true;

	memset(&camera_read, 0, sizeof(camera_read));
	camera_read.version = 0x0100;
	camera_read.buffer = camera_info.buffer;

	cameraReset(0);
	result = gemPrepareCamera(PSMOVE_CAMERA_EXPOSURE,
		PSMOVE_CAMERA_QUALITY);
	if (result != 0)
	{
		Com_Printf("PS Move: gemPrepareCamera failed (0x%x).\n", result);
		PSMove_ShutdownCamera();
		return false;
	}

	result = cameraStart(0);
	if (result != 0)
	{
		Com_Printf("PS Move: couldn't start PlayStation Eye (0x%x).\n", result);
		PSMove_ShutdownCamera();
		return false;
	}
	camera_started = true;
	last_camera_frame = 0;
	last_camera_error = 0;
	Com_Printf("PS Move: PlayStation Eye opened at %dx%d, %d fps.\n",
		camera_info.width, camera_info.height, camera_info.framerate);
	return true;
}

static qboolean
PSMove_UpdateCamera(void)
{
	int result;

	if (!camera_opened)
	{
		return false;
	}

	result = cameraReadEx(0, &camera_read);
	if (result == (int)CAMERA_ERRO_NEED_START)
	{
		cameraReset(0);
		gemPrepareCamera(PSMOVE_CAMERA_EXPOSURE, PSMOVE_CAMERA_QUALITY);
		result = cameraStart(0);
		camera_started = (result == 0);
		return false;
	}
	if (result != 0)
	{
		if (result != last_camera_error)
		{
			Com_Printf("PS Move: PlayStation Eye read failed (0x%x).\n",
				result);
			last_camera_error = result;
		}
		return false;
	}
	if (camera_read.readcount == 0 || camera_read.frame == last_camera_frame)
	{
		return false;
	}

	result = gemUpdateStart((const void *)(uintptr_t)camera_read.buffer,
		camera_read.timestamp);
	if (result != 0)
	{
		if (result != last_camera_error)
		{
			Com_Printf("PS Move: gemUpdateStart failed (0x%x).\n", result);
			last_camera_error = result;
		}
		return false;
	}
	result = gemUpdateFinish();
	if (result != 0)
	{
		if (result != last_camera_error)
		{
			Com_Printf("PS Move: gemUpdateFinish failed (0x%x).\n", result);
			last_camera_error = result;
		}
		return false;
	}

	last_camera_frame = camera_read.frame;
	last_camera_error = 0;
	return true;
}

static void
PSMove_PollConnection(qboolean force)
{
	gemInfo info;
	int now = Sys_Milliseconds();
	int result;

	if (!force && (int)(now - last_connection_poll) <
		PSMOVE_CONNECTION_POLL_MS)
	{
		return;
	}
	last_connection_poll = now;
	memset(&info, 0, sizeof(info));
	result = gemGetInfo(&info);
	move_connected = result == 0 && info.connected > 0 &&
		info.status[0] == PSMOVE_GEM_INFO_READY;

	if (!move_connected)
	{
		PSMove_SetStatus(PSMOVE_BACKEND_WAITING_CONTROLLER, 0);
	}
}

qboolean
PSMove_Init(void)
{
	SpursAttribute spurs_attribute;
	gemAttribute gem_attribute;
	sys_ppu_thread_t thread_id;
	s32 ppu_priority = 1000;
	int memory_size;
	int result;
	unsigned int index;

	if (gem_initialized)
	{
		return true;
	}

	backend_status = PSMOVE_BACKEND_OFF;
	result = sysModuleLoad(SYSMODULE_GEM);
	if (result != 0 && result != (int)SYSMODULE_ERR_DUPLICATE)
	{
		Com_Printf("PS Move: couldn't load GEM module (0x%x).\n", result);
		PSMove_SetStatus(PSMOVE_BACKEND_ERROR, 15000);
		return false;
	}
	gem_module_loaded = (result == 0);

	/* libgem contains five SPURS workloads. Supplying only one runnable
	 * priority (as v2.00 did) leaves the real-device path able to wait forever
	 * once a connected wand or Eye activates the remaining jobs. Match the
	 * working PSL1GHT samples: five SPURS workers and priorities 0..4 enabled.
	 * This pool exists only while Move is explicitly active. */
	sysSpuInitialize(6, 0);
	if (sysThreadGetId(&thread_id) == 0)
	{
		sysThreadGetPriority(thread_id, &ppu_priority);
	}
	if (ppu_priority <= 0)
	{
		ppu_priority = 1000;
	}

	move_spurs = memalign(SPURS_ALIGN, sizeof(Spurs));
	if (!move_spurs)
	{
		Com_Printf("PS Move: couldn't allocate SPURS state.\n");
		PSMove_Shutdown();
		return false;
	}

	result = spursAttributeInitialize(&spurs_attribute, PSMOVE_GEM_SPUS, 250,
		ppu_priority - 1, true);
	if (result == 0)
	{
		result = spursAttributeSetNamePrefix(&spurs_attribute, "yq2move", 7);
	}
	if (result == 0)
	{
		result = spursInitializeWithAttribute(move_spurs, &spurs_attribute);
	}
	if (result != 0)
	{
		Com_Printf("PS Move: SPURS initialization failed (0x%x).\n", result);
		PSMove_Shutdown();
		return false;
	}
	spurs_initialized = true;

	memory_size = gemGetMemorySize(1);
	if (memory_size <= 0)
	{
		Com_Printf("PS Move: GEM returned an invalid memory size (0x%x).\n",
			memory_size);
		PSMove_Shutdown();
		return false;
	}
	move_memory = memalign(128, (size_t)memory_size);
	if (!move_memory)
	{
		Com_Printf("PS Move: couldn't allocate %d bytes for GEM.\n",
			memory_size);
		PSMove_Shutdown();
		return false;
	}

	memset(&gem_attribute, 0, sizeof(gem_attribute));
	gem_attribute.version = MOVE_VERSION;
	gem_attribute.max = 1;
	gem_attribute.memory = move_memory;
	gem_attribute.spurs = move_spurs;
	for (index = 0; index < 8; index++)
	{
		gem_attribute.spu_priorities[index] =
			(index < PSMOVE_GEM_SPUS) ? 1 : 0;
	}

	result = gemInit(&gem_attribute);
	if (result != 0)
	{
		Com_Printf("PS Move: GEM initialization failed (0x%x).\n", result);
		PSMove_Shutdown();
		return false;
	}
	gem_initialized = true;
	gemReset(0);

	previous_buttons = 0;
	calibration_requested = false;
	move_connected = false;
	last_connection_poll = Sys_Milliseconds() - PSMOVE_CONNECTION_POLL_MS;
	last_hue_attempt = Sys_Milliseconds() - PSMOVE_HUE_RETRY_MS;
	status_overlay_until = Sys_Milliseconds() + 15000;
	PSMove_PollConnection(true);

	if (!PSMove_InitCamera())
	{
		PSMove_SetStatus(move_connected ? PSMOVE_BACKEND_INERTIAL_ONLY :
			PSMOVE_BACKEND_WAITING_CONTROLLER, 15000);
	}
	else if (move_connected)
	{
		PSMove_SetStatus(PSMOVE_BACKEND_FINDING_COLOR, 15000);
	}

	Com_Printf("PS Move backend initialized (wand detection, Eye tracking, "
		"calibration and inertial fallback).\n");
	return true;
}

void
PSMove_Shutdown(void)
{
	PSMove_DispatchButtons(0);

	if (gem_initialized)
	{
		gemSetRumble(0, 0);
		gemEnd();
		gem_initialized = false;
	}

	PSMove_ShutdownCamera();

	if (spurs_initialized)
	{
		spursFinalize(move_spurs);
		spurs_initialized = false;
	}
	if (move_memory)
	{
		free(move_memory);
		move_memory = NULL;
	}
	if (move_spurs)
	{
		free(move_spurs);
		move_spurs = NULL;
	}
	if (gem_module_loaded)
	{
		sysModuleUnload(SYSMODULE_GEM);
		gem_module_loaded = false;
	}

	move_connected = false;
	calibration_requested = false;
	last_camera_frame = 0;
	backend_status = PSMOVE_BACKEND_OFF;
}

void
PSMove_RequestCalibration(void)
{
	if (!gem_initialized)
	{
		return;
	}

	calibration_requested = true;
	status_overlay_until = Sys_Milliseconds() + 30000;
	PSMove_PollConnection(true);
	if (!move_connected)
	{
		PSMove_SetStatus(PSMOVE_BACKEND_WAITING_CONTROLLER, 30000);
	}
	else if (!camera_opened)
	{
		PSMove_SetStatus(PSMOVE_BACKEND_WAITING_CAMERA, 30000);
	}
	else
	{
		PSMove_SetStatus(PSMOVE_BACKEND_NEEDS_CALIBRATION, 30000);
	}
}

qboolean
PSMove_Update(float *yaw_rate, float *pitch_rate)
{
	gemInertialState inertial;
	gemState state;
	u16 buttons = 0;
	u16 pressed;
	int state_result = -1;
	int inertial_result;
	int now;

	*yaw_rate = 0.0f;
	*pitch_rate = 0.0f;
	if (!gem_initialized)
	{
		return false;
	}

	now = Sys_Milliseconds();
	PSMove_PollConnection(false);
	if (!move_connected)
	{
		PSMove_DispatchButtons(0);
		return false;
	}

	if (camera_opened)
	{
		PSMove_UpdateCamera();
		memset(&state, 0, sizeof(state));
		state_result = gemGetState(0, STATE_CURRENT_TIME, 0, &state);
		buttons = state.paddata.buttons;
	}

	memset(&inertial, 0, sizeof(inertial));
	inertial_result = gemGetInertialState(0, GEM_INERTIAL_LATEST, 0,
		&inertial);
	if (inertial_result == 0 && buttons == 0)
	{
		buttons = inertial.pad.buttons;
	}
	pressed = buttons & ~previous_buttons;

	if (camera_opened && state_result == PSMOVE_GEM_HUE_NOT_SET &&
		(int)(now - last_hue_attempt) >= PSMOVE_HUE_RETRY_MS)
	{
		u32 requested_hues[MAX_MOVES] = {
			PSMOVE_GEM_DONT_CARE, PSMOVE_GEM_DONT_TRACK,
			PSMOVE_GEM_DONT_TRACK, PSMOVE_GEM_DONT_TRACK
		};
		u32 selected_hues[MAX_MOVES] = {0, 0, 0, 0};
		int result = gemTrackHues(requested_hues, selected_hues);
		last_hue_attempt = now;
		if (result != 0)
		{
			Com_Printf("PS Move: gemTrackHues failed (0x%x).\n", result);
		}
		PSMove_SetStatus(PSMOVE_BACKEND_FINDING_COLOR, 15000);
	}
	else if (camera_opened &&
		(state_result == PSMOVE_GEM_SPHERE_NOT_CALIBRATED ||
		 state_result == PSMOVE_GEM_NOT_CALIBRATED))
	{
		PSMove_SetStatus(PSMOVE_BACKEND_NEEDS_CALIBRATION, 0);
		gemForceRGB(0, 0.5f, 0.5f, 0.5f);
		if (calibration_requested || (pressed & PSMOVE_MOVE))
		{
			int result = gemCalibrate(0);
			if (result == 0)
			{
				calibration_requested = false;
				PSMove_SetStatus(PSMOVE_BACKEND_CALIBRATING, 30000);
			}
			else
			{
				Com_Printf("PS Move: gemCalibrate failed (0x%x).\n", result);
			}
		}
	}
	else if (camera_opened &&
		(state_result == PSMOVE_GEM_SPHERE_CALIBRATING ||
		 state_result == PSMOVE_GEM_COMPUTING_AVAILABLE_COLORS))
	{
		PSMove_SetStatus(PSMOVE_BACKEND_CALIBRATING, 0);
	}
	else if (camera_opened && state_result == 0)
	{
		if (calibration_requested)
		{
			int result = gemCalibrate(0);
			if (result == 0)
			{
				calibration_requested = false;
				PSMove_SetStatus(PSMOVE_BACKEND_CALIBRATING, 30000);
			}
		}
		else if ((state.tracking & GEM_TRACKING_VISIBLE) != 0)
		{
			PSMove_SetStatus(PSMOVE_BACKEND_READY,
				backend_status == PSMOVE_BACKEND_READY ? 0 : 4000);
		}
		else
		{
			PSMove_SetStatus(PSMOVE_BACKEND_TRACKING_LOST, 0);
		}
	}
	else if (camera_opened && state_result == PSMOVE_GEM_NO_VIDEO)
	{
		PSMove_SetStatus(PSMOVE_BACKEND_WAITING_CAMERA, 0);
	}
	else if (!camera_opened && inertial_result == 0)
	{
		PSMove_SetStatus(PSMOVE_BACKEND_INERTIAL_ONLY, 0);
	}

	/* MOVE starts calibration while the alignment prompt is active; do not also
	 * send its Guide binding into the game on that same press. */
	if (backend_status == PSMOVE_BACKEND_NEEDS_CALIBRATION ||
		backend_status == PSMOVE_BACKEND_CALIBRATING)
	{
		buttons &= ~PSMOVE_MOVE;
	}
	PSMove_DispatchButtons(buttons);

	if (camera_opened && state_result == 0)
	{
		/* Camera-corrected angular velocity prevents the unbounded drift of the
		 * raw gyro while retaining low-latency relative aiming. */
		*pitch_rate = PSMove_VectorElement(state.angvel, 0);
		*yaw_rate = PSMove_VectorElement(state.angvel, 1);
		return true;
	}
	if (inertial_result == 0)
	{
		*pitch_rate = PSMove_VectorElement(inertial.gyro, 0) -
			PSMove_VectorElement(inertial.gyro_bias, 0);
		*yaw_rate = PSMove_VectorElement(inertial.gyro, 1) -
			PSMove_VectorElement(inertial.gyro_bias, 1);
		return true;
	}

	return false;
}
