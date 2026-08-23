/*	Copyright 2008 Filipe Azevedo <pasnox@gmail.com>

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
#ifndef UIPORTMANAGER_H
#define UIPORTMANAGER_H

#include "ui_UIPortManager.h"
#include "../QtYabause.h"

// UI-only sentinel for the "Hopper Cabinet" combo entry. Same underlying
// peripheral (PerCab_struct / PERCABINET) as "ST-V Cabinet" - it only
// changes which dialog UIPortManager opens (UIPatocarSetting instead of
// UISTVSetting). NEVER persist this value as-is: YabauseThread.cpp reads
// Input/Port/.../Type directly with a switch() that only recognises real
// peripheral.h type constants, so this is always normalized back down to
// PERCABINET before being written there (see mSettingsHopperUI instead).
// Real types top out at PERCABINET (0xFF), so 0x100 can't collide.
#define PERCABINET_HOPPER (PERCABINET | 0x100)

class UIPortManager : public QGroupBox, public Ui::UIPortManager
{
	Q_OBJECT

public:
	static const QString mSettingsKey;
	static const QString mSettingsType;
	static const QString mSettingsHopperUI; // UI-only: was "Hopper Cabinet" picked for this slot?

	UIPortManager( QWidget* parent = 0 );
	virtual ~UIPortManager();

	void setPort( uint portId );
	void setCore( PerInterface_struct* core );
	void loadSettings();

protected:
	uint mPort;
	PerInterface_struct* mCore;

protected slots:
	void cbTypeController_currentIndexChanged( int id );
	void tbSetJoystick_clicked();
	void tbClearJoystick_clicked();
	void tbRemoveJoystick_clicked();
};

#endif // UIPORTMANAGER_H
