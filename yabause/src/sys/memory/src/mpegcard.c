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

// See mpegcard.h for what this module is and, importantly, what it is
// *not*: it does not emulate the Video CD Card's real decoder chip (no
// public documentation of that chip exists), it decodes the same MPEG-1
// Program Stream data with a general-purpose, MIT-licensed software
// decoder (PL_MPEG) instead. Treat everything in this file as
// experimental best-effort video/audio output, not hardware emulation.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpegcard.h"
#include "junzip.h"
#include "yui.h"
#include "threads.h"

#define PL_MPEG_IMPLEMENTATION
// Silence PL_MPEG's own stdio-based file/filename loader: we only ever
// feed it memory buffers (see MpegCardPushData()), so PLM_NO_STDIO trims
// a chunk of unused code and, more importantly, unused-function warnings.
#define PLM_NO_STDIO
#include "../vendor/pl_mpeg.h"

#define MPEGCARD_INITIAL_BUFFER (256 * 1024)

// Small FIFO of interleaved audio sample-pairs, sized generously (about
// 4 seconds at 44.1kHz) so a burst of decoding (e.g. catching up after a
// pause) can't overrun it before ScspExecAsync() next drains it.
#define MPEGCARD_AUDIOFIFO_CAP (44100 * 4)

typedef struct
{
   int initialized;

   plm_t *plm;
   plm_buffer_t *buffer;

   u8 *rgba;
   int rgbawidth;
   int rgbaheight;
   int haspicture;

   float audiofifo[MPEGCARD_AUDIOFIFO_CAP * 2]; // interleaved L/R
   u32 audiofifocount; // sample-pairs currently queued
   int audiorate;

   u32 frameserial;     // bumped on every displayed picture (texture upload gate)
   int frozen;          // MPEG Set Decoding Method: picture held (freeze)
   int strobe;          // >1: show one picture in `strobe` (strobe playback)
   u32 strobecount;
   int mute;            // bit 0 right, bit 1 left
   u32 pushedsectors;   // diagnostics only
   u32 decodedframes;   // diagnostics only
} mpegcard_state;

static mpegcard_state g_mpegcard;

// The audio FIFO is filled on the emulation thread (plm_decode() ->
// MpegCardAudioCallback(), from MpegCardAdvance() in Vdp2VBlankIN) and
// drained on the SCSP thread (ScspExecAsync() -> MpegCardMixAudio() ->
// MpegCardPullAudioSamples()). Both sides move audiofifocount and memmove
// the samples, and MpegCardDeInit()/MpegCardInit() memset the whole state:
// without a lock the sound thread could copy a half-shifted or wiped FIFO.
// Kept outside g_mpegcard so the memset never touches it.
static YabMutex *g_mpegaudiomtx = NULL;

static void MpegCardAudioLock(void)
{
   if (g_mpegaudiomtx != NULL)
      YabThreadLock(g_mpegaudiomtx);
}

static void MpegCardAudioUnlock(void)
{
   if (g_mpegaudiomtx != NULL)
      YabThreadUnLock(g_mpegaudiomtx);
}

//////////////////////////////////////////////////////////////////////////////

static void MpegCardVideoCallback(plm_t *plm, plm_frame_t *frame, void *user)
{
   (void)plm;
   (void)user;

   if ((int)frame->width != g_mpegcard.rgbawidth || (int)frame->height != g_mpegcard.rgbaheight || g_mpegcard.rgba == NULL)
   {
      free(g_mpegcard.rgba);
      g_mpegcard.rgbawidth = frame->width;
      g_mpegcard.rgbaheight = frame->height;
      g_mpegcard.rgba = (u8 *)malloc((size_t)frame->width * frame->height * 4);
   }

   g_mpegcard.decodedframes++;

   // Freeze (CDC_MpSetDec frztim): decoding goes on, the displayed picture
   // is held. Strobe playback shows one decoded picture in `strobe`.
   if (g_mpegcard.haspicture && g_mpegcard.frozen)
      return;
   if (g_mpegcard.haspicture && g_mpegcard.strobe > 1 &&
       (g_mpegcard.strobecount++ % (u32)g_mpegcard.strobe) != 0)
      return;

   if (g_mpegcard.rgba != NULL)
   {
      plm_frame_to_rgba(frame, g_mpegcard.rgba, frame->width * 4);
      g_mpegcard.haspicture = 1;
      g_mpegcard.frameserial++;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void MpegCardAudioCallback(plm_t *plm, plm_samples_t *samples, void *user)
{
   u32 room;
   u32 tocopy;

   (void)user;

   g_mpegcard.audiorate = plm_get_samplerate(plm);

   // Queue everything this callback delivered rather than overwriting a
   // single "last frame" register: plm_decode() can call this more than
   // once per MpegCardAdvance() call when catching up, and a single
   // register would silently drop all but the last of those calls.
   MpegCardAudioLock();
   room = MPEGCARD_AUDIOFIFO_CAP - g_mpegcard.audiofifocount;
   tocopy = samples->count < room ? samples->count : room; // drop the tail on overflow
   if (tocopy > 0)
   {
      memcpy(&g_mpegcard.audiofifo[g_mpegcard.audiofifocount * 2],
             samples->interleaved,
             (size_t)tocopy * 2 * sizeof(float));
      g_mpegcard.audiofifocount += tocopy;
   }
   MpegCardAudioUnlock();
}

//////////////////////////////////////////////////////////////////////////////

int MpegCardInit(void)
{
   if (g_mpegcard.initialized)
      return 0;

   if (g_mpegaudiomtx == NULL)
      g_mpegaudiomtx = YabThreadCreateMutex();

   MpegCardAudioLock();
   memset(&g_mpegcard, 0, sizeof(g_mpegcard));
   MpegCardAudioUnlock();

   // PLM_BUFFER_MODE_RING (create_with_capacity) auto-discards already
   // decoded bytes and grows on demand -- the right fit for a live,
   // never-ending stream of sectors handed to us by the CD Block, as
   // opposed to create_for_appending()'s keep-everything mode (meant for
   // whole-file buffers you might still want to seek around in).
   g_mpegcard.buffer = plm_buffer_create_with_capacity(MPEGCARD_INITIAL_BUFFER);
   if (g_mpegcard.buffer == NULL)
      return -1;

   g_mpegcard.plm = plm_create_with_buffer(g_mpegcard.buffer, TRUE);
   if (g_mpegcard.plm == NULL)
   {
      plm_buffer_destroy(g_mpegcard.buffer);
      g_mpegcard.buffer = NULL;
      return -1;
   }

   plm_set_video_decode_callback(g_mpegcard.plm, MpegCardVideoCallback, NULL);
   plm_set_audio_decode_callback(g_mpegcard.plm, MpegCardAudioCallback, NULL);
   plm_set_loop(g_mpegcard.plm, FALSE);

   g_mpegcard.initialized = 1;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardDeInit(void)
{
   if (!g_mpegcard.initialized)
      return;

   if (g_mpegcard.plm != NULL)
      plm_destroy(g_mpegcard.plm); // also destroys g_mpegcard.buffer (destroy_when_done=TRUE)

   free(g_mpegcard.rgba);

   MpegCardAudioLock();
   memset(&g_mpegcard, 0, sizeof(g_mpegcard));
   MpegCardAudioUnlock();
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardReset(void)
{
   int frozen, strobe, mute;

   if (!g_mpegcard.initialized)
      return;

   // The decoding-method settings (freeze, strobe, mute) belong to the
   // decoder's control registers, not to the stream: MPEG Init resets them
   // through Cs2, a stream reset must not.
   frozen = g_mpegcard.frozen;
   strobe = g_mpegcard.strobe;
   mute = g_mpegcard.mute;

   MpegCardDeInit();
   MpegCardInit();

   g_mpegcard.frozen = frozen;
   g_mpegcard.strobe = strobe;
   g_mpegcard.mute = mute;
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardSetFreeze(int frozen, int strobe)
{
   g_mpegcard.frozen = frozen ? 1 : 0;
   g_mpegcard.strobe = strobe;
   g_mpegcard.strobecount = 0;
}

void MpegCardSetMute(int mutebits)
{
   g_mpegcard.mute = mutebits & 3;
}

double MpegCardGetFrameRate(void)
{
   if (!g_mpegcard.initialized || g_mpegcard.plm == NULL)
      return 0.0;
   return plm_get_framerate(g_mpegcard.plm);
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardPushData(const u8 *data, u32 length)
{
   if (!g_mpegcard.initialized || data == NULL || length == 0)
      return;

   // plm_buffer_write() takes a non-const pointer only because PL_MPEG
   // also supports write-then-read-in-place buffers elsewhere; it never
   // writes back into `bytes` itself, just memcpy()s out of it.
   plm_buffer_write(g_mpegcard.buffer, (u8 *)data, length);

   g_mpegcard.pushedsectors++;
}

//////////////////////////////////////////////////////////////////////////////

u32 MpegCardGetBufferedBytes(void)
{
   if (!g_mpegcard.initialized || g_mpegcard.buffer == NULL)
      return 0;

   return (u32)plm_buffer_get_remaining(g_mpegcard.buffer);
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardClearPicture(void)
{
   if (!g_mpegcard.initialized)
      return;

   g_mpegcard.haspicture = 0;
   g_mpegcard.frameserial++;
}

//////////////////////////////////////////////////////////////////////////////

u32 MpegCardGetFrameSerial(void)
{
   return g_mpegcard.frameserial;
}

//////////////////////////////////////////////////////////////////////////////

// PL_MPEG refuses to start decoding until it has seen an MPEG-1 *system*
// header, and it takes the stream layout from that header's
// audio_bound/video_bound fields. Two problems here:
//
//  1. A White Book Video CD describes video and audio in two separate system
//     headers, in different packs; PL_MPEG reads the first, concludes "1
//     video, 0 audio", and never creates the audio decoder.
//
//  2. A system header only exists at the very beginning of each MPEG file on
//     the disc. The card, though, is fed from wherever the user started
//     playing -- the middle of a track, after a seek, after a chapter skip.
//     The real decoder simply locks onto the stream from the next pack it
//     sees; PL_MPEG would sit idle until the *next file* happened to come
//     round, which is exactly what was observed: nearly 9000 sectors, about
//     two minutes of disc, before the first picture appeared.
//
// plm_probe() is meant for this, but it scans ahead and seeks back, which a
// live buffer fed sector by sector cannot do. So declare the layout the Video
// CD specification mandates (MPEG-1 video on 0xE0, MPEG-1 Layer II audio on
// 0xC0) and let the demuxer start on the next pack. Decoding then begins as
// soon as a sequence header arrives, which on a VCD is every GOP -- well
// under a second. A disc with no audio packets simply never feeds the audio
// decoder and stays silent.
static void MpegCardForceStreams(void)
{
   plm_t *plm = g_mpegcard.plm;

   if (plm == NULL || plm->has_decoders)
      return;

   // Wait for a few sectors so the demuxer has something to sync on, then
   // stop waiting for a system header that may never come.
   //
   // Note this counts sectors pushed, NOT plm_buffer_get_remaining(): while
   // no decoder exists, plm_decode() keeps scanning the buffer for a pack
   // header and consumes it, so the "remaining" count never grows and a test
   // on it would never fire.
   if (g_mpegcard.pushedsectors < 8)
      return;

   plm->demux->has_pack_header = TRUE;
   plm->demux->has_system_header = TRUE;
   plm->demux->has_headers = TRUE;
   plm->demux->num_video_streams = 1;
   plm->demux->num_audio_streams = 1;
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardAdvance(double elapsedSeconds)
{
   if (!g_mpegcard.initialized)
      return;

   if (elapsedSeconds <= 0.0)
      return;

   // Guard against a huge jump (e.g. the emulator was paused/fast-forwarded
   // for a while) turning into an attempt to decode minutes of video in a
   // single call.
   if (elapsedSeconds > 0.5)
      elapsedSeconds = 0.5;

   MpegCardForceStreams();

   plm_decode(g_mpegcard.plm, elapsedSeconds);

   // PL_MPEG's playback clock (plm->time) keeps running even while the
   // decoder is starved (no sector yet, CD seeking, buffer drained...). On
   // a file that never happens, but here data trickles in from the CD
   // Block: after a few seconds of starvation the clock would be far
   // ahead, and the next sectors would be decoded in one burst (frames
   // flashing by at CD read speed) instead of at the stream's frame rate.
   // Hold the clock back to the video decoder's position (with a little
   // slack for normal jitter) so playback always resumes in real time.
   if (g_mpegcard.plm->has_decoders && g_mpegcard.plm->video_decoder != NULL &&
       g_mpegcard.plm->video_packet_type)
   {
      double vtime = plm_video_get_time(g_mpegcard.plm->video_decoder);
      if (g_mpegcard.plm->time > vtime + 0.25)
         g_mpegcard.plm->time = vtime;
   }
}

//////////////////////////////////////////////////////////////////////////////

const u8 *MpegCardGetFrameRGBA(int *width, int *height)
{
   if (!g_mpegcard.initialized || !g_mpegcard.haspicture || g_mpegcard.rgba == NULL)
      return NULL;

   if (width != NULL) *width = g_mpegcard.rgbawidth;
   if (height != NULL) *height = g_mpegcard.rgbaheight;
   return g_mpegcard.rgba;
}

//////////////////////////////////////////////////////////////////////////////

u32 MpegCardPullAudioSamples(float *out, u32 maxSamples, int *outSampleRate)
{
   u32 tocopy;

   if (out == NULL || maxSamples == 0 || g_mpegaudiomtx == NULL)
      return 0;

   MpegCardAudioLock();
   if (!g_mpegcard.initialized || g_mpegcard.audiofifocount == 0)
   {
      MpegCardAudioUnlock();
      return 0;
   }

   tocopy = g_mpegcard.audiofifocount < maxSamples ? g_mpegcard.audiofifocount : maxSamples;

   memcpy(out, g_mpegcard.audiofifo, (size_t)tocopy * 2 * sizeof(float));

   // Mute (CDC_MpSetDec): bit 0 right channel, bit 1 left channel.
   if (g_mpegcard.mute)
   {
      u32 k;
      for (k = 0; k < tocopy; k++)
      {
         if (g_mpegcard.mute & 2) out[k * 2] = 0.0f;
         if (g_mpegcard.mute & 1) out[k * 2 + 1] = 0.0f;
      }
   }

   // Compact: shift whatever's left down to the front. audiofifocount
   // stays small enough in practice (a handful of 1152-sample frames at
   // most between two SCSP ticks) that this memmove is cheap.
   g_mpegcard.audiofifocount -= tocopy;
   if (g_mpegcard.audiofifocount > 0)
      memmove(g_mpegcard.audiofifo, &g_mpegcard.audiofifo[tocopy * 2], (size_t)g_mpegcard.audiofifocount * 2 * sizeof(float));

   if (outSampleRate != NULL) *outSampleRate = g_mpegcard.audiorate;
   MpegCardAudioUnlock();
   return tocopy;
}

//////////////////////////////////////////////////////////////////////////////

int MpegCardIsActive(void)
{
   return g_mpegcard.initialized && g_mpegcard.haspicture;
}

//////////////////////////////////////////////////////////////////////////////
// Video CD Card boot ROM
//
// Kept apart from g_mpegcard on purpose: MpegCardInit() memset()s the whole
// decoder state and MpegCardReset() is called on every disc change, neither
// of which must drop (or leak) the ROM image.
//////////////////////////////////////////////////////////////////////////////

// Largest ROM we accept. Real Video CD Card mask ROMs are 512KB (4Mbit);
// the margin only exists to reject obviously wrong files (e.g. someone
// pointing the setting at a disc image) instead of allocating hundreds of MB.
#define MPEGCARD_ROM_MAX_SIZE (4 * 1024 * 1024)

static u8 *g_mpegrom = NULL;
static u32 g_mpegromsize = 0;

typedef struct
{
   int found;
   JZFileHeader header;
   char name[256];
} mpegrom_zipentry;

//////////////////////////////////////////////////////////////////////////////

static int MpegCardZipPickLargest(JZFile *zip, int idx, JZFileHeader *header, char *filename, void *user_data)
{
   mpegrom_zipentry *best = (mpegrom_zipentry *)user_data;
   size_t len = strlen(filename);

   (void)zip;
   (void)idx;

   // Skip directory entries ("folder/") and empty files.
   if (len == 0 || filename[len - 1] == '/' || filename[len - 1] == '\\' || header->uncompressedSize == 0)
      return 1;

   if (!best->found || header->uncompressedSize > best->header.uncompressedSize)
   {
      best->found = 1;
      memcpy(&best->header, header, sizeof(JZFileHeader));
      strncpy(best->name, filename, sizeof(best->name) - 1);
      best->name[sizeof(best->name) - 1] = '\0';
   }

   return 1; // keep scanning the central directory
}

//////////////////////////////////////////////////////////////////////////////

static u8 *MpegCardLoadRomFromZip(FILE *fp, const char *path, u32 *outsize)
{
   JZFile *zip;
   JZEndRecord endrecord;
   JZFileHeader localheader;
   mpegrom_zipentry best;
   u8 *data = NULL;

   memset(&best, 0, sizeof(best));

   // From here on `zip` owns `fp` (zip->close() fclose()s it).
   zip = jzfile_from_stdio_file(fp);
   if (zip == NULL)
   {
      fclose(fp);
      return NULL;
   }

   if (jzReadEndRecord(zip, &endrecord) != 0)
   {
      YuiMsg("MPEG ROM: \"%s\" is not a readable zip archive\n", path);
      goto done;
   }

   if (jzReadCentralDirectory(zip, &endrecord, MpegCardZipPickLargest, &best) != 0 || !best.found)
   {
      YuiMsg("MPEG ROM: no usable file found in \"%s\"\n", path);
      goto done;
   }

   if (best.header.uncompressedSize > MPEGCARD_ROM_MAX_SIZE)
   {
      YuiMsg("MPEG ROM: \"%s\" in \"%s\" is too large (%u bytes) to be a Video CD Card ROM\n",
             best.name, path, (unsigned)best.header.uncompressedSize);
      goto done;
   }

   // The central directory gives the local header offset; the local header
   // (whose name/extra field lengths may differ from the central copy) must
   // be parsed to land on the actual data.
   if (zip->seek(zip, best.header.offset, SEEK_SET) != 0 ||
       jzReadLocalFileHeader(zip, &localheader, NULL, 0) != 0)
   {
      YuiMsg("MPEG ROM: corrupt local header for \"%s\" in \"%s\"\n", best.name, path);
      goto done;
   }

   // Some zip writers (data descriptor, bit 3) leave sizes at 0 in the local
   // header; the central directory values are always authoritative.
   localheader.compressedSize = best.header.compressedSize;
   localheader.uncompressedSize = best.header.uncompressedSize;
   localheader.compressionMethod = best.header.compressionMethod;

   if (localheader.compressionMethod != 0 && localheader.compressionMethod != 8)
   {
      YuiMsg("MPEG ROM: \"%s\" uses an unsupported zip compression method (%u); only store/deflate are supported\n",
             best.name, (unsigned)localheader.compressionMethod);
      goto done;
   }

   data = (u8 *)malloc(localheader.uncompressedSize);
   if (data == NULL)
      goto done;

   if (jzReadData(zip, &localheader, data) != 0)
   {
      YuiMsg("MPEG ROM: failed to decompress \"%s\" from \"%s\"\n", best.name, path);
      free(data);
      data = NULL;
      goto done;
   }

   *outsize = localheader.uncompressedSize;
   YuiMsg("MPEG ROM: using \"%s\" (%u bytes) from \"%s\"\n", best.name, (unsigned)*outsize, path);

done:
   zip->close(zip);
   return data;
}

//////////////////////////////////////////////////////////////////////////////

static u8 *MpegCardLoadRomRaw(FILE *fp, const char *path, u32 *outsize)
{
   long size;
   u8 *data;

   if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }

   if (size > MPEGCARD_ROM_MAX_SIZE)
   {
      YuiMsg("MPEG ROM: \"%s\" is too large (%ld bytes) to be a Video CD Card ROM\n", path, size);
      fclose(fp);
      return NULL;
   }

   data = (u8 *)malloc((size_t)size);
   if (data != NULL && fread(data, 1, (size_t)size, fp) != (size_t)size)
   {
      free(data);
      data = NULL;
   }

   fclose(fp);

   if (data != NULL)
   {
      *outsize = (u32)size;
      YuiMsg("MPEG ROM: using \"%s\" (%u bytes)\n", path, (unsigned)*outsize);
   }

   return data;
}

//////////////////////////////////////////////////////////////////////////////
// The SH-1 reads the card's 16-bit mask ROM big-endian. Dumps made from the
// Saturn itself (e.g. through command 0xE2) are already in that order, but
// dumps read on an EPROM programmer are frequently word-swapped. Any Sega
// firmware carries "SEGA" strings, so compare how often that tag appears
// straight vs. byte-swapped ("ESAG") and fix the image up if needed.

static u32 MpegCardCountTag(const u8 *data, u32 size, const char *tag)
{
   u32 i, count = 0;

   for (i = 0; i + 4 <= size; i += 2) // tags are word aligned in both orders
      if (memcmp(data + i, tag, 4) == 0)
         count++;

   return count;
}

static void MpegCardFixByteOrder(u8 *data, u32 size)
{
   u32 straight = MpegCardCountTag(data, size, "SEGA");
   u32 swapped = MpegCardCountTag(data, size, "ESAG");
   u32 i;

   if (swapped > straight)
   {
      for (i = 0; i + 1 < size; i += 2)
      {
         u8 tmp = data[i];
         data[i] = data[i + 1];
         data[i + 1] = tmp;
      }
      YuiMsg("MPEG ROM: word-swapped dump detected, converted to big-endian\n");
   }
}

//////////////////////////////////////////////////////////////////////////////
// Chip dumps vs. Saturn-side dumps.
//
// Reading the MPR-17896-H mask ROM directly (EPROM programmer) gives
// 0x5000 bytes of opaque data first, with the "SEGA SEGASATURN" header of
// the Video CD player application only at 0x5000. Dumps made from the
// Saturn itself through command 0xE2 see the same ROM rotated by 0x5000:
// the header is at offset 0 and those 0x5000 bytes show up at the very end
// (the card applies that offset in hardware and mirrors the 512KB chip),
// see rorirub/CyberWarriorX on SegaXtreme, "Dumping MPEG Card Bios".
//
// The BIOS CD player reads the application from offset 0 of what 0xE2
// returns, so a chip dump fed as-is hands it the opaque block instead of
// the header and it gives up with "Disc requires system application".
// Rotate such images into the Saturn-side layout.

#define MPEGCARD_ROM_HEADER "SEGA SEGASATURN "

static void MpegCardNormalizeLayout(u8 *data, u32 size)
{
   u32 off;
   u8 *tmp;

   if (size >= 16 && memcmp(data, MPEGCARD_ROM_HEADER, 16) == 0)
      return; // already in the layout the Saturn sees

   for (off = 0x100; off + 16 <= size; off += 0x100)
      if (memcmp(data + off, MPEGCARD_ROM_HEADER, 16) == 0)
         break;

   if (off + 16 > size)
   {
      YuiMsg("MPEG ROM: warning, no \"SEGA SEGASATURN\" header found; the BIOS will probably reject this ROM\n");
      return;
   }

   tmp = (u8 *)malloc(off);
   if (tmp == NULL)
      return;

   // Rotate left by `off`: [off..size) first, then [0..off).
   memcpy(tmp, data, off);
   memmove(data, data + off, size - off);
   memcpy(data + size - off, tmp, off);
   free(tmp);

   YuiMsg("MPEG ROM: chip-order dump detected (header at 0x%X), remapped to the Saturn-side layout\n", (unsigned)off);
}

//////////////////////////////////////////////////////////////////////////////

u32 MpegCardLoadRom(const char *path)
{
   FILE *fp;
   u8 magic[4] = { 0, 0, 0, 0 };
   u8 *data;
   u32 size = 0;

   MpegCardFreeRom();

   if (path == NULL || path[0] == '\0')
      return 0;

   if ((fp = fopen(path, "rb")) == NULL)
   {
      YuiMsg("MPEG ROM: cannot open \"%s\"\n", path);
      return 0;
   }

   // Detect zip archives by content ("PK\3\4" local header signature), not
   // by extension, so a renamed file still works.
   if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic))
   {
      fclose(fp);
      return 0;
   }
   fseek(fp, 0, SEEK_SET);

   if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04)
      data = MpegCardLoadRomFromZip(fp, path, &size); // takes ownership of fp
   else
      data = MpegCardLoadRomRaw(fp, path, &size);      // takes ownership of fp

   if (data == NULL || size == 0)
   {
      free(data);
      return 0;
   }

   MpegCardFixByteOrder(data, size);
   MpegCardNormalizeLayout(data, size);

   g_mpegrom = data;
   g_mpegromsize = size;
   return size;
}

//////////////////////////////////////////////////////////////////////////////

void MpegCardFreeRom(void)
{
   free(g_mpegrom);
   g_mpegrom = NULL;
   g_mpegromsize = 0;
}

//////////////////////////////////////////////////////////////////////////////

int MpegCardHasRom(void)
{
   return g_mpegrom != NULL && g_mpegromsize != 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 MpegCardGetRomSize(void)
{
   return g_mpegromsize;
}

//////////////////////////////////////////////////////////////////////////////

u32 MpegCardReadRom(u32 offset, u8 *dst, u32 size)
{
   u32 done = 0;

   if (dst == NULL || size == 0)
      return 0;

   if (g_mpegrom == NULL || g_mpegromsize == 0)
   {
      memset(dst, 0, size);
      return 0;
   }

   // The card only decodes the low address lines: anything past the end of
   // the chip mirrors it (Saturn-side dumps of >512KB show exactly that).
   offset %= g_mpegromsize;

   while (done < size)
   {
      u32 chunk = g_mpegromsize - offset;
      if (chunk > size - done)
         chunk = size - done;
      memcpy(dst + done, g_mpegrom + offset, chunk);
      done += chunk;
      offset = 0;
   }

   return done;
}
