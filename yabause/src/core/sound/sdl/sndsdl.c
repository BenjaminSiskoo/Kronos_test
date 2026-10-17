/*  Copyright 2005-2006 Theo Berkau

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

/*! \file sndsdl.c
    \brief SDL sound interface.
*/

#ifdef HAVE_LIBSDL

#include <stdlib.h>

#if defined(__APPLE__) || defined(GEKKO)
 #ifdef HAVE_LIBSDL2
  #include <SDL2/SDL.h>
 #else
  #include <SDL/SDL.h>
 #endif
#else
 #include "SDL.h"
#endif
#include "error.h"
#include "scsp.h"
#include "sndsdl.h"
#include "debug.h"

static int SNDSDLInit(void);
static void SNDSDLDeInit(void);
static int SNDSDLReset(void);
static int SNDSDLChangeVideoFormat(int vertfreq);
static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len);
static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace(void);
static void SNDSDLMuteAudio(void);
static void SNDSDLUnMuteAudio(void);
static void SNDSDLSetVolume(int volume);
#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport);
u8 SNDSDLMidiIn(int *isdata);
int SNDSDLMidiOut(u8 data);
#endif

SoundInterface_struct SNDSDL = {
SNDCORE_SDL,
"SDL Sound Interface",
SNDSDLInit,
SNDSDLDeInit,
SNDSDLReset,
SNDSDLChangeVideoFormat,
SNDSDLUpdateAudio,
SNDSDLGetAudioSpace,
SNDSDLMuteAudio,
SNDSDLUnMuteAudio,
SNDSDLSetVolume,
#ifdef USE_SCSPMIDI
SNDSDLMidiChangePorts,
SNDSDLMidiIn,
SNDSDLMidiOut
#endif
};

#define NUMSOUNDBLOCKS  4

static u16 *stereodata16;
static u32 soundoffset;          /* write position (bytes), emulator side */
static volatile u32 soundpos;    /* read position (bytes), SDL callback side */
static volatile u32 soundfill;   /* bytes written and not played yet */
static u32 soundlen;
static u32 soundbufsize;
static SDL_AudioSpec audiofmt;
static u8 soundvolume;
static int muted = 0;

//////////////////////////////////////////////////////////////////////////////

/* SDL audio callback (SDL holds the audio lock while it runs).
 *
 * It used to read the ring buffer round and round whether or not the
 * emulator had written to it: when the emulation fell behind, the previous
 * 4 frames were played again (a stutter of stale audio rather than a gap).
 * Only the bytes written and not played yet (soundfill) are played now; the
 * rest of the request is silence. */
static void MixAudio(UNUSED void *userdata, Uint8 *stream, int len) {
	int i;
	Uint8* soundbuf = (Uint8*)stereodata16;

	for (i = 0; i < len; i++)
	{
		if (soundbuf == NULL || soundfill == 0)
		{
			stream[i] = audiofmt.silence;
			continue;
		}
		if (soundpos >= soundbufsize)
			soundpos = 0;
		stream[i] = muted ? audiofmt.silence : soundbuf[soundpos];
		soundpos++;
		soundfill--;
	}
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLInit(void)
{
   //samples should be a power of 2 according to SDL-doc
   //so normalize it to the nearest power of 2 here
   u32 normSamples = 512;

#if defined (_MSC_VER) && SDL_VERSION_ATLEAST(2,0,0)
   SDL_SetMainReady();
#endif
   SDL_InitSubSystem(SDL_INIT_AUDIO);
//   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0);
//      return -1;

   audiofmt.freq = 44100;
   audiofmt.format = AUDIO_S16SYS;
   audiofmt.channels = 2;
   audiofmt.samples = (audiofmt.freq / 60) * 2;
   audiofmt.callback = MixAudio;
   audiofmt.userdata = NULL;

   while (normSamples < audiofmt.samples) 
      normSamples <<= 1;

   audiofmt.samples = normSamples;
   
   soundlen = audiofmt.freq / 60; // 60 for NTSC or 50 for PAL. Initially assume it's going to be NTSC.
   soundbufsize = soundlen * NUMSOUNDBLOCKS * 2 * 2;
   
   soundvolume = SDL_MIX_MAXVOLUME;

   if (SDL_OpenAudio(&audiofmt, NULL) != 0)
   {
      YabSetError(YAB_ERR_SDL, (void *)SDL_GetError());
      return -1;
   }

   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      return -1;

   memset(stereodata16, 0, soundbufsize);

   soundpos = 0;
   soundoffset = 0;
   soundfill = 0;

   SDL_PauseAudio(0);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLDeInit(void)
{
   SDL_CloseAudio();

   if (stereodata16)
      free(stereodata16);
   stereodata16 = NULL;
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLReset(void)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

/* The buffer is replaced under the audio lock: the SDL callback could
   otherwise read the freed one. The positions are reset too: soundoffset
   was kept and, the PAL buffer (882 samples per block) being larger than
   the NTSC one (735), switching from PAL to NTSC left it past the end of
   the new buffer, where the next SNDSDLUpdateAudio wrote. */
static int SNDSDLChangeVideoFormat(int vertfreq)
{
   int ret = 0;

   SDL_LockAudio();

   soundlen = audiofmt.freq / vertfreq;
   soundbufsize = soundlen * NUMSOUNDBLOCKS * 2 * 2;

   if (stereodata16)
      free(stereodata16);

   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      ret = -1;
   else
      memset(stereodata16, 0, soundbufsize);

   soundpos = 0;
   soundoffset = 0;
   soundfill = 0;

   SDL_UnlockAudio();

   return ret;
}

//////////////////////////////////////////////////////////////////////////////

/* Volume and 16-bit clamp. The scaled value used to be written back into
   the source (the SCSP ring buffer, scspchannel[].data32): harmless as long
   as every sample is sent once, but the buffer no longer held what the
   SCSP produced. It is now computed in a local. */
static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len) {
   u32 i;

   for (i = 0; i < len; i++)
   {
      s32 v;

      // Left Channel
      v = (*srcL * soundvolume) / SDL_MIX_MAXVOLUME;
      if (v > 0x7FFF) *dst = 0x7FFF;
      else if (v < -0x8000) *dst = -0x8000;
      else *dst = (s16)v;
      srcL++;
      dst++;
      // Right Channel
      v = (*srcR * soundvolume) / SDL_MIX_MAXVOLUME;
      if (v > 0x7FFF) *dst = 0x7FFF;
      else if (v < -0x8000) *dst = -0x8000;
      else *dst = (s16)v;
      srcR++;
      dst++;
   }
}

static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   u32 copy1size=0, copy2size=0;
   u32 freebytes;
   SDL_LockAudio();

   if (stereodata16 == NULL)
   {
      SDL_UnlockAudio();
      return;
   }

   /* never write over bytes not played yet (the caller asks
      SNDSDLGetAudioSpace first, this is only a guard) */
   freebytes = soundbufsize - soundfill;
   if (num_samples * sizeof(s16) * 2 > freebytes)
      num_samples = freebytes / (sizeof(s16) * 2);

   if ((soundbufsize - soundoffset) < (num_samples * sizeof(s16) * 2))
   {
      copy1size = (soundbufsize - soundoffset);
      copy2size = (num_samples * sizeof(s16) * 2) - copy1size;
   }
   else
   {
      copy1size = (num_samples * sizeof(s16) * 2);
      copy2size = 0;
   }

   sdlConvert32uto16s((s32 *)leftchanbuffer, (s32 *)rightchanbuffer, (s16 *)(((u8 *)stereodata16)+soundoffset), copy1size / sizeof(s16) / 2);

   if (copy2size)
      sdlConvert32uto16s((s32 *)leftchanbuffer + (copy1size / sizeof(s16) / 2), (s32 *)rightchanbuffer + (copy1size / sizeof(s16) / 2), (s16 *)stereodata16, copy2size / sizeof(s16) / 2);

   soundoffset += copy1size + copy2size;
   soundoffset %= soundbufsize;
   soundfill += copy1size + copy2size;

   SDL_UnlockAudio();
}

//////////////////////////////////////////////////////////////////////////////

/* Free space in samples. It was computed from the two positions alone,
   which cannot tell an empty buffer from a full one when they are equal
   (it answered "full"); the fill level is now counted. */
static u32 SNDSDLGetAudioSpace(void)
{
   u32 freespace;

   SDL_LockAudio();
   freespace = soundbufsize - soundfill;
   SDL_UnlockAudio();

   return (freespace / sizeof(s16) / 2);
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLMuteAudio(void)
{
   muted = 1;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLUnMuteAudio(void)
{
   muted = 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLSetVolume(int volume)
{
   soundvolume = ( (double)SDL_MIX_MAXVOLUME /(double)100 ) *volume;
}

//////////////////////////////////////////////////////////////////////////////

#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport)
{
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

u8 SNDSDLMidiIn(int *isdata)
{
	*isdata = 0;
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

int SNDSDLMidiOut(u8 data)
{
	return 1;
}

//////////////////////////////////////////////////////////////////////////////
#endif
#endif
