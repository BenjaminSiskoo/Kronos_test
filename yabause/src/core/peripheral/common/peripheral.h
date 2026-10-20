/*  Copyright 2005 Guillaume Duhamel
    Copyright 2005-2006, 2013 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef PERIPHERAL_H
#define PERIPHERAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "core.h"
#include "yui.h"
#include "stv.h"
#include "smpc.h"
#include "yabause.h"

/** @defgroup peripheral Peripheral
 *
 * @brief This module provides two kind of functions
 * - peripheral core management functions
 * - controller ports management functions
 *
 * @{
 */

#define PERPAD				0x02
#define PERWHEEL			0x13
#define PERMISSIONSTICK	0x15
#define PER3DPAD			0x16
#define PERTWINSTICKS	0x19//double mission sticks use same id
#define PERGUN				0x25
#define PERKEYBOARD		0x34
#define PERMOUSE			0xE3
#define PERCABINET	                0xFF

#define PERCORE_DEFAULT -1
#define PERCORE_DUMMY 0

extern PortData_struct PORTDATA1;
extern PortData_struct PORTDATA2;

#define PERSF_ALL       (0xFFFFFFFF)
#define PERSF_KEY       (1 << 0)
#define PERSF_BUTTON    (1 << 1)
#define PERSF_HAT       (1 << 2)
#define PERSF_AXIS      (1 << 3)
#define PERSF_MOUSEMOVE (1 << 4)

typedef struct
{
   int id;
   const char * Name;
   int (*Init)(void);
   void (*DeInit)(void);
   int (*HandleEvents)(void);
   u32 (*Scan)(u32 flags);
   int canScan;
   void (*Flush)(void);
   void (*KeyName)(u32 key, char * name, int size);
   void (*onKeyEvent)(u32 key, u8 state);
} PerInterface_struct;

/** @brief Pointer to the current peripheral core.
 *
 * You should not set this manually but use
 * PerInit() and PerDeInit() instead. */
extern PerInterface_struct * PERCore;

extern PerInterface_struct PERDummy;

extern u8 FASTCALL IOPortReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr);
extern u16 FASTCALL IOPortReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr);
extern void FASTCALL IOPortWriteByte(SH2_struct *context, UNUSED u8* memory,UNUSED u32 addr, UNUSED u8 val);
/* Word/long access to the IOGA: the chip sits on the odd byte lanes of the
   32-bit bus (MAME umask32(0x00ff00ff)), so a word reaches its low byte and
   a long reaches the low byte of each of its two words. */
extern u32 FASTCALL IOPortReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr);
extern void FASTCALL IOPortWriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val);
extern void FASTCALL IOPortWriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val);

/**
 * @brief Critter Crusher / Tatacot (critcrsh, tatacot) hammer cabinet.
 *
 * The hammer position (6 bits per axis) is read on PORT-A (X) and PORT-B (Y)
 * of the I/O gate array, the hit on PORT-C bit 4 (see IOPortReadByte()).
 * PerCritcrshMove() takes a position in emulated screen pixels (the host
 * mouse), PerCritcrshHammer() the host mouse button.
 */
int PerCritcrshEnabled(void);
void PerCritcrshMove(s32 x, s32 y);
void PerCritcrshHammer(int pressed);

/**
 * @brief Advance the medal/ticket hopper simulation by one frame.
 *
 * Games using the PATOCAR/MICROMBC extension I/O layout (patocar,
 * skychal, supgoal, techbowl, micrombc) drive a hopper motor through
 * PORT-D bit 0x80 and read a sensor pulse back on PORT-A bit 0x02.
 * Call this once per emulated frame (e.g. on VBlankIN) so the sensor
 * line keeps toggling for as long as the motor is running, mirroring
 * real hopper behaviour. No-op for games that don't use a hopper.
 */
void PerHopperExec(void);
u32 PerGetCoinCounter(int index); /* ST-V PORT-D coin counter 0/1 (bookkeeping) */
void PerSetCoinCounters(u32 coin1, u32 coin2); /* restore saved bookkeeping */
int PerGetCoinLockout(int index); /* ST-V PORT-D coin lockout 0/1 active */
void PerIogaSaveState(void ** stream); /* ST-V IOGA registers, PORT-G counters, hopper (CART chunk) */
int PerIogaLoadState(const void * stream, int size); /* 0 = applied */

/**
 * @brief Init a peripheral core
 *
 * Searches through the PERCoreList array for the given coreid.
 * If found, PERCore is set to the address of that core and
 * the core's Init function is called.
 *
 * @param coreid the peripheral core to be used
 * @return 0 if core has been inited, -1 otherwise
 */
int PerInit(int coreid);
/**
 * @brief De-init a peripheral core
 *
 * Calls the core's DeInit callback and set PERCore to NULL.
 */
void PerDeInit(void);

/** @brief Adds a peripheral
 *
 * You shouldn't directly use this function but
 * PerPadAdd() or PerMouseAdd() instead.
 */
void * PerAddPeripheral(PortData_struct *port, int perid);
int PerGetId(void * peripheral);
void PerPortReset(void);
/**
 * Iterate the list of peripherals connected to a port
 * and flush them if necesseray. This is needed for mouses.
 */
void PerFlush(PortData_struct * port);

void PerKeyDown(u32 key);
void PerKeyUp(u32 key);
void PerSetKey(u32 key, u8 name, void * controller);
void PerAxisValue(u32 key, u8 val);
void PerAxisMove(u32 key, s32 dispx, s32 dispy);

/** @defgroup pad Pad
 *
 * @{
 */
#define PERPAD_UP	0
#define PERPAD_RIGHT	1
#define PERPAD_DOWN	2
#define PERPAD_LEFT	3
#define PERPAD_RIGHT_TRIGGER 4
#define PERPAD_LEFT_TRIGGER 5
#define PERPAD_START	6
#define PERPAD_A	7
#define PERPAD_B	8
#define PERPAD_C	9
#define PERPAD_X	10
#define PERPAD_Y	11
#define PERPAD_Z	12

extern const char * PerPadNames[14];

typedef struct
{
   u8 perid;
   u8 padbits[2];
} PerPad_struct;

typedef enum {
	PORT_A = 0,
	PORT_B,
	PORT_C,
	PORT_D,
	PORT_E,
	PORT_F,
	PORT_G,
	PORT_G0,
	PORT_G1,
	PORT_G2,
	PORT_G3,
	ioPortMAX
} ioPort;


int IOPortAdd(int key, ioPort port, u8 index);

/** @brief Adds a pad to one of the controller ports.
 *
 * @param port can be either &PORTDATA1 or &PORTDATA2
 * @return pointer to a PerPad_struct or NULL if it fails
 * */
PerPad_struct * PerPadAdd(PortData_struct * port);

void PerPadUpPressed(PerPad_struct * pad);
void PerPadUpReleased(PerPad_struct * pad);

void PerPadDownPressed(PerPad_struct * pad);
void PerPadDownReleased(PerPad_struct * pad);

void PerPadRightPressed(PerPad_struct * pad);
void PerPadRightReleased(PerPad_struct * pad);

void PerPadLeftPressed(PerPad_struct * pad);
void PerPadLeftReleased(PerPad_struct * pad);

void PerPadStartPressed(PerPad_struct * pad);
void PerPadStartReleased(PerPad_struct * pad);

void PerPadAPressed(PerPad_struct * pad);
void PerPadAReleased(PerPad_struct * pad);

void PerPadBPressed(PerPad_struct * pad);
void PerPadBReleased(PerPad_struct * pad);

void PerPadCPressed(PerPad_struct * pad);
void PerPadCReleased(PerPad_struct * pad);

void PerPadXPressed(PerPad_struct * pad);
void PerPadXReleased(PerPad_struct * pad);

void PerPadYPressed(PerPad_struct * pad);
void PerPadYReleased(PerPad_struct * pad);

void PerPadZPressed(PerPad_struct * pad);
void PerPadZReleased(PerPad_struct * pad);

void PerPadRTriggerPressed(PerPad_struct * pad);
void PerPadRTriggerReleased(PerPad_struct * pad);

void PerPadLTriggerPressed(PerPad_struct * pad);
void PerPadLTriggerReleased(PerPad_struct * pad);

void PerCabAP2Pressed(PerPad_struct * pad);
void PerCabAP2Released(PerPad_struct * pad);

void PerCabBP2Pressed(PerPad_struct * pad);
void PerCabBP2Released(PerPad_struct * pad);

void PerCabCP2Pressed(PerPad_struct * pad);
void PerCabCP2Released(PerPad_struct * pad);

void PerCabXP2Pressed(PerPad_struct * pad);
void PerCabXP2Released(PerPad_struct * pad);

void PerCabUPP2Pressed(PerPad_struct * pad);
void PerCabUPP2Released(PerPad_struct * pad);

void PerCabRIGHTP2Pressed(PerPad_struct * pad);
void PerCabRIGHTP2Released(PerPad_struct * pad);

void PerCabDOWNP2Pressed(PerPad_struct * pad);
void PerCabDOWNP2Released(PerPad_struct * pad);

void PerCabLEFTP2Pressed(PerPad_struct * pad);
void PerCabLEFTP2Released(PerPad_struct * pad);


/** @} */

/** @defgroup mouse Mouse
 *
 * @{
 * */
#define PERMOUSE_LEFT	13
#define PERMOUSE_MIDDLE	14
#define PERMOUSE_RIGHT	15
#define PERMOUSE_START	16
#define PERMOUSE_AXIS	17

extern const char * PerMouseNames[5];

typedef struct
{
   u8 perid;
   u8 mousebits[3];
} PerMouse_struct;

/** @brief Adds a mouse to one of the controller ports.
 *
 * @param port can be either &PORTDATA1 or &PORTDATA2
 * @return pointer to a PerMouse_struct or NULL if it fails
 * */
PerMouse_struct * PerMouseAdd(PortData_struct * port);

void PerMouseLeftPressed(PerMouse_struct * mouse);
void PerMouseLeftReleased(PerMouse_struct * mouse);

void PerMouseMiddlePressed(PerMouse_struct * mouse);
void PerMouseMiddleReleased(PerMouse_struct * mouse);

void PerMouseRightPressed(PerMouse_struct * mouse);
void PerMouseRightReleased(PerMouse_struct * mouse);

void PerMouseStartPressed(PerMouse_struct * mouse);
void PerMouseStartReleased(PerMouse_struct * mouse);

void PerMouseMove(PerMouse_struct * mouse, s32 dispx, s32 dispy);
/** @} */

/** @defgroup 3danalog 3DAnalog
 *
 * @{
 * */

#define PERANALOG_AXIS1	18
#define PERANALOG_AXIS2	19
#define PERANALOG_AXIS3	20
#define PERANALOG_AXIS4	21
#define PERANALOG_AXIS5	22
#define PERANALOG_AXIS6	23
#define PERANALOG_AXIS7	24

typedef struct
{
   u8 perid;
   u8 analogbits[9];
} PerAnalog_struct;

/** @brief Adds a Analog control pad to one of the controller ports.
 *
 * @param port can be either &PORTDATA1 or &PORTDATA2
 * @return pointer to a Per3DAnalog_struct or NULL if it fails
 * */

PerAnalog_struct * PerWheelAdd(PortData_struct * port);
PerAnalog_struct * PerMissionStickAdd(PortData_struct * port);
PerAnalog_struct * Per3DPadAdd(PortData_struct * port);
PerAnalog_struct * PerTwinSticksAdd(PortData_struct * port);

void PerAxis1Value(PerAnalog_struct * analog, u32 val);
void PerAxis2Value(PerAnalog_struct * analog, u32 val);
void PerAxis3Value(PerAnalog_struct * analog, u32 val);
void PerAxis4Value(PerAnalog_struct * analog, u32 val);
void PerAxis5Value(PerAnalog_struct * analog, u32 val);
void PerAxis6Value(PerAnalog_struct * analog, u32 val);
void PerAxis7Value(PerAnalog_struct * analog, u32 val);

/** @} */

/** @defgroup gun Gun
 *
 * @{
 * */

#define PERGUN_TRIGGER	25
#define PERGUN_START		27
#define PERGUN_AXIS		28

typedef struct
{
   u8 perid;
   u8 gunbits[5];
} PerGun_struct;

/** @brief Adds a Gun to one of the controller ports up to a maximum of one per port.
 *
 * @param port can be either &PORTDATA1 or &PORTDATA2
 * @return pointer to a PerGun_struct or NULL if it fails
 * */

PerGun_struct * PerGunAdd(PortData_struct * port);

void PerGunTriggerPressed(PerGun_struct * gun);
void PerGunTriggerReleased(PerGun_struct * gun);

void PerGunStartPressed(PerGun_struct * gun);
void PerGunStartReleased(PerGun_struct * gun);

void PerGunMove(PerGun_struct * gun, s32 dispx, s32 dispy);

/* Keyboard
 *
 * Sega Saturn keyboard (peripheral ID 34h), data format from
 * Saturn Technical Bulletin #45 "Saturn Keyboard Data Format Ver.1.00":
 *
 *   1st data : Right Left Down Up | Start A C B        (0 = pressed)
 *   2nd data : R X Y Z | L KBTYPE2 KBTYPE1 KBTYPE0     (KBTYPE 000 = Saturn keyboard)
 *   3rd data : 0 CapsLock NumLock ScrLock | Make 1 1 Break
 *   4th data : key code (valid only when Make or Break is 1)
 *
 * Unlike the other peripherals, the keyboard is not driven by per-button
 * bindings: the port feeds the state of the host keys directly, already
 * translated to Saturn key codes, with PerKeyboardHostKeyDown()/Up().
 * The core turns that state into the Make/Break code stream (one code
 * per INTBACK read, key repeat on the last key pressed) and derives the
 * pad-compatible buttons and the lock flags from it.
 */

#define PERKB_NUM_CODES 0x90 /* key codes 00h-8Fh (STTECH45 "Meaning of Key Codes") */

typedef struct
{
   u8 perid;
   u8 kbbits[4];
} PerKeyboard_struct;

/** @brief Adds a keyboard to one of the controller ports.
 *
 * @param port can be either &PORTDATA1 or &PORTDATA2
 * @return pointer to a PerKeyboard_struct or NULL if it fails
 * */
PerKeyboard_struct * PerKeyboardAdd(PortData_struct * port);

/** @brief Host side: a key translated to Saturn key code @p code went down / up.
 *
 * Safe to call repeatedly with the same state (duplicates are ignored).
 * Ignored when @p code is not a valid key code. */
void PerKeyboardHostKeyDown(u8 code);
void PerKeyboardHostKeyUp(u8 code);
/** @brief Host side: releases every key (window lost the focus...). */
void PerKeyboardHostReleaseAll(void);
/** @brief Returns non-zero when at least one keyboard is plugged in. */
int PerKeyboardIsConnected(void);

/** @brief Refreshes the keyboards connected to @p port.
 *
 * Called by the SMPC right before it takes a new copy of the port data
 * for an INTBACK: emits the next Make/Break code (if any) into the
 * keyboard data. */
void PerKeyboardUpdate(PortData_struct * port);


#define PERJAMMA_COIN1 29
#define PERJAMMA_COIN2 30
#define PERJAMMA_TEST 31
#define PERJAMMA_SERVICE 32
#define PERJAMMA_START1 33
#define PERJAMMA_START2 34
#define PERJAMMA_MULTICART 35
#define PERJAMMA_PAUSE 36
#define PERJAMMA_P2_UP 37
#define PERJAMMA_P2_RIGHT 38
#define PERJAMMA_P2_DOWN 39
#define PERJAMMA_P2_LEFT 40
#define PERJAMMA_P2_BUTTON1 41
#define PERJAMMA_P2_BUTTON2 42
#define PERJAMMA_P2_BUTTON3 43
#define PERJAMMA_P2_BUTTON4 44
#define PERJAMMA_P2_BUTTON5 45
#define PERJAMMA_P2_BUTTON6 46

/* PATOCAR/MICROMBC hopper cabinets (patocar, skychal, supgoal, techbowl,
   micrombc): controls specific to their extension I/O layout. See the
   comment above PerHopperExec()'s state block in peripheral.c and MAME's
   sega/stv.cpp "patocar"/"micrombc" input ports for the reference wiring. */
#define PERJAMMA_POWER_BUTTON 47 /* PORT-C bit 0x20 (patocar family) / PORT-A bit 0x20 (micrombc) */
#define PERJAMMA_MEDAL        48 /* PORT-B bit 0x20, Coin3 - medal insert slot */
#define PERJAMMA_HOPPER_BTN2  49 /* PORT-A bit 0x01, active-high, MAME labels this "hopper ?" */
#define PERJAMMA_DOOR_SWITCH  50 /* PORT-B bit 0x02, active-high */
#define PERJAMMA_MAGNET_BTN5  51 /* PORT-B bits 0x0c, "Button 5" on patocar family / "Magnet Sensor" on micrombc */
#define PERJAMMA_HOPPER_TEST  52 /* Not a real cabinet input: hold to force the hopper sensor line active, mirrors MAME's hopper "Sensor Test" service input */

/* patocar/skychal/supgoal/techbowl only (NOT micrombc, which explicitly
   has no trackball - see MAME's micrombc PORTG.0/.1 override). Digital
   nudge fallback for the analog trackball (PORT-G counter registers 0/1),
   mirroring MAME's own PORT_KEYDELTA(100) digital-input fallback for the
   same control. See PerTrackballExec() in peripheral.c. */
#define PERJAMMA_TRACKBALL_UP    53
#define PERJAMMA_TRACKBALL_DOWN  54
#define PERJAMMA_TRACKBALL_LEFT  55
#define PERJAMMA_TRACKBALL_RIGHT 56

/* Sega Mahjong Panel (used by "Pro Mahjong Kiwame S" / kiwames [2P],
   "Virtual Mahjong" / vmahjong [1P] and "Virtual Mahjong 2 - My Fair
   Lady" / myfairld [1P]). Unlike the JAMMA cabinet controls above, this
   is a 5x8-bit row-scanned key matrix: the game selects one or more of
   5 rows via a mux register on PORT-E, then reads the ANDed row byte(s)
   back through the normal P1 (PORT-A) / P2 (PORT-B) locations while
   panel mode is active. See PerMahjongPanelEnabled(), IOPortReadByte()
   and IOPortWriteByte() in peripheral.c, which mirror MAME's sega/stv.cpp
   stvmp_ioga_r()/stvmp_ioga_w() and the INPUT_PORTS_START(stvmp)/
   (vmahjong)/(myfairld) tables bit-for-bit:
     KEY0: Kan, Start, -, -, E, A, M, I
     KEY1: Reach, Bet, -, -, F, B, N, J
     KEY2: Ron,  -,   -, -, G, C, Chi, K
     KEY3: -,    -,   -, -, H, D, Pon, L
     KEY4: -,    -,   -, -, -, -, FlipFlop, -
   Bet is only wired on the single-player panel (vmahjong/myfairld); Kan/
   Reach/Ron/Chi/Pon/FlipFlop/Start are wired for both P1 and P2 on the
   two-player panel (kiwames). A row/bit combination the loaded game
   doesn't actually wire is simply never read by its code, so binding it
   is harmless either way. */
/* Player 1 side */
#define PERMAHJONG_KAN          57
#define PERMAHJONG_START        58
#define PERMAHJONG_E            59
#define PERMAHJONG_A            60
#define PERMAHJONG_M            61
#define PERMAHJONG_I            62
#define PERMAHJONG_REACH        63
#define PERMAHJONG_BET          64
#define PERMAHJONG_F            65
#define PERMAHJONG_B            66
#define PERMAHJONG_N            67
#define PERMAHJONG_J            68
#define PERMAHJONG_RON          69
#define PERMAHJONG_G            70
#define PERMAHJONG_C            71
#define PERMAHJONG_CHI          72
#define PERMAHJONG_K            73
#define PERMAHJONG_H            74
#define PERMAHJONG_D            75
#define PERMAHJONG_PON          76
#define PERMAHJONG_L            77
#define PERMAHJONG_FLIP_FLOP    78
/* Player 2 side */
#define PERMAHJONG_P2_KAN       79
#define PERMAHJONG_P2_START     80
#define PERMAHJONG_P2_E         81
#define PERMAHJONG_P2_A         82
#define PERMAHJONG_P2_M         83
#define PERMAHJONG_P2_I         84
#define PERMAHJONG_P2_REACH     85
#define PERMAHJONG_P2_F         86
#define PERMAHJONG_P2_B         87
#define PERMAHJONG_P2_N         88
#define PERMAHJONG_P2_J         89
#define PERMAHJONG_P2_RON       90
#define PERMAHJONG_P2_G         91
#define PERMAHJONG_P2_C         92
#define PERMAHJONG_P2_CHI       93
#define PERMAHJONG_P2_K         94
#define PERMAHJONG_P2_H         95
#define PERMAHJONG_P2_D         96
#define PERMAHJONG_P2_PON       97
#define PERMAHJONG_P2_L         98
#define PERMAHJONG_P2_FLIP_FLOP 99

// The true numeric bounds of the whole PERMAHJONG_* range, for code that
// needs to tell "is this a Mahjong Panel key?" apart from a JAMMA/pad key
// by range-check (see libretro.c and YabauseThread.cpp) - PERMAHJONG_KAN
// (57), not PERMAHJONG_A (60), is actually the smallest value here, so
// don't use PERMAHJONG_A as the lower bound.
#define PERMAHJONG_FIRST PERMAHJONG_KAN
#define PERMAHJONG_LAST  PERMAHJONG_P2_FLIP_FLOP

typedef u8 PerMahjongPanel_struct;

/** @brief Connects the Sega Mahjong Panel (both P1 and P2 sides share the
 *  single controller this returns, exactly like PerCabAdd()'s cabinet).
 *
 * @param port unused, kept for consistency with PerCabAdd().
 * @return pointer to a PerMahjongPanel_struct.
 * */
PerMahjongPanel_struct * PerMahjongAdd(PortData_struct * port);

void PerMahjongP1KanPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1KanReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1StartPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1StartReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1EPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1EReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1APressed(PerMahjongPanel_struct * mj);
void PerMahjongP1AReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1MPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1MReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1IPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1IReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1ReachPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1ReachReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1BetPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1BetReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1FPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1FReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1BPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1BReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1NPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1NReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1JPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1JReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1RonPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1RonReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1GPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1GReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1CPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1CReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1ChiPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1ChiReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1KPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1KReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1HPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1HReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1DPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1DReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1PonPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1PonReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1LPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1LReleased(PerMahjongPanel_struct * mj);
void PerMahjongP1FlipFlopPressed(PerMahjongPanel_struct * mj);
void PerMahjongP1FlipFlopReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2KanPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2KanReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2StartPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2StartReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2EPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2EReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2APressed(PerMahjongPanel_struct * mj);
void PerMahjongP2AReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2MPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2MReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2IPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2IReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2ReachPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2ReachReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2FPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2FReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2BPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2BReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2NPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2NReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2JPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2JReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2RonPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2RonReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2GPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2GReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2CPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2CReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2ChiPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2ChiReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2KPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2KReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2HPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2HReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2DPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2DReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2PonPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2PonReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2LPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2LReleased(PerMahjongPanel_struct * mj);
void PerMahjongP2FlipFlopPressed(PerMahjongPanel_struct * mj);
void PerMahjongP2FlipFlopReleased(PerMahjongPanel_struct * mj);

typedef u8 PerCab_struct;

PerCab_struct * PerCabAdd(PortData_struct * port);

void PerCabUpPressed(PerCab_struct * pad);
void PerCabUpReleased(PerCab_struct * pad);

void PerCabDownPressed(PerCab_struct * pad);
void PerCabDownReleased(PerCab_struct * pad);

void PerCabRightPressed(PerCab_struct * pad);
void PerCabRightReleased(PerCab_struct * pad);

void PerCabLeftPressed(PerCab_struct * pad);
void PerCabLeftReleased(PerCab_struct * pad);

void PerCabAPressed(PerCab_struct * pad);
void PerCabAReleased(PerCab_struct * pad);

void PerCabBPressed(PerCab_struct * pad);
void PerCabBReleased(PerCab_struct * pad);

void PerCabCPressed(PerCab_struct * pad);
void PerCabCReleased(PerCab_struct * pad);

void PerCabXPressed(PerCab_struct * pad);
void PerCabXReleased(PerCab_struct * pad);

void PerCabYPressed(PerCab_struct * pad);
void PerCabYReleased(PerCab_struct * pad);

void PerCabZPressed(PerCab_struct * pad);
void PerCabZReleased(PerCab_struct * pad);

void PerCabTestPressed(PerCab_struct * pad);
void PerCabTestReleased(PerCab_struct * pad);

void PerCabServicePressed(PerCab_struct * pad);
void PerCabServiceReleased(PerCab_struct * pad);

void PerCabCoin1Pressed(PerCab_struct * pad);
void PerCabCoin1Released(PerCab_struct * pad);

void PerCabCoin2Pressed(PerCab_struct * pad);
void PerCabCoin2Released(PerCab_struct * pad);

void PerCabStart1Pressed(PerCab_struct * pad);
void PerCabStart1Released(PerCab_struct * pad);

void PerCabStart2Pressed(PerCab_struct * pad);
void PerCabStart2Released(PerCab_struct * pad);

void PerCabMultiCartPressed(PerCab_struct * pad);
void PerCabMultiCartReleased(PerCab_struct * pad);

void PerCabPausePressed(PerCab_struct * pad);
void PerCabPauseReleased(PerCab_struct * pad);

void PerCabP2UpPressed(PerCab_struct * pad);
void PerCabP2UpReleased(PerCab_struct * pad);

void PerCabP2RightPressed(PerCab_struct * pad);
void PerCabP2RightReleased(PerCab_struct * pad);

void PerCabP2DownPressed(PerCab_struct * pad);
void PerCabP2DownReleased(PerCab_struct * pad);

void PerCabP2LeftPressed(PerCab_struct * pad);
void PerCabP2LeftReleased(PerCab_struct * pad);

void PerCabP2Button1Pressed(PerCab_struct * pad);
void PerCabP2Button1Released(PerCab_struct * pad);

void PerCabP2Button2Pressed(PerCab_struct * pad);
void PerCabP2Button2Released(PerCab_struct * pad);

void PerCabP2Button3Pressed(PerCab_struct * pad);
void PerCabP2Button3Released(PerCab_struct * pad);

void PerCabP2Button4Pressed(PerCab_struct * pad);
void PerCabP2Button4Released(PerCab_struct * pad);

void PerCabP2Button5Pressed(PerCab_struct * pad);
void PerCabP2Button5Released(PerCab_struct * pad);

void PerCabP2Button6Pressed(PerCab_struct * pad);
void PerCabP2Button6Released(PerCab_struct * pad);

/* PATOCAR/MICROMBC hopper cabinet controls, see PERJAMMA_* defines above. */
void PerCabPowerButtonPressed(PerCab_struct * pad);
void PerCabPowerButtonReleased(PerCab_struct * pad);

void PerCabMedalPressed(PerCab_struct * pad);
void PerCabMedalReleased(PerCab_struct * pad);

void PerCabHopperBtn2Pressed(PerCab_struct * pad);
void PerCabHopperBtn2Released(PerCab_struct * pad);

void PerCabDoorSwitchPressed(PerCab_struct * pad);
void PerCabDoorSwitchReleased(PerCab_struct * pad);

void PerCabMagnetBtn5Pressed(PerCab_struct * pad);
void PerCabMagnetBtn5Released(PerCab_struct * pad);

/* Not a real cabinet input: manual hopper sensor override for testing. */
void PerCabHopperTestPressed(PerCab_struct * pad);
void PerCabHopperTestReleased(PerCab_struct * pad);

/* Digital trackball nudge, patocar/skychal/supgoal/techbowl only. */
void PerCabTrackballUpPressed(PerCab_struct * pad);
void PerCabTrackballUpReleased(PerCab_struct * pad);
void PerCabTrackballDownPressed(PerCab_struct * pad);
void PerCabTrackballDownReleased(PerCab_struct * pad);
void PerCabTrackballLeftPressed(PerCab_struct * pad);
void PerCabTrackballLeftReleased(PerCab_struct * pad);
void PerCabTrackballRightPressed(PerCab_struct * pad);
void PerCabTrackballRightReleased(PerCab_struct * pad);

/**
 * @brief Advance the digital trackball nudge by one frame.
 *
 * Adds/subtracts a fixed step to the PORT-G0 (X) / PORT-G1 (Y) 16-bit
 * counter registers for each direction currently held, wrapping like the
 * real free-running quadrature counter these emulate. No-op unless the
 * loaded game is patocar/skychal/supgoal/techbowl. Call once per frame,
 * alongside PerHopperExec().
 */
void PerTrackballExec(void);

/** @} */

/** @} */

#ifdef __cplusplus
}
#endif

#endif
