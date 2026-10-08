/*  Copyright 2026 Kronos_test contributors

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

#ifndef MPEGCARD_H
#define MPEGCARD_H

/*
   Video CD Card / MPEG Card - real MPEG-1 decode path (EXPERIMENTAL).

   Unlike the CD Block command protocol implemented in cs2.c (which follows
   the reverse-engineered-but-widely-corroborated command set on the
   Yabause wiki), there is no public documentation at all -- official or
   otherwise -- describing how the Video CD Card's actual decoder LSI
   works internally, or exactly how it latches onto the CD Block's data
   stream. No other Saturn emulator (Yabause, Mednafen, SSF) decodes real
   video for this peripheral for the same reason.

   This module is a best-effort, standalone substitute: it demuxes/decodes
   the MPEG-1 Program Stream data straight from the mounted Video CD using
   the public-domain-equivalent, MIT-licensed PL_MPEG library (see
   vendor/pl_mpeg.h, https://github.com/phoboslab/pl_mpeg), rather than
   emulating the real decoder chip cycle-for-cycle. It is wired up so that
   when nothing is decoding (no Video CD Card configured, or no Video CD
   mounted), every call below is a cheap no-op and existing games/behavior
   are completely unaffected.
*/

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocates the decoder state. Safe to call multiple times (idempotent). */
int  MpegCardInit(void);

/* Releases the decoder state. Safe to call even if never initialized. */
void MpegCardDeInit(void);

/* Drops whatever stream position/picture/audio the decoder had (disc
   swapped, seek, or emulator reset) without freeing the decoder itself. */
void MpegCardReset(void);

/* Feeds raw bytes read from the CD Block's CD-ROM XA Mode 2 Form 2
   sectors (the payload only -- no sync/header/subheader/EDC, see
   Cs2ReadFilteredSector() in cs2.c) into the MPEG-PS demuxer's input
   buffer. This is the actual "wire" between the CD Block and the card. */
void MpegCardPushData(const u8 *data, u32 length);

/* Advances decoding by the given amount of emulated wall-clock time so
   picture/audio output keeps pace with playback instead of being decoded
   as fast as sectors arrive. */
void MpegCardAdvance(double elapsedSeconds);

/* Most recently decoded picture, converted to tightly-packed RGBA8888,
   top row first. Returns NULL if nothing has been decoded yet (in which
   case *width and *height are left untouched). The returned pointer is only
   valid until the next MpegCardAdvance() call. */
const u8 *MpegCardGetFrameRGBA(int *width, int *height);

/* Drains up to maxSamples sample-pairs of decoded audio -- interleaved
   stereo (left,right,left,...), 32-bit float, at the stream's native rate
   -- from the decoder's internal FIFO into `out` (caller-allocated, at
   least maxSamples*2 floats). Returns how many sample-pairs were actually
   copied (0 if none pending); *outSampleRate is only written when the
   return value is >0. Draining everything pending, rather than exposing
   only "the last decoded frame", matters here: MpegCardAdvance() can
   decode more than one 1152-sample MPEG audio frame per call, and a
   single-frame register would silently drop the earlier ones. */
u32 MpegCardPullAudioSamples(float *out, u32 maxSamples, int *outSampleRate);

/* True once MpegCardPushData()/MpegCardAdvance() have actually produced a
   picture -- used to gate the (experimental) video overlay so an
   emulator session that never touches the Video CD Card renders exactly
   as it did before this module existed. */
int MpegCardIsActive(void);

/* Bytes pushed but not yet consumed by the demuxer. Used by the CD Block
   side (Cs2MpegDecoderPump()) as the decoder's "input buffer full" flow
   control, like the real card's limited MPEG buffer. */
u32 MpegCardGetBufferedBytes(void);

/* Forgets the current picture (MPEG Init, stop...). The decoder keeps
   running; MpegCardIsActive() turns true again at the next picture. */
void MpegCardClearPicture(void);

/* Incremented whenever the picture returned by MpegCardGetFrameRGBA()
   changes, so renderers only re-upload it when needed. */
u32 MpegCardGetFrameSerial(void);

/* MPEG Set Decoding Method (CD Communication Interface, MPEG part, 20.8
   CDC_MpSetDec), display and output side:
   - freeze: the decoder keeps decoding but the displayed picture is held
     (frztim 0000H). strobe > 1 shows one decoded picture in `strobe`
     (frztim 0002H-FFFEH, strobe playback); 0/1 = normal display.
   - mute: bit 0 mutes the right channel, bit 1 the left one. */
void MpegCardSetFreeze(int frozen, int strobe);
void MpegCardSetMute(int mutebits);

/* Picture rate of the current stream in pictures per second (25.0 for a
   PAL Video CD, 29.97 NTSC), or 0 if no sequence header was seen yet. */
double MpegCardGetFrameRate(void);

/* ------------------------------------------------------------------------
   Video CD Card boot ROM (the "system application" the BIOS CD player
   loads through CD Block command 0xE2 "Get MPEG ROM").

   The ROM is kept in memory, independently of the decoder state above
   (MpegCardInit()/MpegCardReset() never touch it), so it survives disc
   swaps and decoder resets.

   `path` may point to:
     - a raw dump (.bin/.rom/...), or
     - a .zip archive containing the dump (stored or deflated). When the
       archive holds several files, the largest one is taken as the ROM.

   Word-swapped dumps (typical of EPROM-programmer reads of the 16-bit
   mask ROM) are detected and put back in the SH-1's big-endian order.
   Chip-order dumps (the "SEGA SEGASATURN" header at 0x5000 instead of 0)
   are rotated into the layout the Saturn sees through command 0xE2.

   Returns the ROM size in bytes, or 0 on failure (no/empty path, file not
   found, unreadable zip...). Calling it again replaces the loaded ROM.  */
u32  MpegCardLoadRom(const char *path);
void MpegCardFreeRom(void);
int  MpegCardHasRom(void);
u32  MpegCardGetRomSize(void);

/* Copies `size` bytes starting at `offset` in the ROM into `dst`.
   Like the real card, addresses past the end of the chip mirror it.
   Returns the number of bytes copied (0 if no ROM is loaded, in which
   case `dst` is zero-filled). */
u32  MpegCardReadRom(u32 offset, u8 *dst, u32 size);

#ifdef __cplusplus
}
#endif

#endif
