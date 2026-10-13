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

/*! \file peripheral.c
    \brief Peripheral shared functions.
*/

#include "debug.h"
#include "peripheral.h"
#include "ygl.h"
#include <string.h>

const char * PerPadNames[] =
{
"Up",
"Right",
"Down",
"Left",
"R",
"L",
"Start",
"A",
"B",
"C",
"X",
"Y",
"Z",
NULL
};

const char * PerMouseNames[] =
{
"A",
"B",
"C",
"Start",
NULL
};

PortData_struct PORTDATA1;
PortData_struct PORTDATA2;
static u8 IOPORT[ioPortMAX];

PerInterface_struct * PERCore = NULL;
extern PerInterface_struct * PERCoreList[];

typedef struct {
	u8 name;
	void (*Press)(void *);
	void (*Release)(void *);
   void (*SetAxisValue)(void *, u32);
   void (*MoveAxis)(void *, s32, s32);
} PerBaseConfig_struct;

typedef struct {
	u32 key;
	PerBaseConfig_struct * base;
	void * controller;
} PerConfig_struct;

typedef struct {
        u32 key;
        u8* port;
        u8 mask;
} PerIO_struct;

static PerIO_struct* IOkeys[256];

static PerIO_struct IOalloc[ioPortMAX][8];

#define PERCB(func) ((void (*) (void *)) func)
#define PERVALCB(func) ((void (*) (void *, u32)) func)
#define PERMOVECB(func) ((void (*) (void *, s32, s32)) func)

PerBaseConfig_struct perpadbaseconfig[] = {
	{ PERPAD_UP, PERCB(PerPadUpPressed), PERCB(PerPadUpReleased), NULL, NULL },
	{ PERPAD_RIGHT, PERCB(PerPadRightPressed), PERCB(PerPadRightReleased), NULL, NULL },
	{ PERPAD_DOWN, PERCB(PerPadDownPressed), PERCB(PerPadDownReleased), NULL, NULL },
	{ PERPAD_LEFT, PERCB(PerPadLeftPressed), PERCB(PerPadLeftReleased), NULL, NULL },
	{ PERPAD_RIGHT_TRIGGER, PERCB(PerPadRTriggerPressed), PERCB(PerPadRTriggerReleased), NULL, NULL },
	{ PERPAD_LEFT_TRIGGER, PERCB(PerPadLTriggerPressed), PERCB(PerPadLTriggerReleased), NULL, NULL },
	{ PERPAD_START, PERCB(PerPadStartPressed), PERCB(PerPadStartReleased), NULL, NULL },
	{ PERPAD_A, PERCB(PerPadAPressed), PERCB(PerPadAReleased), NULL, NULL },
	{ PERPAD_B, PERCB(PerPadBPressed), PERCB(PerPadBReleased), NULL, NULL },
	{ PERPAD_C, PERCB(PerPadCPressed), PERCB(PerPadCReleased), NULL, NULL },
	{ PERPAD_X, PERCB(PerPadXPressed), PERCB(PerPadXReleased), NULL, NULL },
	{ PERPAD_Y, PERCB(PerPadYPressed), PERCB(PerPadYReleased), NULL, NULL },
	{ PERPAD_Z, PERCB(PerPadZPressed), PERCB(PerPadZReleased), NULL, NULL },
};

PerBaseConfig_struct percabinetbaseconfig[] = {
	{ PERPAD_UP, PERCB(PerCabUpPressed), PERCB(PerCabUpReleased), NULL, NULL },
	{ PERPAD_RIGHT, PERCB(PerCabRightPressed), PERCB(PerCabRightReleased), NULL, NULL },
	{ PERPAD_DOWN, PERCB(PerCabDownPressed), PERCB(PerCabDownReleased), NULL, NULL },
	{ PERPAD_LEFT, PERCB(PerCabLeftPressed), PERCB(PerCabLeftReleased), NULL, NULL },
	{ PERJAMMA_TEST, PERCB(PerCabTestPressed), PERCB(PerCabTestReleased), NULL, NULL },
	{ PERJAMMA_SERVICE, PERCB(PerCabServicePressed), PERCB(PerCabServiceReleased), NULL, NULL },
	{ PERJAMMA_START1, PERCB(PerCabStart1Pressed), PERCB(PerCabStart1Released), NULL, NULL },
	{ PERJAMMA_START2, PERCB(PerCabStart2Pressed), PERCB(PerCabStart2Released), NULL, NULL },
	{ PERJAMMA_COIN1, PERCB(PerCabCoin1Pressed), PERCB(PerCabCoin1Released), NULL, NULL },
	{ PERJAMMA_COIN2, PERCB(PerCabCoin2Pressed), PERCB(PerCabCoin2Released), NULL, NULL },
	{ PERJAMMA_MULTICART, PERCB(PerCabMultiCartPressed), PERCB(PerCabMultiCartReleased), NULL, NULL },
	{ PERJAMMA_PAUSE, PERCB(PerCabPausePressed), PERCB(PerCabPauseReleased), NULL, NULL },
	{ PERPAD_A, PERCB(PerCabAPressed), PERCB(PerCabAReleased), NULL, NULL },
	{ PERPAD_B, PERCB(PerCabBPressed), PERCB(PerCabBReleased), NULL, NULL },
	{ PERPAD_C, PERCB(PerCabCPressed), PERCB(PerCabCReleased), NULL, NULL },
	{ PERPAD_X, PERCB(PerCabXPressed), PERCB(PerCabXReleased), NULL, NULL },
	{ PERPAD_Y, PERCB(PerCabYPressed), PERCB(PerCabYReleased), NULL, NULL },
	{ PERPAD_Z, PERCB(PerCabZPressed), PERCB(PerCabZReleased), NULL, NULL },
	{ PERJAMMA_P2_UP, PERCB(PerCabP2UpPressed), PERCB(PerCabP2UpReleased), NULL, NULL },
	{ PERJAMMA_P2_RIGHT, PERCB(PerCabP2RightPressed), PERCB(PerCabP2RightReleased), NULL, NULL },
	{ PERJAMMA_P2_DOWN, PERCB(PerCabP2DownPressed), PERCB(PerCabP2DownReleased), NULL, NULL },
	{ PERJAMMA_P2_LEFT, PERCB(PerCabP2LeftPressed), PERCB(PerCabP2LeftReleased), NULL, NULL },
	{ PERJAMMA_P2_BUTTON1, PERCB(PerCabP2Button1Pressed), PERCB(PerCabP2Button1Released), NULL, NULL },
	{ PERJAMMA_P2_BUTTON2, PERCB(PerCabP2Button2Pressed), PERCB(PerCabP2Button2Released), NULL, NULL },
	{ PERJAMMA_P2_BUTTON3, PERCB(PerCabP2Button3Pressed), PERCB(PerCabP2Button3Released), NULL, NULL },
	{ PERJAMMA_P2_BUTTON4, PERCB(PerCabP2Button4Pressed), PERCB(PerCabP2Button4Released), NULL, NULL },
	{ PERJAMMA_P2_BUTTON5, PERCB(PerCabP2Button5Pressed), PERCB(PerCabP2Button5Released), NULL, NULL },
	{ PERJAMMA_P2_BUTTON6, PERCB(PerCabP2Button6Pressed), PERCB(PerCabP2Button6Released), NULL, NULL },
	{ PERJAMMA_POWER_BUTTON, PERCB(PerCabPowerButtonPressed), PERCB(PerCabPowerButtonReleased), NULL, NULL },
	{ PERJAMMA_MEDAL, PERCB(PerCabMedalPressed), PERCB(PerCabMedalReleased), NULL, NULL },
	{ PERJAMMA_HOPPER_BTN2, PERCB(PerCabHopperBtn2Pressed), PERCB(PerCabHopperBtn2Released), NULL, NULL },
	{ PERJAMMA_DOOR_SWITCH, PERCB(PerCabDoorSwitchPressed), PERCB(PerCabDoorSwitchReleased), NULL, NULL },
	{ PERJAMMA_MAGNET_BTN5, PERCB(PerCabMagnetBtn5Pressed), PERCB(PerCabMagnetBtn5Released), NULL, NULL },
	{ PERJAMMA_HOPPER_TEST, PERCB(PerCabHopperTestPressed), PERCB(PerCabHopperTestReleased), NULL, NULL },
	{ PERJAMMA_TRACKBALL_UP, PERCB(PerCabTrackballUpPressed), PERCB(PerCabTrackballUpReleased), NULL, NULL },
	{ PERJAMMA_TRACKBALL_DOWN, PERCB(PerCabTrackballDownPressed), PERCB(PerCabTrackballDownReleased), NULL, NULL },
	{ PERJAMMA_TRACKBALL_LEFT, PERCB(PerCabTrackballLeftPressed), PERCB(PerCabTrackballLeftReleased), NULL, NULL },
	{ PERJAMMA_TRACKBALL_RIGHT, PERCB(PerCabTrackballRightPressed), PERCB(PerCabTrackballRightReleased), NULL, NULL },
};

PerBaseConfig_struct permahjongbaseconfig[] = {
	{ PERMAHJONG_KAN, PERCB(PerMahjongP1KanPressed), PERCB(PerMahjongP1KanReleased), NULL, NULL },
	{ PERMAHJONG_START, PERCB(PerMahjongP1StartPressed), PERCB(PerMahjongP1StartReleased), NULL, NULL },
	{ PERMAHJONG_E, PERCB(PerMahjongP1EPressed), PERCB(PerMahjongP1EReleased), NULL, NULL },
	{ PERMAHJONG_A, PERCB(PerMahjongP1APressed), PERCB(PerMahjongP1AReleased), NULL, NULL },
	{ PERMAHJONG_M, PERCB(PerMahjongP1MPressed), PERCB(PerMahjongP1MReleased), NULL, NULL },
	{ PERMAHJONG_I, PERCB(PerMahjongP1IPressed), PERCB(PerMahjongP1IReleased), NULL, NULL },
	{ PERMAHJONG_REACH, PERCB(PerMahjongP1ReachPressed), PERCB(PerMahjongP1ReachReleased), NULL, NULL },
	{ PERMAHJONG_BET, PERCB(PerMahjongP1BetPressed), PERCB(PerMahjongP1BetReleased), NULL, NULL },
	{ PERMAHJONG_F, PERCB(PerMahjongP1FPressed), PERCB(PerMahjongP1FReleased), NULL, NULL },
	{ PERMAHJONG_B, PERCB(PerMahjongP1BPressed), PERCB(PerMahjongP1BReleased), NULL, NULL },
	{ PERMAHJONG_N, PERCB(PerMahjongP1NPressed), PERCB(PerMahjongP1NReleased), NULL, NULL },
	{ PERMAHJONG_J, PERCB(PerMahjongP1JPressed), PERCB(PerMahjongP1JReleased), NULL, NULL },
	{ PERMAHJONG_RON, PERCB(PerMahjongP1RonPressed), PERCB(PerMahjongP1RonReleased), NULL, NULL },
	{ PERMAHJONG_G, PERCB(PerMahjongP1GPressed), PERCB(PerMahjongP1GReleased), NULL, NULL },
	{ PERMAHJONG_C, PERCB(PerMahjongP1CPressed), PERCB(PerMahjongP1CReleased), NULL, NULL },
	{ PERMAHJONG_CHI, PERCB(PerMahjongP1ChiPressed), PERCB(PerMahjongP1ChiReleased), NULL, NULL },
	{ PERMAHJONG_K, PERCB(PerMahjongP1KPressed), PERCB(PerMahjongP1KReleased), NULL, NULL },
	{ PERMAHJONG_H, PERCB(PerMahjongP1HPressed), PERCB(PerMahjongP1HReleased), NULL, NULL },
	{ PERMAHJONG_D, PERCB(PerMahjongP1DPressed), PERCB(PerMahjongP1DReleased), NULL, NULL },
	{ PERMAHJONG_PON, PERCB(PerMahjongP1PonPressed), PERCB(PerMahjongP1PonReleased), NULL, NULL },
	{ PERMAHJONG_L, PERCB(PerMahjongP1LPressed), PERCB(PerMahjongP1LReleased), NULL, NULL },
	{ PERMAHJONG_FLIP_FLOP, PERCB(PerMahjongP1FlipFlopPressed), PERCB(PerMahjongP1FlipFlopReleased), NULL, NULL },
	{ PERMAHJONG_P2_KAN, PERCB(PerMahjongP2KanPressed), PERCB(PerMahjongP2KanReleased), NULL, NULL },
	{ PERMAHJONG_P2_START, PERCB(PerMahjongP2StartPressed), PERCB(PerMahjongP2StartReleased), NULL, NULL },
	{ PERMAHJONG_P2_E, PERCB(PerMahjongP2EPressed), PERCB(PerMahjongP2EReleased), NULL, NULL },
	{ PERMAHJONG_P2_A, PERCB(PerMahjongP2APressed), PERCB(PerMahjongP2AReleased), NULL, NULL },
	{ PERMAHJONG_P2_M, PERCB(PerMahjongP2MPressed), PERCB(PerMahjongP2MReleased), NULL, NULL },
	{ PERMAHJONG_P2_I, PERCB(PerMahjongP2IPressed), PERCB(PerMahjongP2IReleased), NULL, NULL },
	{ PERMAHJONG_P2_REACH, PERCB(PerMahjongP2ReachPressed), PERCB(PerMahjongP2ReachReleased), NULL, NULL },
	{ PERMAHJONG_P2_F, PERCB(PerMahjongP2FPressed), PERCB(PerMahjongP2FReleased), NULL, NULL },
	{ PERMAHJONG_P2_B, PERCB(PerMahjongP2BPressed), PERCB(PerMahjongP2BReleased), NULL, NULL },
	{ PERMAHJONG_P2_N, PERCB(PerMahjongP2NPressed), PERCB(PerMahjongP2NReleased), NULL, NULL },
	{ PERMAHJONG_P2_J, PERCB(PerMahjongP2JPressed), PERCB(PerMahjongP2JReleased), NULL, NULL },
	{ PERMAHJONG_P2_RON, PERCB(PerMahjongP2RonPressed), PERCB(PerMahjongP2RonReleased), NULL, NULL },
	{ PERMAHJONG_P2_G, PERCB(PerMahjongP2GPressed), PERCB(PerMahjongP2GReleased), NULL, NULL },
	{ PERMAHJONG_P2_C, PERCB(PerMahjongP2CPressed), PERCB(PerMahjongP2CReleased), NULL, NULL },
	{ PERMAHJONG_P2_CHI, PERCB(PerMahjongP2ChiPressed), PERCB(PerMahjongP2ChiReleased), NULL, NULL },
	{ PERMAHJONG_P2_K, PERCB(PerMahjongP2KPressed), PERCB(PerMahjongP2KReleased), NULL, NULL },
	{ PERMAHJONG_P2_H, PERCB(PerMahjongP2HPressed), PERCB(PerMahjongP2HReleased), NULL, NULL },
	{ PERMAHJONG_P2_D, PERCB(PerMahjongP2DPressed), PERCB(PerMahjongP2DReleased), NULL, NULL },
	{ PERMAHJONG_P2_PON, PERCB(PerMahjongP2PonPressed), PERCB(PerMahjongP2PonReleased), NULL, NULL },
	{ PERMAHJONG_P2_L, PERCB(PerMahjongP2LPressed), PERCB(PerMahjongP2LReleased), NULL, NULL },
	{ PERMAHJONG_P2_FLIP_FLOP, PERCB(PerMahjongP2FlipFlopPressed), PERCB(PerMahjongP2FlipFlopReleased), NULL, NULL },
};

PerBaseConfig_struct permousebaseconfig[] = {
	{ PERMOUSE_LEFT, PERCB(PerMouseLeftPressed), PERCB(PerMouseLeftReleased), NULL, NULL },
	{ PERMOUSE_MIDDLE, PERCB(PerMouseMiddlePressed), PERCB(PerMouseMiddleReleased), NULL, NULL },
	{ PERMOUSE_RIGHT, PERCB(PerMouseRightPressed), PERCB(PerMouseRightReleased), NULL, NULL },
	{ PERMOUSE_START, PERCB(PerMouseStartPressed), PERCB(PerMouseStartReleased), NULL, NULL },
	{ PERMOUSE_AXIS, NULL, NULL, NULL, PERMOVECB(PerMouseMove) },
};

PerBaseConfig_struct peranalogbaseconfig[] = {
	{ PERANALOG_AXIS1, NULL, NULL, PERVALCB(PerAxis1Value), NULL },
	{ PERANALOG_AXIS2, NULL, NULL, PERVALCB(PerAxis2Value), NULL },
	{ PERANALOG_AXIS3, NULL, NULL, PERVALCB(PerAxis3Value), NULL },
	{ PERANALOG_AXIS4, NULL, NULL, PERVALCB(PerAxis4Value), NULL },
	{ PERANALOG_AXIS5, NULL, NULL, PERVALCB(PerAxis5Value), NULL },
	{ PERANALOG_AXIS6, NULL, NULL, PERVALCB(PerAxis6Value), NULL },
	{ PERANALOG_AXIS7, NULL, NULL, PERVALCB(PerAxis7Value), NULL },
};

PerBaseConfig_struct pergunbaseconfig[] = {
	{ PERGUN_TRIGGER, PERCB(PerGunTriggerPressed), PERCB(PerGunTriggerReleased), NULL, NULL },
	{ PERGUN_START, PERCB(PerGunStartPressed), PERCB(PerGunStartReleased), NULL, NULL },
	{ PERGUN_AXIS, NULL, NULL, NULL, PERMOVECB(PerGunMove) },
};

static u32 perkeyconfigsize = 0;
static PerConfig_struct * perkeyconfig = NULL;

static void PerUpdateConfig(PerBaseConfig_struct * baseconfig, int nelems, void * controller);

u8 m_system_output = 0;
u8 m_ioga_mode = 0;
u8 m_ioga_portg = 0;

/*
   Medal/ticket hopper simulation.

   patocar, skychal, supgoal, techbowl (PATOCAR layout) and micrombc
   (MICROMBC layout) drive a physical hopper: PORT-D bit 0x80 turns the
   hopper motor on/off, and a photo-sensor pulses PORT-A bit 0x02 once
   per dispensed medal/ball while the motor runs. On real hardware (and
   in MAME's ticket_dispenser/hopper device, which this mirrors) the
   sensor line idles low and toggles roughly every 100ms while the
   motor is energised, so the game's dispense loop sees a steady stream
   of pulses until it has counted enough medals and cuts the motor.
*/
/* NOTE: on real hopper cabinets PORT-A is wired entirely differently from
   the generic STV/STV6B control panel (see stv.h: "Only STV & STV6B are
   properly hooked at the moment, the others might require tweaks"), so
   bit 0x02 is hardwired to the sensor and nothing else. Kronos doesn't
   yet have a dedicated PATOCAR/MICROMBC control-panel mapping, so the
   generic pad "B" binding still targets the same PORT_A bit; avoid
   binding/pressing B while testing a hopper game until that mapping
   is added, or the two writers will fight over the bit. */
#define HOPPER_SENSOR_PORT PORT_A
#define HOPPER_SENSOR_MASK 0x02
#define HOPPER_MOTOR_MASK  0x80
#define HOPPER_PULSE_MS    100

static u8 hopper_motor = 0;    /* current PORT-D bit 0x80 state */
static u8 hopper_status = 0;   /* current sensor line state (0 = idle, 1 = pulse) */
static u8 hopper_stopping = 0; /* motor released mid-pulse: finish the toggle, then stop */
static int hopper_frame_count = 0;
static u8 hopper_test_override = 0; /* PERJAMMA_HOPPER_TEST held: force the sensor line active */

/*
   PORT-G "counter mode" registers (patocar/skychal/supgoal/techbowl's
   trackball - PORTG.0 = X, PORTG.1 = Y in MAME; PORTG.2/.3 unused here).
   The SH2 reads these as a sequence of 8 byte-wide reads (auto-incrementing
   through 4 slots x high/low byte, via m_ioga_portg below); each slot is a
   genuine free-running 16-bit counter, unlike the rest of IOPORT[] which
   is 8 bits wide. micrombc explicitly has no trackball (see MAME) and
   never touches these.
*/
static u16 IOPORT_COUNTER[4];

#define TRACKBALL_X 0
#define TRACKBALL_Y 1
/* MAME flags its own patocar trackball tuning as unconfirmed ("TODO:
   sense/delta values seems wrong" - PORT_SENSITIVITY(500)/KEYDELTA(100)).
   This digital nudge is a fallback for the same control (no host mouse/
   analog wiring here), so treat this step as an approximate starting
   point to taste rather than a verified hardware value. */
#define TRACKBALL_STEP 96

static u8 trackball_up = 0, trackball_down = 0, trackball_left = 0, trackball_right = 0;

static int PerTrackballEnabled(void) {
  /* Trackball is specific to the patocar-family control panel; micrombc
     shares the same PORTA/PORTB/PORTC layout but has no trackball. */
  return (yabsys.stvInputType == PATOCAR);
}

void PerTrackballExec(void) {
  if (!PerTrackballEnabled())
    return;

  if (trackball_right)
    IOPORT_COUNTER[TRACKBALL_X] += TRACKBALL_STEP;
  if (trackball_left)
    IOPORT_COUNTER[TRACKBALL_X] -= TRACKBALL_STEP;
  if (trackball_down)
    IOPORT_COUNTER[TRACKBALL_Y] += TRACKBALL_STEP;
  if (trackball_up)
    IOPORT_COUNTER[TRACKBALL_Y] -= TRACKBALL_STEP;
}

/* Sega Mahjong Panel row-scan matrix: one global MJPORT[] byte per row,
   active-low like IOPORT[]. Rows 0-4 are P1's KEY0-KEY4, rows 5-9 are
   P2's KEY0-KEY4 (kiwames only - vmahjong/myfairld never select them).
   See PerMahjongAdd()/PerMahjongPanelRead() and
   PerMahjongPanelEnabled()/IOPortReadByte()/IOPortWriteByte() below. */
static u8 MJPORT[10];
static u8 mj_mux_data = 0; /* row-select bits, already normalized (1 = row selected) */

/* Each button is just "clear/set bit `bit` of mux row `row`", exactly
   like PerCab's PORT_x bit twiddling above - a macro avoids 86 near-
   identical hand-written functions for the panel's tile/action buttons. */
#define DEFINE_MAHJONG_BUTTON(func, row, bit) \
void func##Pressed(PerMahjongPanel_struct * mj) { \
   mj[row] &= ~(0x1 << (bit)); \
} \
void func##Released(PerMahjongPanel_struct * mj) { \
   mj[row] |= (0x1 << (bit)); \
}

DEFINE_MAHJONG_BUTTON(PerMahjongP1Kan, 0, 0)
DEFINE_MAHJONG_BUTTON(PerMahjongP1Start, 0, 1)
DEFINE_MAHJONG_BUTTON(PerMahjongP1E, 0, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP1A, 0, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP1M, 0, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP1I, 0, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP1Reach, 1, 0)
DEFINE_MAHJONG_BUTTON(PerMahjongP1Bet, 1, 1)
DEFINE_MAHJONG_BUTTON(PerMahjongP1F, 1, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP1B, 1, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP1N, 1, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP1J, 1, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP1Ron, 2, 0)
DEFINE_MAHJONG_BUTTON(PerMahjongP1G, 2, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP1C, 2, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP1Chi, 2, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP1K, 2, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP1H, 3, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP1D, 3, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP1Pon, 3, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP1L, 3, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP1FlipFlop, 4, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP2Kan, 5, 0)
DEFINE_MAHJONG_BUTTON(PerMahjongP2Start, 5, 1)
DEFINE_MAHJONG_BUTTON(PerMahjongP2E, 5, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP2A, 5, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP2M, 5, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP2I, 5, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP2Reach, 6, 0)
DEFINE_MAHJONG_BUTTON(PerMahjongP2F, 6, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP2B, 6, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP2N, 6, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP2J, 6, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP2Ron, 7, 0)
DEFINE_MAHJONG_BUTTON(PerMahjongP2G, 7, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP2C, 7, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP2Chi, 7, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP2K, 7, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP2H, 8, 4)
DEFINE_MAHJONG_BUTTON(PerMahjongP2D, 8, 5)
DEFINE_MAHJONG_BUTTON(PerMahjongP2Pon, 8, 6)
DEFINE_MAHJONG_BUTTON(PerMahjongP2L, 8, 7)
DEFINE_MAHJONG_BUTTON(PerMahjongP2FlipFlop, 9, 6)

#undef DEFINE_MAHJONG_BUTTON

static int PerMahjongPanelEnabled(void) {
  return (yabsys.stvInputType == STVMP) || (yabsys.stvInputType == VMAHJONG) ||
         (yabsys.stvInputType == MYFAIRLD);
}

PerMahjongPanel_struct * PerMahjongAdd(PortData_struct * port) {
  PerUpdateConfig(permahjongbaseconfig, sizeof(permahjongbaseconfig) / sizeof(PerBaseConfig_struct), MJPORT);
  return MJPORT;
}

static int PerHopperEnabled(void) {
  return (yabsys.stvInputType == PATOCAR) || (yabsys.stvInputType == MICROMBC);
}

static int PerHopperFramesPerPulse(void) {
  int fps = yabsys.IsPal ? 50 : 60;
  int frames = (HOPPER_PULSE_MS * fps + 500) / 1000; /* round to nearest frame */
  return (frames < 1) ? 1 : frames;
}

static void PerHopperSetSensor(u8 active) {
  hopper_status = active;
  if (active)
    IOPORT[HOPPER_SENSOR_PORT] |= HOPPER_SENSOR_MASK;
  else
    IOPORT[HOPPER_SENSOR_PORT] &= ~HOPPER_SENSOR_MASK;
}

static void PerHopperMotorWrite(u8 val) {
  u8 motor;

  if (!PerHopperEnabled())
    return;

  motor = (val & HOPPER_MOTOR_MASK) ? 1 : 0;

  if (motor && !hopper_motor) {
    /* Rising edge: power on, start pulsing from idle. */
    hopper_motor = 1;
    hopper_stopping = 0;
    hopper_frame_count = 0;
    PerHopperSetSensor(0);
  } else if (!motor && hopper_motor) {
    /* Falling edge: stop, but if we're mid-pulse let PerHopperExec()
       finish toggling the line back down before it goes quiet. */
    hopper_motor = 0;
    if (hopper_status)
      hopper_stopping = 1;
    else
      hopper_frame_count = 0;
  }
}

void PerHopperExec(void) {
  if (!PerHopperEnabled())
    return;

  if (!hopper_motor && !hopper_stopping)
    return;

  if (++hopper_frame_count < PerHopperFramesPerPulse())
    return;

  hopper_frame_count = 0;
  PerHopperSetSensor(hopper_status ^ 1);

  if (!hopper_motor && !hopper_status)
    hopper_stopping = 0; /* trailing toggle done, fully idle now */
}

int IOPortAdd(int key, ioPort port, u8 index) {
  if (index >= 8) return -1;
  IOalloc[port][index].key = key;
  IOalloc[port][index].port = &IOPORT[port];
  IOalloc[port][index].mask = (0x1 << index);
  IOkeys[key] = &(IOalloc[port][index]);
  return 0;
}

static void IOPortPressed(int key) {
  if (IOkeys[(key & 0xFF)] != NULL){
    (*IOkeys[(key & 0xFF)]->port) &= ~IOkeys[(key & 0xFF)]->mask;
  }
}
static void IOPortReleased(int key) {
  if (IOkeys[(key & 0xFF)] != NULL){
    (*IOkeys[(key & 0xFF)]->port) |= IOkeys[(key & 0xFF)]->mask;
  }
}


/* Mahjong Panel row-scan read: ANDs together every MJPORT[player*5+row] row
   currently selected by mj_mux_data (as written to PORT-E, offset 0x09,
   below), exactly like MAME's stvmp_ioga_r(). Active-low, so a deselected
   row (0xff) never masks out a selected one. */
static u8 PerMahjongPanelRead(int player) {
   u8 val = 0xff;
   int i;
   for (i = 0; i < 5; i++) {
      if (mj_mux_data & (0x1 << i))
         val &= MJPORT[player * 5 + i];
   }
   return val;
}

u8 FASTCALL IOPortReadByte(SH2_struct *context, UNUSED u8* memory,  u32 addr)
{
/*
    0x0001 PORT-A (P1)
    0x0003 PORT-B (P2)
    0x0005 PORT-C (SYSTEM)
    0x0007 PORT-D (OUTPUT)
    0x0009 PORT-E (P3)
    0x000b PORT-F (P4 / Extra 6B layout)
    0x000d PORT-G
    0x000f unused
    0x0011 PORT_DIRECTION (each bit configure above IO ports, 1 - input, 0 - output)
    ---x ---- joystick/mahjong panel select
    ---- x--- used in danchih (different mux scheme for the hanafuda panel?)
    0x0013 RS422 TXD1
    0x0015 RS422 TXD2 SERIAL COM Tx
    0x0017 RS422 RXD1
    0x0019 RS422 RXD2 SERIAL COM Rx
    0x001b RS422 FLAG
    0x001d MODE (bit 7 - set PORT-G to counter-mode, bits 0-5 - RS422 satellite mode and node#)  <Technical Bowling>
    0x001f PORT-AD (8ch, write: bits 0-2 - set channel, read: channel data with autoinc channel number)
*/
   addr = addr&0x1F;
   u8 val = 0x0;
   switch(addr) {
     case 0x01: // P1
       if (PerMahjongPanelEnabled()) {
         val = PerMahjongPanelRead(0);
         break;
       }
       val = IOPORT[PORT_A];
       if (hopper_test_override && PerHopperEnabled())
         val |= HOPPER_SENSOR_MASK; // force the sensor line active, like a held "Sensor Test" input
       break;
     case 0x03: // P2
       val = PerMahjongPanelEnabled() ? PerMahjongPanelRead(1) : IOPORT[PORT_B];
       break;
     case 0x05: val = IOPORT[PORT_C]; break; // SYSTEM //Return press status like declared in stv.cpp
     case 0x07: val = m_system_output; break; // port D, read-backs value written
     case 0x09: val = IOPORT[PORT_E]; break; // P3
     case 0x0b: val = IOPORT[PORT_F]; break; // P4
     case 0x0d: //PORT-G
       if (m_ioga_mode & 0x80) // PORT-G in counter mode
       {
         u16 word = IOPORT_COUNTER[(m_ioga_portg >> 1) & 3];
         val = (u8)(word >> (((m_ioga_portg & 1) ^ 1) * 8));
         m_ioga_portg = (m_ioga_portg & 0xf8) | ((m_ioga_portg + 1) & 7); // counter# is auto-incremented then read
       }
       else
         val = IOPORT[PORT_G];
       break;
     case 0x1b: val = 0x0; break; // Serial COM READ status
     case 0x1d: val = m_ioga_mode; break;

   }
   return val;
}

u16 FASTCALL IOPortReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   return IOPortReadByte(context, memory, addr|1);
}

void FASTCALL IOPortWriteByte(SH2_struct *context, UNUSED u8* memory,UNUSED u32 addr, UNUSED u8 val)
{
   addr = addr&0x1F;
  switch(addr)
  {
    case 0x07:
      m_system_output = val;
      PerHopperMotorWrite(val);
      break;
    case 0x09: // P3 / Mahjong Panel row select
      if (PerMahjongPanelEnabled()) {
        mj_mux_data = val ^ 0xff; // active-low on the real mux register, like MAME's stvmp_ioga_w()
        break;
      }
      IOPORT[PORT_F] = val;
      IOPORT[PORT_G] = val;
      break;
    case 0x0b: IOPORT[PORT_E] = val; break; // P4
    case 0x0d:
      //port-g
      m_ioga_portg = val;
      break;
    case 0x1d:
      m_ioga_mode = val;
      break;
  }
}

//////////////////////////////////////////////////////////////////////////////

int PerInit(int coreid) {
   int i;

   // So which core do we want?
   if (coreid == PERCORE_DEFAULT)
      coreid = 0; // Assume we want the first one

   // Go through core list and find the id
   for (i = 0; PERCoreList[i] != NULL; i++)
   {
      if (PERCoreList[i]->id == coreid)
      {
         // Set to current core
         PERCore = PERCoreList[i];
         break;
      }
   }

   if (PERCore == NULL)
      return -1;

   if (PERCore->Init() != 0)
      return -1;

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void PerDeInit(void) {
   if (PERCore)
      PERCore->DeInit();
   PERCore = NULL;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadUpPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xEF;
   SMPCLOG("Up\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadUpReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xEF;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadDownPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xDF;
   SMPCLOG("Down\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadDownReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xDF;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadRightPressed(PerPad_struct * pad) {
   *pad->padbits &= 0x7F;
   SMPCLOG("Right\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadRightReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0x7F;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadLeftPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xBF;
   SMPCLOG("Left\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadLeftReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xBF;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadStartPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xF7;
   SMPCLOG("Start\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadStartReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xF7;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadAPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xFB;
   SMPCLOG("A\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadAReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xFB;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadBPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xFE;
   SMPCLOG("B\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadBReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xFE;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadCPressed(PerPad_struct * pad) {
   *pad->padbits &= 0xFD;
   SMPCLOG("C\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadCReleased(PerPad_struct * pad) {
   *pad->padbits |= ~0xFD;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadXPressed(PerPad_struct * pad) {
   *(pad->padbits + 1) &= 0xBF;
   SMPCLOG("X\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadXReleased(PerPad_struct * pad) {
   *(pad->padbits + 1) |= ~0xBF;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadYPressed(PerPad_struct * pad) {
   *(pad->padbits + 1) &= 0xDF;
   SMPCLOG("Y\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadYReleased(PerPad_struct * pad) {
   *(pad->padbits + 1) |= ~0xDF;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadZPressed(PerPad_struct * pad) {
   *(pad->padbits + 1) &= 0xEF;
   SMPCLOG("Z\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadZReleased(PerPad_struct * pad) {
   *(pad->padbits + 1) |= ~0xEF;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadRTriggerPressed(PerPad_struct * pad) {
   *(pad->padbits + 1) &= 0x7F;
   SMPCLOG("Right Trigger\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadRTriggerReleased(PerPad_struct * pad) {
   *(pad->padbits + 1) |= ~0x7F;
}

//////////////////////////////////////////////////////////////////////////////

void PerPadLTriggerPressed(PerPad_struct * pad) {
   *(pad->padbits + 1) &= 0xF7;
   SMPCLOG("Left Trigger\n");
}

//////////////////////////////////////////////////////////////////////////////

void PerPadLTriggerReleased(PerPad_struct * pad) {
   *(pad->padbits + 1) |= ~0xF7;
}

//////////////////////////////////////////////////////////////////////////////

/*
  IOPortAdd(KEYPAD(PERPAD_UP, 0), PORT_A, 0x5);
  IOPortAdd(KEYPAD(PERPAD_RIGHT, 0), PORT_A, 0x6);
  IOPortAdd(KEYPAD(PERPAD_DOWN, 0), PORT_A, 0x4);
  IOPortAdd(KEYPAD(PERPAD_LEFT, 0), PORT_A, 0x7);
  IOPortAdd(KEYPAD(PERPAD_A, 0), PORT_A, 0x0);
  IOPortAdd(KEYPAD(PERPAD_B, 0), PORT_A, 0x1);
  IOPortAdd(KEYPAD(PERPAD_C, 0), PORT_A, 0x2);
  IOPortAdd(KEYPAD(PERPAD_X, 0), PORT_A, 0x3);

  IOPortAdd(PERJAMMA_COIN1, PORT_C, 0x0 );
  IOPortAdd(PERJAMMA_COIN2, PORT_C, 0x1 );
  IOPortAdd(PERJAMMA_TEST, PORT_C, 0x2);
  IOPortAdd(PERJAMMA_SERVICE, PORT_C, 0x3);
  IOPortAdd(PERJAMMA_START1, PORT_C, 0x4);
  IOPortAdd(PERJAMMA_START2, PORT_C, 0x5);
  IOPortAdd(PERJAMMA_MULTICART, PORT_C, 0x6);
  IOPortAdd(PERJAMMA_PAUSE, PORT_C, 0x7);
*/

/* P1 inputs */

void PerCabUpPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x5);
}

void PerCabUpReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabDownPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x4);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabDownReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x4);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabRightPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabRightReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabLeftPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x7);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabLeftReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x7);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabAPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabAReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabBPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabBReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabCPressed(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabCReleased(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabXPressed(PerCab_struct * pad) {
   switch (yabsys.stvInputType)
   {
      case STV6B:
         pad[PORT_F] &= ~(0x1 << 0x0); break;
      case STV:
      default:
         pad[PORT_A] &= ~(0x1 << 0x3); break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerCabXReleased(PerCab_struct * pad) {
   switch (yabsys.stvInputType)
   {
      case STV6B:
         pad[PORT_F] |= (0x1 << 0x0); break;
      case STV:
      default:
         pad[PORT_A] |= (0x1 << 0x3); break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerCabYPressed(PerCab_struct * pad) {
   pad[PORT_F] &= ~(0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabYReleased(PerCab_struct * pad) {
   pad[PORT_F] |= (0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabZPressed(PerCab_struct * pad) {
   pad[PORT_F] &= ~(0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabZReleased(PerCab_struct * pad) {
   pad[PORT_F] |= (0x1 << 0x2);
}

/* P2 Inputs */

void PerCabP2UpPressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x5);
}

void PerCabP2UpReleased(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2DownPressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x4);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2DownReleased(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x4);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2RightPressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2RightReleased(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2LeftPressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x7);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2LeftReleased(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x7);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button1Pressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button1Released(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button2Pressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button2Released(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button3Pressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button3Released(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button4Pressed(PerCab_struct * pad) {
   switch (yabsys.stvInputType)
   {
      case STV6B:
         pad[PORT_F] &= ~(0x1 << 0x4); break;
      case STV:
      default:
         pad[PORT_B] &= ~(0x1 << 0x3); break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button4Released(PerCab_struct * pad) {
   switch (yabsys.stvInputType)
   {
      case STV6B:
         pad[PORT_F] |= (0x1 << 0x4); break;
      case STV:
      default:
         pad[PORT_B] |= (0x1 << 0x3); break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button5Pressed(PerCab_struct * pad) {
   pad[PORT_F] &= ~(0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button5Released(PerCab_struct * pad) {
   pad[PORT_F] |= (0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button6Pressed(PerCab_struct * pad) {
   pad[PORT_F] &= ~(0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabP2Button6Released(PerCab_struct * pad) {
   pad[PORT_F] |= (0x1 << 0x6);
}

/* PATOCAR/MICROMBC hopper cabinet inputs (patocar, skychal, supgoal,
   techbowl, micrombc). Layout taken from MAME's sega/stv.cpp "patocar"
   and "micrombc" input ports. */

//////////////////////////////////////////////////////////////////////////////

void PerCabPowerButtonPressed(PerCab_struct * pad) {
   switch (yabsys.stvInputType) {
      case MICROMBC:
         pad[PORT_A] &= ~(0x1 << 0x5); break; // micrombc moves it to PORT-A bit 0x20
      case PATOCAR:
      default:
         pad[PORT_C] &= ~(0x1 << 0x5); break; // PORT-C bit 0x20 on patocar/skychal/supgoal/techbowl
   }
}

void PerCabPowerButtonReleased(PerCab_struct * pad) {
   switch (yabsys.stvInputType) {
      case MICROMBC:
         pad[PORT_A] |= (0x1 << 0x5); break;
      case PATOCAR:
      default:
         pad[PORT_C] |= (0x1 << 0x5); break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerCabMedalPressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x5); // Coin3 / medal insert, PORT-B bit 0x20
}

void PerCabMedalReleased(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

/* PORT-A bit 0 is wired active-high on these boards (idle = 0), unlike
   the rest of the panel which idles high. MAME itself isn't sure what
   this input does ("hopper ?" in its port list) but it shares the same
   input port as the hopper sensor, so it's exposed here for parity. */
void PerCabHopperBtn2Pressed(PerCab_struct * pad) {
   pad[PORT_A] |= (0x1 << 0x0);
}

void PerCabHopperBtn2Released(PerCab_struct * pad) {
   pad[PORT_A] &= ~(0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

/* PORT-B bit 1 (door switch) is also active-high, idle = 0. */
void PerCabDoorSwitchPressed(PerCab_struct * pad) {
   pad[PORT_B] |= (0x1 << 0x1);
}

void PerCabDoorSwitchReleased(PerCab_struct * pad) {
   pad[PORT_B] &= ~(0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

/* PORT-B bits 0x0c move together as a single logical input: an unlabeled
   "Button 5" on patocar/skychal/supgoal/techbowl, the "Magnet Sensor" on
   micrombc. Same wiring, only the UI label differs (see libretro.c). */
void PerCabMagnetBtn5Pressed(PerCab_struct * pad) {
   pad[PORT_B] &= ~0x0c;
}

void PerCabMagnetBtn5Released(PerCab_struct * pad) {
   pad[PORT_B] |= 0x0c;
}

//////////////////////////////////////////////////////////////////////////////

/* Not a real cabinet input. Mirrors the "Sensor Test" service switch on
   MAME's ticket_dispenser device: forces the hopper sensor line active
   while held, regardless of motor state, so the wiring/counter can be
   checked without having to actually run the hopper motor. */
void PerCabHopperTestPressed(PerCab_struct * pad) {
   (void)pad;
   hopper_test_override = 1;
}

void PerCabHopperTestReleased(PerCab_struct * pad) {
   (void)pad;
   hopper_test_override = 0;
}

//////////////////////////////////////////////////////////////////////////////

/* Digital trackball nudge (patocar/skychal/supgoal/techbowl only - see
   PerTrackballEnabled()). These just latch a held-direction flag; the
   actual counter movement happens once per frame in PerTrackballExec(),
   the same pattern used for the hopper motor/sensor above. */
void PerCabTrackballUpPressed(PerCab_struct * pad) {
   (void)pad;
   trackball_up = 1;
}

void PerCabTrackballUpReleased(PerCab_struct * pad) {
   (void)pad;
   trackball_up = 0;
}

void PerCabTrackballDownPressed(PerCab_struct * pad) {
   (void)pad;
   trackball_down = 1;
}

void PerCabTrackballDownReleased(PerCab_struct * pad) {
   (void)pad;
   trackball_down = 0;
}

void PerCabTrackballLeftPressed(PerCab_struct * pad) {
   (void)pad;
   trackball_left = 1;
}

void PerCabTrackballLeftReleased(PerCab_struct * pad) {
   (void)pad;
   trackball_left = 0;
}

void PerCabTrackballRightPressed(PerCab_struct * pad) {
   (void)pad;
   trackball_right = 1;
}

void PerCabTrackballRightReleased(PerCab_struct * pad) {
   (void)pad;
   trackball_right = 0;
}

/* System Inputs*/

void PerCabCoin1Released(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabCoin1Pressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x0);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabCoin2Released(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabCoin2Pressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x1);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabTestPressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabTestReleased(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x2);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabServicePressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x3);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabServiceReleased(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x3);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabStart1Released(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x4);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabStart1Pressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x4);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabStart2Released(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabStart2Pressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x5);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabMultiCartPressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabMultiCartReleased(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x6);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabPausePressed(PerCab_struct * pad) {
   pad[PORT_C] &= ~(0x1 << 0x7);
}

//////////////////////////////////////////////////////////////////////////////

void PerCabPauseReleased(PerCab_struct * pad) {
   pad[PORT_C] |= (0x1 << 0x7);
}

//////////////////////////////////////////////////////////////////////////////


void PerMouseLeftPressed(PerMouse_struct * mouse) {
   *(mouse->mousebits) |= 1;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseLeftReleased(PerMouse_struct * mouse) {
   *(mouse->mousebits) &= 0xFE;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseMiddlePressed(PerMouse_struct * mouse) {
   *(mouse->mousebits) |= 4;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseMiddleReleased(PerMouse_struct * mouse) {
   *(mouse->mousebits) &= 0xFFFB;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseRightPressed(PerMouse_struct * mouse) {
   *(mouse->mousebits) |= 2;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseRightReleased(PerMouse_struct * mouse) {
   *(mouse->mousebits) &= 0xFD;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseStartPressed(PerMouse_struct * mouse) {
   *(mouse->mousebits) |= 8;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseStartReleased(PerMouse_struct * mouse) {
   *(mouse->mousebits) &= 0xF7;
}

//////////////////////////////////////////////////////////////////////////////

void PerMouseMove(PerMouse_struct * mouse, s32 dispx, s32 dispy)
{
   int negx, negy, overflowx, overflowy;
   u8 diffx, diffy;

   negx = ((mouse->mousebits[0] >> 4) & 1);
   negy = ((mouse->mousebits[0] >> 5) & 1);
   overflowx = ((mouse->mousebits[0] >> 6) & 1);
   overflowy = ((mouse->mousebits[0] >> 7) & 1);

   if (negx) diffx = ~(mouse->mousebits[1]) & 0xFF;
   else diffx = mouse->mousebits[1];
   if (negy) diffy = ~(mouse->mousebits[2]) & 0xFF;
   else diffy = mouse->mousebits[2];

   if (dispx >= 0)
   {
      if (negx)
      {
         if (dispx - diffx > 0)
         {
            diffx = dispx - diffx;
            negx = 0;
         }
         else diffx -= -dispx;
      }
      else diffx += dispx;
   }
   else
   {
      if (negx) diffx += -dispx;
      else
      {
         if (diffx + dispx > 0) diffx += dispx;
         else
         {
            diffx = -dispx - diffx;
            negx = 1;
         }
      }
   }

   if (dispy >= 0)
   {
      if (negy)
      {
         if (dispy - diffy > 0)
         {
            diffy = dispy - diffy;
            negy = 0;
         }
         else diffy -= -dispy;
      }
      else diffy += dispy;
   }
   else
   {
      if (negy) diffy += -dispy;
      else
      {
         if (diffy + dispy > 0) diffy += dispy;
         else
         {
            diffy = -dispy - diffy;
            negy = 1;
         }
      }
   }

   mouse->mousebits[0] = (overflowy << 7) | (overflowx << 6) | (negy << 5) | (negx << 4) | (mouse->mousebits[0] & 0x0F);
   if (negx) mouse->mousebits[1] = ~(diffx);
   else mouse->mousebits[1] = diffx;
   if (negy) mouse->mousebits[2] = ~(diffy);
   else mouse->mousebits[2] = diffy;
}

//////////////////////////////////////////////////////////////////////////////

void PerAxis1Value(PerAnalog_struct * analog, u32 val)
{
   analog->analogbits[2] = (u8)val;

   //handle wheel left/right standard pad presses depending on wheel position
   if (analog->perid == PERWHEEL)
   {
      int left_is_pressed = (analog->analogbits[0] & (1 << 6)) == 0;
      int right_is_pressed = (analog->analogbits[0] & (1 << 7)) == 0;

      if (val <= 0x67)
         analog->analogbits[0] &= 0xBF;//press left
      else if (left_is_pressed && val >= 0x6f)
         analog->analogbits[0] |= ~0xBF;//release left


      if (val >= 0x97)
         analog->analogbits[0] &= 0x7F;//press right
      else if(right_is_pressed && val <= 0x8f)
         analog->analogbits[0] |= ~0x7F;//release right
   }
   else if (analog->perid == PERMISSIONSTICK ||
      analog->perid == PERTWINSTICKS)
   {
      int left_is_pressed = (analog->analogbits[0] & (1 << 6)) == 0;
      int right_is_pressed = (analog->analogbits[0] & (1 << 7)) == 0;

      if (val <= 0x56)
         analog->analogbits[0] &= 0xBF;//press left
      else if (left_is_pressed && val >= 0x6a)
         analog->analogbits[0] |= ~0xBF;//release left


      if (val >= 0xab)
         analog->analogbits[0] &= 0x7F;//press right
      else if (right_is_pressed && val <= 0x95)
         analog->analogbits[0] |= ~0x7F;//release right
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerAxis2Value(PerAnalog_struct * analog, u32 val)
{
   analog->analogbits[3] = (u8)val;

   if (analog->perid == PERMISSIONSTICK ||
      analog->perid == PERTWINSTICKS)
   {
      int up_is_pressed = (analog->analogbits[0] & (1 << 4)) == 0;
      int down_is_pressed = (analog->analogbits[0] & (1 << 5)) == 0;

      if (val <= 0x65)
         analog->analogbits[0] &= 0xEF;//press up
      else if (up_is_pressed && val >= 0x6a)
         analog->analogbits[0] |= ~0xEF;//release up


      if (val >= 0xa9)
         analog->analogbits[0] &= 0xDF;//press down
      else if (down_is_pressed && val <= 0x94)
         analog->analogbits[0] |= ~0xDF;//release down
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerAxis3Value(PerAnalog_struct * analog, u32 val)
{
   if (analog->perid != PERMISSIONSTICK)
      analog->analogbits[4] = (u8)val;
   else
      analog->analogbits[4] = (u8)(-(s8)val);//axis inverted on mission stick
}

//////////////////////////////////////////////////////////////////////////////

void PerAxis4Value(PerAnalog_struct * analog, u32 val)
{
   analog->analogbits[5] = (u8)val;
}

//////////////////////////////////////////////////////////////////////////////

//left stick L/R
void PerAxis5Value(PerAnalog_struct * analog, u32 val)
{
   analog->analogbits[6] = (u8)val;
}

//////////////////////////////////////////////////////////////////////////////

//left stick U/D
void PerAxis6Value(PerAnalog_struct * analog, u32 val)
{
      analog->analogbits[7] = (u8)val;
}

//////////////////////////////////////////////////////////////////////////////

//left stick throttle
void PerAxis7Value(PerAnalog_struct * analog, u32 val)
{
   if (analog->perid != PERTWINSTICKS)
      analog->analogbits[8] = (u8)val;
   else
      analog->analogbits[8] = (u8)(-(s8)val);//axis inverted on mission stick
}

//////////////////////////////////////////////////////////////////////////////

void PerGunTriggerPressed(PerGun_struct * gun)
{
   *(gun->gunbits) &= ~0x10;
}

//////////////////////////////////////////////////////////////////////////////

void PerGunTriggerReleased(PerGun_struct * gun)
{
   *(gun->gunbits) |= 0x10;
}

//////////////////////////////////////////////////////////////////////////////

void PerGunStartPressed(PerGun_struct * gun)
{
   *(gun->gunbits) &= ~0x20;
}

//////////////////////////////////////////////////////////////////////////////

void PerGunStartReleased(PerGun_struct * gun)
{
   *(gun->gunbits) |= 0x20;
}

//////////////////////////////////////////////////////////////////////////////

void PerGunMove(PerGun_struct * gun, s32 x, s32 y)
{
   if ((x < 0) || (x>_Ygl->rwidth) || (y<0) || (y>_Ygl->rheight)) {
		 *(gun->gunbits) |= 0x40;
	 } else {
		 *(gun->gunbits) &= ~0x40;
	 }

   *(gun->gunbits+1) = x >> 8;
   *(gun->gunbits+2) = x;
   *(gun->gunbits+3) = y >> 8;
   *(gun->gunbits+4) = y;
}

//////////////////////////////////////////////////////////////////////////////

void * PerAddPeripheral(PortData_struct *port, int perid)
{
   int pernum = port->data[0] & 0xF;
   int i;
   int peroffset=1;
   u8 size;
   int current = 1;
   void * controller;

   if (pernum == 0xF)
     return NULL;
   else if (perid == PERGUN && pernum == 1) // Gun doesn't work with multi-tap
     return NULL;

   // if only one peripheral is connected use 0xF0(unless Gun), otherwise use 0x00 or 0x10
   if (pernum == 0)
   {
      pernum = 1;

      if (perid != PERGUN)
         port->data[0] = 0xF1;
      else
         port->data[0] = 0xA0;
   }
   else
   {
      if (pernum == 1)
      {
         u8 tmp = peroffset;
         tmp += (port->data[peroffset] & 0xF) + 1;

         for(i = 0;i < 5;i++)
            port->data[tmp + i] = 0xFF;
      }
      pernum = 6;
      port->data[0] = 0x16;

      // figure out where we're at, then add peripheral id + 1
      current = 0;
      size = port->data[peroffset] & 0xF;
      while ((current < pernum) && (size != 0xF))
      {
         peroffset += size + 1;
         current++;
         size = port->data[peroffset] & 0xF;
      }

      if (current == pernum)
      {
         return NULL;
      }
      current++;
   }

   port->data[peroffset] = perid;
   peroffset++;

   // set peripheral data for peripheral to default values and adjust size
   // of port data
   switch (perid)
   {
      case PERPAD:
         port->data[peroffset] = 0xFF;
         port->data[peroffset+1] = 0xFF;
         port->size = peroffset+(perid&0xF);
         break;
      case PERWHEEL:
         port->data[peroffset] = 0xFF;
         port->data[peroffset+1] = 0xFF;
         port->data[peroffset+2] = 0x7F;
         port->size = peroffset+(perid&0xF);
         break;
      case PERMISSIONSTICK:
         port->data[peroffset] = 0xFF;
         port->data[peroffset + 1] = 0xFF;
         port->data[peroffset + 2] = 0x7F;
         port->data[peroffset + 3] = 0x7F;
         port->data[peroffset + 4] = 0x7F;
         port->size = peroffset + (perid & 0xF);
         break;
      case PERTWINSTICKS://also double mission sticks
         port->data[peroffset] = 0xFF;
         port->data[peroffset + 1] = 0xFF;
         //right stick
         port->data[peroffset + 2] = 0x7F;
         port->data[peroffset + 3] = 0x7F;
         port->data[peroffset + 4] = 0x7F;
         //left stick
         port->data[peroffset + 5] = 0x7F;
         port->data[peroffset + 6] = 0x7F;
         port->data[peroffset + 7] = 0x7F;
         port->size = peroffset + (perid & 0xF);
         break;
      case PER3DPAD:
         port->data[peroffset] = 0xFF;
         port->data[peroffset+1] = 0xFF;
         port->data[peroffset+2] = 0x7F;
         port->data[peroffset+3] = 0x7F;
         port->data[peroffset+4] = 0x7F;
         port->data[peroffset+5] = 0x7F;
         port->size = peroffset+(perid&0xF);
         break;
      case PERGUN:
         port->data[peroffset] = 0x7C;
         port->data[peroffset + 1] = 0xFF;
         port->data[peroffset + 2] = 0xFF;
         port->data[peroffset + 3] = 0xFF;
         port->data[peroffset + 4] = 0xFF;
         port->size = 1;
         break;
      case PERKEYBOARD:
         port->data[peroffset] = 0xFF;
         port->data[peroffset+1] = 0xF8;
         port->data[peroffset+2] = 0x06;
         port->data[peroffset+3] = 0x00;
         port->size = peroffset+(perid&0xF);
         break;
      case PERMOUSE:
         port->data[peroffset] = 0;
         port->data[peroffset + 1] = 0;
         port->data[peroffset + 2] = 0;
         port->size = peroffset+(perid&0xF);
         break;
      default: break;
   }

   if (perid != PERGUN)
   {
      u8 tmp = peroffset;
      tmp += (perid & 0xF);
      for(i = 0;i < (pernum - current);i++)
      {
         port->data[tmp + i] = 0xFF;
         port->size++;
      }
   }

   controller = (port->data + (peroffset - 1));
   switch (perid)
   {
      case PERPAD:
         PerUpdateConfig(perpadbaseconfig, sizeof(perpadbaseconfig)/sizeof(PerBaseConfig_struct), controller);
         break;
      case PERWHEEL:
      case PERMISSIONSTICK:
      case PER3DPAD:
      case PERTWINSTICKS:
         PerUpdateConfig(perpadbaseconfig, sizeof(perpadbaseconfig)/sizeof(PerBaseConfig_struct), controller);
         PerUpdateConfig(peranalogbaseconfig, sizeof(peranalogbaseconfig)/sizeof(PerBaseConfig_struct), controller);
         break;
      case PERGUN:
         PerUpdateConfig(pergunbaseconfig, sizeof(pergunbaseconfig)/sizeof(PerBaseConfig_struct), controller);
         break;
      case PERMOUSE:
         PerUpdateConfig(permousebaseconfig, sizeof(permousebaseconfig)/sizeof(PerBaseConfig_struct), controller);
         break;
   }
   return controller;
}

//////////////////////////////////////////////////////////////////////////////

int PerGetId(void * peripheral)
{
   u8 * id = (u8*)peripheral;
   return *id;
}

//////////////////////////////////////////////////////////////////////////////
// Keyboard
//
// Data format, key codes and pad-compatible buttons: Saturn Technical
// Bulletin #45 "Saturn Keyboard Data Format Ver.1.00" (STTECH45):
//  - one Make or Break code per read, keys are sent one after the other
//    on a roll over, and the Make code is repeated while the last key
//    pressed is held ("Repeat function (on all keys)", a new key starts
//    a new repeat cycle);
//  - CapsLock/NumLock/ScrLock flags toggle on the Make of their key;
//  - pad buttons: arrows = D-pad, ESC = Start, Z/X/C = A/B/C,
//    A/S/D = X/Y/Z, Q/E = L/R. (The bulletin lists "Right: [ESC] key",
//    a typo for Start: the button table and Mednafen both use Start.)
//
// Not given by the bulletin, taken from Mednafen (ss/smpc_iodevice.c,
// IODevice_Keyboard), as allowed when the workspace docs are silent:
//  - repeat delay TS = 400 ms and repeat interval TR = 33.3 ms;
//  - the pad buttons and lock flags change only when the code of their
//    key is actually sent (not when the host key moves), and pressing a
//    direction releases the opposite one (Up/Down, Left/Right);
//  - Pause has no Break of its own: Make and Break are both queued when
//    the key goes down;
//  - a 16 entries code FIFO; when it is full, new codes wait.
// The "wraparound" (ghosting) wait of the real keyboard is not emulated.
//
// Threading: the host state (perkb_host_*) is only written by the port
// (PerKeyboardHostKey*), the rest only by the emulation (SMPC side).
//////////////////////////////////////////////////////////////////////////////

#define PERKB_MAX          12   /* 2 ports x 6 multi-tap slots */
#define PERKB_FIFO_SIZE    16
#define PERKB_EV_MAKE      0x0800
#define PERKB_EV_BREAK     0x0100
#define PERKB_EV_REPEAT    0x1000 /* internal: repeated Make, doesn't toggle the locks */
#define PERKB_CODE_PAUSE   0x82
#define PERKB_REPEAT_DELAY 400000 /* us */
#define PERKB_REPEAT_RATE  33333  /* us */

/* 3rd data */
#define PERKB_LOCK_SCROLL  0x10
#define PERKB_LOCK_NUM     0x20
#define PERKB_LOCK_CAPS    0x40
#define PERKB_MAKE         0x08
#define PERKB_BREAK        0x01
#define PERKB_DATA3_ONES   0x06

/* Pad-compatible buttons, as a 16 bit "pressed" mask laid out like the
   1st (high byte) and 2nd (low byte, bits 7-3) data bytes. */
#define PERKB_BTN_RIGHT 0x8000
#define PERKB_BTN_LEFT  0x4000
#define PERKB_BTN_DOWN  0x2000
#define PERKB_BTN_UP    0x1000
#define PERKB_BTN_START 0x0800
#define PERKB_BTN_A     0x0400
#define PERKB_BTN_C     0x0200
#define PERKB_BTN_B     0x0100
#define PERKB_BTN_R     0x0080
#define PERKB_BTN_X     0x0040
#define PERKB_BTN_Y     0x0020
#define PERKB_BTN_Z     0x0010
#define PERKB_BTN_L     0x0008

typedef struct
{
   PerKeyboard_struct * kb;
   u8 down[PERKB_NUM_CODES]; /* key state as already sent to the Saturn */
   u8 seq[PERKB_NUM_CODES];  /* last perkb_host_seq[] value taken into account */
   u16 fifo[PERKB_FIFO_SIZE];
   u8 fifo_rdp, fifo_wrp, fifo_cnt;
   int rep_code;             /* key being repeated, -1 = none */
   s32 rep_us;               /* time left before the next repeated Make */
   u32 last_frame;
   u16 buttons;              /* PERKB_BTN_* currently reported as pressed */
   u8 lock;                  /* PERKB_LOCK_* */
   u8 last_code;
} PerKeyboardState;

static volatile u8 perkb_host_down[PERKB_NUM_CODES];
static volatile u8 perkb_host_seq[PERKB_NUM_CODES]; /* +1 on each host press */
static PerKeyboardState perkb[PERKB_MAX];
static int perkb_count = 0;

void PerKeyboardHostKeyDown(u8 code)
{
   if (code >= PERKB_NUM_CODES || perkb_host_down[code])
      return;
   perkb_host_down[code] = 1;
   perkb_host_seq[code]++;
}

void PerKeyboardHostKeyUp(u8 code)
{
   if (code >= PERKB_NUM_CODES)
      return;
   perkb_host_down[code] = 0;
}

void PerKeyboardHostReleaseAll(void)
{
   int i;
   for (i = 0; i < PERKB_NUM_CODES; i++)
      perkb_host_down[i] = 0;
}

int PerKeyboardIsConnected(void)
{
   return perkb_count > 0;
}

static void PerKeyboardPush(PerKeyboardState * st, u16 ev)
{
   st->fifo[st->fifo_wrp] = ev;
   st->fifo_wrp = (st->fifo_wrp + 1) % PERKB_FIFO_SIZE;
   st->fifo_cnt++;
}

static void PerKeyboardMake(PerKeyboardState * st, int code)
{
   PerKeyboardPush(st, PERKB_EV_MAKE | code);
   st->down[code] = 1;
   st->rep_code = code;
   st->rep_us = PERKB_REPEAT_DELAY;
}

static void PerKeyboardBreak(PerKeyboardState * st, int code)
{
   PerKeyboardPush(st, PERKB_EV_BREAK | code);
   st->down[code] = 0;
   if (st->rep_code == code)
      st->rep_code = -1;
}

/* Turns the host key changes since the last call into Make/Break codes. */
static void PerKeyboardScan(PerKeyboardState * st)
{
   int code;
   s32 elapsed;
   u32 frames = yabsys.frame_count - st->last_frame;

   st->last_frame = yabsys.frame_count;
   if (frames > 60) frames = 60;
   elapsed = (s32)frames * (yabsys.IsPal ? 20000 : 16683);

   /* Repeat of the last key pressed, before the new changes: a key that
      goes down now restarts the cycle with the full TS delay. At most one
      repeated Make per read, and only when nothing else is waiting, so
      repeats never pile up in front of real key changes when the game
      reads the keyboard less often than TR. No repeat once the host key
      has gone up or down again: its Break / new Make comes next. */
   if (st->rep_code >= 0)
   {
      st->rep_us -= elapsed;
      if (st->rep_us <= 0)
      {
         if (st->fifo_cnt == 0 && perkb_host_down[st->rep_code] &&
             perkb_host_seq[st->rep_code] == st->seq[st->rep_code])
            PerKeyboardPush(st, PERKB_EV_MAKE | PERKB_EV_REPEAT | st->rep_code);
         st->rep_us += PERKB_REPEAT_RATE;
         if (st->rep_us <= 0)
            st->rep_us = PERKB_REPEAT_RATE;
      }
   }

   for (code = 0; code < PERKB_NUM_CODES; code++)
   {
      const u8 host_down = perkb_host_down[code];
      const u8 host_seq = perkb_host_seq[code];
      const int pressed = (host_seq != st->seq[code]); /* host press(es) not seen yet */
      int need;

      if (code == PERKB_CODE_PAUSE)
      {
         if (pressed)
         {
            if (st->fifo_cnt > PERKB_FIFO_SIZE - 2)
               break;
            PerKeyboardPush(st, PERKB_EV_MAKE | code);
            PerKeyboardPush(st, PERKB_EV_BREAK | code);
            st->seq[code] = host_seq;
         }
         continue;
      }

      /* Codes needed to bring the Saturn side up to date. A key pressed
         and released between two reads still gives a Make and a Break. */
      need = 0;
      if (st->down[code] && (pressed || !host_down)) need++;          /* Break */
      if ((!st->down[code] || pressed) && (pressed || host_down)) need++; /* Make */
      if (pressed && !host_down) need++;                               /* Break */
      if (need == 0)
         continue;
      if (st->fifo_cnt + need > PERKB_FIFO_SIZE)
         break; /* FIFO full: the remaining changes are sent later */

      if (st->down[code] && (pressed || !host_down))
         PerKeyboardBreak(st, code);
      if (!st->down[code] && (pressed || host_down))
         PerKeyboardMake(st, code);
      if (st->down[code] && !host_down)
         PerKeyboardBreak(st, code);
      st->seq[code] = host_seq;
   }

}

static u16 PerKeyboardButton(int code)
{
   switch (code)
   {
      case 0x8D: return PERKB_BTN_RIGHT; /* -> */
      case 0x86: return PERKB_BTN_LEFT;  /* <- */
      case 0x8A: return PERKB_BTN_DOWN;  /* down arrow */
      case 0x89: return PERKB_BTN_UP;    /* up arrow */
      case 0x76: return PERKB_BTN_START; /* Esc */
      case 0x1A: return PERKB_BTN_A;     /* Z */
      case 0x22: return PERKB_BTN_B;     /* X */
      case 0x21: return PERKB_BTN_C;     /* C */
      case 0x1C: return PERKB_BTN_X;     /* A */
      case 0x1B: return PERKB_BTN_Y;     /* S */
      case 0x23: return PERKB_BTN_Z;     /* D */
      case 0x15: return PERKB_BTN_L;     /* Q */
      case 0x24: return PERKB_BTN_R;     /* E */
      default: return 0;
   }
}

/* Sends the next code (if any) and refreshes the 4 data bytes. */
static void PerKeyboardOutput(PerKeyboardState * st)
{
   u8 mkbrk = 0;

   if (st->fifo_cnt)
   {
      const u16 ev = st->fifo[st->fifo_rdp];
      const int code = ev & 0xFF;
      const int make = (ev & PERKB_EV_MAKE) != 0;
      const u16 btn = PerKeyboardButton(code);

      st->fifo_rdp = (st->fifo_rdp + 1) % PERKB_FIFO_SIZE;
      st->fifo_cnt--;

      if (btn)
      {
         if (make)
         {
            st->buttons |= btn;
            if (btn == PERKB_BTN_UP)    st->buttons &= ~PERKB_BTN_DOWN;
            if (btn == PERKB_BTN_DOWN)  st->buttons &= ~PERKB_BTN_UP;
            if (btn == PERKB_BTN_LEFT)  st->buttons &= ~PERKB_BTN_RIGHT;
            if (btn == PERKB_BTN_RIGHT) st->buttons &= ~PERKB_BTN_LEFT;
         }
         else
            st->buttons &= ~btn;
      }

      if (make && !(ev & PERKB_EV_REPEAT))
      {
         if (code == 0x58) st->lock ^= PERKB_LOCK_CAPS;   /* CapsLock */
         if (code == 0x77) st->lock ^= PERKB_LOCK_NUM;    /* NumLock */
         if (code == 0x7E) st->lock ^= PERKB_LOCK_SCROLL; /* ScrLock */
      }

      mkbrk = make ? PERKB_MAKE : PERKB_BREAK;
      st->last_code = code;
   }

   st->kb->kbbits[0] = ~(st->buttons >> 8) & 0xFF;
   st->kb->kbbits[1] = (~st->buttons & 0xF8) | 0x00; /* KB TYPE 000 = Saturn keyboard */
   st->kb->kbbits[2] = st->lock | mkbrk | PERKB_DATA3_ONES;
   st->kb->kbbits[3] = st->last_code;
}

void PerKeyboardUpdate(PortData_struct * port)
{
   int i;

   for (i = 0; i < perkb_count; i++)
   {
      PerKeyboardState * st = &perkb[i];
      const u8 * p = (const u8 *)st->kb;

      if (p < port->data || p >= port->data + sizeof(port->data))
         continue;

      PerKeyboardScan(st);
      PerKeyboardOutput(st);
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerFlush(PortData_struct * port)
{
   /* FIXME this function only flush data if there's a mouse connected as
    * first peripheral */
  u8 perid = port->data[1];
  if (perid == 0xE3)
  {
     PerMouse_struct * mouse = (PerMouse_struct *) (port->data + 1);

     mouse->mousebits[0] &= 0x0F;
     mouse->mousebits[1] = 0;
     mouse->mousebits[2] = 0;
  }
}

//////////////////////////////////////////////////////////////////////////////

void PerKeyDown(u32 key)
{
	unsigned int i = 0;

	while(i < perkeyconfigsize)
	{
		if (key == perkeyconfig[i].key)
		{
			if (perkeyconfig[i].base->Press) perkeyconfig[i].base->Press(perkeyconfig[i].controller);
		}
		i++;
	}
}

//////////////////////////////////////////////////////////////////////////////

void PerKeyUp(u32 key)
{
	unsigned int i = 0;

	while(i < perkeyconfigsize)
	{
		if (key == perkeyconfig[i].key)
		{
			if (perkeyconfig[i].base->Release) perkeyconfig[i].base->Release(perkeyconfig[i].controller);
		}
		i++;
	}
}

//////////////////////////////////////////////////////////////////////////////

void PerSetKey(u32 key, u8 name, void * controller)
{
	unsigned int i = 0;
	while(i < perkeyconfigsize)
	{
		if ((name == perkeyconfig[i].base->name) && (controller == perkeyconfig[i].controller))
		{
			perkeyconfig[i].key = key;
		}
		i++;
	}
}

//////////////////////////////////////////////////////////////////////////////

void PerAxisValue(u32 key, u8 val)
{
   unsigned int i = 0;

   while(i < perkeyconfigsize)
   {
      if (key == perkeyconfig[i].key)
      {
         if (perkeyconfig[i].base->SetAxisValue)
            perkeyconfig[i].base->SetAxisValue(perkeyconfig[i].controller, val);
      }
      i++;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerAxisMove(u32 key, s32 dispx, s32 dispy)
{
   unsigned int i = 0;

   while(i < perkeyconfigsize)
   {
      if (key == perkeyconfig[i].key)
      {
         if (perkeyconfig[i].base->MoveAxis)
            perkeyconfig[i].base->MoveAxis(perkeyconfig[i].controller, dispx, dispy);
      }
      i++;
   }
}

//////////////////////////////////////////////////////////////////////////////

void PerPortReset(void)
{
  int i;
        PORTDATA1.data[0] = 0xF0;
        PORTDATA1.size = 1;
        PORTDATA2.data[0] = 0xF0;
        PORTDATA2.size = 1;

        for (i=0; i<ioPortMAX; i++)
          IOPORT[i] = 0xFF; //IOPORT are in pull up mode.
        for (i=0; i<256; i++)
          IOkeys[i] = NULL;

        /* Hopper sensor line idles low (0), unlike the general pull-up
           default above, so force it back down for games that have one.
           PORT-A bit 0 (Button 2 / "hopper ?") and PORT-B bit 1 (door
           switch) are likewise active-high/idle-low on these boards. */
        hopper_motor = 0;
        hopper_status = 0;
        hopper_stopping = 0;
        hopper_frame_count = 0;
        hopper_test_override = 0;
        if (PerHopperEnabled()) {
          IOPORT[HOPPER_SENSOR_PORT] &= ~HOPPER_SENSOR_MASK;
          IOPORT[PORT_A] &= ~0x01;
          IOPORT[PORT_B] &= ~0x02;
        }

        IOPORT_COUNTER[0] = IOPORT_COUNTER[1] = IOPORT_COUNTER[2] = IOPORT_COUNTER[3] = 0;
        trackball_up = trackball_down = trackball_left = trackball_right = 0;

        /* Mahjong Panel rows are pull-up too, and no row is selected until
           the game writes PORT-E for the first time. */
        for (i=0; i<10; i++)
          MJPORT[i] = 0xFF;
        mj_mux_data = 0;

        /* Keyboards are plugged in again by the port (PerKeyboardAdd). The
           host key state is kept: keys still held are sent again as Make. */
        perkb_count = 0;

	perkeyconfigsize = 0;
        if (perkeyconfig)
           free(perkeyconfig);
	perkeyconfig = NULL;
}

//////////////////////////////////////////////////////////////////////////////

void PerUpdateConfig(PerBaseConfig_struct * baseconfig, int nelems, void * controller)
{
   u32 oldsize = perkeyconfigsize;
   u32 i, j;

   perkeyconfigsize += nelems;

	 PerConfig_struct *new_data = (PerConfig_struct*)realloc(perkeyconfig, perkeyconfigsize * sizeof(PerConfig_struct));
 	if (new_data == NULL)
 	{
		YuiMsg("Peripheral realloc Error\n");
 	} else {
     perkeyconfig = new_data;
 	}

   j = 0;
   for(i = oldsize;i < perkeyconfigsize;i++)
   {
      perkeyconfig[i].base = baseconfig + j;
      perkeyconfig[i].controller = controller;
			perkeyconfig[i].key = -1;
      j++;
   }
}

//////////////////////////////////////////////////////////////////////////////

PerPad_struct * PerPadAdd(PortData_struct * port)
{
   return (PerPad_struct *) PerAddPeripheral(port, PERPAD);
}

//////////////////////////////////////////////////////////////////////////////

PerMouse_struct * PerMouseAdd(PortData_struct * port)
{
   return (PerMouse_struct *) PerAddPeripheral(port, PERMOUSE);
}

//////////////////////////////////////////////////////////////////////////////

PerAnalog_struct * PerWheelAdd(PortData_struct * port)
{
   return (PerAnalog_struct *) PerAddPeripheral(port, PERWHEEL);
}

//////////////////////////////////////////////////////////////////////////////

PerAnalog_struct * PerMissionStickAdd(PortData_struct * port)
{
   return (PerAnalog_struct *) PerAddPeripheral(port, PERMISSIONSTICK);
}

//////////////////////////////////////////////////////////////////////////////

PerAnalog_struct * Per3DPadAdd(PortData_struct * port)
{
   return (PerAnalog_struct *) PerAddPeripheral(port, PER3DPAD);
}

//////////////////////////////////////////////////////////////////////////////

PerAnalog_struct * PerTwinSticksAdd(PortData_struct * port)
{
   return (PerAnalog_struct *) PerAddPeripheral(port, PERTWINSTICKS);
}

//////////////////////////////////////////////////////////////////////////////

PerGun_struct * PerGunAdd(PortData_struct * port)
{
   return (PerGun_struct *) PerAddPeripheral(port, PERGUN);
}

//////////////////////////////////////////////////////////////////////////////

PerKeyboard_struct * PerKeyboardAdd(PortData_struct * port)
{
   PerKeyboard_struct * kb;
   PerKeyboardState * st;
   int i;

   if (perkb_count >= PERKB_MAX)
      return NULL;

   kb = (PerKeyboard_struct *) PerAddPeripheral(port, PERKEYBOARD);
   if (kb == NULL)
      return NULL;

   st = &perkb[perkb_count++];
   memset(st, 0, sizeof(*st));
   st->kb = kb;
   st->rep_code = -1;
   st->last_frame = yabsys.frame_count;
   /* Keys already held when the keyboard is plugged in are sent as Make
      on the first read (down[] = 0, host_down = 1), not twice. */
   for (i = 0; i < PERKB_NUM_CODES; i++)
      st->seq[i] = perkb_host_seq[i];

   return kb;
}

PerCab_struct * PerCabAdd(PortData_struct * port)
{
  PerUpdateConfig(percabinetbaseconfig, sizeof(percabinetbaseconfig)/sizeof(PerBaseConfig_struct), IOPORT);
  return IOPORT;
}

//////////////////////////////////////////////////////////////////////////////
// Dummy Interface
//////////////////////////////////////////////////////////////////////////////

int PERDummyInit(void);
void PERDummyDeInit(void);
int PERDummyHandleEvents(void);

u32 PERDummyScan(u32 flags);
void PERDummyFlush(void);
void PERDummyKeyName(u32 key, char * name, int size);

PerInterface_struct PERDummy = {
PERCORE_DUMMY,
"Dummy Input Interface",
PERDummyInit,
PERDummyDeInit,
PERDummyHandleEvents,
PERDummyScan,
0,
PERDummyFlush,
PERDummyKeyName
};

//////////////////////////////////////////////////////////////////////////////

int PERDummyInit(void) {

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void PERDummyDeInit(void) {
}

//////////////////////////////////////////////////////////////////////////////

int PERDummyHandleEvents(void) {

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 PERDummyScan(u32 flags) {
   // Scan and return next action based on flags value
   // See PERSF_* in peripheral.h for full list of flags.
   // If no specified flags are supported return 0

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void PERDummyFlush(void) {
}

//////////////////////////////////////////////////////////////////////////////

void PERDummyKeyName(UNUSED u32 key, char * name, UNUSED int size) {
	*name = 0;
}
