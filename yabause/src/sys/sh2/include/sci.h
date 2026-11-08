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

/*! \file sci.h
    \brief SH-2 serial communication interface (SCI) and the Saturn
    communication connector.

    SCI: SH7604 Hardware Manual (Hitachi ADE-602-085C rev. 4.0), section 13.
    Communication connector: Sega Saturn PAL service manual (01301, 1995),
    board schematic, connector CN6 "SERIAL": MRXD / MTXD / MSCK go to the
    master SH-2 SCI, SRXD / STXD / SSCK to the slave SH-2 SCI. A device
    plugged into it (floppy disk drive, printer interface, link cable...)
    implements CommPortDevice.
*/

#ifndef SCI_H
#define SCI_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SH2_struct_s;

#define SCI_CPU_MASTER 0
#define SCI_CPU_SLAVE  1

#define SCI_RX_END_OF_FRAME 2

typedef struct
{
   const char * name;
   /** Console reset / device plugged in. */
   void (*Reset)(void);
   /** One character shifted out on TxD of the SCI of @p cpu.
       @p sync: 1 = clocked synchronous mode (8 data bits, no framing). */
   void (*Transmit)(int cpu, u8 data, int sync);
   /** Asks the device for the next character on RxD of @p cpu.
       Returns 1 and fills @p data if the device sends one now, 0 if the
       line stays idle (mark, all 1s). In clocked synchronous mode with the
       SCI clock (SCK output), the device is asked at every character the
       SCI clocks, and an idle line gives FFh.
       Returns SCI_RX_END_OF_FRAME (2) for the last character of a frame
       when the device waits for the receiver to be enabled again (RE 0 -> 1)
       before it sends the next frame: the device clocks (CKE1 = 1) and
       doesn't send while the Saturn checks the frame. */
   int (*Receive)(int cpu, u8 * data, int sync);
   /** External clock on SCK (CKE1 = 1): SH-2 cycles per bit the device
       drives, 0 = no clock (nothing moves, like the real chip). In
       asynchronous mode the SCK input is 16 x the bit rate (13.2.6, note 3):
       return the cycles of one BIT, not of one SCK period. */
   u32 (*ExternalBitCycles)(int cpu);
} CommPortDevice;

/** Plugs @p dev into the communication connector (NULL = nothing). */
void SCISetCommPortDevice(CommPortDevice * dev);
CommPortDevice * SCIGetCommPortDevice(void);

/* Called by sh2core.c */
void SCIReset(struct SH2_struct_s * context);
void SCIExec(struct SH2_struct_s * context);
u8   SCIReadSSR(struct SH2_struct_s * context);
void SCIWriteSMR(struct SH2_struct_s * context, u8 val);
void SCIWriteBRR(struct SH2_struct_s * context, u8 val);
void SCIWriteSCR(struct SH2_struct_s * context, u8 val);
void SCIWriteTDR(struct SH2_struct_s * context, u8 val);
void SCIWriteSSR(struct SH2_struct_s * context, u8 val);
u8   SCIReadRDR(struct SH2_struct_s * context);

/** Instrumentation: SCI trace file (KRONOS_SCI_TRACE environment variable,
    see sci.c). Also logs DMAC DRCR writes, so a transfer driven by the SCI
    interrupts through the DMAC can be spotted. */
void SCITraceDRCR(struct SH2_struct_s * context, int channel, u8 val);

#ifdef __cplusplus
}
#endif

#endif
