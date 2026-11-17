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
#include "UIButtonListSetting.h"
#include "../ControllerTypes.h"
#include "../Settings.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QMap>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

static UIButtonListSetting::Entry entry( int id, const QString& name, const QString& hint = QString() )
{
	UIButtonListSetting::Entry e;
	e.id = id;
	e.name = name;
	e.hint = hint;
	return e;
}

static UIButtonListSetting::Entry section( const QString& name )
{
	return entry( -1, name );
}

/* STN-50 "Twin Stick User's Manual" Ver.0.90, table 1 "Key compatibility
   table with standard pad": the pad button each control is wired to is
   given as a hint. */
QList<UIButtonListSetting::Entry> UIButtonListSetting::twinStickEntries()
{
	QList<Entry> l;
	l << section( QtYabause::translate( "Left stick" ) );
	l << entry( PERPAD_UP, QtYabause::translate( "Up" ), "Up" );
	l << entry( PERPAD_DOWN, QtYabause::translate( "Down" ), "Down" );
	l << entry( PERPAD_LEFT, QtYabause::translate( "Left" ), "Left" );
	l << entry( PERPAD_RIGHT, QtYabause::translate( "Right" ), "Right" );
	l << entry( PERPAD_LEFT_TRIGGER, QtYabause::translate( "Trigger" ), "L" );
	l << entry( PERPAD_RIGHT_TRIGGER, QtYabause::translate( "Button" ), "R" );
	l << section( QtYabause::translate( "Right stick" ) );
	l << entry( PERPAD_Y, QtYabause::translate( "Up" ), "Y" );
	l << entry( PERPAD_B, QtYabause::translate( "Down" ), "B" );
	l << entry( PERPAD_X, QtYabause::translate( "Left" ), "X" );
	l << entry( PERPAD_Z, QtYabause::translate( "Right" ), "Z" );
	l << entry( PERPAD_A, QtYabause::translate( "Trigger" ), "A" );
	l << entry( PERPAD_C, QtYabause::translate( "Button" ), "C" );
	l << section( QtYabause::translate( "Panel" ) );
	l << entry( PERPAD_START, "Start", "Start" );
	return l;
}

QList<UIButtonListSetting::Entry> UIButtonListSetting::joycardEntries()
{
	QList<Entry> l;
	l << section( QtYabause::translate( "Buttons" ) );
	l << entry( PERPAD_UP, QtYabause::translate( "Up" ) );
	l << entry( PERPAD_DOWN, QtYabause::translate( "Down" ) );
	l << entry( PERPAD_LEFT, QtYabause::translate( "Left" ) );
	l << entry( PERPAD_RIGHT, QtYabause::translate( "Right" ) );
	l << entry( PERPAD_START, "Start" );
	l << entry( PERPAD_A, "A" );
	l << entry( PERPAD_B, "B" );
	l << entry( PERPAD_C, "C" );
	l << entry( PERPAD_X, "X" );
	l << entry( PERPAD_Y, "Y" );
	l << entry( PERPAD_Z, "Z" );
	l << entry( PERPAD_LEFT_TRIGGER, QtYabause::translate( "Left trigger" ) );
	l << entry( PERPAD_RIGHT_TRIGGER, QtYabause::translate( "Right trigger" ) );
	const QString turboHint = QtYabause::translate( "Held: the button is pressed with turbo (turbo switch on)" );
	l << section( QtYabause::translate( "Turbo" ) );
	l << entry( PERJOYCARD_TURBO_A, "A", turboHint );
	l << entry( PERJOYCARD_TURBO_B, "B", turboHint );
	l << entry( PERJOYCARD_TURBO_C, "C", turboHint );
	l << entry( PERJOYCARD_TURBO_X, "X", turboHint );
	l << entry( PERJOYCARD_TURBO_Y, "Y", turboHint );
	l << entry( PERJOYCARD_TURBO_Z, "Z", turboHint );
	l << entry( PERJOYCARD_TURBO_L, QtYabause::translate( "Left trigger" ), turboHint );
	l << entry( PERJOYCARD_TURBO_R, QtYabause::translate( "Right trigger" ), turboHint );
	l << section( QtYabause::translate( "SLOW switch" ) );
	l << entry( PERJOYCARD_SLOW, QtYabause::translate( "SLOW position" ),
		QtYabause::translate( "Held: switch in the SLOW position (Start pressed repeatedly, slow motion)" ) );
	l << entry( PERJOYCARD_HU, QtYabause::translate( "Hu position" ),
		QtYabause::translate( "Held: switch in the Hu position (Start 15 times per second, unlocks extras in Hudson games)" ) );
	return l;
}

namespace
{
/* ---- Dialog layout ------------------------------------------------------
   The dialogs are drawn with absolute positions, like the ST-V cabinet one:
   panels (QFrame), titles, direction "gates", and one QToolButton per input.
   Item kinds: P panel, T title (flag: prefix the status text with it),
   G gate, B ball, K key button (flag: show the pad mapping under the name),
   N label of the turbo speed box, S turbo speed box. */
struct Item
{
	char kind;
	int id;            /* PERPAD_* / PERJOYCARD_* for K, -1 otherwise */
	int x, y, w, h;
	const char* role;  /* style sheet role */
	const char* text;  /* title / caption, "" = use the entry name */
	unsigned short glyph; /* arrow character used as caption, 0 = none */
	int flag;
};

struct Layout
{
	int w, h, hintY;
	const char* css;
	const Item* items;
	int count;
};

static const Item sTwinStickItems[] =
{
	{ 'P', -1, 20, 20, 330, 330, "", "", 0x0000, 0 },
	{ 'T', -1, 38, 28, 290, 20, "title", "Left stick", 0x0000, 1 },
	{ 'G', -1, 107, 92, 156, 156, "", "", 0x0000, 0 },
	{ 'B', -1, 150, 135, 70, 70, "", "", 0x0000, 0 },
	{ 'K', PERPAD_UP, 163, 86, 44, 44, "dir", "", 0x25B2, 0 },
	{ 'K', PERPAD_RIGHT, 225, 148, 44, 44, "dir", "", 0x25B6, 0 },
	{ 'K', PERPAD_DOWN, 163, 210, 44, 44, "dir", "", 0x25BC, 0 },
	{ 'K', PERPAD_LEFT, 101, 148, 44, 44, "dir", "", 0x25C0, 0 },
	{ 'K', PERPAD_LEFT_TRIGGER, 56, 272, 120, 58, "trig", "", 0x0000, 1 },
	{ 'K', PERPAD_RIGHT_TRIGGER, 194, 272, 120, 58, "btn", "", 0x0000, 1 },
	{ 'P', -1, 370, 20, 330, 330, "", "", 0x0000, 0 },
	{ 'T', -1, 388, 28, 290, 20, "title", "Right stick", 0x0000, 1 },
	{ 'G', -1, 457, 92, 156, 156, "", "", 0x0000, 0 },
	{ 'B', -1, 500, 135, 70, 70, "", "", 0x0000, 0 },
	{ 'K', PERPAD_Y, 513, 86, 44, 44, "dir", "", 0x25B2, 0 },
	{ 'K', PERPAD_Z, 575, 148, 44, 44, "dir", "", 0x25B6, 0 },
	{ 'K', PERPAD_B, 513, 210, 44, 44, "dir", "", 0x25BC, 0 },
	{ 'K', PERPAD_X, 451, 148, 44, 44, "dir", "", 0x25C0, 0 },
	{ 'K', PERPAD_A, 406, 272, 120, 58, "trig", "", 0x0000, 1 },
	{ 'K', PERPAD_C, 544, 272, 120, 58, "btn", "", 0x0000, 1 },
	{ 'P', -1, 20, 364, 680, 64, "", "", 0x0000, 0 },
	{ 'T', -1, 38, 372, 300, 20, "title", "Panel", 0x0000, 0 },
	{ 'K', PERPAD_START, 300, 384, 120, 36, "start", "Start", 0x0000, 0 },
};

static const char sTwinStickCss[] =
	"QDialog#UIButtonListSetting { background:#0e141c; }\n"
	"QFrame[role=\"panel\"] { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #1b2634,stop:1 #121a25); border:1px solid #31425a; border-radius:12px; }\n"
	"QFrame[role=\"gate\"] { background:#0b1017; border:2px solid #31425a; border-radius:78px; }\n"
	"QFrame[role=\"ball\"] { background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #9ee0ff,stop:1 #16466b); border:1px solid #000000; border-radius:35px; }\n"
	"QToolButton { font-weight:bold; font-size:11px; padding:0px; }\n"
	"QToolButton:focus { outline:none; }\n"
	"QToolButton[role=\"dir\"] { color:#ffffff; font-size:12px; border-radius:22px; border:2px solid rgba(0,0,0,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #8da2bd,stop:1 #2f3d52); }\n"
	"QToolButton[role=\"dir\"]:hover { border:2px solid rgba(255,255,255,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #aabfd9,stop:1 #42536b); }\n"
	"QToolButton[role=\"dir\"]:checked { border:3px solid #ff9d2e; }\n"
	"QToolButton[role=\"dir\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"trig\"] { color:#ffffff; font-size:11px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #ff9a4a,stop:1 #b3470a); }\n"
	"QToolButton[role=\"trig\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #ffb877,stop:1 #d2590f); }\n"
	"QToolButton[role=\"trig\"]:checked { border:3px solid #ff9d2e; }\n"
	"QToolButton[role=\"trig\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"btn\"] { color:#ffffff; font-size:11px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #6aa8ff,stop:1 #1f55b8); }\n"
	"QToolButton[role=\"btn\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #93c0ff,stop:1 #2f6fd6); }\n"
	"QToolButton[role=\"btn\"]:checked { border:3px solid #ff9d2e; }\n"
	"QToolButton[role=\"btn\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"start\"] { color:#ffffff; font-size:12px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #4cc16b,stop:1 #25773c); }\n"
	"QToolButton[role=\"start\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #6ddc8b,stop:1 #2f9150); }\n"
	"QToolButton[role=\"start\"]:checked { border:3px solid #ff9d2e; }\n"
	"QToolButton[role=\"start\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QLabel[role=\"title\"] { color:#8fb2d9; font-weight:bold; font-size:12px; letter-spacing:2px; }\n"
	"QLabel[role=\"note\"] { color:#7f93ad; font-size:10px; }\n"
	"QLabel#lTitle, QLabel[role=\"hint\"] { color:#7f93ad; font-size:11px; }\n"
	"QLabel#lInfos { color:#ff9d2e; font-weight:bold; font-size:13px; }\n"
	"QLabel[role=\"spinlabel\"] { color:#8fb2d9; font-size:11px; }\n"
	"QSpinBox { background:#121a25; color:#dfe9f7; border:1px solid #31425a; border-radius:5px; padding:1px 4px; }\n"
	"QToolTip { background:#1f2530; color:#e6ecf7; border:1px solid #3a4352; padding:3px; }\n"
	"QDialogButtonBox QPushButton { background:#243246; color:#dfe9f7; border:1px solid #31425a; border-radius:6px; padding:6px 18px; min-width:70px; }\n"
	"QDialogButtonBox QPushButton:hover { background:#2f4160; }\n"
	"QDialogButtonBox QPushButton:pressed { background:#1b2634; }\n";

static const Item sJoycardItems[] =
{
	{ 'P', -1, 20, 20, 720, 284, "", "", 0x0000, 0 },
	{ 'T', -1, 38, 28, 300, 20, "title", "Buttons", 0x0000, 0 },
	{ 'K', PERPAD_LEFT_TRIGGER, 44, 56, 150, 34, "shoulder", "", 0x0000, 0 },
	{ 'K', PERPAD_RIGHT_TRIGGER, 566, 56, 150, 34, "shoulder", "", 0x0000, 0 },
	{ 'G', -1, 72, 118, 156, 156, "", "", 0x0000, 0 },
	{ 'K', PERPAD_UP, 128, 116, 44, 44, "dir", "", 0x25B2, 0 },
	{ 'K', PERPAD_RIGHT, 186, 174, 44, 44, "dir", "", 0x25B6, 0 },
	{ 'K', PERPAD_DOWN, 128, 232, 44, 44, "dir", "", 0x25BC, 0 },
	{ 'K', PERPAD_LEFT, 70, 174, 44, 44, "dir", "", 0x25C0, 0 },
	{ 'K', PERPAD_START, 330, 236, 100, 36, "start", "Start", 0x0000, 0 },
	{ 'K', PERPAD_X, 482, 128, 56, 56, "xyz", "X", 0x0000, 0 },
	{ 'K', PERPAD_Y, 552, 118, 56, 56, "xyz", "Y", 0x0000, 0 },
	{ 'K', PERPAD_Z, 622, 108, 56, 56, "xyz", "Z", 0x0000, 0 },
	{ 'K', PERPAD_A, 482, 200, 56, 56, "abc", "A", 0x0000, 0 },
	{ 'K', PERPAD_B, 552, 190, 56, 56, "abc", "B", 0x0000, 0 },
	{ 'K', PERPAD_C, 622, 180, 56, 56, "abc", "C", 0x0000, 0 },
	{ 'P', -1, 20, 316, 720, 100, "", "", 0x0000, 0 },
	{ 'T', -1, 38, 324, 300, 20, "title", "Turbo", 0x0000, 1 },
	{ 'N', -1, 380, 322, 250, 22, "spinlabel", "Turbo speed (presses per second)", 0x0000, 0 },
	{ 'S', -1, 642, 322, 76, 24, "", "", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_A, 38, 358, 78, 40, "turbo", "A", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_B, 123, 358, 78, 40, "turbo", "B", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_C, 208, 358, 78, 40, "turbo", "C", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_X, 293, 358, 78, 40, "turbo", "X", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_Y, 378, 358, 78, 40, "turbo", "Y", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_Z, 463, 358, 78, 40, "turbo", "Z", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_L, 548, 358, 78, 40, "turbo", "L", 0x0000, 0 },
	{ 'K', PERJOYCARD_TURBO_R, 633, 358, 78, 40, "turbo", "R", 0x0000, 0 },
	{ 'P', -1, 20, 428, 720, 84, "", "", 0x0000, 0 },
	{ 'T', -1, 38, 436, 300, 20, "title", "SLOW switch", 0x0000, 0 },
	{ 'K', PERJOYCARD_SLOW, 38, 466, 200, 36, "sw", "", 0x0000, 0 },
	{ 'K', PERJOYCARD_HU, 250, 466, 200, 36, "sw", "", 0x0000, 0 },
};

static const char sJoycardCss[] =
	"QDialog#UIButtonListSetting { background:#17110d; }\n"
	"QFrame[role=\"panel\"] { background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #2d221a,stop:1 #201812); border:1px solid #52402f; border-radius:12px; }\n"
	"QFrame[role=\"gate\"] { background:#120d0a; border:2px solid #52402f; border-radius:78px; }\n"
	"QFrame[role=\"ball\"] { background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #ffffff,stop:1 #999999); border:1px solid #000000; border-radius:35px; }\n"
	"QToolButton { font-weight:bold; font-size:11px; padding:0px; }\n"
	"QToolButton:focus { outline:none; }\n"
	"QToolButton[role=\"dir\"] { color:#ffffff; font-size:12px; border-radius:22px; border:2px solid rgba(0,0,0,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #a39a90,stop:1 #3e3731); }\n"
	"QToolButton[role=\"dir\"]:hover { border:2px solid rgba(255,255,255,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #c0b7ac,stop:1 #554c44); }\n"
	"QToolButton[role=\"dir\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"dir\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"shoulder\"] { color:#ffffff; font-size:11px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #b8ac9e,stop:1 #625850); }\n"
	"QToolButton[role=\"shoulder\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #d3c8ba,stop:1 #7a6f64); }\n"
	"QToolButton[role=\"shoulder\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"shoulder\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"start\"] { color:#ffffff; font-size:12px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #4cc16b,stop:1 #25773c); }\n"
	"QToolButton[role=\"start\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #6ddc8b,stop:1 #2f9150); }\n"
	"QToolButton[role=\"start\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"start\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"abc\"] { color:#2b1600; font-size:13px; border-radius:28px; border:2px solid rgba(0,0,0,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #ffc266,stop:1 #c46a0a); }\n"
	"QToolButton[role=\"abc\"]:hover { border:2px solid rgba(255,255,255,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #ffd592,stop:1 #e07c14); }\n"
	"QToolButton[role=\"abc\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"abc\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"xyz\"] { color:#04201d; font-size:13px; border-radius:28px; border:2px solid rgba(0,0,0,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #6fe0d2,stop:1 #1d7f76); }\n"
	"QToolButton[role=\"xyz\"]:hover { border:2px solid rgba(255,255,255,170); background:qradialgradient(cx:0.35,cy:0.3,radius:0.95,fx:0.35,fy:0.3,stop:0 #97efe4,stop:1 #2a9e93); }\n"
	"QToolButton[role=\"xyz\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"xyz\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"turbo\"] { color:#ffffff; font-size:12px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #a994ec,stop:1 #5b45a8); }\n"
	"QToolButton[role=\"turbo\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #c0b0f6,stop:1 #7358c6); }\n"
	"QToolButton[role=\"turbo\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"turbo\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QToolButton[role=\"sw\"] { color:#ffffff; font-size:11px; border-radius:10px; border:2px solid rgba(0,0,0,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #ef6a5e,stop:1 #a3231a); }\n"
	"QToolButton[role=\"sw\"]:hover { border:2px solid rgba(255,255,255,170); background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #ff8a7e,stop:1 #c4352a); }\n"
	"QToolButton[role=\"sw\"]:checked { border:3px solid #ffe14d; }\n"
	"QToolButton[role=\"sw\"]:disabled { color:#6b6f78; border:2px solid #2a2d33; background:#23262c; }\n"
	"QLabel[role=\"title\"] { color:#d9b27c; font-weight:bold; font-size:12px; letter-spacing:2px; }\n"
	"QLabel[role=\"note\"] { color:#a8917a; font-size:10px; }\n"
	"QLabel#lTitle, QLabel[role=\"hint\"] { color:#a8917a; font-size:11px; }\n"
	"QLabel#lInfos { color:#ffe14d; font-weight:bold; font-size:13px; }\n"
	"QLabel[role=\"spinlabel\"] { color:#d9b27c; font-size:11px; }\n"
	"QSpinBox { background:#201812; color:#f1e4d6; border:1px solid #52402f; border-radius:5px; padding:1px 4px; }\n"
	"QToolTip { background:#1f2530; color:#e6ecf7; border:1px solid #3a4352; padding:3px; }\n"
	"QDialogButtonBox QPushButton { background:#3a2c21; color:#f1e4d6; border:1px solid #52402f; border-radius:6px; padding:6px 18px; min-width:70px; }\n"
	"QDialogButtonBox QPushButton:hover { background:#4c3a2b; }\n"
	"QDialogButtonBox QPushButton:pressed { background:#2d221a; }\n";

static const Layout sTwinStick = { 720, 508, 442, sTwinStickCss, sTwinStickItems, (int)( sizeof( sTwinStickItems ) / sizeof( sTwinStickItems[ 0 ] ) ) };
static const Layout sJoycard = { 760, 592, 524, sJoycardCss, sJoycardItems, (int)( sizeof( sJoycardItems ) / sizeof( sJoycardItems[ 0 ] ) ) };
}

UIButtonListSetting::UIButtonListSetting( PerInterface_struct* core, uint port, uint pad, uint perType,
	const QString& title, const QList<Entry>& entries, QWidget* parent )
	: UIControllerSetting( core, port, pad, perType, parent )
{
	setWindowTitle( title );
	setObjectName( "UIButtonListSetting" );

	const Layout& lay = ( perType == PERPAD_JOYCARD ) ? sJoycard : sTwinStick;
	setFixedSize( lay.w, lay.h );
	setStyleSheet( QString::fromLatin1( lay.css ) );

	QMap<int, Entry> byId;
	foreach ( const Entry& e, entries )
	{
		if ( e.id >= 0 )
			byId[ e.id ] = e;
	}

	const u32 scan = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	QString prefix;

	for ( int n = 0; n < lay.count; n++ )
	{
		const Item& it = lay.items[ n ];
		const QRect r( it.x, it.y, it.w, it.h );

		switch ( it.kind )
		{
			case 'P':
			case 'G':
			case 'B':
			{
				QFrame* f = new QFrame( this );
				f->setProperty( "role", it.kind == 'P' ? "panel" : ( it.kind == 'G' ? "gate" : "ball" ) );
				f->setGeometry( r );
				break;
			}
			case 'T':
			{
				const QString text = QtYabause::translate( QString::fromLatin1( it.text ) );
				QLabel* l = new QLabel( text.toUpper(), this );
				l->setProperty( "role", "title" );
				l->setGeometry( r );
				prefix = it.flag ? text : QString();
				break;
			}
			case 'N':
			{
				QLabel* l = new QLabel( QtYabause::translate( QString::fromLatin1( it.text ) ), this );
				l->setProperty( "role", it.role );
				l->setAlignment( Qt::AlignRight | Qt::AlignVCenter );
				l->setGeometry( r );
				break;
			}
			case 'S':
			{
				QSpinBox* sb = new QSpinBox( this );
				sb->setRange( 1, 30 );
				sb->setValue( QtYabause::settings()->value( SETTINGS_JOYCARD_TURBO_RATE, JOYCARD_TURBO_RATE_DEFAULT ).toInt() );
				sb->setToolTip( QtYabause::translate( "Up to 26 on the real pad. Common to every SBom Joycard." ) );
				sb->setGeometry( r );
				connect( sb, SIGNAL( valueChanged( int ) ), this, SLOT( turboRateChanged( int ) ) );
				break;
			}
			case 'K':
			{
				QMap<int, Entry>::const_iterator ei = byId.constFind( it.id );
				if ( ei == byId.constEnd() )
					break;
				const Entry& e = ei.value();

				/* "Left stick - Up": the same name exists on both sticks */
				const QString statusName = prefix.isEmpty() ? e.name : QString( "%1 - %2" ).arg( prefix ).arg( e.name );

				QString caption;
				if ( it.glyph )
					caption = QString( QChar( it.glyph ) );
				else if ( it.text[ 0 ] )
					caption = QtYabause::translate( QString::fromLatin1( it.text ) );
				else
					caption = e.name;
				if ( it.flag && !e.hint.isEmpty() && e.hint.length() <= 5 )
					caption += QString( "\n[%1]" ).arg( e.hint );

				QToolButton* tb = new QToolButton( this );
				tb->setProperty( "role", it.role );
				tb->setGeometry( r );
				tb->setText( caption );
				tb->setToolTip( e.hint.isEmpty() ? statusName : QString( "%1 (%2)" ).arg( statusName ).arg( e.hint ) );
				tb->setCheckable( true );
				tb->setAutoRaise( false );
				tb->setFocusPolicy( Qt::NoFocus );
				tb->setIconSize( QSize( 12, 12 ) );
				tb->setToolButtonStyle( ( it.w <= 80 || caption.contains( '\n' ) ) ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonTextBesideIcon );

				mButtons[ tb ] = (u8)it.id;
				mNames[ (u8)it.id ] = statusName;
				mScanMasks[ (u8)it.id ] = scan;
				break;
			}
			default:
				break;
		}
	}

	QLabel* hint = new QLabel( QtYabause::translate( "Click a control, then press the key or button to assign. Esc cancels. A check mark shows an assigned control." ), this );
	hint->setProperty( "role", "hint" );
	hint->setGeometry( 24, lay.hintY, lay.w - 48, 16 );

	mInfos = new QLabel( this );
	mInfos->setObjectName( "lInfos" );
	mInfos->setGeometry( 24, lay.hintY + 20, lay.w - 200, 40 );
	setInfos( mInfos );

	QDialogButtonBox* box = new QDialogButtonBox( QDialogButtonBox::Close, this );
	box->setGeometry( lay.w - 144, lay.hintY + 24, 120, 34 );
	connect( box, SIGNAL( rejected() ), this, SLOT( reject() ) );

	loadPadSettings();

	/* No installEventFilter() here: the base class eventFilter() repaints a
	   checked button with the native style, which would hide the style sheet
	   ("awaiting input" is drawn by the :checked rule). */
	foreach ( QToolButton* tb, mButtons.keys() )
		connect( tb, SIGNAL( clicked() ), this, SLOT( tbButton_clicked() ) );

	connect( mTimer, SIGNAL( timeout() ), this, SLOT( timer_timeout() ) );
}

UIButtonListSetting::~UIButtonListSetting()
{
}

void UIButtonListSetting::turboRateChanged( int rate )
{
	QtYabause::settings()->setValue( SETTINGS_JOYCARD_TURBO_RATE, rate );
	/* Takes effect at once if a game is running (a single 32-bit value read
	   by the emulation thread). */
	PerJoycardSetTurboRate( rate );
}
