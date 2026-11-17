/*	Copyright 2026 The Kronos Team

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
#include "UIMahjongSetting.h"
#include "UIPortManager.h"
#include "../Settings.h"
#include "stv.h"

#include <QKeyEvent>
#include <QTimer>
#include <QStylePainter>
#include <QStyleOptionToolButton>

UIMahjongSetting::UIMahjongSetting( PerInterface_struct* core, uint port, uint pad, uint perType, QWidget* parent )
	: UIControllerSetting( core, port, pad, perType, parent )
{
	setupUi( this );
	setInfos(lInfos);

	// Every tile/action button on both panel halves - see the row/bit
	// layout comment above PERMAHJONG_A in peripheral.h. All of them are
	// plain digital buttons, so they all share the same scan mask.
	mButtons[ tbP1Kan ] = PERMAHJONG_KAN;
	mButtons[ tbP1Start ] = PERMAHJONG_START;
	mButtons[ tbP1E ] = PERMAHJONG_E;
	mButtons[ tbP1A ] = PERMAHJONG_A;
	mButtons[ tbP1M ] = PERMAHJONG_M;
	mButtons[ tbP1I ] = PERMAHJONG_I;
	mButtons[ tbP1Reach ] = PERMAHJONG_REACH;
	mButtons[ tbP1Bet ] = PERMAHJONG_BET;
	mButtons[ tbP1F ] = PERMAHJONG_F;
	mButtons[ tbP1B ] = PERMAHJONG_B;
	mButtons[ tbP1N ] = PERMAHJONG_N;
	mButtons[ tbP1J ] = PERMAHJONG_J;
	mButtons[ tbP1Ron ] = PERMAHJONG_RON;
	mButtons[ tbP1G ] = PERMAHJONG_G;
	mButtons[ tbP1C ] = PERMAHJONG_C;
	mButtons[ tbP1Chi ] = PERMAHJONG_CHI;
	mButtons[ tbP1K ] = PERMAHJONG_K;
	mButtons[ tbP1H ] = PERMAHJONG_H;
	mButtons[ tbP1D ] = PERMAHJONG_D;
	mButtons[ tbP1Pon ] = PERMAHJONG_PON;
	mButtons[ tbP1L ] = PERMAHJONG_L;
	mButtons[ tbP1FlipFlop ] = PERMAHJONG_FLIP_FLOP;
	mButtons[ tbP2Kan ] = PERMAHJONG_P2_KAN;
	mButtons[ tbP2Start ] = PERMAHJONG_P2_START;
	mButtons[ tbP2E ] = PERMAHJONG_P2_E;
	mButtons[ tbP2A ] = PERMAHJONG_P2_A;
	mButtons[ tbP2M ] = PERMAHJONG_P2_M;
	mButtons[ tbP2I ] = PERMAHJONG_P2_I;
	mButtons[ tbP2Reach ] = PERMAHJONG_P2_REACH;
	mButtons[ tbP2F ] = PERMAHJONG_P2_F;
	mButtons[ tbP2B ] = PERMAHJONG_P2_B;
	mButtons[ tbP2N ] = PERMAHJONG_P2_N;
	mButtons[ tbP2J ] = PERMAHJONG_P2_J;
	mButtons[ tbP2Ron ] = PERMAHJONG_P2_RON;
	mButtons[ tbP2G ] = PERMAHJONG_P2_G;
	mButtons[ tbP2C ] = PERMAHJONG_P2_C;
	mButtons[ tbP2Chi ] = PERMAHJONG_P2_CHI;
	mButtons[ tbP2K ] = PERMAHJONG_P2_K;
	mButtons[ tbP2H ] = PERMAHJONG_P2_H;
	mButtons[ tbP2D ] = PERMAHJONG_P2_D;
	mButtons[ tbP2Pon ] = PERMAHJONG_P2_PON;
	mButtons[ tbP2L ] = PERMAHJONG_P2_L;
	mButtons[ tbP2FlipFlop ] = PERMAHJONG_P2_FLIP_FLOP;

	mNames[ PERMAHJONG_KAN ] = QtYabause::translate( "P1 - Kan" );
	mNames[ PERMAHJONG_START ] = QtYabause::translate( "P1 - Start" );
	mNames[ PERMAHJONG_E ] = QtYabause::translate( "P1 - E" );
	mNames[ PERMAHJONG_A ] = QtYabause::translate( "P1 - A" );
	mNames[ PERMAHJONG_M ] = QtYabause::translate( "P1 - M" );
	mNames[ PERMAHJONG_I ] = QtYabause::translate( "P1 - I" );
	mNames[ PERMAHJONG_REACH ] = QtYabause::translate( "P1 - Reach" );
	mNames[ PERMAHJONG_BET ] = QtYabause::translate( "P1 - Bet" );
	mNames[ PERMAHJONG_F ] = QtYabause::translate( "P1 - F" );
	mNames[ PERMAHJONG_B ] = QtYabause::translate( "P1 - B" );
	mNames[ PERMAHJONG_N ] = QtYabause::translate( "P1 - N" );
	mNames[ PERMAHJONG_J ] = QtYabause::translate( "P1 - J" );
	mNames[ PERMAHJONG_RON ] = QtYabause::translate( "P1 - Ron" );
	mNames[ PERMAHJONG_G ] = QtYabause::translate( "P1 - G" );
	mNames[ PERMAHJONG_C ] = QtYabause::translate( "P1 - C" );
	mNames[ PERMAHJONG_CHI ] = QtYabause::translate( "P1 - Chi" );
	mNames[ PERMAHJONG_K ] = QtYabause::translate( "P1 - K" );
	mNames[ PERMAHJONG_H ] = QtYabause::translate( "P1 - H" );
	mNames[ PERMAHJONG_D ] = QtYabause::translate( "P1 - D" );
	mNames[ PERMAHJONG_PON ] = QtYabause::translate( "P1 - Pon" );
	mNames[ PERMAHJONG_L ] = QtYabause::translate( "P1 - L" );
	mNames[ PERMAHJONG_FLIP_FLOP ] = QtYabause::translate( "P1 - Flip/Flop" );
	mNames[ PERMAHJONG_P2_KAN ] = QtYabause::translate( "P2 - Kan" );
	mNames[ PERMAHJONG_P2_START ] = QtYabause::translate( "P2 - Start" );
	mNames[ PERMAHJONG_P2_E ] = QtYabause::translate( "P2 - E" );
	mNames[ PERMAHJONG_P2_A ] = QtYabause::translate( "P2 - A" );
	mNames[ PERMAHJONG_P2_M ] = QtYabause::translate( "P2 - M" );
	mNames[ PERMAHJONG_P2_I ] = QtYabause::translate( "P2 - I" );
	mNames[ PERMAHJONG_P2_REACH ] = QtYabause::translate( "P2 - Reach" );
	mNames[ PERMAHJONG_P2_F ] = QtYabause::translate( "P2 - F" );
	mNames[ PERMAHJONG_P2_B ] = QtYabause::translate( "P2 - B" );
	mNames[ PERMAHJONG_P2_N ] = QtYabause::translate( "P2 - N" );
	mNames[ PERMAHJONG_P2_J ] = QtYabause::translate( "P2 - J" );
	mNames[ PERMAHJONG_P2_RON ] = QtYabause::translate( "P2 - Ron" );
	mNames[ PERMAHJONG_P2_G ] = QtYabause::translate( "P2 - G" );
	mNames[ PERMAHJONG_P2_C ] = QtYabause::translate( "P2 - C" );
	mNames[ PERMAHJONG_P2_CHI ] = QtYabause::translate( "P2 - Chi" );
	mNames[ PERMAHJONG_P2_K ] = QtYabause::translate( "P2 - K" );
	mNames[ PERMAHJONG_P2_H ] = QtYabause::translate( "P2 - H" );
	mNames[ PERMAHJONG_P2_D ] = QtYabause::translate( "P2 - D" );
	mNames[ PERMAHJONG_P2_PON ] = QtYabause::translate( "P2 - Pon" );
	mNames[ PERMAHJONG_P2_L ] = QtYabause::translate( "P2 - L" );
	mNames[ PERMAHJONG_P2_FLIP_FLOP ] = QtYabause::translate( "P2 - Flip/Flop" );

	mScanMasks[ PERMAHJONG_KAN ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_START ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_E ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_A ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_M ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_I ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_REACH ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_BET ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_F ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_B ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_N ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_J ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_RON ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_G ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_C ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_CHI ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_K ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_H ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_D ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_PON ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_L ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_FLIP_FLOP ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_KAN ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_START ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_E ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_A ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_M ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_I ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_REACH ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_F ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_B ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_N ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_J ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_RON ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_G ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_C ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_CHI ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_K ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_H ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_D ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_PON ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_L ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERMAHJONG_P2_FLIP_FLOP ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;

	loadPadSettings();

	foreach ( QToolButton* tb, findChildren<QToolButton*>() )
	{
		/* No installEventFilter(): the base class eventFilter() repaints a checked
		   button with the native style, which would hide the style sheet. The
		   "awaiting input" state is drawn by the :checked rule. */
		connect( tb, SIGNAL( clicked() ), this, SLOT( tbButton_clicked() ) );
	}

	connect( mTimer, SIGNAL( timeout() ), this, SLOT( timer_timeout() ) );

	QtYabause::retranslateWidget( this );
}

UIMahjongSetting::~UIMahjongSetting()
{
}
