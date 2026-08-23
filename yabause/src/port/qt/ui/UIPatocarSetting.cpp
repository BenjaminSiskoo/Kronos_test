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
#include "UIPatocarSetting.h"
#include "UIPortManager.h"
#include "../Settings.h"
#include "stv.h"

#include <QKeyEvent>
#include <QTimer>
#include <QStylePainter>
#include <QStyleOptionToolButton>

UIPatocarSetting::UIPatocarSetting( PerInterface_struct* core, uint port, uint pad, uint perType, QWidget* parent )
	: UIControllerSetting( core, port, pad, perType, parent )
{
	setupUi( this );
	setInfos(lInfos);

	// mPerType here is the peripheral kind (always PERCABINET for this
	// dialog) - which of the two hopper control layouts is active
	// depends on the loaded game, given by yabsys.stvInputType instead.
	const bool isMicrombc = ( yabsys.stvInputType == MICROMBC );

	// Controls shared by patocar/skychal/supgoal/techbowl and micrombc.
	// Note: the hopper sensor line itself (PORT-A bit 0x02) is driven
	// automatically by PerHopperExec() and is never user-bindable.
	mButtons[ tbCoin1 ] = PERJAMMA_COIN1;
	mButtons[ tbCoin2 ] = PERJAMMA_COIN2;
	mButtons[ tbStart1 ] = PERJAMMA_START1;
	mButtons[ tbPowerButton ] = PERJAMMA_POWER_BUTTON;
	mButtons[ tbMedal ] = PERJAMMA_MEDAL;
	mButtons[ tbDoorSwitch ] = PERJAMMA_DOOR_SWITCH;
	mButtons[ tbHopperBtn2 ] = PERJAMMA_HOPPER_BTN2;
	mButtons[ tbMagnetBtn5 ] = PERJAMMA_MAGNET_BTN5;
	mButtons[ tbHopperTest ] = PERJAMMA_HOPPER_TEST;
	mButtons[ tbTrackballUp ] = PERJAMMA_TRACKBALL_UP;
	mButtons[ tbTrackballDown ] = PERJAMMA_TRACKBALL_DOWN;
	mButtons[ tbTrackballLeft ] = PERJAMMA_TRACKBALL_LEFT;
	mButtons[ tbTrackballRight ] = PERJAMMA_TRACKBALL_RIGHT;

	mNames[ PERJAMMA_COIN1 ] = "10 Yen";
	mNames[ PERJAMMA_COIN2 ] = "100 Yen";
	mNames[ PERJAMMA_START1 ] = isMicrombc ? QtYabause::translate( "Shoot Button" ) : QtYabause::translate( "Select Button" );
	mNames[ PERJAMMA_POWER_BUTTON ] = QtYabause::translate( "Power Button" );
	mNames[ PERJAMMA_MEDAL ] = QtYabause::translate( "Medal" );
	mNames[ PERJAMMA_DOOR_SWITCH ] = QtYabause::translate( "Door Switch" );
	mNames[ PERJAMMA_HOPPER_BTN2 ] = QtYabause::translate( "Button 2" );
	mNames[ PERJAMMA_MAGNET_BTN5 ] = isMicrombc ? QtYabause::translate( "Magnet Sensor" ) : QtYabause::translate( "Button 5" );
	mNames[ PERJAMMA_HOPPER_TEST ] = QtYabause::translate( "Hopper Sensor Test" );
	mNames[ PERJAMMA_TRACKBALL_UP ] = QtYabause::translate( "Trackball Up" );
	mNames[ PERJAMMA_TRACKBALL_DOWN ] = QtYabause::translate( "Trackball Down" );
	mNames[ PERJAMMA_TRACKBALL_LEFT ] = QtYabause::translate( "Trackball Left" );
	mNames[ PERJAMMA_TRACKBALL_RIGHT ] = QtYabause::translate( "Trackball Right" );

	mScanMasks[ PERJAMMA_COIN1 ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_COIN2 ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_START1 ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_POWER_BUTTON ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_MEDAL ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_DOOR_SWITCH ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_HOPPER_BTN2 ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_MAGNET_BTN5 ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_HOPPER_TEST ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_TRACKBALL_UP ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_TRACKBALL_DOWN ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_TRACKBALL_LEFT ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;
	mScanMasks[ PERJAMMA_TRACKBALL_RIGHT ] = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;

	// micrombc doesn't have the mystery "Button 2" input at all, and
	// patocar/skychal/supgoal/techbowl don't have the Magnet Sensor -
	// grey out whichever one doesn't apply to the loaded game instead of
	// silently letting the player bind a key that will never do anything.
	if ( isMicrombc )
		tbHopperBtn2->setEnabled( false );
	else
		tbMagnetBtn5->setEnabled( false );

	// micrombc explicitly has no trackball (see MAME's micrombc PORTG.0/.1
	// override, which zeroes it out) - only patocar/skychal/supgoal/techbowl
	// have one.
	if ( isMicrombc )
	{
		tbTrackballUp->setEnabled( false );
		tbTrackballDown->setEnabled( false );
		tbTrackballLeft->setEnabled( false );
		tbTrackballRight->setEnabled( false );
	}

	loadPadSettings();

	foreach ( QToolButton* tb, findChildren<QToolButton*>() )
	{
		tb->installEventFilter( this );
		connect( tb, SIGNAL( clicked() ), this, SLOT( tbButton_clicked() ) );
	}

	connect( mTimer, SIGNAL( timeout() ), this, SLOT( timer_timeout() ) );

	QtYabause::retranslateWidget( this );
}

UIPatocarSetting::~UIPatocarSetting()
{
}
