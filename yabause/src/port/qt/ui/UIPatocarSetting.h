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
#ifndef UIPATOCARSETTING_H
#define UIPATOCARSETTING_H

#include "ui_UIPatocarSetting.h"
#include "UIControllerSetting.h"
#include "QtYabause.h"

#include <QMap>

class QTimer;

// Key-binding dialog for the PATOCAR/MICROMBC hopper cabinet extension I/O
// layout used by patocar, skychal, supgoal, techbowl and micrombc. See the
// hopper simulation comment block in peripheral.c for the port/bit wiring
// this mirrors.
class UIPatocarSetting : public UIControllerSetting, public Ui::UIPatocarSetting
{
	Q_OBJECT

public:
	UIPatocarSetting( PerInterface_struct* core, uint port, uint pad, uint perType, QWidget* parent = 0 );
	virtual ~UIPatocarSetting();

protected:

protected slots:
};

#endif // UIPATOCARSETTING_H
