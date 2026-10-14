/*  Copyright 2026 Kronos_test contributors

    Acclaim RAX sound board (ADSP-2181 + banked sample ROM), as fitted to
    the Batman Forever ST-V cartridge. C port of MAME
    src/mame/shared/rax.cpp (license BSD-3-Clause, copyright-holders:
    Phil Bennett) and of the batmanfr glue in src/mame/sega/stv.cpp.

    This file is part of Yabause/Kronos.

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef RAX_H
#define RAX_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the "rax" ROM region in MAME (ROM_REGION(0x1000000, "rax", 0)) */
#define RAX_ROM_SIZE 0x1000000

/* Main thread: ROM loading and lifetime (stv.c). */
void RaxLoadRom(u32 offset, const u8 *data, u32 length);
void RaxEnable(void);        /* batmanfr init: board present, power-on reset */
void RaxShutdown(void);      /* board absent, ROM freed */
int  RaxIsEnabled(void);

/* Main thread: system reset (ScspReset). No-op when the board is absent. */
void RaxReset(void);

/* SH-2 side: write to the sound latch at 0x04800000 (stv.cpp
   batmanfr_sound_comms_w -> acclaim_rax_device::data_w). */
void RaxHostWrite(u16 data);

/* Sound thread: run the board for one 44.1 kHz output sample and add its
   stereo output to *l / *r. No-op when the board is absent. */
void RaxRunSample(s32 *l, s32 *r);

/* Main thread: save states ("RAX " block of the CART chunk, cs0.c).
   Load returns 0 when applied (or when the state has no board). */
void RaxSaveState(void **stream);
int  RaxLoadState(const void *stream, int size);

#ifdef __cplusplus
}
#endif

#endif
