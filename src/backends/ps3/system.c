// system.c for PSL1GHT

#include "../../common/header/common.h"
#include "../../common/header/glob.h"

#include <ppu-lv2.h>
#include <lv2/syscalls.h>
#include <lv2/systime.h>

#include <sys/file.h>
#include <sys/memory.h>
#include <sys/stat.h>
#include <sysutil/osk.h>
#include <sysutil/sysutil.h>

#include <sys/cdefs.h>

/* PSL1GHT's public register helper is a small OPD adapter around this native
 * export. Calling the export directly keeps that extra wrapper out of the
 * tightly packed main SELF while preserving the exact +16 descriptor ABI. */
extern s32 sysUtilRegisterCallbackEx(s32 slot, void *callback_opd,
	void *userdata);

// replacing it here for compatability with yq2's common.h
#ifdef CFGDIR
#undef CFGDIR
#endif
#define CFGDIR "USRDIR"

// Config dir
char cfgdir[MAX_OSPATH] = CFGDIR;

static volatile qboolean ps3_exit_requested = false;
static qboolean ps3_sysutil_registered = false;
static qboolean ps3_exit_traced = false;
static qboolean ps3_callback_error_traced = false;

#define PS3_OSK_MAX_CHARS 79
#define PS3_OSK_PROMPT_CHARS 127
#define PS3_OSK_CONTAINER_BYTES (4u * 1024u * 1024u)
#define PS3_OSK_INVALID_CONTAINER ((sys_mem_container_t)0xffffffffu)

static volatile qboolean ps3_osk_loaded_event = false;
static volatile qboolean ps3_osk_entered_event = false;
static volatile qboolean ps3_osk_canceled_event = false;
static volatile qboolean ps3_osk_done_event = false;
static volatile qboolean ps3_osk_unloaded_event = false;
static qboolean ps3_osk_active = false;
static qboolean ps3_osk_text_fetched = false;
static qboolean ps3_osk_unload_started = false;
static qboolean ps3_osk_result_ready = false;
static qboolean ps3_osk_result_accepted = false;
static qboolean ps3_osk_numbers_only = false;
static qboolean ps3_osk_container_owned = false;
static qboolean ps3_osk_loaded_traced = false;
static qboolean ps3_osk_get_error_traced = false;
static qboolean ps3_osk_unload_error_traced = false;
static int ps3_osk_max_length = 0;
static sys_mem_container_t ps3_osk_container = PS3_OSK_INVALID_CONTAINER;
static u16 ps3_osk_prompt[PS3_OSK_PROMPT_CHARS + 1];
static u16 ps3_osk_initial[PS3_OSK_MAX_CHARS + 1];
static u16 ps3_osk_result_u16[PS3_OSK_MAX_CHARS + 1];
static char ps3_osk_result[PS3_OSK_MAX_CHARS + 1];
static oskCallbackReturnParam ps3_osk_output;

static void
Sys_PS3_CopyASCIIToU16(u16 *destination, size_t destination_count,
	const char *source)
{
	size_t i = 0;

	if (!destination || destination_count == 0)
	{
		return;
	}
	if (source)
	{
		while (i + 1 < destination_count && source[i])
		{
			unsigned char value = (unsigned char)source[i];
			destination[i] = value < 0x80 ? (u16)value : (u16)'?';
			i++;
		}
	}
	destination[i] = 0;
}

static void
Sys_PS3_ConvertOSKResult(void)
{
	int input_count = ps3_osk_output.len;
	int input_index;
	int output_index = 0;

	if (input_count < 0 || input_count > ps3_osk_max_length)
	{
		input_count = ps3_osk_max_length;
	}
	for (input_index = 0;
		 input_index < input_count && output_index < ps3_osk_max_length;
		 input_index++)
	{
		u16 value = ps3_osk_result_u16[input_index];
		if (!value)
		{
			break;
		}
		if (ps3_osk_numbers_only)
		{
			if (value >= (u16)'0' && value <= (u16)'9')
			{
				ps3_osk_result[output_index++] = (char)value;
			}
			continue;
		}
		/* Quake II's menu fields and network protocol are byte strings. Keep
		 * the native dialog's result within the same printable ASCII contract
		 * as Field_Key() rather than injecting partial UTF-8 sequences. */
		if (value >= 0x20 && value <= 0x7e)
		{
			ps3_osk_result[output_index++] = (char)value;
		}
		else
		{
			ps3_osk_result[output_index++] = '?';
		}
	}
	ps3_osk_result[output_index] = '\0';
}

static void
Sys_PS3_OSKReleaseContainer(void)
{
	if (ps3_osk_container_owned)
	{
		sysMemContainerDestroy(ps3_osk_container);
	}
	ps3_osk_container = PS3_OSK_INVALID_CONTAINER;
	ps3_osk_container_owned = false;
}

static void
Sys_PS3_OSKService(void)
{
	if (!ps3_osk_active)
	{
		return;
	}

	if (ps3_osk_loaded_event && !ps3_osk_loaded_traced)
	{
		PS3_RUNTIME_TRACE("PS3 OSK: system keyboard loaded");
		ps3_osk_loaded_traced = true;
	}

	/* INPUT_ENTERED normally precedes DONE. Also retry at DONE because older
	 * firmware revisions can omit the intermediate notification. */
	if ((ps3_osk_entered_event || ps3_osk_done_event) &&
		!ps3_osk_canceled_event && !ps3_osk_text_fetched)
	{
		s32 result;
		ps3_osk_output.res = OSK_OK;
		ps3_osk_output.len = ps3_osk_max_length;
		ps3_osk_output.str = ps3_osk_result_u16;
		result = oskGetInputText(&ps3_osk_output);
		if (result == 0)
		{
			ps3_osk_text_fetched = true;
		}
		else if (!ps3_osk_get_error_traced)
		{
			char stage[96];
			snprintf(stage, sizeof(stage),
				"PS3 OSK: input retrieval failed (%d)", (int)result);
			PS3_RUNTIME_TRACE(stage);
			ps3_osk_get_error_traced = true;
		}
	}

	if (ps3_osk_done_event && !ps3_osk_unload_started)
	{
		s32 result;
		if (ps3_osk_canceled_event)
		{
			ps3_osk_output.res = OSK_CANCELED;
			ps3_osk_output.len = 0;
		}
		result = oskUnloadAsync(&ps3_osk_output);
		if (result == 0)
		{
			ps3_osk_unload_started = true;
		}
		else if (!ps3_osk_unload_error_traced)
		{
			char stage[96];
			snprintf(stage, sizeof(stage),
				"PS3 OSK: asynchronous unload failed (%d)", (int)result);
			PS3_RUNTIME_TRACE(stage);
			ps3_osk_unload_error_traced = true;
		}
	}

	if (ps3_osk_unloaded_event)
	{
		ps3_osk_result_accepted = !ps3_osk_canceled_event &&
			ps3_osk_unload_started &&
			(ps3_osk_output.res == OSK_OK ||
			 ps3_osk_output.res == OSK_NO_TEXT);
		if (ps3_osk_result_accepted)
		{
			Sys_PS3_ConvertOSKResult();
			PS3_RUNTIME_TRACE("PS3 OSK: input accepted and resources released");
		}
		else
		{
			ps3_osk_result[0] = '\0';
			PS3_RUNTIME_TRACE("PS3 OSK: input canceled and resources released");
		}
		Sys_PS3_OSKReleaseContainer();
		ps3_osk_active = false;
		ps3_osk_result_ready = true;
	}
}

static s32
Sys_PS3_RegisterCallback(s32 slot, sysutilCallback callback, void *userdata)
{
	void *callback_opd = callback ?
		(void *)((uintptr_t)callback + 16u) : NULL;
	return sysUtilRegisterCallbackEx(slot, callback_opd, userdata);
}

static void
Sys_PS3_EventCallback(u64 status, u64 param, void *userdata)
{
	(void)param;
	(void)userdata;

	if (status == SYSUTIL_EXIT_GAME)
	{
		/* Keep the callback itself async-safe. The main thread performs the
		 * actual engine shutdown after sysUtilCheckCallback() returns. */
		ps3_exit_requested = true;
	}
	else if (status == SYSUTIL_OSK_LOADED)
	{
		ps3_osk_loaded_event = true;
	}
	else if (status == SYSUTIL_OSK_INPUT_ENTERED)
	{
		ps3_osk_entered_event = true;
	}
	else if (status == SYSUTIL_OSK_INPUT_CANCELED)
	{
		ps3_osk_canceled_event = true;
	}
	else if (status == SYSUTIL_OSK_DONE)
	{
		ps3_osk_done_event = true;
	}
	else if (status == SYSUTIL_OSK_UNLOADED)
	{
		ps3_osk_unloaded_event = true;
	}
}

qboolean
Sys_PS3_OSKOpen(const char *prompt, const char *initial_text,
	int max_length, qboolean numbers_only, qboolean address_input)
{
	oskParam parameters;
	oskInputFieldInfo input;
	s32 result;
	u32 panels;
	u32 first_panel;

	if (!ps3_sysutil_registered || ps3_exit_requested || ps3_osk_active ||
		ps3_osk_result_ready)
	{
		return false;
	}
	if (max_length < 1)
	{
		max_length = 1;
	}
	else if (max_length > PS3_OSK_MAX_CHARS)
	{
		max_length = PS3_OSK_MAX_CHARS;
	}

	memset(&parameters, 0, sizeof(parameters));
	memset(&input, 0, sizeof(input));
	memset(&ps3_osk_output, 0, sizeof(ps3_osk_output));
	memset(ps3_osk_result_u16, 0, sizeof(ps3_osk_result_u16));
	memset(ps3_osk_result, 0, sizeof(ps3_osk_result));
	Sys_PS3_CopyASCIIToU16(ps3_osk_prompt,
		sizeof(ps3_osk_prompt) / sizeof(ps3_osk_prompt[0]), prompt);
	Sys_PS3_CopyASCIIToU16(ps3_osk_initial,
		(size_t)max_length + 1, initial_text);

	if (numbers_only)
	{
		panels = OSK_PANEL_TYPE_NUMERAL;
		first_panel = OSK_PANEL_TYPE_NUMERAL;
		parameters.prohibitFlags = OSK_PROHIBIT_SPACE | OSK_PROHIBIT_RETURN;
	}
	else if (address_input)
	{
		panels = OSK_PANEL_TYPE_URL | OSK_PANEL_TYPE_ENGLISH |
			OSK_PANEL_TYPE_NUMERAL;
		first_panel = OSK_PANEL_TYPE_URL;
		parameters.prohibitFlags = OSK_PROHIBIT_SPACE | OSK_PROHIBIT_RETURN;
	}
	else
	{
		panels = OSK_PANEL_TYPE_DEFAULT_NO_JAPANESE |
			OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL |
			OSK_PANEL_TYPE_ENGLISH;
		first_panel = OSK_PANEL_TYPE_ENGLISH;
		parameters.prohibitFlags = OSK_PROHIBIT_RETURN;
	}
	parameters.allowedPanels = panels;
	parameters.firstViewPanel = first_panel;
	parameters.controlPoint.x = 0.0f;
	parameters.controlPoint.y = 0.0f;

	input.message = ps3_osk_prompt;
	input.startText = ps3_osk_initial;
	input.maxLength = max_length;
	ps3_osk_output.res = OSK_OK;
	ps3_osk_output.len = max_length;
	ps3_osk_output.str = ps3_osk_result_u16;

	/* The system documentation recommends a four-MiB container. Reserve it
	 * only for the dialog lifetime so normal play keeps the full budget. If
	 * that reservation is unavailable, let sysutil use the process container. */
	ps3_osk_container = PS3_OSK_INVALID_CONTAINER;
	ps3_osk_container_owned = false;
	if (sysMemContainerCreate(&ps3_osk_container,
		PS3_OSK_CONTAINER_BYTES) == 0)
	{
		ps3_osk_container_owned = true;
	}

	oskSetInitialInputDevice(OSK_DEVICE_PAD);
	oskSetKeyLayoutOption(numbers_only ? OSK_10KEY_PANEL :
		(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL));
	oskSetInitialKeyLayout(numbers_only ? OSK_INITIAL_10KEY_PANEL :
		OSK_INITIAL_FULLKEY_PANEL);
	oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER |
		OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER);

	result = oskLoadAsync(ps3_osk_container, &parameters, &input);
	if (result != 0 && ps3_osk_container_owned)
	{
		Sys_PS3_OSKReleaseContainer();
		result = oskLoadAsync(PS3_OSK_INVALID_CONTAINER, &parameters, &input);
	}
	if (result != 0)
	{
		char stage[96];
		Sys_PS3_OSKReleaseContainer();
		snprintf(stage, sizeof(stage),
			"PS3 OSK: load failed (%d)", (int)result);
		PS3_RUNTIME_TRACE(stage);
		return false;
	}

	ps3_osk_max_length = max_length;
	ps3_osk_numbers_only = numbers_only;
	ps3_osk_loaded_event = false;
	ps3_osk_entered_event = false;
	ps3_osk_canceled_event = false;
	ps3_osk_done_event = false;
	ps3_osk_unloaded_event = false;
	ps3_osk_text_fetched = false;
	ps3_osk_unload_started = false;
	ps3_osk_result_accepted = false;
	ps3_osk_loaded_traced = false;
	ps3_osk_get_error_traced = false;
	ps3_osk_unload_error_traced = false;
	ps3_osk_active = true;
	PS3_RUNTIME_TRACE("PS3 OSK: opening system keyboard");
	return true;
}

qboolean
Sys_PS3_OSKActive(void)
{
	return ps3_osk_active;
}

qboolean
Sys_PS3_OSKPoll(char *text, size_t text_size, qboolean *accepted)
{
	if (!ps3_osk_result_ready)
	{
		return false;
	}
	if (accepted)
	{
		*accepted = ps3_osk_result_accepted;
	}
	if (text && text_size > 0)
	{
		Q_strlcpy(text, ps3_osk_result, text_size);
	}
	ps3_osk_result_ready = false;
	return true;
}

static void
Sys_PS3_OSKAbortForShutdown(void)
{
	if (ps3_osk_active)
	{
		/* Abort is asynchronous. The process is terminating, so leave an owned
		 * container to LV2 instead of freeing memory the dialog may still touch. */
		oskAbort();
		ps3_osk_output.res = OSK_ABORT;
		ps3_osk_output.len = 0;
		oskUnloadAsync(&ps3_osk_output);
		ps3_osk_active = false;
		ps3_osk_result_ready = false;
		ps3_osk_container_owned = false;
		ps3_osk_container = PS3_OSK_INVALID_CONTAINER;
		PS3_RUNTIME_TRACE("PS3 OSK: shutdown aborted active keyboard");
	}
	else
	{
		Sys_PS3_OSKReleaseContainer();
	}
}

static void
Sys_PS3_UnregisterCallback(void)
{
	char trace_stage[96];
	s32 result;

	if (!ps3_sysutil_registered)
	{
		return;
	}

	Sys_PS3_OSKAbortForShutdown();
	result = sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	ps3_sysutil_registered = false;
	snprintf(trace_stage, sizeof(trace_stage),
		"PS3 sysutil: callback unregistered (%d)", (int)result);
	PS3_RUNTIME_TRACE(trace_stage);
}

qboolean
Sys_PS3_CheckCallbacks(void)
{
	if (ps3_sysutil_registered && !ps3_exit_requested)
	{
		s32 result = sysUtilCheckCallback();
		if (result != SYSUTIL_OK && !ps3_callback_error_traced)
		{
			char trace_stage[96];
			snprintf(trace_stage, sizeof(trace_stage),
				"PS3 sysutil: callback check failed (%d)", (int)result);
			PS3_RUNTIME_TRACE(trace_stage);
			ps3_callback_error_traced = true;
		}
		Sys_PS3_OSKService();
	}

	if (ps3_exit_requested && !ps3_exit_traced)
	{
		PS3_RUNTIME_TRACE("PS3 sysutil: XMB exit request received");
		ps3_exit_traced = true;
	}

	return ps3_exit_requested;
}

qboolean
Sys_PS3_ExitRequested(void)
{
	return ps3_exit_requested;
}

void
Sys_BootTrace(const char *stage, qboolean reset)
{
#ifdef PS3_DISC_BUILD
	static const char *primary_path =
		"/dev_hdd0/tmp/q2ps3-v221/USRDIR/boot-trace.log";
#else
	static const char *primary_path =
		"/dev_hdd0/game/QUAKE2000/USRDIR/boot-trace.log";
#endif
	static const char *fallback_path = "/dev_hdd0/tmp/q2ps3-boot-trace.log";
	FILE *trace = fopen(primary_path, reset ? "w" : "a");

	if (!trace)
	{
		trace = fopen(fallback_path, reset ? "w" : "a");
	}

	if (trace)
	{
		fprintf(trace, "%s\n", stage ? stage : "(null stage)");
		fflush(trace);
		fclose(trace);
	}
}

/* ================================================================ */

void 
Sys_Error(char *error, ...)
{
	va_list argptr;
	char string[1024];
	FILE *error_log;

	/* change stdin to non blocking */
	// fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) & ~FNDELAY);

	va_start(argptr, error);
	vsnprintf(string, sizeof(string), error, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", string);
	Sys_BootTrace(string, false);
	Sys_PS3_UnregisterCallback();

	/* Leave a persistent clue when initialization fails before video output. */
#ifdef PS3_DISC_BUILD
	error_log = fopen("/dev_hdd0/tmp/q2ps3-v221/USRDIR/boot-error.log", "a");
#else
	error_log = fopen("/dev_hdd0/game/QUAKE2000/USRDIR/boot-error.log", "a");
#endif
	if (error_log)
	{
		fprintf(error_log, "Error: %s\n", string);
		fclose(error_log);
	}

#ifndef DEDICATED_ONLY
	CL_Shutdown();
#endif
	Qcommon_Shutdown();

	exit(1);
}

void Sys_Quit(void)
{
	PS3_RUNTIME_TRACE("Sys_Quit: entered");
	Sys_PS3_UnregisterCallback();
#ifndef DEDICATED_ONLY
	PS3_RUNTIME_TRACE("Sys_Quit: before CL_Shutdown");
 	CL_Shutdown();
	PS3_RUNTIME_TRACE("Sys_Quit: CL_Shutdown complete");
#endif

	// if (logfile)
	// {
	// 	fclose(logfile);
	// 	logfile = NULL;
	// }

	PS3_RUNTIME_TRACE("Sys_Quit: before Qcommon_Shutdown");
	Qcommon_Shutdown();
	PS3_RUNTIME_TRACE("Sys_Quit: Qcommon_Shutdown complete");
	// fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) & ~FNDELAY);

	printf("------------------------------------\n");

	PS3_RUNTIME_TRACE("Sys_Quit: exiting process");
	exit(0);
}

void 
Sys_Init(void)
{
	char trace_stage[96];
	s32 result;

	ps3_exit_requested = false;
	ps3_exit_traced = false;
	ps3_callback_error_traced = false;
	ps3_osk_active = false;
	ps3_osk_result_ready = false;
	ps3_osk_container = PS3_OSK_INVALID_CONTAINER;
	ps3_osk_container_owned = false;
	result = Sys_PS3_RegisterCallback(SYSUTIL_EVENT_SLOT0,
		Sys_PS3_EventCallback, NULL);
	if (result == SYSUTIL_OK)
	{
		ps3_sysutil_registered = true;
		PS3_RUNTIME_TRACE("PS3 sysutil: exit callback registered");
		return;
	}

	ps3_sysutil_registered = false;
	snprintf(trace_stage, sizeof(trace_stage),
		"PS3 sysutil: callback registration failed (%d)", (int)result);
	PS3_RUNTIME_TRACE(trace_stage);
}


// ---------------------------------------------
//  Input/Output
// ---------------------------------------------

// returns static null terminated string or NULL
char*
Sys_ConsoleInput(void)
{
	/* PS3 had tty input but let's be real
	   we will never use ps3 as dedicated server
	   moreover with tty console input.
	   In future it could be onscreen keyboard
	   call. */
	return NULL;
}

// prints null terminated <string> to stdout
void
Sys_ConsoleOutput(char *string)
{
	fputs(string, stdout);
}


// ---------------------------------------------
//  Time
// ---------------------------------------------

// returns microseconds since first call
long long
Sys_Microseconds(void)
{
	static s64 first;
	s64 now = sysGetSystemTime();

	/* Game and interpolation timing must not follow the adjustable wall clock.
	 * LV2's system timer is monotonic and already expressed in microseconds. */
	if (!first)
	{
		/* Preserve Yamagi's expectation that the first reading is nonzero. */
		first = now - 1000;
	}

	return (long long)(now - first);
}

int
Sys_Milliseconds(void)
{
	return (int)(Sys_Microseconds()/1000ll);
}

// sleep for <nanosec> nanoseconds
void 
Sys_Nanosleep(int nanosec)
{
	/* Uses usleep syscall instead of nanosleep
	   due lack of it on PowerPC.
	   It's ain't problem b/c current codebase of YQ2
	   not calling it for precision higher then 1 us. */
	lv2syscall1(SYSCALL_TIMER_USLEEP, (uint64_t)(nanosec/1000ll));
}


// ---------------------------------------------
//  Filesytem
// ---------------------------------------------

// /absolute/path/to/directory[/]
void 
Sys_Mkdir(const char *path)
{
	// code based on one in apollo's savetool
	
	char fullpath[MAX_OSPATH];

	//snprintf(fullpath, sizeof(fullpath), "%s", path);
	strlcpy(fullpath, path, MAX_OSPATH);

	// remove trailing '/'
	char* ptr = fullpath;
	while (*ptr != '\0')
	{
		++ptr;
	}
	--ptr;
	if (*ptr == '/')
	{
		*ptr = '\0';
	}
	ptr = fullpath;

	// creating path to directory
	while (*ptr)
    {
    	ptr++;
        while (*ptr && *ptr != '/')
		{
            ptr++;
		}

        char last = *ptr;
		*ptr = 0;

        if (Sys_IsDir(fullpath) == false)
        {
            if (mkdir(fullpath, 0777) < 0)
			{
                return;
			}
            sysLv2FsChmod(fullpath, S_IFDIR | 0777);
        }
        
        *ptr = last;
    }

    return;
}

qboolean
Sys_IsDir(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) != -1)
	{
		if (S_ISDIR(sb.st_mode))
		{
			return true;
		}
	}

	return false;
}

qboolean
Sys_IsFile(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) != -1)
	{
		if (S_ISREG(sb.st_mode))
		{
			return true;
		}
	}

	return false;
}

// returns null terminated string -- path to
// config directory should end with '<cfgdir>/'
// variable whichi is defined in macro CFGDIR
char*
Sys_GetHomeDir(void)
{
	static char homedir[MAX_OSPATH];
#ifdef PS3_DISC_BUILD
	static const char* home = "/dev_hdd0/tmp/q2ps3-v221";
#else
	static const char* home = "/dev_hdd0/game/QUAKE2000";
#endif

	snprintf(homedir, sizeof(homedir), "%s/%s/", home, cfgdir);
	Sys_Mkdir(homedir);

	return homedir;
}

const char* Sys_GetBinaryDir()
{
	/* We don't load any libraries at runtime so we use this trick
       to not allow filesystem to add this path to search base, but
	   vid.c still needed to be changed for single file build.
	   Also this used in portable run which should not be allowed. */
	return "\0";
}

// See remove(3)
// Used in *download and Load/Save functions
void
Sys_Remove(const char *path)
{
	remove(path);
}

// See rename(2)
// Used in *download functions
int
Sys_Rename(const char *from, const char *to)
{
	return rename(from, to);
}

// removes dir <path> if it is exists
// <path> is absolute
void
Sys_RemoveDir(const char *path)
{
	if (sysLv2FsRmdir(path) != 0) 
	{
		printf("Sys_RemoveDir: can't remove dir: '%s'\n", path);
	}
}

// writes return of realpath(3) (<in>, NULL)
// to buffer <out> with max size <size>
// if realpath returns NULL returns false
// otherwise returns true
qboolean
Sys_Realpath(const char *in, char *out, size_t size)
{
	// Block off relative paths
	if (strstr(in, "..") != NULL)
	{
		Com_Printf("WARNING: Sys_Realpath2: refusing to solve relative path '%s'.\n", in);
		return false;
	}

	// Absolute paths remain the same
	// TODOOOO: /path/././dir_in_path/
	// TODO: Move to separate function
	if (in[0] == '/')
    {
		int len = Q_strlcpy(out, in, size);
		//size_t len = strlcpy(out, in, size);
		// But we remove last forwarslash
		if (out[len-1] == '/')
		{
			out[len-1] = '\0';
		}
		return Sys_IsDir(out);
	}

	size_t path_start = 0;
	if (in[0] == '.' && (in[1] == '/' || in[1] == '\0'))
	{
		++path_start;
		if (in[1] == '/')
		{
			++path_start;
		}
	}

	char* buf = malloc(size);
	Sys_GetWorkDir(buf, size);
	if (buf[0] == '\0')
	{
		Com_Printf("WARNING: Sys_Realpath2: Sys_GetWorkDir returned null string.\n");
		free(buf);
		return false;
	}
	char* bufend = buf;
	size_t freespace = size; 

	while (*bufend)
	{
		++bufend;
		--freespace;
	}
	Q_strlcpy(bufend, &in[path_start], freespace);
	// strlcpy(bufend, &in[path_start], freespace);

	// Threat buf as absolute path by calling
	// recursively itself.
	qboolean ret = Sys_Realpath(buf, out, size);

	free(buf);
	return ret;
}


#ifdef NEED_GET_PROC_ADDRESS
#error "Sys_GetProcAddress could not be implemented"
// void*
// Sys_GetProcAddress(void *handle, const char *sym);
#endif

#ifndef UNICORE
#error "dynamic libs could not be implemented"
// void
// Sys_FreeLibrary(void *handle);
// void*
// Sys_LoadLibrary(const char *path, const char *sym, void **handle);
// void*
// Sys_GetGameAPI(void *parms);
// void 
// Sys_UnloadGame(void);
#endif


// ---------------------------------------------
//  Process location
// ---------------------------------------------

/* This functions are used in server savegame read/write 
   functions. In YQ2 8.02pre game switches it's working
   directory to save path and then saves/read files at ./<name>
   after that switches back to orig WorkDir 
   So both functions could ignored which is leads to
   saving save/read files on default running directory.
   I don't know where is it. */

// writes YQ2's working directory path* 
// to <buffer> up to <len> bytes
// *path should be null terminated
void
Sys_GetWorkDir(char *buffer, size_t len)
{
	if (getcwd(buffer, len) != 0)
	{
		return;
	}

	buffer[0] = '\0';
}

// Sets YQ2's working directory to <path>
// <path> is null terminate string
// returns true on success false on fail
qboolean
Sys_SetWorkDir(char *path)
{
	if (chdir(path) == 0)
	{
		return true;
	}

	return false;
}

/* ================================================================ */

/* The musthave and canhave arguments are unused in YQ2. We
   can't remove them since Sys_FindFirst() and Sys_FindNext()
   are defined in shared.h and may be used in custom game DLLs. */

static char findbase[MAX_OSPATH];
static char findpath[MAX_OSPATH];
static char findpattern[MAX_OSPATH];
static s32 fdir = -1;


// /dev_hdd0/game/QUAKE2000/USRDIR/baseq2/*.pak - should open
// /dev_hdd0/game/baseq2/*.pak - should not open (return NULL)
char *Sys_FindFirst(char *path, unsigned musthave, unsigned canthave)
{
	sysFSDirent entry;
	// entry.d_type
	// files -> 0x02
	// dirs  -> 0x01

	char* ptr;

	// Block off relative paths
	if (strstr(path, "..") != NULL)
	{
		Com_Printf("WARNING: Sys_FindFirst: relative paths not allowed '%s'.\n", path);
		return NULL;
	}

	if (fdir >= 0)
	{
		Com_Printf("WARNING: Sys_FindFirst: without closing previous search, closing...\n");
		Sys_FindClose();
	}

	strcpy(findbase, path);

	if ((ptr = strrchr(findbase, '/')) != NULL)
	{
		*ptr = 0;
		strcpy(findpattern, ptr + 1);
		if (strcmp(findpattern, "*.*") == 0)
		{
			strcpy(findpattern, "*");
		}
	}
	else
	{
		strcpy(findpattern, "*");
	}

	// /dev_hdd0/game/QUAKE2/USRDIR/baseq2/ - fdir > 0
	// /dev_hdd0/game/QUAKE2/USRDIR/baseq2/not_existing*msa!%#@ - fdir < 0
	sysLv2FsOpenDir(findbase, &fdir);
	if (fdir < 0)
	{
		// Com_Printf("WARNING: Sys_FindFirst: could not open search directory '%s'\n", findbase);
		return NULL;
	}

	u64 readn = 0; 
	// 0 - '.' -> 0x102 - '..' -> 0x102 - 'file.pak -> 0x102 - 'scrnshot' -> 0x102 - EOF -> 0

	while (true)
	{
		sysLv2FsReadDir(fdir, &entry, &readn);
		if (readn != 0x102)
		{
			break;
		}
		if (!*findpattern || glob_match(findpattern, entry.d_name))
		{
			if ((strcmp(entry.d_name, ".") != 0) || (strcmp(entry.d_name, "..") != 0))
			{
				// Safe way to create path

				int c_cnt = MAX_OSPATH;
				int n_cpd;
				ptr = findpath;

				n_cpd = Q_strlcpy(ptr, findbase, c_cnt);
				ptr += n_cpd;
				c_cnt -= n_cpd;

				n_cpd = Q_strlcpy(ptr, "/", c_cnt);
				ptr += n_cpd;
				c_cnt -= n_cpd;

				Q_strlcpy(ptr, entry.d_name, c_cnt);
				return findpath;
			}
		}
	}

	return NULL;
}

char *Sys_FindNext(unsigned musthave, unsigned canthave)
{
	sysFSDirent entry;
	u64 readn = 0; 

	if (fdir < 0)
	{
		Com_Printf("WARNING: Sys_FindNext: search not started\n");
		return NULL;
	}

	while (true)
	{
		sysLv2FsReadDir(fdir, &entry, &readn);
		if (readn != 0x102)
		{
			break;
		}
		if (!*findpattern || glob_match(findpattern, entry.d_name))
		{
			if ((strcmp(entry.d_name, ".") != 0) || (strcmp(entry.d_name, "..") != 0))
			{
				// Safe way to create path

				int c_cnt = MAX_OSPATH;
				int n_cpd;
				char* ptr = findpath;

				n_cpd = Q_strlcpy(ptr, findbase, c_cnt);
				ptr += n_cpd;
				c_cnt -= n_cpd;

				n_cpd = Q_strlcpy(ptr, "/", c_cnt);
				ptr += n_cpd;
				c_cnt -= n_cpd;

				Q_strlcpy(ptr, entry.d_name, c_cnt);
				return findpath;
			}
		}
	}

	return NULL;
}

void Sys_FindClose(void)
{
	if (fdir >= 0)
	{
		sysLv2FsCloseDir(fdir);
	}
		
	fdir = -1;
}
