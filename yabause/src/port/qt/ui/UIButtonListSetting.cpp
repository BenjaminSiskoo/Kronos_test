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
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

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

UIButtonListSetting::UIButtonListSetting( PerInterface_struct* core, uint port, uint pad, uint perType,
	const QString& title, const QList<Entry>& entries, QWidget* parent )
	: UIControllerSetting( core, port, pad, perType, parent )
{
	setWindowTitle( title );

	QVBoxLayout* main = new QVBoxLayout( this );
	QHBoxLayout* columns = new QHBoxLayout();
	main->addLayout( columns );

	const QIcon unset( ":/actions/icons/actions/button_cancel.png" );
	const u32 scan = PERSF_KEY | PERSF_BUTTON | PERSF_HAT;

	QGroupBox* group = NULL;
	QGridLayout* grid = NULL;
	int row = 0;
	foreach ( const Entry& e, entries )
	{
		if ( e.id < 0 || group == NULL )
		{
			group = new QGroupBox( e.id < 0 ? e.name : QString(), this );
			grid = new QGridLayout( group );
			columns->addWidget( group, 0, Qt::AlignTop );
			row = 0;
			if ( e.id < 0 )
				continue;
		}

		QLabel* label = new QLabel( e.name, group );
		QToolButton* tb = new QToolButton( group );
		tb->setCheckable( true );
		tb->setAutoRaise( true );
		tb->setFocusPolicy( Qt::NoFocus );
		tb->setIcon( unset );
		tb->setToolTip( e.hint.isEmpty() ? e.name : QString( "%1 (%2)" ).arg( e.name ).arg( e.hint ) );
		tb->setMinimumSize( 42, 28 );
		if ( !e.hint.isEmpty() && e.hint.length() <= 5 )
			label->setText( QString( "%1  [%2]" ).arg( e.name ).arg( e.hint ) );
		grid->addWidget( label, row, 0 );
		grid->addWidget( tb, row, 1 );
		row++;

		mButtons[ tb ] = (u8)e.id;
		mNames[ (u8)e.id ] = e.name;
		mScanMasks[ (u8)e.id ] = scan;
	}

	if ( perType == PERPAD_JOYCARD )
	{
		QHBoxLayout* rate = new QHBoxLayout();
		QLabel* l = new QLabel( QtYabause::translate( "Turbo speed (presses per second)" ), this );
		QSpinBox* sb = new QSpinBox( this );
		sb->setRange( 1, 30 );
		sb->setValue( QtYabause::settings()->value( SETTINGS_JOYCARD_TURBO_RATE, JOYCARD_TURBO_RATE_DEFAULT ).toInt() );
		sb->setToolTip( QtYabause::translate( "Up to 26 on the real pad. Common to every SBom Joycard." ) );
		rate->addWidget( l );
		rate->addWidget( sb );
		rate->addStretch();
		main->addLayout( rate );
		connect( sb, SIGNAL( valueChanged( int ) ), this, SLOT( turboRateChanged( int ) ) );
	}

	mInfos = new QLabel( this );
	mInfos->setMinimumHeight( 36 );
	mInfos->setWordWrap( true );
	main->addWidget( mInfos );
	setInfos( mInfos );

	QDialogButtonBox* box = new QDialogButtonBox( QDialogButtonBox::Close, this );
	main->addWidget( box );
	connect( box, SIGNAL( rejected() ), this, SLOT( reject() ) );

	loadPadSettings();

	foreach ( QToolButton* tb, mButtons.keys() )
	{
		tb->installEventFilter( this );
		connect( tb, SIGNAL( clicked() ), this, SLOT( tbButton_clicked() ) );
	}

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
