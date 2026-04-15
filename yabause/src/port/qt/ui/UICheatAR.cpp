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
#include "UICheatAR.h"
#include "../QtYabause.h"
#include <QRegularExpressionValidator>

UICheatAR::UICheatAR( QWidget* p )
	: QDialog( p )
{
	setupUi( this );
	QtYabause::retranslateWidget( this );

	// CORRECTION : Validation de la saisie (Format Action Replay : XXXXXXXX XXXXX)
	// Autorise 8 hex, un espace optionnel, puis 4 hex
	QRegularExpression arRegex("^[0-9A-Fa-f]{8}\\s?[0-9A-Fa-f]{4}$");
	leCode->setValidator(new QRegularExpressionValidator(arRegex, this));
    
	// UI : Met le focus sur le champ de code au démarrage
	leCode->setFocus();
}
