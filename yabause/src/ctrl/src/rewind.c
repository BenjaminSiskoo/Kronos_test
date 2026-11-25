/*  Rewind for Kronos (Qt port). See ctrl/include/rewind.h. */
#include <stdlib.h>
#include <string.h>
#include "rewind.h"
#include "memory.h"
#include "movie.h"
#include "yabause.h"
#include "osdcore.h"
#include "yui.h"

typedef struct {
   u8 *data;       /* DeltaEncode() of (older state, newer state) */
   u32 packed;
   u32 oldSize;    /* size of the older state */
   u32 span;       /* bytes covered by the delta: the larger of the two sizes */
} RewindDelta;

static int cfgEnabled = 0, cfgInterval = 3, cfgSeconds = 30, cfgMemoryMB = 512;

static RewindDelta *ring = NULL;   /* circular, 'cap' entries */
static int cap = 0, head = 0, count = 0;   /* head = oldest */
static size_t usedBytes = 0;

static u8 *cur = NULL;             /* state at the newest capture */
static u32 curSize = 0;
static int sinceLast = 0;          /* frames since the newest capture */
/* Rewinding mode: set by a step, left when no step comes for REWIND_RESUME_FRAMES
 * frames. While it lasts nothing is captured and every step goes one capture
 * further back. Without it, a held key (about 2 frames between two key repeats,
 * fewer than the capture interval) restored the same capture each time and the
 * game stayed stuck on the spot. The first key repeat comes about half a second
 * after the press, hence the window (the Qt port steps on a timer while the key is
 * held, so only separate taps depend on it). */
#define REWIND_RESUME_FRAMES 30
static int rewinding = 0;
static int playFrames = 0;         /* frames run since the last step */
static int busy = 0;


/* ---- Difference coder ---------------------------------------------------
   A delta is a list of runs: varint(skip) varint(len) len bytes, where skip is
   the number of unchanged bytes since the end of the previous run and the
   bytes are (old ^ new). Unchanged areas are skipped 8 bytes at a time: a frame
   of a game changes a small part of the 5 MB state. Runs closer than 8 bytes
   are merged (a literal byte is cheaper than a new header). */
static u32 PutVar(u8 *o, u32 v)
{
   u32 n = 0;
   while (v >= 0x80) { o[n++] = (u8)(v | 0x80); v >>= 7; }
   o[n++] = (u8)v;
   return n;
}

static int GetVar(const u8 *src, u32 len, u32 *pos, u32 *v)
{
   u32 r = 0, sh = 0;
   while (*pos < len && sh < 35) {
      u8 b = src[(*pos)++];
      r |= (u32)(b & 0x7F) << sh;
      if (!(b & 0x80)) { *v = r; return 0; }
      sh += 7;
   }
   return -1;
}

/* out must hold size + size / 2 + 64 bytes. Returns the length written. */
static u32 DeltaEncode(const u8 *a, const u8 *b, u32 size, u8 *out)
{
   u32 i = 0, o = 0, last = 0;
   while (i < size) {
      u32 start, end, k;
      /* skip equal bytes */
      while (i + 8 <= size) {
         u64 x, y;
         memcpy(&x, a + i, 8); memcpy(&y, b + i, 8);
         if (x != y) break;
         i += 8;
      }
      while (i < size && a[i] == b[i]) i++;
      if (i >= size) break;
      start = i;
      /* extend the run while another differing byte follows within 8 bytes */
      end = start + 1;
      for (;;) {
         u32 lim = (end + 8 < size) ? end + 8 : size;
         for (k = end; k < lim; k++)
            if (a[k] != b[k]) break;
         if (k >= lim) break;
         end = k + 1;
      }
      /* 'end' is one past the last differing byte */
      o += PutVar(out + o, start - last);
      o += PutVar(out + o, end - start);
      for (k = start; k < end; k++) out[o++] = (u8)(a[k] ^ b[k]);
      last = end;
      i = end;
   }
   return o;
}

/* a ^= delta. Returns 0, or -1 when the delta does not fit 'size'. */
static int DeltaApply(u8 *a, u32 size, const u8 *d, u32 len)
{
   u32 pos = 0, at = 0;
   while (pos < len) {
      u32 skip, n, k;
      if (GetVar(d, len, &pos, &skip) != 0 || GetVar(d, len, &pos, &n) != 0) return -1;
      if (skip > size - at || n > size - at - skip || n > len - pos) return -1;
      at += skip;
      for (k = 0; k < n; k++) a[at + k] ^= d[pos + k];
      pos += n; at += n;
   }
   return 0;
}

static int WantedCapacity(void)
{
   int n = (cfgSeconds * 60) / (cfgInterval > 0 ? cfgInterval : 1);
   if (n < 2) n = 2;
   return n;
}

static void FreeRing(void)
{
   int i;
   if (ring != NULL) {
      for (i = 0; i < count; i++)
         free(ring[(head + i) % cap].data);
      free(ring);
   }
   ring = NULL; cap = 0; head = 0; count = 0; usedBytes = 0;
}

void RewindClear(void)
{
   FreeRing();
   free(cur);
   cur = NULL; curSize = 0; sinceLast = 0; rewinding = 0; playFrames = 0;
}

void RewindDeInit(void)
{
   RewindClear();
}

void RewindConfigure(int enabled, int interval, int seconds, int memoryMB)
{
   if (interval < 1) interval = 1;
   if (interval > 60) interval = 60;
   if (seconds < 1) seconds = 1;
   if (seconds > 600) seconds = 600;
   if (memoryMB < 16) memoryMB = 16;
   if (memoryMB > 4096) memoryMB = 4096;
   if (interval != cfgInterval || seconds != cfgSeconds || memoryMB != cfgMemoryMB || !enabled)
      RewindClear();             /* geometry changed: older differences no longer match */
   cfgInterval = interval; cfgSeconds = seconds; cfgMemoryMB = memoryMB;
   cfgEnabled = enabled ? 1 : 0;
}

void RewindSetEnabled(int enabled)
{
   cfgEnabled = enabled ? 1 : 0;
   if (!cfgEnabled)
      RewindClear();
}

int RewindIsEnabled(void)
{
   return cfgEnabled;
}

int RewindAvailableSeconds(void)
{
   return (count * cfgInterval) / 60;
}

static int Active(void)
{
   /* a state load while a movie is recorded or played would rewrite the movie */
   return cfgEnabled && !IsMovieLoaded();
}

static void DropOldest(void)
{
   if (count <= 0) return;
   usedBytes -= ring[head].packed;
   free(ring[head].data);
   ring[head].data = NULL;
   head = (head + 1) % cap;
   count--;
}

static void Capture(void)
{
   void *buf = NULL;
   size_t size = 0;
   int status;
   u8 *packed, *shrunk;
   u32 plen, span;
   const size_t budget = (size_t)cfgMemoryMB * 1024u * 1024u;

   MovieStateSetLight(1);
   status = YabSaveStateBuffer(&buf, &size);
   MovieStateSetLight(0);
   if (status != 0 || buf == NULL || size == 0 || size > 0x7FFFFFFFu) {
      static int warned = 0;
      if (!warned) { warned = 1; YuiMsg("Rewind: the state could not be captured (status %d), nothing will be kept\n", status); }
      free(buf);
      return;
   }

   if (cur == NULL) {
      cur = (u8 *)buf;
      curSize = (u32)size;
      return;
   }

   if (cap != WantedCapacity()) {
      FreeRing();
      cap = WantedCapacity();
      ring = (RewindDelta *)calloc((size_t)cap, sizeof(RewindDelta));
      if (ring == NULL) { cap = 0; free(buf); return; }
   }

   /* The size of a state can change from one capture to the next (data that
    * grows, such as a recorded movie). The two states are then compared as
    * if both were padded with zeros up to the larger size. */
   span = curSize > (u32)size ? curSize : (u32)size;
   packed = (u8 *)malloc((size_t)span + span / 2 + 64);
   if (packed == NULL) { free(buf); return; }
   if (curSize == (u32)size) {
      plen = DeltaEncode(cur, (const u8 *)buf, span, packed);
   } else {
      u8 *pa = (u8 *)calloc(1, span), *pb = (u8 *)calloc(1, span);
      if (pa == NULL || pb == NULL) { free(pa); free(pb); free(packed); free(buf); return; }
      memcpy(pa, cur, curSize);
      memcpy(pb, buf, size);
      plen = DeltaEncode(pa, pb, span, packed);
      free(pa); free(pb);
   }
   shrunk = (u8 *)realloc(packed, plen ? plen : 1);
   if (shrunk != NULL) packed = shrunk;

   while (count >= cap || (count > 0 && usedBytes + plen > budget))
      DropOldest();
   ring[(head + count) % cap].data = packed;
   ring[(head + count) % cap].packed = plen;
   ring[(head + count) % cap].oldSize = curSize;
   ring[(head + count) % cap].span = span;
   count++;
   usedBytes += plen;

   free(cur);
   cur = (u8 *)buf;
   curSize = (u32)size;
}

void RewindFrameEnd(void)
{
   if (busy || !Active())
      return;
   if (rewinding) {
      if (++playFrames <= REWIND_RESUME_FRAMES)
         return;
      /* no more steps: play on from here and capture again at once */
      rewinding = 0;
      sinceLast = cfgInterval;
   }
   if (++sinceLast < cfgInterval)
      return;
   sinceLast = 0;
   busy = 1;
   Capture();
   busy = 0;
}

int RewindStep(void)
{
   int status;
   int n;
   if (busy || !Active() || cur == NULL)
      return -1;

   if (rewinding || sinceLast == 0) {
      /* Rewinding, or exactly at the newest capture: go to the previous one. */
      RewindDelta *d;
      if (count <= 0) {
         if (!rewinding) return -1;
         goto reload;                /* oldest capture reached: stay on it */
      }
      d = &ring[(head + count - 1) % cap];
      if (d->span > curSize) {
         u8 *grown = (u8 *)realloc(cur, d->span);
         if (grown == NULL) { RewindClear(); return -1; }
         memset(grown + curSize, 0, d->span - curSize);
         cur = grown;
         curSize = d->span;
      }
      if (DeltaApply(cur, curSize, d->data, d->packed) != 0) {
         YuiMsg("Rewind: damaged delta, buffer cleared\n");
         RewindClear();
         return -1;
      }
      curSize = d->oldSize;
      usedBytes -= d->packed;
      free(d->data);
      d->data = NULL;
      count--;
   }
reload:
   sinceLast = 0;
   rewinding = 1;
   playFrames = 0;

   busy = 1;
   MovieStateSetLight(1);
   status = YabLoadStateBuffer(cur, curSize);
   MovieStateSetLight(0);
   busy = 0;
   if (status != 0) {
      YuiMsg("Rewind: loading the state failed (status %d), buffer cleared\n", status);
      RewindClear();
      return -1;
   }
   n = RewindAvailableSeconds();
   OSDPushMessage(OSDMSG_STATUS, 40, "REWIND (%d s left)", n);
   return 0;
}
