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
#ifndef UIBUTTONLISTSETTING_H
#define UIBUTTONLISTSETTING_H

#include "UIControllerSetting.h"

#include <QList>
#include <QString>

class QLabel;

/*
   Key binding dialog built from a list of inputs instead of a .ui drawing
   of the controller: one row "name / bind button" per input, grouped in
   sections. Used by the controllers that are a standard pad electrically
   but have their own controls (Twin Stick, SBom Joycard). The bindings are
   stored like every other controller (UIControllerSetting::setPadKey()).
*/
class UIButtonListSetting : public UIControllerSetting
{
	Q_OBJECT

public:
	struct Entry
	{
		int id;          /* binding id (PERPAD_*, PERJOYCARD_*...), -1 = section title */
		QString name;    /* shown name, already translated */
		QString hint;    /* tooltip, may be empty */
	};

	UIButtonListSetting( PerInterface_struct* core, uint port, uint pad, uint perType,
		const QString& title, const QList<Entry>& entries, QWidget* parent = 0 );
	virtual ~UIButtonListSetting();

	/* The inputs of the Twin Stick (STN-50) and of the SBom Joycard. */
	static QList<Entry> twinStickEntries();
	static QList<Entry> joycardEntries();

protected:
	QLabel* mInfos;

protected slots:
	void turboRateChanged( int rate );
};

#endif // UIBUTTONLISTSETTING_H
