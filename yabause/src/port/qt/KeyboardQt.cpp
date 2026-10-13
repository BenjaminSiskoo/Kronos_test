/*	Copyright 2026 Kronos team

	This file is part of Kronos.

	Kronos is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Kronos is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Kronos; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "KeyboardQt.h"
#include "PerQt.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QHash>

/*
   Saturn key codes: Saturn Technical Bulletin #45 "Saturn Keyboard Data
   Format Ver.1.00", table "Meaning of Key Codes". It covers the main
   block, F1-F12, the arrows and the edit keys. The numeric keypad,
   NumLock, keypad Enter and Right Ctrl/Alt codes are not in that table
   (the Japanese Saturn keyboard has no keypad); they come from Mednafen
   (US keyboard through the PS/2 adapter, same 34h data format). The
   ISO "< >" key (left of Z on AZERTY/QWERTZ keyboards) is in neither:
   61h is its PS/2 scan code set 2 value, which every other code of the
   table follows.
*/
enum
{
	SK_NONE = -1,

	SK_F9 = 0x01, SK_F5 = 0x03, SK_F3 = 0x04, SK_F1 = 0x05, SK_F2 = 0x06,
	SK_F12 = 0x07, SK_F10 = 0x09, SK_F8 = 0x0A, SK_F6 = 0x0B, SK_F4 = 0x0C,
	SK_TAB = 0x0D, SK_GRAVE = 0x0E,
	SK_LALT = 0x11, SK_LSHIFT = 0x12, SK_LCTRL = 0x14, SK_Q = 0x15, SK_1 = 0x16,
	SK_RALT = 0x17, SK_RCTRL = 0x18, SK_KP_ENTER = 0x19,
	SK_Z = 0x1A, SK_S = 0x1B, SK_A = 0x1C, SK_W = 0x1D, SK_2 = 0x1E,
	SK_C = 0x21, SK_X = 0x22, SK_D = 0x23, SK_E = 0x24, SK_4 = 0x25, SK_3 = 0x26,
	SK_SPACE = 0x29, SK_V = 0x2A, SK_F = 0x2B, SK_T = 0x2C, SK_R = 0x2D, SK_5 = 0x2E,
	SK_N = 0x31, SK_B = 0x32, SK_H = 0x33, SK_G = 0x34, SK_Y = 0x35, SK_6 = 0x36,
	SK_M = 0x3A, SK_J = 0x3B, SK_U = 0x3C, SK_7 = 0x3D, SK_8 = 0x3E,
	SK_COMMA = 0x41, SK_K = 0x42, SK_I = 0x43, SK_O = 0x44, SK_0 = 0x45, SK_9 = 0x46,
	SK_PERIOD = 0x49, SK_SLASH = 0x4A, SK_L = 0x4B, SK_SEMICOLON = 0x4C, SK_P = 0x4D, SK_MINUS = 0x4E,
	SK_RO = 0x51,           /* JP "_" key */
	SK_APOSTROPHE = 0x52,   /* JP ":" */
	SK_LBRACKET = 0x54,     /* JP "@" */
	SK_EQUAL = 0x55,        /* JP "^" */
	SK_CAPSLOCK = 0x58, SK_RSHIFT = 0x59, SK_ENTER = 0x5A,
	SK_RBRACKET = 0x5B,     /* JP "[" */
	SK_BACKSLASH = 0x5D,    /* JP "]" */
	SK_ISO_102ND = 0x61,
	SK_BACKSPACE = 0x66,
	SK_YEN = 0x6A,          /* JP "\" */
	SK_KP1 = 0x69, SK_KP4 = 0x6B, SK_KP7 = 0x6C, SK_KP0 = 0x70, SK_KP_PERIOD = 0x71,
	SK_KP2 = 0x72, SK_KP5 = 0x73, SK_KP6 = 0x74, SK_KP8 = 0x75,
	SK_ESC = 0x76, SK_NUMLOCK = 0x77, SK_F11 = 0x78, SK_KP_PLUS = 0x79, SK_KP3 = 0x7A,
	SK_KP_MINUS = 0x7B, SK_KP_MULTIPLY = 0x7C, SK_KP9 = 0x7D, SK_SCROLLLOCK = 0x7E,
	SK_KP_DIVIDE = 0x80, SK_INSERT = 0x81, SK_PAUSE = 0x82, SK_F7 = 0x83, SK_PRINT = 0x84,
	SK_DELETE = 0x85, SK_LEFT = 0x86, SK_HOME = 0x87, SK_END = 0x88, SK_UP = 0x89,
	SK_DOWN = 0x8A, SK_PAGEUP = 0x8B, SK_PAGEDOWN = 0x8C, SK_RIGHT = 0x8D
};

/* PC/AT scan code set 1 (codes 01h-58h). Used for both Windows (scan code
   from WM_KEYDOWN, extended keys have bit 8 set) and Linux evdev, whose
   KEY_* values are identical to set 1 for this range (KEY_ESC = 1 ...
   KEY_F12 = 88). Index 0x45 is NumLock for evdev, see translateEvdev() /
   translateWindows() for the difference. */
static const short kSet1[0x59] =
{
	/* 00 */ SK_NONE,      SK_ESC,       SK_1,         SK_2,         SK_3,        SK_4,         SK_5,        SK_6,
	/* 08 */ SK_7,         SK_8,         SK_9,         SK_0,         SK_MINUS,    SK_EQUAL,     SK_BACKSPACE,SK_TAB,
	/* 10 */ SK_Q,         SK_W,         SK_E,         SK_R,         SK_T,        SK_Y,         SK_U,        SK_I,
	/* 18 */ SK_O,         SK_P,         SK_LBRACKET,  SK_RBRACKET,  SK_ENTER,    SK_LCTRL,     SK_A,        SK_S,
	/* 20 */ SK_D,         SK_F,         SK_G,         SK_H,         SK_J,        SK_K,         SK_L,        SK_SEMICOLON,
	/* 28 */ SK_APOSTROPHE,SK_GRAVE,     SK_LSHIFT,    SK_BACKSLASH, SK_Z,        SK_X,         SK_C,        SK_V,
	/* 30 */ SK_B,         SK_N,         SK_M,         SK_COMMA,     SK_PERIOD,   SK_SLASH,     SK_RSHIFT,   SK_KP_MULTIPLY,
	/* 38 */ SK_LALT,      SK_SPACE,     SK_CAPSLOCK,  SK_F1,        SK_F2,       SK_F3,        SK_F4,       SK_F5,
	/* 40 */ SK_F6,        SK_F7,        SK_F8,        SK_F9,        SK_F10,      SK_NUMLOCK,   SK_SCROLLLOCK,SK_KP7,
	/* 48 */ SK_KP8,       SK_KP9,       SK_KP_MINUS,  SK_KP4,       SK_KP5,      SK_KP6,       SK_KP_PLUS,  SK_KP1,
	/* 50 */ SK_KP2,       SK_KP3,       SK_KP0,       SK_KP_PERIOD, SK_NONE,     SK_NONE,      SK_ISO_102ND,SK_F11,
	/* 58 */ SK_F12
};
static const quint32 kSet1Count = sizeof( kSet1 ) / sizeof( kSet1[0] );

/* Linux evdev (linux/input-event-codes.h). X11 (xcb), Wayland and
   eglfs/libinput all give nativeScanCode() = evdev code + 8. */
static int translateEvdev( quint32 evdev )
{
	if ( evdev < kSet1Count )
		return kSet1[ evdev ];

	switch ( evdev )
	{
		case 85:  return SK_GRAVE;     /* KEY_ZENKAKUHANKAKU (same position) */
		case 89:  return SK_RO;        /* KEY_RO */
		case 96:  return SK_KP_ENTER;  /* KEY_KPENTER */
		case 97:  return SK_RCTRL;     /* KEY_RIGHTCTRL */
		case 98:  return SK_KP_DIVIDE; /* KEY_KPSLASH */
		case 99:  return SK_PRINT;     /* KEY_SYSRQ */
		case 100: return SK_RALT;      /* KEY_RIGHTALT */
		case 102: return SK_HOME;
		case 103: return SK_UP;
		case 104: return SK_PAGEUP;
		case 105: return SK_LEFT;
		case 106: return SK_RIGHT;
		case 107: return SK_END;
		case 108: return SK_DOWN;
		case 109: return SK_PAGEDOWN;
		case 110: return SK_INSERT;
		case 111: return SK_DELETE;
		case 119: return SK_PAUSE;     /* KEY_PAUSE */
		case 124: return SK_YEN;       /* KEY_YEN */
		default:  return SK_NONE;
	}
}

/* Windows: Qt gives (lParam >> 16) & 0x1FF, i.e. the set 1 scan code with
   the "extended key" flag in bit 8. */
static int translateWindows( quint32 sc )
{
	switch ( sc )
	{
		case 0x045: return SK_PAUSE;     /* Pause comes without the extended flag... */
		case 0x145: return SK_NUMLOCK;   /* ...and NumLock with it */
		case 0x146: return SK_PAUSE;     /* Ctrl+Pause (Break) */
		case 0x054: return SK_PRINT;     /* Alt+Print (SysRq) */
		case 0x070: return SK_NONE;      /* JP Katakana/Hiragana: no code in STTECH45 */
		case 0x073: return SK_RO;
		case 0x07D: return SK_YEN;
		case 0x11C: return SK_KP_ENTER;
		case 0x11D: return SK_RCTRL;
		case 0x135: return SK_KP_DIVIDE;
		case 0x137: return SK_PRINT;
		case 0x138: return SK_RALT;
		case 0x147: return SK_HOME;
		case 0x148: return SK_UP;
		case 0x149: return SK_PAGEUP;
		case 0x14B: return SK_LEFT;
		case 0x14D: return SK_RIGHT;
		case 0x14F: return SK_END;
		case 0x150: return SK_DOWN;
		case 0x151: return SK_PAGEDOWN;
		case 0x152: return SK_INSERT;
		case 0x153: return SK_DELETE;
		default: break;
	}
	if ( sc < kSet1Count )
		return kSet1[ sc ];
	return SK_NONE;
}

/* macOS: nativeVirtualKey() is the Carbon kVK_* code (position based),
   nativeScanCode() is not usable there (Qt documentation). */
static int translateMac( quint32 vk )
{
	switch ( vk )
	{
		case 0x00: return SK_A;          case 0x01: return SK_S;
		case 0x02: return SK_D;          case 0x03: return SK_F;
		case 0x04: return SK_H;          case 0x05: return SK_G;
		case 0x06: return SK_Z;          case 0x07: return SK_X;
		case 0x08: return SK_C;          case 0x09: return SK_V;
		case 0x0A: return SK_ISO_102ND;  /* kVK_ISO_Section */
		case 0x0B: return SK_B;          case 0x0C: return SK_Q;
		case 0x0D: return SK_W;          case 0x0E: return SK_E;
		case 0x0F: return SK_R;          case 0x10: return SK_Y;
		case 0x11: return SK_T;          case 0x12: return SK_1;
		case 0x13: return SK_2;          case 0x14: return SK_3;
		case 0x15: return SK_4;          case 0x16: return SK_6;
		case 0x17: return SK_5;          case 0x18: return SK_EQUAL;
		case 0x19: return SK_9;          case 0x1A: return SK_7;
		case 0x1B: return SK_MINUS;      case 0x1C: return SK_8;
		case 0x1D: return SK_0;          case 0x1E: return SK_RBRACKET;
		case 0x1F: return SK_O;          case 0x20: return SK_U;
		case 0x21: return SK_LBRACKET;   case 0x22: return SK_I;
		case 0x23: return SK_P;          case 0x24: return SK_ENTER;
		case 0x25: return SK_L;          case 0x26: return SK_J;
		case 0x27: return SK_APOSTROPHE; case 0x28: return SK_K;
		case 0x29: return SK_SEMICOLON;  case 0x2A: return SK_BACKSLASH;
		case 0x2B: return SK_COMMA;      case 0x2C: return SK_SLASH;
		case 0x2D: return SK_N;          case 0x2E: return SK_M;
		case 0x2F: return SK_PERIOD;     case 0x30: return SK_TAB;
		case 0x31: return SK_SPACE;      case 0x32: return SK_GRAVE;
		case 0x33: return SK_BACKSPACE;  case 0x35: return SK_ESC;
		case 0x41: return SK_KP_PERIOD;  case 0x43: return SK_KP_MULTIPLY;
		case 0x45: return SK_KP_PLUS;    case 0x47: return SK_NUMLOCK; /* Clear */
		case 0x4B: return SK_KP_DIVIDE;  case 0x4C: return SK_KP_ENTER;
		case 0x4E: return SK_KP_MINUS;
		case 0x52: return SK_KP0;        case 0x53: return SK_KP1;
		case 0x54: return SK_KP2;        case 0x55: return SK_KP3;
		case 0x56: return SK_KP4;        case 0x57: return SK_KP5;
		case 0x58: return SK_KP6;        case 0x59: return SK_KP7;
		case 0x5B: return SK_KP8;        case 0x5C: return SK_KP9;
		case 0x5D: return SK_YEN;        case 0x5E: return SK_RO;
		case 0x60: return SK_F5;         case 0x61: return SK_F6;
		case 0x62: return SK_F7;         case 0x63: return SK_F3;
		case 0x64: return SK_F8;         case 0x65: return SK_F9;
		case 0x67: return SK_F11;        case 0x6D: return SK_F10;
		case 0x6F: return SK_F12;        case 0x72: return SK_INSERT; /* Help */
		case 0x73: return SK_HOME;       case 0x74: return SK_PAGEUP;
		case 0x75: return SK_DELETE;     case 0x76: return SK_F4;
		case 0x77: return SK_END;        case 0x78: return SK_F2;
		case 0x79: return SK_PAGEDOWN;   case 0x7A: return SK_F1;
		case 0x7B: return SK_LEFT;       case 0x7C: return SK_RIGHT;
		case 0x7D: return SK_DOWN;       case 0x7E: return SK_UP;
		default:   return SK_NONE;
	}
}

static bool isModifierKey( int key )
{
	return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta ||
	       key == Qt::Key_Alt || key == Qt::Key_AltGr || key == Qt::Key_CapsLock ||
	       key == Qt::Key_Super_L || key == Qt::Key_Super_R;
}

/* Last resort, by Qt key (depends on the host layout, assumes US). */
static int translateQtKey( const QKeyEvent* e, bool mac )
{
	const int key = e->key();
	const bool keypad = ( e->modifiers() & Qt::KeypadModifier ) != 0;

	if ( keypad )
	{
		switch ( key )
		{
			case Qt::Key_0: return SK_KP0;   case Qt::Key_1: return SK_KP1;
			case Qt::Key_2: return SK_KP2;   case Qt::Key_3: return SK_KP3;
			case Qt::Key_4: return SK_KP4;   case Qt::Key_5: return SK_KP5;
			case Qt::Key_6: return SK_KP6;   case Qt::Key_7: return SK_KP7;
			case Qt::Key_8: return SK_KP8;   case Qt::Key_9: return SK_KP9;
			case Qt::Key_Asterisk: return SK_KP_MULTIPLY;
			case Qt::Key_Plus:     return SK_KP_PLUS;
			case Qt::Key_Minus:    return SK_KP_MINUS;
			case Qt::Key_Period:
			case Qt::Key_Comma:    return SK_KP_PERIOD;
			case Qt::Key_Slash:    return SK_KP_DIVIDE;
			case Qt::Key_Enter:    return SK_KP_ENTER;
			default: break;
		}
	}

	if ( key >= Qt::Key_A && key <= Qt::Key_Z )
	{
		static const short letters[26] =
		{
			SK_A, SK_B, SK_C, SK_D, SK_E, SK_F, SK_G, SK_H, SK_I, SK_J, SK_K, SK_L, SK_M,
			SK_N, SK_O, SK_P, SK_Q, SK_R, SK_S, SK_T, SK_U, SK_V, SK_W, SK_X, SK_Y, SK_Z
		};
		return letters[ key - Qt::Key_A ];
	}

	switch ( key )
	{
		case Qt::Key_0: return SK_0;  case Qt::Key_1: return SK_1;
		case Qt::Key_2: return SK_2;  case Qt::Key_3: return SK_3;
		case Qt::Key_4: return SK_4;  case Qt::Key_5: return SK_5;
		case Qt::Key_6: return SK_6;  case Qt::Key_7: return SK_7;
		case Qt::Key_8: return SK_8;  case Qt::Key_9: return SK_9;
		case Qt::Key_F1: return SK_F1;   case Qt::Key_F2: return SK_F2;
		case Qt::Key_F3: return SK_F3;   case Qt::Key_F4: return SK_F4;
		case Qt::Key_F5: return SK_F5;   case Qt::Key_F6: return SK_F6;
		case Qt::Key_F7: return SK_F7;   case Qt::Key_F8: return SK_F8;
		case Qt::Key_F9: return SK_F9;   case Qt::Key_F10: return SK_F10;
		case Qt::Key_F11: return SK_F11; case Qt::Key_F12: return SK_F12;
		case Qt::Key_Escape:    return SK_ESC;
		case Qt::Key_Tab:
		case Qt::Key_Backtab:   return SK_TAB;
		case Qt::Key_Backspace: return SK_BACKSPACE;
		case Qt::Key_Return:    return SK_ENTER;
		case Qt::Key_Enter:     return SK_KP_ENTER;
		case Qt::Key_Space:     return SK_SPACE;
		case Qt::Key_Insert:    return SK_INSERT;
		case Qt::Key_Delete:    return SK_DELETE;
		case Qt::Key_Home:      return SK_HOME;
		case Qt::Key_End:       return SK_END;
		case Qt::Key_PageUp:    return SK_PAGEUP;
		case Qt::Key_PageDown:  return SK_PAGEDOWN;
		case Qt::Key_Left:      return SK_LEFT;
		case Qt::Key_Right:     return SK_RIGHT;
		case Qt::Key_Up:        return SK_UP;
		case Qt::Key_Down:      return SK_DOWN;
		case Qt::Key_Shift:     return SK_LSHIFT;
		/* Qt swaps Control and Meta on macOS: Key_Meta is the Control key
		   there, Key_Control the Command key (no Saturn equivalent). */
		case Qt::Key_Control:   return mac ? SK_NONE : SK_LCTRL;
		case Qt::Key_Meta:      return mac ? SK_LCTRL : SK_NONE;
		case Qt::Key_Alt:       return SK_LALT;
		case Qt::Key_AltGr:     return SK_RALT;
		case Qt::Key_CapsLock:  return SK_CAPSLOCK;
		case Qt::Key_NumLock:   return SK_NUMLOCK;
		case Qt::Key_ScrollLock:return SK_SCROLLLOCK;
		case Qt::Key_Pause:     return SK_PAUSE;
		case Qt::Key_Print:
		case Qt::Key_SysReq:    return SK_PRINT;
		case Qt::Key_Minus:     return SK_MINUS;
		case Qt::Key_Equal:     return SK_EQUAL;
		case Qt::Key_BracketLeft:  return SK_LBRACKET;
		case Qt::Key_BracketRight: return SK_RBRACKET;
		case Qt::Key_Backslash: return SK_BACKSLASH;
		case Qt::Key_Semicolon: return SK_SEMICOLON;
		case Qt::Key_Apostrophe:return SK_APOSTROPHE;
		case Qt::Key_QuoteLeft: return SK_GRAVE;
		case Qt::Key_Comma:     return SK_COMMA;
		case Qt::Key_Period:    return SK_PERIOD;
		case Qt::Key_Slash:     return SK_SLASH;
		case Qt::Key_Less:
		case Qt::Key_Greater:   return SK_ISO_102ND;
		case Qt::Key_yen:       return SK_YEN;
		default:                return SK_NONE;
	}
}

enum Platform { PLATFORM_EVDEV, PLATFORM_WINDOWS, PLATFORM_MAC };

static Platform platform()
{
	static int p = -1;
	if ( p < 0 )
	{
		const QString name = QGuiApplication::platformName();
		if ( name == QLatin1String( "windows" ) )
			p = PLATFORM_WINDOWS;
		else if ( name == QLatin1String( "cocoa" ) )
			p = PLATFORM_MAC;
		else
			p = PLATFORM_EVDEV; /* xcb, wayland, eglfs, linuxfb... */
	}
	return (Platform)p;
}

/* Identifies a physical host key, so its release goes to the Saturn code
   given at press time even if the Qt key changed in between (modifiers). */
static quint32 hostKeyId( const QKeyEvent* e )
{
	if ( platform() == PLATFORM_MAC )
	{
		if ( isModifierKey( e->key() ) )
			return 0x80000000u | (quint32)e->key();
		return 0x40000000u | e->nativeVirtualKey();
	}
	if ( e->nativeScanCode() != 0 )
		return e->nativeScanCode();
	return 0x80000000u | (quint32)e->key();
}

static int translate( const QKeyEvent* e )
{
	int code = SK_NONE;

	switch ( platform() )
	{
		case PLATFORM_WINDOWS:
			if ( e->nativeScanCode() != 0 )
				code = translateWindows( e->nativeScanCode() );
			break;
		case PLATFORM_MAC:
			/* Modifier changes don't carry a usable virtual key (and
			   kVK_ANSI_A is 0), so they go through the Qt key. */
			if ( !isModifierKey( e->key() ) )
				code = translateMac( e->nativeVirtualKey() );
			break;
		case PLATFORM_EVDEV:
			if ( e->nativeScanCode() >= 8 )
				code = translateEvdev( e->nativeScanCode() - 8 );
			break;
	}

	if ( code == SK_NONE )
		code = translateQtKey( e, platform() == PLATFORM_MAC );
	return code;
}

/* Host keys currently down -> Saturn code sent for them. */
static QHash<quint32, int> sPressed;

namespace KeyboardQt
{
	void keyPress( QKeyEvent* e )
	{
		if ( !PerKeyboardIsConnected() || e->isAutoRepeat() )
			return; /* the emulated keyboard makes its own repeat */

		const quint32 id = hostKeyId( e );
		if ( sPressed.contains( id ) )
			return; /* same event seen twice (event filter + widget) */

		const int code = translate( e );
		if ( code == SK_NONE )
			return;

		/* macOS only reports Caps Lock when the lock state changes (down
		   = locked, up = unlocked): turn each report into a full key
		   stroke, so the emulated CapsLock toggles every time. */
		if ( platform() == PLATFORM_MAC && e->key() == Qt::Key_CapsLock )
		{
			PerKeyboardHostKeyDown( (u8)code );
			PerKeyboardHostKeyUp( (u8)code );
			return;
		}

		sPressed.insert( id, code );
		PerKeyboardHostKeyDown( (u8)code );
	}

	void keyRelease( QKeyEvent* e )
	{
		if ( e->isAutoRepeat() )
			return;

		if ( platform() == PLATFORM_MAC && e->key() == Qt::Key_CapsLock )
		{
			if ( !PerKeyboardIsConnected() )
				return;
			const int code = translate( e );
			if ( code != SK_NONE )
			{
				PerKeyboardHostKeyDown( (u8)code );
				PerKeyboardHostKeyUp( (u8)code );
			}
			return;
		}

		const quint32 id = hostKeyId( e );
		QHash<quint32, int>::iterator it = sPressed.find( id );
		if ( it == sPressed.end() )
			return;

		const int code = it.value();
		sPressed.erase( it );

		/* Two host keys can give the same code (e.g. through the Qt key
		   fallback): release it only when the last one goes up. */
		foreach ( int other, sPressed )
		{
			if ( other == code )
				return;
		}
		PerKeyboardHostKeyUp( (u8)code );
	}

	void releaseAll()
	{
		sPressed.clear();
		PerKeyboardHostReleaseAll();
	}
}
