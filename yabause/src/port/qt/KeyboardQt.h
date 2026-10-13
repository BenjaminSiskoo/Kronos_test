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
#ifndef KEYBOARDQT_H
#define KEYBOARDQT_H

class QKeyEvent;

/* Feeds the host keyboard to the emulated Sega Saturn keyboard
   (PERKEYBOARD, see peripheral.h).

   Keys are translated by their physical position (native scan code), the
   way a real keyboard works: the Saturn software applies its own layout,
   so an AZERTY or QWERTZ host keyboard behaves like the same physical
   keys on a Saturn keyboard. When no native code is available, the Qt
   key is used instead, assuming a US layout. */
namespace KeyboardQt
{
	void keyPress( QKeyEvent* e );
	void keyRelease( QKeyEvent* e );
	/* Releases every key, e.g. when the window loses the focus, so no key
	   stays stuck down in the emulated keyboard. */
	void releaseAll();
}

#endif // KEYBOARDQT_H
