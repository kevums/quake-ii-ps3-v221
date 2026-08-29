/*
 * Optional native PS3 USB keyboard and mouse input.
 *
 * This wraps the established pad/Move backend instead of replacing it.  The
 * PS3 keyboard is read in raw packet mode so Quake bindings receive stable
 * physical keys while ioKbCnvRawCode supplies layout-aware text entry.
 */

#include <io/kb.h>
#include <io/mouse.h>
#include <string.h>

#include "header/input.h"
#include "../../client/header/keyboard.h"
#include "../../client/header/client.h"

#define USB_KEY_COUNT 256
#define USB_MOUSE_BUTTON_COUNT 5
#define USB_KEY_REPEAT_DELAY 500
#define USB_KEY_REPEAT_RATE 50
#define USB_MOUSE_MAX 3000.0f
#define USB_MOUSE_MIN 40.0f

void In_PS3_BaseInit(void);
void In_PS3_BaseUpdate(void);
void In_PS3_BaseMove(usercmd_t *cmd);
void In_PS3_BaseShutdown(void);
void In_PS3_BaseFlushQueue(void);

static qboolean usb_keyboard_initialized;
static qboolean usb_keyboard_connected;
static qboolean usb_keyboard_configured;
static qboolean usb_mouse_initialized;
static qboolean usb_mouse_connected;
static qboolean usb_osk_suppressed;

static qboolean usb_raw_down[USB_KEY_COUNT];
static int usb_raw_qkey[USB_KEY_COUNT];
static qboolean usb_raw_special[USB_KEY_COUNT];
static unsigned int usb_raw_repeat[USB_KEY_COUNT];
static unsigned int usb_modifier_state;
static unsigned int usb_mouse_buttons;
static float usb_mouse_x;
static float usb_mouse_y;
static KbConfig usb_keyboard_config;

static cvar_t *usb_m_filter;
static cvar_t *usb_exponential_speedup;

enum
{
	USB_MOD_CTRL = 1,
	USB_MOD_SHIFT = 2,
	USB_MOD_ALT = 4,
	USB_MOD_COMMAND = 8
};

static int
USB_QKeyForRaw(unsigned int raw, unsigned int modifiers)
{
	if ((raw >= KB_RAWKEY_A) && (raw <= KB_RAWKEY_Z))
	{
		return 'a' + (int)(raw - KB_RAWKEY_A);
	}

	if ((raw >= KB_RAWKEY_1) && (raw <= KB_RAWKEY_9))
	{
		return '1' + (int)(raw - KB_RAWKEY_1);
	}

	if (raw == KB_RAWKEY_0)
	{
		return '0';
	}

	switch (raw)
	{
		case KB_RAWKEY_ENTER: return K_ENTER;
		case KB_RAWKEY_ESC: return K_ESCAPE;
		case KB_RAWKEY_BS: return K_BACKSPACE;
		case KB_RAWKEY_TAB: return K_TAB;
		case KB_RAWKEY_SPACE: return K_SPACE;
		case KB_RAWKEY_MINUS: return '-';
		case KB_RAWKEY_EQUAL_101: return '=';
		case KB_RAWKEY_LEFT_BRACKET_101: return '[';
		case KB_RAWKEY_RIGHT_BRACKET_101: return ']';
		case KB_RAWKEY_BACKSLASH_101: return '\\';
		case KB_RAWKEY_SEMICOLON: return ';';
		case KB_RAWKEY_QUOTATION_101: return '\'';
		case KB_RAWKEY_106_KANJI:
			return modifiers ? '`' : K_CONSOLE;
		case KB_RAWKEY_COMMA: return ',';
		case KB_RAWKEY_PERIOD: return '.';
		case KB_RAWKEY_SLASH: return '/';
		case KB_RAWKEY_CAPS_LOCK: return K_CAPSLOCK;
		case KB_RAWKEY_F1: return K_F1;
		case KB_RAWKEY_F2: return K_F2;
		case KB_RAWKEY_F3: return K_F3;
		case KB_RAWKEY_F4: return K_F4;
		case KB_RAWKEY_F5: return K_F5;
		case KB_RAWKEY_F6: return K_F6;
		case KB_RAWKEY_F7: return K_F7;
		case KB_RAWKEY_F8: return K_F8;
		case KB_RAWKEY_F9: return K_F9;
		case KB_RAWKEY_F10: return K_F10;
		case KB_RAWKEY_F11: return K_F11;
		case KB_RAWKEY_F12: return K_F12;
		case KB_RAWKEY_PRINTSCREEN: return K_PRINT;
		case KB_RAWKEY_SCROLL_LOCK: return K_SCROLLOCK;
		case KB_RAWKEY_PAUSE: return K_PAUSE;
		case KB_RAWKEY_INSERT: return K_INS;
		case KB_RAWKEY_HOME: return K_HOME;
		case KB_RAWKEY_PAGE_UP: return K_PGUP;
		case KB_RAWKEY_DELETE: return K_DEL;
		case KB_RAWKEY_END: return K_END;
		case KB_RAWKEY_PAGE_DOWN: return K_PGDN;
		case KB_RAWKEY_RIGHT_ARROW: return K_RIGHTARROW;
		case KB_RAWKEY_LEFT_ARROW: return K_LEFTARROW;
		case KB_RAWKEY_DOWN_ARROW: return K_DOWNARROW;
		case KB_RAWKEY_UP_ARROW: return K_UPARROW;
		case KB_RAWKEY_KPAD_NUMLOCK: return K_KP_NUMLOCK;
		case KB_RAWKEY_KPAD_SLASH: return K_KP_SLASH;
		case KB_RAWKEY_KPAD_ASTERISK: return K_KP_STAR;
		case KB_RAWKEY_KPAD_MINUS: return K_KP_MINUS;
		case KB_RAWKEY_KPAD_PLUS: return K_KP_PLUS;
		case KB_RAWKEY_KPAD_ENTER: return K_KP_ENTER;
		case KB_RAWKEY_KPAD_1: return K_KP_END;
		case KB_RAWKEY_KPAD_2: return K_KP_DOWNARROW;
		case KB_RAWKEY_KPAD_3: return K_KP_PGDN;
		case KB_RAWKEY_KPAD_4: return K_KP_LEFTARROW;
		case KB_RAWKEY_KPAD_5: return K_KP_5;
		case KB_RAWKEY_KPAD_6: return K_KP_RIGHTARROW;
		case KB_RAWKEY_KPAD_7: return K_KP_HOME;
		case KB_RAWKEY_KPAD_8: return K_KP_UPARROW;
		case KB_RAWKEY_KPAD_9: return K_KP_PGUP;
		case KB_RAWKEY_KPAD_0: return K_KP_INS;
		case KB_RAWKEY_KPAD_PERIOD: return K_KP_DEL;
		case KB_RAWKEY_APPLICATION: return K_MENU;
		default: return 0;
	}
}

static qboolean
USB_KeyIsSpecial(int key)
{
	return (key >= K_COMMAND) || (key == K_TAB) || (key == K_ENTER) ||
		(key == K_ESCAPE) || (key == K_BACKSPACE);
}

static unsigned int
USB_Modifiers(const KbMkey *mkey)
{
	unsigned int raw = mkey->_KbMkeyU.mkeys;
	unsigned int result = 0;

	if (raw & 0x11U) result |= USB_MOD_CTRL;
	if (raw & 0x22U) result |= USB_MOD_SHIFT;
	if (raw & 0x44U) result |= USB_MOD_ALT;
	if (raw & 0x88U) result |= USB_MOD_COMMAND;
	return result;
}

static void
USB_UpdateModifier(unsigned int current, unsigned int bit, int key)
{
	qboolean was_down = (usb_modifier_state & bit) != 0;
	qboolean is_down = (current & bit) != 0;

	if (was_down != is_down)
	{
		Key_Event(key, is_down, true);
	}
}

static void
USB_SendCharacter(unsigned int raw, const KbData *data)
{
	u16 character;

	if (usb_raw_qkey[raw] == K_CONSOLE)
	{
		return;
	}

	character = ioKbCnvRawCode((KbMapping)usb_keyboard_config.mapping,
		data->mkey, data->led, (u16)raw);
	if ((character >= ' ') && (character <= '~'))
	{
		Char_Event((int)character);
	}
}

static void
USB_ReleaseKeyboard(qboolean emit_events)
{
	unsigned int raw;

	if (emit_events)
	{
		for (raw = 0; raw < USB_KEY_COUNT; raw++)
		{
			if (usb_raw_down[raw] && usb_raw_qkey[raw])
			{
				Key_Event(usb_raw_qkey[raw], false, usb_raw_special[raw]);
			}
		}

		if (usb_modifier_state & USB_MOD_CTRL) Key_Event(K_CTRL, false, true);
		if (usb_modifier_state & USB_MOD_SHIFT) Key_Event(K_SHIFT, false, true);
		if (usb_modifier_state & USB_MOD_ALT) Key_Event(K_ALT, false, true);
		if (usb_modifier_state & USB_MOD_COMMAND) Key_Event(K_COMMAND, false, true);
	}

	memset(usb_raw_down, 0, sizeof(usb_raw_down));
	memset(usb_raw_qkey, 0, sizeof(usb_raw_qkey));
	memset(usb_raw_special, 0, sizeof(usb_raw_special));
	memset(usb_raw_repeat, 0, sizeof(usb_raw_repeat));
	usb_modifier_state = 0;
}

static int
USB_MouseQKey(unsigned int index)
{
	static const int keys[USB_MOUSE_BUTTON_COUNT] =
		{K_MOUSE1, K_MOUSE2, K_MOUSE3, K_MOUSE4, K_MOUSE5};
	return keys[index];
}

static void
USB_ReleaseMouse(qboolean emit_events)
{
	unsigned int i;

	if (emit_events)
	{
		for (i = 0; i < USB_MOUSE_BUTTON_COUNT; i++)
		{
			if (usb_mouse_buttons & (1U << i))
			{
				Key_Event(USB_MouseQKey(i), false, true);
			}
		}
	}

	usb_mouse_buttons = 0;
	usb_mouse_x = 0.0f;
	usb_mouse_y = 0.0f;
}

static void
USB_UpdateKeyboard(void)
{
	KbInfo info;
	KbData data;
	qboolean current[USB_KEY_COUNT];
	unsigned int modifiers;
	unsigned int raw;
	unsigned int now;
	int count;
	int i;

	memset(&info, 0, sizeof(info));
	if ((ioKbGetInfo(&info) != 0) || !info.status[0])
	{
		if (usb_keyboard_connected)
		{
			USB_ReleaseKeyboard(true);
		}
		usb_keyboard_connected = false;
		usb_keyboard_configured = false;
		return;
	}

	usb_keyboard_connected = true;
	if (!usb_keyboard_configured)
	{
		if ((ioKbSetReadMode(0, KB_RMODE_PACKET) != 0) ||
			(ioKbSetCodeType(0, KB_CODETYPE_RAW) != 0))
		{
			return;
		}

		memset(&usb_keyboard_config, 0, sizeof(usb_keyboard_config));
		if (ioKbGetConfiguration(0, &usb_keyboard_config) != 0)
		{
			usb_keyboard_config.mapping = KB_MAPPING_101;
		}
		ioKbClearBuf(0);
		usb_keyboard_configured = true;
		Com_Printf("PS3 USB keyboard connected.\n");
		return;
	}

	memset(&data, 0, sizeof(data));
	if (ioKbRead(0, &data) != 0)
	{
		return;
	}

	memset(current, 0, sizeof(current));
	count = data.nb_keycode;
	if (count < 0) count = 0;
	if (count > MAX_KEYCODES) count = MAX_KEYCODES;
	for (i = 0; i < count; i++)
	{
		raw = data.keycode[i] & 0xffU;
		if (raw > KB_RAWKEY_E_UNDEF)
		{
			current[raw] = true;
		}
	}

	modifiers = USB_Modifiers(&data.mkey);
	USB_UpdateModifier(modifiers, USB_MOD_CTRL, K_CTRL);
	USB_UpdateModifier(modifiers, USB_MOD_SHIFT, K_SHIFT);
	USB_UpdateModifier(modifiers, USB_MOD_ALT, K_ALT);
	USB_UpdateModifier(modifiers, USB_MOD_COMMAND, K_COMMAND);
	usb_modifier_state = modifiers;
	now = (unsigned int)Sys_Milliseconds();

	for (raw = 0; raw < USB_KEY_COUNT; raw++)
	{
		if (current[raw] && !usb_raw_down[raw])
		{
			int key = USB_QKeyForRaw(raw, modifiers);
			usb_raw_down[raw] = true;
			usb_raw_qkey[raw] = key;
			usb_raw_special[raw] = USB_KeyIsSpecial(key);
			usb_raw_repeat[raw] = now + USB_KEY_REPEAT_DELAY;
			if (key)
			{
				Key_Event(key, true, usb_raw_special[raw]);
				if (!usb_raw_special[raw])
				{
					USB_SendCharacter(raw, &data);
				}
			}
		}
		else if (!current[raw] && usb_raw_down[raw])
		{
			if (usb_raw_qkey[raw])
			{
				Key_Event(usb_raw_qkey[raw], false, usb_raw_special[raw]);
			}
			usb_raw_down[raw] = false;
			usb_raw_qkey[raw] = 0;
			usb_raw_special[raw] = false;
			usb_raw_repeat[raw] = 0;
		}
		else if (current[raw] &&
			((int)(now - usb_raw_repeat[raw]) >= 0))
		{
			if (!usb_raw_special[raw])
			{
				USB_SendCharacter(raw, &data);
			}
			else if (usb_raw_qkey[raw] == K_BACKSPACE)
			{
				Key_Event(K_BACKSPACE, true, true);
			}
			usb_raw_repeat[raw] = now + USB_KEY_REPEAT_RATE;
		}
	}
}

static void
USB_UpdateMouseButtons(unsigned int buttons)
{
	unsigned int i;

	for (i = 0; i < USB_MOUSE_BUTTON_COUNT; i++)
	{
		unsigned int mask = 1U << i;
		if ((buttons & mask) != (usb_mouse_buttons & mask))
		{
			Key_Event(USB_MouseQKey(i), (buttons & mask) != 0, true);
		}
	}
	usb_mouse_buttons = buttons;
}

static void
USB_UpdateMouse(void)
{
	mouseInfo info;
	mouseDataList list;
	unsigned int i;

	memset(&info, 0, sizeof(info));
	if ((ioMouseGetInfo(&info) != 0) || !info.status[0])
	{
		if (usb_mouse_connected)
		{
			USB_ReleaseMouse(true);
		}
		usb_mouse_connected = false;
		return;
	}

	if (!usb_mouse_connected)
	{
		usb_mouse_connected = true;
		ioMouseClearBuf(0);
		Com_Printf("PS3 USB mouse connected.\n");
		return;
	}

	memset(&list, 0, sizeof(list));
	if (ioMouseGetDataList(0, &list) != 0)
	{
		return;
	}
	if (list.count > MOUSE_MAX_DATA_LIST)
	{
		list.count = MOUSE_MAX_DATA_LIST;
	}

	for (i = 0; i < list.count; i++)
	{
		mouseData *data = &list.list[i];
		USB_UpdateMouseButtons(data->buttons);
		if ((cls.key_dest == key_game) && ((int)cl_paused->value == 0))
		{
			usb_mouse_x += data->x_axis;
			usb_mouse_y += data->y_axis;
		}
		if (data->wheel)
		{
			int key = data->wheel > 0 ? K_MWHEELUP : K_MWHEELDOWN;
			Key_Event(key, true, true);
			Key_Event(key, false, true);
		}
	}
}

void
IN_Init(void)
{
	int result;

	In_PS3_BaseInit();
	usb_m_filter = Cvar_Get("m_filter", "0", CVAR_ARCHIVE);
	usb_exponential_speedup = Cvar_Get("exponential_speedup", "0", CVAR_ARCHIVE);

	result = ioKbInit(1);
	usb_keyboard_initialized = (result == 0);
	Com_Printf("PS3 USB keyboard: %s (0x%08x).\n",
		usb_keyboard_initialized ? "ready" : "unavailable", result);

	result = ioMouseInit(1);
	usb_mouse_initialized = (result == 0);
	Com_Printf("PS3 USB mouse: %s (0x%08x).\n",
		usb_mouse_initialized ? "ready" : "unavailable", result);
	PS3_BOOT_TRACE("IN_Init: optional USB keyboard/mouse complete");
}

void
IN_Update(void)
{
	In_PS3_BaseUpdate();

	if (Sys_PS3_OSKActive())
	{
		if (!usb_osk_suppressed)
		{
			USB_ReleaseKeyboard(true);
			USB_ReleaseMouse(true);
			if (usb_keyboard_initialized) ioKbClearBuf(0);
			if (usb_mouse_initialized) ioMouseClearBuf(0);
			usb_osk_suppressed = true;
		}
		return;
	}

	if (usb_osk_suppressed)
	{
		if (usb_keyboard_initialized) ioKbClearBuf(0);
		if (usb_mouse_initialized) ioMouseClearBuf(0);
		usb_osk_suppressed = false;
	}

	if (usb_keyboard_initialized) USB_UpdateKeyboard();
	if (usb_mouse_initialized) USB_UpdateMouse();
}

void
IN_Move(usercmd_t *cmd)
{
	static float old_x;
	static float old_y;
	float mouse_x = usb_mouse_x;
	float mouse_y = usb_mouse_y;

	In_PS3_BaseMove(cmd);
	usb_mouse_x = 0.0f;
	usb_mouse_y = 0.0f;

	if (usb_m_filter && usb_m_filter->value)
	{
		if ((mouse_x > 1.0f) || (mouse_x < -1.0f))
			mouse_x = (mouse_x + old_x) * 0.5f;
		if ((mouse_y > 1.0f) || (mouse_y < -1.0f))
			mouse_y = (mouse_y + old_y) * 0.5f;
	}
	old_x = mouse_x;
	old_y = mouse_y;

	if (!mouse_x && !mouse_y)
	{
		return;
	}

	if (!usb_exponential_speedup || !usb_exponential_speedup->value)
	{
		mouse_x *= sensitivity->value;
		mouse_y *= sensitivity->value;
	}
	else if ((mouse_x > USB_MOUSE_MIN) || (mouse_y > USB_MOUSE_MIN) ||
		(mouse_x < -USB_MOUSE_MIN) || (mouse_y < -USB_MOUSE_MIN))
	{
		mouse_x = (mouse_x * mouse_x * mouse_x) / 4.0f;
		mouse_y = (mouse_y * mouse_y * mouse_y) / 4.0f;
		if (mouse_x > USB_MOUSE_MAX) mouse_x = USB_MOUSE_MAX;
		if (mouse_x < -USB_MOUSE_MAX) mouse_x = -USB_MOUSE_MAX;
		if (mouse_y > USB_MOUSE_MAX) mouse_y = USB_MOUSE_MAX;
		if (mouse_y < -USB_MOUSE_MAX) mouse_y = -USB_MOUSE_MAX;
	}

	if ((in_strafe.state & 1) || (lookstrafe->value && freelook->value))
		cmd->sidemove += m_side->value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw->value * mouse_x;

	if (freelook->value && !(in_strafe.state & 1))
		cl.viewangles[PITCH] += m_pitch->value * mouse_y;
	else
		cmd->forwardmove -= m_forward->value * mouse_y;
}

void
In_FlushQueue(void)
{
	In_PS3_BaseFlushQueue();
	USB_ReleaseKeyboard(false);
	USB_ReleaseMouse(false);
}

void
IN_Shutdown(void)
{
	USB_ReleaseKeyboard(true);
	USB_ReleaseMouse(true);
	if (usb_mouse_initialized)
	{
		ioMouseEnd();
		usb_mouse_initialized = false;
	}
	if (usb_keyboard_initialized)
	{
		ioKbEnd();
		usb_keyboard_initialized = false;
	}
	In_PS3_BaseShutdown();
}
