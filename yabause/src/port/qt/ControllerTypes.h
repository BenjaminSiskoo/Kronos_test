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
#ifndef CONTROLLERTYPES_H
#define CONTROLLERTYPES_H

#include "peripheral.h"

/*
   Controller types of the Qt port that are not a peripheral ID of their
   own: the Saturn sees one of the peripherals of peripheral.h, the type
   only changes the bindings, the dialog or the peripheral options. They are
   stored as-is in Input/Port/N/Id/M/Type (unlike PERCABINET_HOPPER /
   PERCABINET_MAHJONG, see UIPortManager.h) and YabauseThread::
   reloadControllers() turns them into the right peripheral. Real peripheral
   IDs are bytes (<= FFh), so values above FFh can't collide; the low byte
   is the ID the Saturn sees.

   - PERPAD_TWINSTICK: Sega Saturn Twin Stick (HSS-0151, Cyber Troopers
     Virtual-On). STN-50 "Twin Stick User's Manual" Ver.0.90: same hardware
     and data format as the standard pad (ID 02h), the two sticks are wired
     to pad buttons. Same PERPAD_* bindings, the dialog names them after the
     sticks.
   - PERPAD_JOYCARD: Hudson SBom Joycard (HC-735), a standard pad with turbo
     and SLOW / Hu switches (PerJoycardAdd(), peripheral.h).
   - PERKEYBOARD_NETLINK: NetLink Keyboard (US) / PS/2 keyboard through the
     NetLink Keyboard Adapter, same 34h keyboard, PERKB_MODEL_NETLINK_US.
     PERKEYBOARD itself is the Japanese Saturn Keyboard (HSS-0129).
*/
#define PERPAD_TWINSTICK    (PERPAD | 0x100)
#define PERPAD_JOYCARD      (PERPAD | 0x200)
#define PERKEYBOARD_NETLINK (PERKEYBOARD | 0x100)

/* Settings shared by the port manager and YabauseThread. */
#define SETTINGS_PORT_MULTITAP      "Input/Port/%1/Multitap"     /* bool: 6Player plugged in port %1 */
#define SETTINGS_OVERRIDE_MULTITAP  "Input/Override/Multitap%1"  /* volatile, --multitap */
#define SETTINGS_JOYCARD_TURBO_RATE "Input/Joycard/TurboRate"    /* presses per second */
#define JOYCARD_TURBO_RATE_DEFAULT  15

#endif // CONTROLLERTYPES_H
