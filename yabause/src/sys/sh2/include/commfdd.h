/*  Copyright 2026 Kronos team

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

/*! \file commfdd.h
    \brief Sega Saturn Floppy Disk Drive (HSS-0128) on the communication
    connector, as a CommPortDevice (sci.h). The floppy is a host folder.
*/

#ifndef COMMFDD_H
#define COMMFDD_H

#include "sci.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The drive. Plug it with SCISetCommPortDevice(FddGetDevice()). */
CommPortDevice * FddGetDevice(void);

/** Host folder holding the floppy (one sub-folder per partition, created
    when needed). NULL or "" = no disk in the drive. */
void FddSetFolder(const char * folder);

/** Write protect tab of the emulated floppy. */
void FddSetWriteProtect(int on);

#ifdef __cplusplus
}
#endif

#endif
