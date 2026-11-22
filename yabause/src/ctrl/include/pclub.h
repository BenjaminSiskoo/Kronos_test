/*  Copyright 2026 Kronos contributors

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/*! \file pclub.h
    \brief ST-V Print Club: video board 837-12936 and video printer.

    Atlus / Sega Print Club cabinets add a video board on the four expansion
    connectors of the ST-V main board, a camera and a Sony video printer.
    None of it is documented by Sega; the behaviour below was taken from the
    pclub2 ROM (diagnostic program, test mode, main program) and checked by
    running it in Kronos:

    0x00500000  W   control. bit 1: freeze the camera picture (taken before
                    the picture is read), bit 2: 1 = VRAM shown on the video
                    output (Bt856 "DRAM mode"), 0 = VRAM open to the CPU,
                    bits 6 then 3: pulses around a printer command.
    0x00500002  W   bit-banged I2C to the Brooktree chips (camera decoder
                    Bt81x/Bt829 family, encoder Bt856). Never read back.
    0x00500004  W   printer remote line, bit 0 (1 = mark). One write per
                    time slot: Sony SIRC 15-bit frames ("printer kind 0") or
                    a pulse-distance code of 6 bits ("printer kind 1").
    0x00500006  W   preview monitor: leave "freeze".
    0x0050000A/C/E  colour gains (R/G/B adjust of the diagnostic program).
    0x00500010-1E   16550 UART #1, one register every 2 bytes.
    0x00540000/2 R  captured picture, 640 x 240 (one field), 3 bytes per
                    pixel: the word at +0 gives bytes 0 and 1 (low, high),
                    the word at +2 byte 2 (low). Two streams, each with its
                    own auto-increment: the game reads +0 then +2 for each
                    pixel, or the whole field at +0 then the whole field at
                    +2.
    0x00540004  R   capture status; reading it rewinds both streams.
    0x00580000-0E   16550 UART #2.
    0x00600000-0x006FFFFF, 0x00700000-0x007FFFFF  print VRAM, 512 lines of
                    1024 words. A pixel is the word of bank 6 (two channels)
                    plus the low byte of the word of bank 7 (third channel).

    The printer status comes back on PORT-G of the IOGA (0x0040000D),
    active low on bit 0, active high on bits 2 and 3:
      bit 0 = 0  printer error          -> "Please call attendant"
      bit 2 = 1  printer missing / off   -> "Please call attendant"
      bit 3 = 1  printing                -> the game waits on "NOW PRINTING"
    An idle printer therefore gives bit 0 = 1, bit 2 = 0, bit 3 = 0
    (PORT-G = 0xF3 with the other inputs released), and 0xFB while printing.
*/

#ifndef PCLUB_H
#define PCLUB_H

#include "core.h"
#include "sh2core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCLUB_CAPTURE_WIDTH  640
#define PCLUB_CAPTURE_HEIGHT 240
#define PCLUB_PRINT_WIDTH    640
#define PCLUB_PRINT_HEIGHT   480

/* Board on/off. stv.c turns it on for the Print Club romsets, before the
   memory map is built (MappedMemoryInit() maps 0x00500000-0x007FFFFF to the
   handlers below only while the board is on). */
void PClubEnable(void);
void PClubShutdown(void);
int  PClubIsEnabled(void);
void PClubReset(void);

/* Directory where the prints are written (print_NNNN.bmp). NULL = ".". */
void PClubSetOutputDir(const char *dir);

/* Camera picture from the host, RGB 8:8:8, any size (scaled to 640 x 240).
   Without a call, the board shows a test pattern; rgb = NULL goes back to
   the test pattern (camera closed or unplugged). Any thread. */
void PClubSetCameraFrame(const u8 *rgb, int width, int height, int stride);

/* Called with each print (RGB 8:8:8, PCLUB_PRINT_WIDTH x PCLUB_PRINT_HEIGHT),
   after the .bmp file is written. NULL = none. */
typedef void (*PClubPrintFunc)(const u8 *rgb, int width, int height, const char *path);
void PClubSetPrintCallback(PClubPrintFunc cb);

/* How long the printer stays busy after PRINT, in frames (default 600). */
void PClubSetPrintFrames(u32 frames);

/* Picture on the board's video output to the VDP2 external screen input
   (EXBG, shown while EXTEN.EXBGEN = 1): the camera, or the frozen field
   once control bit 1 is set. RGBA 8:8:8:8, PCLUB_CAPTURE_WIDTH x
   PCLUB_CAPTURE_HEIGHT; NULL when the board is off. Any thread. The serial
   changes whenever the picture does. */
const u8 *PClubGetExbgFrameRGBA(int *width, int *height);
u32 PClubGetExbgSerial(void);

/* PORT-G bits 0, 2 and 3 as the printer drives them (other bits 0). */
u8 PClubPrinterPortG(void);

/* Memory handlers for 0x00500000-0x007FFFFF. */
u8   FASTCALL PClubReadByte(SH2_struct *context, u8 *memory, u32 addr);
u16  FASTCALL PClubReadWord(SH2_struct *context, u8 *memory, u32 addr);
u32  FASTCALL PClubReadLong(SH2_struct *context, u8 *memory, u32 addr);
void FASTCALL PClubWriteByte(SH2_struct *context, u8 *memory, u32 addr, u8 val);
void FASTCALL PClubWriteWord(SH2_struct *context, u8 *memory, u32 addr, u16 val);
void FASTCALL PClubWriteLong(SH2_struct *context, u8 *memory, u32 addr, u32 val);

/* Save state block "PCLB" of the CART chunk (cs0.c). */
void PClubSaveState(void **stream);
int  PClubLoadState(const void *stream, int size); /* 0 = applied */

#ifdef __cplusplus
}
#endif

#endif
