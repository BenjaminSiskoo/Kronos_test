/*  Copyright 2026 Kronos fork contributors

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

/*  Graphics edits made from the debugger. See vdpedit.h. */

#include <stdlib.h>
#include <string.h>
#include "vdp1.h"
#include "vdp2.h"
#include "vdpedit.h"

/* ======================================================================== */
/* Memory access per space                                                  */
/* ======================================================================== */

static u32 veVdp2Mask(void)
{
   return (Vdp2Regs && (Vdp2Regs->VRSIZE & 0x8000)) ? 0xFFFFF : 0x7FFFF;
}

static u32 veSpaceSize(int space)
{
   switch (space) {
      case VDPEDIT_VDP1: return 0x80000;
      case VDPEDIT_VDP2: return veVdp2Mask() + 1;
      case VDPEDIT_CRAM: return 0x1000;
      default:           return 0;
   }
}

static int veSpaceReady(int space)
{
   switch (space) {
      case VDPEDIT_VDP1: return Vdp1Ram != NULL;
      case VDPEDIT_VDP2: return Vdp2Ram != NULL && Vdp2Regs != NULL;
      case VDPEDIT_CRAM: return Vdp2ColorRam != NULL;
      default:           return 0;
   }
}

/* Byte of a space, Saturn order (color RAM included: T2ReadByte undoes the
   host word order). */
static u8 veRead(int space, u32 a)
{
   switch (space) {
      case VDPEDIT_VDP1: return T1ReadByte(Vdp1Ram, a & 0x7FFFF);
      case VDPEDIT_VDP2: return T1ReadByte(Vdp2Ram, a & veVdp2Mask());
      default:           return T2ReadByte(Vdp2ColorRam, a & 0xFFF);
   }
}

static int veMatches(int space, u32 addr, const u8 *ref, u32 len)
{
   u32 k;
   if (space == VDPEDIT_VDP1)
      return memcmp(Vdp1Ram + addr, ref, len) == 0;
   for (k = 0; k < len; k++)
      if (veRead(space, addr + k) != ref[k]) return 0;
   return 1;
}

static void veMarkVdp1(u32 addr, u32 len)
{
   Vdp1External.updateVdp1Ram = 1;
   if (vdp1Ram_update_start > (int)addr) vdp1Ram_update_start = (int)addr;
   if (vdp1Ram_update_end < (int)(addr + len)) vdp1Ram_update_end = (int)(addr + len);
}

/* Writes through the same paths as the CPU (dirty flags, VDP2 bank capture,
   color RAM mirror and line sync), but without bus timing. */
static void veWrite(int space, u32 addr, const u8 *data, u32 len)
{
   u32 k;
   switch (space) {
      case VDPEDIT_VDP1:
         memcpy(Vdp1Ram + addr, data, len);
         veMarkVdp1(addr, len);
         break;
      case VDPEDIT_VDP2:
         for (k = 0; k < len; k++)
            if (veRead(space, addr + k) != data[k])
               Vdp2RamWriteByte(NULL, Vdp2Ram, (addr + k) & veVdp2Mask(), data[k]);
         break;
      default:
         for (k = 0; k + 1 < len; k += 2)
            Vdp2ColorRamWriteWord(NULL, Vdp2ColorRam, (addr + k) & 0xFFF,
                                  (u16)((data[k] << 8) | data[k + 1]));
         break;
   }
}

/* The VDP1 debugger reads a copy of the VRAM taken at draw start: keep it in
   step, so the edited sprite is what the debugger shows right away. */
static void vePatchVdp1Snapshot(u32 addr, const u8 *from, const u8 *to, u32 len)
{
   u8 *snap = Vdp1DebugGetFrameRam();
   if (snap && memcmp(snap + addr, from, len) == 0)
      memcpy(snap + addr, to, len);
}

/* ======================================================================== */
/* Edit list                                                                */
/* ======================================================================== */

typedef struct {
   int space;
   u32 addr, len;
   u8 *orig, *repl;
} VeEdit;

static VeEdit *veList = NULL;
static int veCount = 0, veCap = 0;
static u32 veGen = 0;

static void veFreeAt(int i)
{
   free(veList[i].orig);
   free(veList[i].repl);
   memmove(&veList[i], &veList[i + 1], sizeof(VeEdit) * (size_t)(veCount - i - 1));
   veCount--;
   veGen++;
}

static int veValid(int space, u32 addr, u32 len)
{
   if (!veSpaceReady(space) || len == 0) return 0;
   if (addr >= veSpaceSize(space) || len > veSpaceSize(space) - addr) return 0;
   if (space == VDPEDIT_CRAM && ((addr | len) & 1)) return 0;
   return 1;
}

static u8 *veDup(const u8 *p, u32 len)
{
   u8 *d = (u8 *)malloc(len);
   if (d) memcpy(d, p, len);
   return d;
}

/* Writes 'to' if memory holds 'from' (or 'from2'). 1 if written. */
static int veApplyOne(int space, u32 addr, u32 len, const u8 *from, const u8 *from2, const u8 *to)
{
   if (veMatches(space, addr, to, len)) return 1;
   if (veMatches(space, addr, from, len) || (from2 && veMatches(space, addr, from2, len))) {
      veWrite(space, addr, to, len);
      return 1;
   }
   return 0;
}

static void veInstallHooks(void)
{
   VdpEditHookVdp1 = VdpEditApplyVdp1;
   VdpEditHookVblank = VdpEditApplyAll;
   VdpEditHookDeInit = VdpEditClearAll;
}

static int veWriteInternal(int space, u32 addr, const u8 *orig, const u8 *repl, u32 len)
{
   int i;
   if (!orig || !repl || !veValid(space, addr, len)) return -1;
   veInstallHooks();

   for (i = 0; i < veCount; i++) {
      VeEdit *e = &veList[i];
      if (e->space != space || e->addr != addr || e->len != len) continue;
      if (memcmp(e->repl, orig, len) != 0 && memcmp(e->orig, orig, len) != 0) continue;

      /* Editing again something already edited: keep the true original. */
      if (memcmp(repl, e->orig, len) == 0) {
         /* back to the original: undo and forget */
         if (veMatches(space, addr, e->repl, len)) {
            if (space == VDPEDIT_VDP1) vePatchVdp1Snapshot(addr, e->repl, e->orig, len);
            veWrite(space, addr, e->orig, len);
         }
         veFreeAt(i);
         return 2;
      } else {
         u8 *old = e->repl, *nr = veDup(repl, len);
         int done;
         if (!nr) return -1;
         if (space == VDPEDIT_VDP1) {
            vePatchVdp1Snapshot(addr, old, nr, len);
            vePatchVdp1Snapshot(addr, e->orig, nr, len);
         }
         done = veApplyOne(space, addr, len, old, e->orig, nr);
         e->repl = nr;
         free(old);
         veGen++;
         return done;
      }
   }

   if (memcmp(orig, repl, len) == 0) return 2;

   if (veCount == veCap) {
      int nc = veCap ? veCap * 2 : 64;
      VeEdit *nl = (VeEdit *)realloc(veList, sizeof(VeEdit) * (size_t)nc);
      if (!nl) return -1;
      veList = nl; veCap = nc;
   }
   {
      VeEdit *e = &veList[veCount];
      e->space = space; e->addr = addr; e->len = len;
      e->orig = veDup(orig, len);
      e->repl = veDup(repl, len);
      if (!e->orig || !e->repl) { free(e->orig); free(e->repl); return -1; }
      veCount++;
      veGen++;
      /* the debugger snapshot may hold the data even if live VRAM moved on */
      if (space == VDPEDIT_VDP1) vePatchVdp1Snapshot(addr, orig, repl, len);
      return veApplyOne(space, addr, len, orig, NULL, repl);
   }
}

int VdpEditWrite(int space, u32 addr, const u8 *orig, const u8 *repl, u32 len)
{
   return veWriteInternal(space, addr, orig, repl, len);
}

int VdpEditRestore(const EditRecord *r)
{
   if (!r) return -1;
   return veWriteInternal(r->type, r->addr, r->orig, r->repl, r->len);
}

int VdpEditRevertRange(int space, u32 addr, u32 len)
{
   int i, n = 0;
   for (i = veCount - 1; i >= 0; i--) {
      VeEdit *e = &veList[i];
      if (e->space != space || e->addr >= addr + len || e->addr + e->len <= addr) continue;
      if (space == VDPEDIT_VDP1) vePatchVdp1Snapshot(e->addr, e->repl, e->orig, e->len);
      if (veSpaceReady(space) && veMatches(space, e->addr, e->repl, e->len))
         veWrite(space, e->addr, e->orig, e->len);
      veFreeAt(i);
      n++;
   }
   return n;
}

void VdpEditRevertAll(void)
{
   int i;
   for (i = veCount - 1; i >= 0; i--) {
      VeEdit *e = &veList[i];
      if (veSpaceReady(e->space) && e->addr + e->len <= veSpaceSize(e->space)) {
         if (e->space == VDPEDIT_VDP1) vePatchVdp1Snapshot(e->addr, e->repl, e->orig, e->len);
         if (veMatches(e->space, e->addr, e->repl, e->len))
            veWrite(e->space, e->addr, e->orig, e->len);
      }
      free(e->orig); free(e->repl);
   }
   veCount = 0;
   veGen++;
}

void VdpEditClearAll(void)
{
   int i;
   for (i = 0; i < veCount; i++) { free(veList[i].orig); free(veList[i].repl); }
   free(veList);
   veList = NULL; veCount = veCap = 0;
   veGen++;
}

static void veApplySpaces(int vdp1Only)
{
   int i;
   for (i = 0; i < veCount; i++) {
      VeEdit *e = &veList[i];
      if (vdp1Only && e->space != VDPEDIT_VDP1) continue;
      if (!veSpaceReady(e->space) || e->addr + e->len > veSpaceSize(e->space)) continue;
      if (veMatches(e->space, e->addr, e->repl, e->len)) continue;
      if (veMatches(e->space, e->addr, e->orig, e->len))
         veWrite(e->space, e->addr, e->repl, e->len);
   }
}

void VdpEditApplyVdp1(void) { if (veCount) veApplySpaces(1); }
void VdpEditApplyAll(void)  { if (veCount) veApplySpaces(0); }

int VdpEditCount(int space)
{
   int i, n = 0;
   if (space < 0) return veCount;
   for (i = 0; i < veCount; i++) if (veList[i].space == space) n++;
   return n;
}

u32 VdpEditGeneration(void) { return veGen; }

int VdpEditIsEdited(int space, u32 addr, u32 len)
{
   int i;
   for (i = 0; i < veCount; i++)
      if (veList[i].space == space && veList[i].addr < addr + len && veList[i].addr + veList[i].len > addr)
         return 1;
   return 0;
}

int VdpEditCollect(EditRecord *out, int max)
{
   int i, n = 0;
   for (i = 0; i < veCount && n < max; i++) {
      EditRecord *r = &out[n];
      r->type = veList[i].space; r->addr = veList[i].addr; r->len = veList[i].len;
      r->orig = veDup(veList[i].orig, r->len);
      r->repl = veDup(veList[i].repl, r->len);
      if (!r->orig || !r->repl) { free(r->orig); free(r->repl); continue; }
      n++;
   }
   return n;
}

void EditsFree(EditRecord *list, int n)
{
   int i;
   for (i = 0; i < n; i++) {
      free(list[i].orig); free(list[i].repl);
      list[i].orig = list[i].repl = NULL;
   }
}

/* ======================================================================== */
/* Color RAM                                                                */
/* ======================================================================== */

int VdpEditCramColorCount(void) { return (Vdp2Internal.ColorMode == 1) ? 2048 : 1024; }
int VdpEditCramBytes(void)      { return (Vdp2Internal.ColorMode == 2) ? 4 : 2; }
u32 VdpEditCramAddr(u32 idx)    { return (idx * (u32)VdpEditCramBytes()) & 0xFFF; }

u32 VdpEditRgb555ToArgb(u16 w)
{
   const u32 r = (w & 0x1F) << 3, g = ((w >> 5) & 0x1F) << 3, b = ((w >> 10) & 0x1F) << 3;
   return 0xFF000000u | (r << 16) | (g << 8) | b;
}

u16 VdpEditArgbToRgb555(u32 argb)
{
   const u32 r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
   return (u16)(0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

u32 VdpEditCramArgb(u32 idx)
{
   if (!Vdp2ColorRam) return 0;
   if (Vdp2Internal.ColorMode == 2) {
      /* 24-bit mode: bits 23-16 B, 15-8 G, 7-0 R */
      const u32 a = VdpEditCramAddr(idx);
      const u16 hi = T2ReadWord(Vdp2ColorRam, a), lo = T2ReadWord(Vdp2ColorRam, a + 2);
      return 0xFF000000u | ((u32)(lo & 0xFF) << 16) | (u32)(lo & 0xFF00) | (hi & 0xFF);
   }
   return VdpEditRgb555ToArgb(T2ReadWord(Vdp2ColorRam, VdpEditCramAddr(idx)));
}

int VdpEditSetCramColor(u32 idx, int r, int g, int b)
{
   u8 orig[4], repl[4];
   const u32 a = VdpEditCramAddr(idx);
   const int n = VdpEditCramBytes();
   int k;
   if (!Vdp2ColorRam) return -1;
   for (k = 0; k < n; k++) orig[k] = T2ReadByte(Vdp2ColorRam, a + k);
   if (n == 4) {
      repl[0] = orig[0];            /* MSB / unused bits kept */
      repl[1] = (u8)b; repl[2] = (u8)g; repl[3] = (u8)r;
   } else {
      const u16 cur = (u16)((orig[0] << 8) | orig[1]);
      const u16 v = (u16)((cur & 0x8000) | (((b >> 3) & 0x1F) << 10) | (((g >> 3) & 0x1F) << 5) | ((r >> 3) & 0x1F));
      repl[0] = (u8)(v >> 8); repl[1] = (u8)v;
   }
   return VdpEditWrite(VDPEDIT_CRAM, a, orig, repl, (u32)n) < 0 ? -1 : 0;
}

int VdpEditCramBlock(u32 firstIdx, int n, u8 *origBytes, u8 *replBytes)
{
   const int bpc = VdpEditCramBytes();
   int i, k, j;
   if (!origBytes || !replBytes || n <= 0 || !Vdp2ColorRam) return -1;
   for (i = 0; i < n; i++) {
      const u32 a = VdpEditCramAddr(firstIdx + (u32)i);
      for (k = 0; k < bpc; k++)
         origBytes[i * bpc + k] = replBytes[i * bpc + k] = T2ReadByte(Vdp2ColorRam, a + k);
      for (j = 0; j < veCount; j++) {
         const VeEdit *e = &veList[j];
         if (e->space != VDPEDIT_CRAM) continue;
         for (k = 0; k < bpc; k++) {
            const u32 b = a + (u32)k;
            if (b >= e->addr && b < e->addr + e->len) {
               origBytes[i * bpc + k] = e->orig[b - e->addr];
               replBytes[i * bpc + k] = e->repl[b - e->addr];
            }
         }
      }
   }
   return 0;
}

/* ======================================================================== */
/* VDP1 sprites                                                             */
/* ======================================================================== */

void VdpEditReadVdp1(u32 addr, u8 *out, u32 len)
{
   u32 k;
   for (k = 0; k < len; k++) out[k] = Vdp1Ram ? T1ReadByte(Vdp1Ram, (addr + k) & 0x7FFFF) : 0;
}

void VdpEditReadVdp2(u32 addr, u8 *out, u32 len)
{
   u32 k;
   for (k = 0; k < len; k++) out[k] = Vdp2Ram ? T1ReadByte(Vdp2Ram, (addr + k) & veVdp2Mask()) : 0;
}

int VdpEditSpriteFromCmd(const u8 *ram, u32 cmdAddr, VdpEditSprite *s)
{
   u16 ctrl;
   if (!s || cmdAddr > 0x7FFE0) return -1;
   if (!ram) ram = Vdp1DebugGetRam();
   if (!ram) return -1;
   ctrl = T1ReadWord((u8 *)ram, cmdAddr);
   if ((ctrl & 0xC000) || (ctrl & 0xF) > 3) return -3;
   s->CMDCTRL = ctrl;
   s->CMDPMOD = T1ReadWord((u8 *)ram, cmdAddr + 0x04);
   s->CMDCOLR = T1ReadWord((u8 *)ram, cmdAddr + 0x06);
   s->CMDSRCA = T1ReadWord((u8 *)ram, cmdAddr + 0x08);
   s->CMDSIZE = T1ReadWord((u8 *)ram, cmdAddr + 0x0A);
   if (((s->CMDPMOD >> 3) & 7) > 5) return -3;
   return 0;
}

void VdpEditSpriteGeometry(const VdpEditSprite *s, int *w, int *h, int *mode, u32 *addr, u32 *bytes)
{
   const int sw = MAX(1, (s->CMDSIZE & 0x3F00) >> 5), sh = MAX(1, s->CMDSIZE & 0xFF);
   const int m = (s->CMDPMOD >> 3) & 7;
   const u32 rowBytes = (m <= 1) ? (u32)(sw / 2) : (m <= 4) ? (u32)sw : (u32)sw * 2;
   if (w) *w = sw;
   if (h) *h = sh;
   if (mode) *mode = m;
   if (addr) *addr = ((u32)s->CMDSRCA * 8) & 0x7FFFF;
   if (bytes) {
      u32 n = rowBytes * (u32)sh;
      const u32 a = ((u32)s->CMDSRCA * 8) & 0x7FFFF;
      if (a + n > 0x80000) n = 0x80000 - a;   /* the hardware wraps; keep the edit in range */
      *bytes = n;
   }
}

int VdpEditSpritePaletteSize(const VdpEditSprite *s)
{
   switch ((s->CMDPMOD >> 3) & 7) {
      case 0: case 1: return 16;
      case 2: return 64;
      case 3: return 128;
      case 4: return 256;
      default: return 0;
   }
}

u32 VdpEditSpriteLutAddr(const VdpEditSprite *s)
{
   /* ST-013-R3: LUT address = CMDCOLR x 8, bits 1-0 of CMDCOLR ignored
      (same as vdp1_prog_compute.h and Vdp1DebugTextureAtAddr) */
   if (((s->CMDPMOD >> 3) & 7) != 1) return 0xFFFFFFFF;
   return ((u32)(s->CMDCOLR & 0xFFFC) * 8) & 0x7FFFF;
}

u32 VdpEditSpriteCodeData(const VdpEditSprite *s, const u8 *lut, u32 code)
{
   switch ((s->CMDPMOD >> 3) & 7) {
      case 0: return (code & 0xF) | (s->CMDCOLR & 0xFFF0);
      case 1: {
         const u32 i = (code & 0xF) * 2;
         if (lut) return ((u32)lut[i] << 8) | lut[i + 1];
         {
            u8 *ram = Vdp1DebugGetRam();
            return ram ? T1ReadWord(ram, (VdpEditSpriteLutAddr(s) + i) & 0x7FFFF) : 0;
         }
      }
      case 2: return (code & 0x3F) | (s->CMDCOLR & 0xFFC0);
      case 3: return (code & 0x7F) | (s->CMDCOLR & 0xFF80);
      case 4: return (code & 0xFF) | (s->CMDCOLR & 0xFF00);
      default: return code & 0xFFFF;
   }
}

u32 VdpEditSpriteDataArgb(u32 data)
{
   u32 v;
   if (!Vdp2Regs || !Vdp2ColorRam) return 0;   /* no game loaded */
   /* Vdp1DebugSpriteDataColor returns the debugger texture format:
      bytes R, G, B, A in memory */
   v = Vdp1DebugSpriteDataColor(data);
#ifdef WORDS_BIGENDIAN
   const u32 r = (v >> 24) & 0xFF, g = (v >> 16) & 0xFF, b = (v >> 8) & 0xFF;
#else
   const u32 r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
#endif
   return 0xFF000000u | (r << 16) | (g << 8) | b;
}

int VdpEditSpriteDataCram(u32 data)
{
   if (!Vdp2Regs) return -1;
   return Vdp1DebugSpriteDataCramIndex(data);
}

int VdpEditSpriteCodeKind(const VdpEditSprite *s, u32 code)
{
   const int mode = (s->CMDPMOD >> 3) & 7;
   const int spd = (s->CMDPMOD & 0x40) != 0;
   const int ecd = !(s->CMDPMOD & 0x80);
   const u32 endCode = (mode <= 1) ? 0xF : (mode <= 4) ? 0xFF : 0x7FFF;
   if (ecd && code == endCode) return 2;
   if (!spd && ((mode == 5) ? !(code & 0x8000) : (code == 0))) return 1;
   return 0;
}

void VdpEditSpriteDecode(int mode, const u8 *bytes, int w, int h, u32 *codes)
{
   const int n = w * h;
   int i;
   for (i = 0; i < n; i++) {
      switch (mode) {
         case 0: case 1: codes[i] = (i & 1) ? (bytes[i >> 1] & 0xF) : (bytes[i >> 1] >> 4); break;
         case 2: case 3: case 4: codes[i] = bytes[i]; break;
         default: codes[i] = ((u32)bytes[2 * i] << 8) | bytes[2 * i + 1]; break;
      }
   }
}

void VdpEditSpriteEncode(int mode, const u32 *codes, int w, int h, u8 *bytes)
{
   const int n = w * h;
   int i;
   for (i = 0; i < n; i++) {
      switch (mode) {
         case 0: case 1:
            if (i & 1) bytes[i >> 1] = (u8)((bytes[i >> 1] & 0xF0) | (codes[i] & 0xF));
            else       bytes[i >> 1] = (u8)((bytes[i >> 1] & 0x0F) | ((codes[i] & 0xF) << 4));
            break;
         case 2: case 3: case 4: bytes[i] = (u8)codes[i]; break;
         default: bytes[2 * i] = (u8)(codes[i] >> 8); bytes[2 * i + 1] = (u8)codes[i]; break;
      }
   }
}

/* ======================================================================== */
/* VDP2 layers                                                              */
/* ======================================================================== */

static u8  v2B(u32 a) { return T1ReadByte(Vdp2Ram, a & veVdp2Mask()); }
static u16 v2W(u32 a) { return T1ReadWord(Vdp2Ram, a & veVdp2Mask() & ~1u); }
static u32 v2L(u32 a) { return ((u32)v2W(a) << 16) | v2W(a + 2); }

static const u32 v2CellBytes[5] = { 0x20, 0x40, 0x80, 0x80, 0x100 };

/* Same as CalcPlaneAddr() (vidshared.h). */
static u32 v2PlaneAddr(const VdpEditBg *b, u32 tmp)
{
   const int deca = b->planeh + b->planew - 2, multi = b->planeh * b->planew;
   if (b->pdsize == 1)
      return (b->pw == 1) ? ((tmp & 0x3F) >> deca) * (u32)(multi * 0x2000)
                          : (tmp >> deca) * (u32)(multi * 0x800);
   return (b->pw == 1) ? ((tmp & 0x1F) >> deca) * (u32)(multi * 0x4000)
                       : ((tmp & 0x7F) >> deca) * (u32)(multi * 0x1000);
}

static void v2PlaneSize(int plsz, int *pw, int *ph)
{
   switch (plsz & 3) {
      case 1: *pw = 2; *ph = 1; break;
      case 3: *pw = 2; *ph = 2; break;
      default: *pw = 1; *ph = 1; break;   /* 2 is prohibited */
   }
}

int VdpEditBgGet(int layer, VdpEditBg *b)
{
   u16 pnc = 0, chctlwh = 0;
   int plsz = 0, bmsz = 0, i;
   u16 mapregs[16];
   u32 mpof = 0;
   const Vdp2 *r = Vdp2Regs;

   if (!b) return -1;
   memset(b, 0, sizeof(*b));
   b->layer = layer;
   if (!r || !Vdp2Ram || !Vdp2ColorRam || layer < 0 || layer >= VDPEDIT_LAYERS) return -1;
   memset(mapregs, 0, sizeof(mapregs));

   switch (layer) {
      case VDPEDIT_NBG0:
         b->enabled = r->BGON & 0x1;   b->tpon = (r->BGON >> 8) & 1;
         chctlwh = r->CHCTLA & 1; b->bitmap = (r->CHCTLA >> 1) & 1; bmsz = (r->CHCTLA >> 2) & 3;
         b->cn = (r->CHCTLA >> 4) & 7; pnc = r->PNCN0; plsz = r->PLSZ & 3;
         mpof = r->MPOFN & 7; b->bmPal = (u32)(r->BMPNA & 7) << 4; b->coff = (u32)(r->CRAOFA & 7) << 8;
         mapregs[0] = r->MPABN0 & 0xFF; mapregs[1] = r->MPABN0 >> 8;
         mapregs[2] = r->MPCDN0 & 0xFF; mapregs[3] = r->MPCDN0 >> 8;
         break;
      case VDPEDIT_NBG1:
         b->enabled = (r->BGON >> 1) & 1; b->tpon = (r->BGON >> 9) & 1;
         chctlwh = (r->CHCTLA >> 8) & 1; b->bitmap = (r->CHCTLA >> 9) & 1; bmsz = (r->CHCTLA >> 10) & 3;
         b->cn = (r->CHCTLA >> 12) & 3; pnc = r->PNCN1; plsz = (r->PLSZ >> 2) & 3;
         mpof = (r->MPOFN >> 4) & 7; b->bmPal = (u32)((r->BMPNA >> 8) & 7) << 4; b->coff = (u32)((r->CRAOFA >> 4) & 7) << 8;
         mapregs[0] = r->MPABN1 & 0xFF; mapregs[1] = r->MPABN1 >> 8;
         mapregs[2] = r->MPCDN1 & 0xFF; mapregs[3] = r->MPCDN1 >> 8;
         break;
      case VDPEDIT_NBG2:
         b->enabled = (r->BGON >> 2) & 1; b->tpon = (r->BGON >> 10) & 1;
         chctlwh = r->CHCTLB & 1; b->cn = (r->CHCTLB >> 1) & 1; pnc = r->PNCN2;
         plsz = (r->PLSZ >> 4) & 3; mpof = (r->MPOFN >> 8) & 7; b->coff = (u32)((r->CRAOFA >> 8) & 7) << 8;
         mapregs[0] = r->MPABN2 & 0xFF; mapregs[1] = r->MPABN2 >> 8;
         mapregs[2] = r->MPCDN2 & 0xFF; mapregs[3] = r->MPCDN2 >> 8;
         break;
      case VDPEDIT_NBG3:
         b->enabled = (r->BGON >> 3) & 1; b->tpon = (r->BGON >> 11) & 1;
         chctlwh = (r->CHCTLB >> 4) & 1; b->cn = (r->CHCTLB >> 5) & 1; pnc = r->PNCN3;
         plsz = (r->PLSZ >> 6) & 3; mpof = (r->MPOFN >> 12) & 7; b->coff = (u32)((r->CRAOFA >> 12) & 7) << 8;
         mapregs[0] = r->MPABN3 & 0xFF; mapregs[1] = r->MPABN3 >> 8;
         mapregs[2] = r->MPCDN3 & 0xFF; mapregs[3] = r->MPCDN3 >> 8;
         break;
      case VDPEDIT_RBG0:
      case VDPEDIT_RBG0B: {
         /* vidcs.c Vdp2DrawRBG0_part: RPMD 0, 2, 3 -> parameter A, 1 -> B */
         const int useB = (layer == VDPEDIT_RBG0B) || ((r->RPMD & 3) == 1);
         b->enabled = (r->BGON >> 4) & 1; b->tpon = (r->BGON >> 12) & 1;
         chctlwh = (r->CHCTLB >> 8) & 1; b->bitmap = (r->CHCTLB >> 9) & 1; bmsz = (r->CHCTLB >> 10) & 1;
         b->cn = (r->CHCTLB >> 12) & 7; pnc = r->PNCR;
         b->bmPal = (u32)(r->BMPNB & 7) << 4; b->coff = (u32)(r->CRAOFB & 7) << 8;
         if (!useB) {
            plsz = (r->PLSZ >> 8) & 3; mpof = r->MPOFR & 7;
            mapregs[0] = r->MPABRA; mapregs[2] = r->MPCDRA; mapregs[4] = r->MPEFRA; mapregs[6] = r->MPGHRA;
            mapregs[8] = r->MPIJRA; mapregs[10] = r->MPKLRA; mapregs[12] = r->MPMNRA; mapregs[14] = r->MPOPRA;
         } else {
            plsz = (r->PLSZ >> 12) & 3; mpof = (r->MPOFR >> 4) & 7;
            mapregs[0] = r->MPABRB; mapregs[2] = r->MPCDRB; mapregs[4] = r->MPEFRB; mapregs[6] = r->MPGHRB;
            mapregs[8] = r->MPIJRB; mapregs[10] = r->MPKLRB; mapregs[12] = r->MPMNRB; mapregs[14] = r->MPOPRB;
         }
         for (i = 0; i < 16; i += 2) { mapregs[i + 1] = mapregs[i] >> 8; mapregs[i] &= 0xFF; }
         break;
      }
      case VDPEDIT_RBG1:
         /* vidcs.c Vdp2DrawRBG1_part: NBG0 character / pattern name
            registers, rotation parameter B planes, CRAOFA bits 2-0 */
         b->enabled = (r->BGON >> 5) & 1; b->tpon = (r->BGON >> 8) & 1;
         chctlwh = r->CHCTLA & 1; b->bitmap = (r->CHCTLA >> 1) & 1; bmsz = (r->CHCTLA >> 2) & 3;
         b->cn = (r->CHCTLA >> 4) & 7; pnc = r->PNCN0;
         b->bmPal = (u32)(r->BMPNA & 7) << 4; b->coff = (u32)(r->CRAOFA & 7) << 8;
         plsz = (r->PLSZ >> 12) & 3; mpof = (r->MPOFR >> 4) & 7;
         mapregs[0] = r->MPABRB; mapregs[2] = r->MPCDRB; mapregs[4] = r->MPEFRB; mapregs[6] = r->MPGHRB;
         mapregs[8] = r->MPIJRB; mapregs[10] = r->MPKLRB; mapregs[12] = r->MPMNRB; mapregs[14] = r->MPOPRB;
         for (i = 0; i < 16; i += 2) { mapregs[i + 1] = mapregs[i] >> 8; mapregs[i] &= 0xFF; }
         break;
   }

   if (b->cn > 4) b->cn = 4;   /* 5-7 prohibited */
   b->pdsize = (pnc & 0x8000) ? 1 : 2;
   b->auxmode = (pnc & 0x4000) >> 14;
   b->supp = pnc & 0x3FF;
   b->pw = chctlwh ? 2 : 1;
   b->csize = 8 * b->pw;
   v2PlaneSize(plsz, &b->planew, &b->planeh);
   b->planesX = b->planesY = (layer >= VDPEDIT_RBG0) ? 4 : 2;

   if (b->bitmap) {
      static const int bw[4] = { 512, 512, 1024, 1024 }, bh[4] = { 256, 512, 256, 512 };
      b->bmW = bw[bmsz & 3]; b->bmH = bh[bmsz & 3];
      b->bmBase = (mpof * 0x20000) & veVdp2Mask();
      b->mapW = b->bmW; b->mapH = b->bmH;
   } else {
      const int n = b->planesX * b->planesY;
      for (i = 0; i < n; i++)
         b->planeAddr[i] = v2PlaneAddr(b, (mpof << 6) | mapregs[i]);
      b->mapW = b->planesX * b->planew * 512;
      b->mapH = b->planesY * b->planeh * 512;
   }
   return b->enabled ? 0 : -3;
}

int VdpEditBgCellAt(const VdpEditBg *b, int x, int y, VdpEditCell *c)
{
   int pageX, pageY, plane, px, py, cx, cy, pagewh;
   u32 pageAddr, pn, charaddr, paladdr;
   int flip;

   if (!b || !c || b->bitmap || x < 0 || y < 0 || x >= b->mapW || y >= b->mapH) return -1;
   pageX = x / 512; pageY = y / 512;
   plane = (pageY / b->planeh) * b->planesX + (pageX / b->planew);
   px = pageX % b->planew; py = pageY % b->planeh;
   pagewh = 64 / b->pw;
   cx = (x % 512) / b->csize; cy = (y % 512) / b->csize;

   pageAddr = b->planeAddr[plane]
            + (u32)(pagewh * pagewh * b->planew * py + pagewh * pagewh * px) * (u32)b->pdsize * 2;
   pn = pageAddr + (u32)(pagewh * cy + cx) * (u32)b->pdsize * 2;

   if (b->pdsize == 1) {
      const u16 t = v2W(pn);
      /* VDP2 manual (pattern name data, 1 word): palette bits 15-12, or
         14-12 for 256+ colors; supplement bits 7-5 extend the 16-color
         palette number. Same decoding as Vdp2PatternAddr (vidshared.c). */
      paladdr = (b->cn == 0) ? (u32)(((t & 0xF000) >> 12) | ((b->supp & 0xE0) >> 1)) : (u32)((t & 0x7000) >> 8);
      if (b->auxmode == 0) {
         flip = (t & 0xC00) >> 10;
         charaddr = (b->pw == 1) ? (u32)((t & 0x3FF) | ((b->supp & 0x1F) << 10))
                                 : (u32)(((t & 0x3FF) << 2) | (b->supp & 3) | ((b->supp & 0x1C) << 10));
      } else {
         flip = 0;
         charaddr = (b->pw == 1) ? (u32)((t & 0xFFF) | ((b->supp & 0x1C) << 10))
                                 : (u32)(((t & 0xFFF) << 2) | (b->supp & 3) | ((b->supp & 0x10) << 10));
      }
   } else {
      const u16 t1 = v2W(pn), t2 = v2W(pn + 2);
      charaddr = t2 & 0x7FFF;
      flip = (t1 & 0xC000) >> 14;
      paladdr = (b->cn == 0) ? (u32)(t1 & 0x7F) : (u32)(t1 & 0x70);
   }
   if (!(Vdp2Regs->VRSIZE & 0x8000)) charaddr &= 0x3FFF;

   c->pnAddr = pn & veVdp2Mask();
   c->charAddr = (charaddr * 0x20) & veVdp2Mask();
   c->charBytes = (u32)(b->pw * b->pw) * v2CellBytes[b->cn];
   c->flip = flip;
   c->palBase = paladdr;
   return 0;
}

int VdpEditBgPaletteSize(int cn)
{
   switch (cn) { case 0: return 16; case 1: return 256; case 2: return 2048; default: return 0; }
}

int VdpEditBgCodeCramIndex(const VdpEditBg *b, u32 palBase, u32 code)
{
   switch (b->cn) {
      case 0: return (int)((((palBase << 4) | (code & 0xF)) + b->coff) & 0x7FF);
      case 1: return (int)((((palBase << 4) | (code & 0xFF)) + b->coff) & 0x7FF);
      case 2: return (int)(((code & 0x7FF) + b->coff) & 0x7FF);
      default: return -1;
   }
}

u32 VdpEditBgCodeArgb(const VdpEditBg *b, u32 palBase, u32 code)
{
   switch (b->cn) {
      case 3:
         if (!(code & 0x8000) && !b->tpon) return 0;
         return VdpEditRgb555ToArgb((u16)code);
      case 4:
         if (!(code & 0x80000000u) && !b->tpon) return 0;
         return 0xFF000000u | ((code & 0xFF) << 16) | (code & 0xFF00) | ((code >> 16) & 0xFF);
      default:
         if (code == 0 && !b->tpon) return 0;
         return VdpEditCramArgb((u32)VdpEditBgCodeCramIndex(b, palBase, code));
   }
}

/* Offset of dot (ix, iy) of a character in its bytes, and the nibble side. */
static u32 v2DotOffset(const VdpEditBg *b, int ix, int iy, int *hiNibble)
{
   const int cell = (iy >> 3) * b->pw + (ix >> 3), x = ix & 7, y = iy & 7;
   const u32 base = (u32)cell * v2CellBytes[b->cn];
   *hiNibble = !(x & 1);
   switch (b->cn) {
      case 0:  return base + (u32)(y * 4 + (x >> 1));
      case 1:  return base + (u32)(y * 8 + x);
      case 4:  return base + (u32)(y * 8 + x) * 4;
      default: return base + (u32)(y * 8 + x) * 2;
   }
}

static u32 v2GetDot(const VdpEditBg *b, const u8 *p, int hi)
{
   switch (b->cn) {
      case 0:  return hi ? (p[0] >> 4) : (p[0] & 0xF);
      case 1:  return p[0];
      case 2:  return (((u32)p[0] << 8) | p[1]) & 0x7FF;
      case 3:  return ((u32)p[0] << 8) | p[1];
      default: return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
   }
}

static void v2PutDot(const VdpEditBg *b, u8 *p, int hi, u32 v)
{
   switch (b->cn) {
      case 0:  p[0] = hi ? (u8)((p[0] & 0x0F) | ((v & 0xF) << 4)) : (u8)((p[0] & 0xF0) | (v & 0xF)); break;
      case 1:  p[0] = (u8)v; break;
      case 2:  p[0] = (u8)((p[0] & 0xF8) | ((v >> 8) & 7)); p[1] = (u8)v; break;   /* bits 15-11 kept */
      case 3:  p[0] = (u8)(v >> 8); p[1] = (u8)v; break;
      default: p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; break;
   }
}

int VdpEditCharDecode(const VdpEditBg *b, const u8 *bytes, u32 *codes)
{
   int ix, iy, hi;
   for (iy = 0; iy < b->csize; iy++)
      for (ix = 0; ix < b->csize; ix++) {
         const u32 off = v2DotOffset(b, ix, iy, &hi);
         codes[iy * b->csize + ix] = v2GetDot(b, bytes + off, hi);
      }
   return 0;
}

int VdpEditCharEncode(const VdpEditBg *b, const u32 *codes, u8 *bytes)
{
   int ix, iy, hi;
   for (iy = 0; iy < b->csize; iy++)
      for (ix = 0; ix < b->csize; ix++) {
         const u32 off = v2DotOffset(b, ix, iy, &hi);
         v2PutDot(b, bytes + off, hi, codes[iy * b->csize + ix]);
      }
   return 0;
}

static u32 v2BitmapPixBytesX2(const VdpEditBg *b)
{
   /* bytes per dot x 2 (16 colors = half a byte) */
   switch (b->cn) { case 0: return 1; case 1: return 2; case 4: return 8; default: return 4; }
}

u32 VdpEditBitmapRowBytes(const VdpEditBg *b, int n)
{
   return ((u32)n * v2BitmapPixBytesX2(b) + 1) / 2;
}

u32 VdpEditBitmapAddr(const VdpEditBg *b, int x, int y)
{
   const u32 row = VdpEditBitmapRowBytes(b, b->bmW);
   return (b->bmBase + (u32)y * row + ((u32)x * v2BitmapPixBytesX2(b)) / 2) & veVdp2Mask();
}

void VdpEditBitmapDecode(const VdpEditBg *b, const u8 *bytes, int n, u32 *codes)
{
   int i;
   for (i = 0; i < n; i++) {
      const u32 off = ((u32)i * v2BitmapPixBytesX2(b)) / 2;
      codes[i] = v2GetDot(b, bytes + off, !(i & 1));
   }
}

void VdpEditBitmapEncode(const VdpEditBg *b, const u32 *codes, int n, u8 *bytes)
{
   int i;
   for (i = 0; i < n; i++) {
      const u32 off = ((u32)i * v2BitmapPixBytesX2(b)) / 2;
      v2PutDot(b, bytes + off, !(i & 1), codes[i]);
   }
}

/* Renders one character at (sx, sy) of 'img' (stride W). */
static void v2RenderChar(const VdpEditBg *b, const VdpEditCell *c, u32 *img, int W, int sx, int sy)
{
   u8 buf[0x400];
   u32 codes[256];
   int i, j;
   VdpEditReadVdp2(c->charAddr, buf, c->charBytes);
   VdpEditCharDecode(b, buf, codes);
   for (j = 0; j < b->csize; j++)
      for (i = 0; i < b->csize; i++) {
         const int fx = (c->flip & 1) ? b->csize - 1 - i : i;
         const int fy = (c->flip & 2) ? b->csize - 1 - j : j;
         img[(size_t)(sy + j) * W + sx + i] = VdpEditBgCodeArgb(b, c->palBase, codes[fy * b->csize + fx]);
      }
}

u32 *VdpEditBgRender(const VdpEditBg *b, int *w, int *h)
{
   u32 *img;
   int x, y;
   if (!b || !Vdp2Ram || b->mapW <= 0 || b->mapH <= 0) return NULL;
   img = (u32 *)calloc((size_t)b->mapW * b->mapH, sizeof(u32));
   if (!img) return NULL;
   *w = b->mapW; *h = b->mapH;

   if (b->bitmap) {
      const u32 rowBytes = VdpEditBitmapRowBytes(b, b->bmW);
      u8 *row = (u8 *)malloc(rowBytes);
      u32 *codes = (u32 *)malloc(sizeof(u32) * (size_t)b->bmW);
      if (!row || !codes) { free(row); free(codes); free(img); return NULL; }
      for (y = 0; y < b->bmH; y++) {
         VdpEditReadVdp2(VdpEditBitmapAddr(b, 0, y), row, rowBytes);
         VdpEditBitmapDecode(b, row, b->bmW, codes);
         for (x = 0; x < b->bmW; x++)
            img[(size_t)y * b->bmW + x] = VdpEditBgCodeArgb(b, b->bmPal, codes[x]);
      }
      free(row); free(codes);
      return img;
   }

   for (y = 0; y < b->mapH; y += b->csize)
      for (x = 0; x < b->mapW; x += b->csize) {
         VdpEditCell c;
         if (VdpEditBgCellAt(b, x, y, &c) == 0)
            v2RenderChar(b, &c, img, b->mapW, x, y);
      }
   return img;
}

/* ---- import of an edited map picture ---- */

static int v2PixEq(u32 a, u32 b)
{
   if ((a >> 24) < 128 && (b >> 24) < 128) return 1;
   return ((a >> 24) >= 128) && ((b >> 24) >= 128) && ((a & 0xFFFFFF) == (b & 0xFFFFFF));
}

static int v2RegionEq(const u32 *x, const u32 *y, int W, int sx, int sy, int n)
{
   int i, j;
   for (j = 0; j < n; j++)
      for (i = 0; i < n; i++)
         if (!v2PixEq(x[(size_t)(sy + j) * W + sx + i], y[(size_t)(sy + j) * W + sx + i])) return 0;
   return 1;
}

static u32 v2Nearest(const VdpEditBg *b, u32 palBase, u32 p, int *approx)
{
   int d, n, best = 0;
   long bestD = 0x7FFFFFFF;
   if ((p >> 24) < 128) return 0;    /* transparent (with TPON there is no transparent code) */
   if (b->cn == 3) return VdpEditArgbToRgb555(p);
   if (b->cn == 4) return 0x80000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
   n = VdpEditBgPaletteSize(b->cn);
   for (d = b->tpon ? 0 : 1; d < n; d++) {
      const u32 c = VdpEditCramArgb((u32)VdpEditBgCodeCramIndex(b, palBase, (u32)d));
      const long dr = (long)((c >> 16) & 0xFF) - (long)((p >> 16) & 0xFF);
      const long dg = (long)((c >> 8) & 0xFF) - (long)((p >> 8) & 0xFF);
      const long db = (long)(c & 0xFF) - (long)(p & 0xFF);
      const long dist = dr * dr + dg * dg + db * db;
      if (dist < bestD) { bestD = dist; best = d; if (dist == 0) break; }
   }
   if (bestD > 0 && approx) (*approx)++;
   return (u32)best;
}

int VdpEditBgImport(const VdpEditBg *b, const u32 *argb, int w, int h, int *stats)
{
   int W = 0, H = 0, x, y, st[4] = { 0, 0, 0, 0 };
   u32 *cur, *after;

   if (!b || !argb) return -1;
   if (!b->enabled) return -3;
   cur = VdpEditBgRender(b, &W, &H);
   if (!cur) return -1;
   if (W != w || H != h) { free(cur); return -2; }

   if (b->bitmap) {
      const u32 rowBytes = VdpEditBitmapRowBytes(b, b->bmW);
      u8 *orow = (u8 *)malloc(rowBytes), *nrow = (u8 *)malloc(rowBytes);
      u32 *codes = (u32 *)malloc(sizeof(u32) * (size_t)W);
      if (!orow || !nrow || !codes) { free(orow); free(nrow); free(codes); free(cur); return -1; }
      for (y = 0; y < H; y++) {
         int eq = 1;
         for (x = 0; x < W && eq; x++) if (!v2PixEq(argb[(size_t)y * W + x], cur[(size_t)y * W + x])) eq = 0;
         if (eq) continue;
         VdpEditReadVdp2(VdpEditBitmapAddr(b, 0, y), orow, rowBytes);
         VdpEditBitmapDecode(b, orow, W, codes);
         for (x = 0; x < W; x++)
            if (!v2PixEq(argb[(size_t)y * W + x], cur[(size_t)y * W + x]))
               codes[x] = v2Nearest(b, b->bmPal, argb[(size_t)y * W + x], &st[3]);
         memcpy(nrow, orow, rowBytes);
         VdpEditBitmapEncode(b, codes, W, nrow);
         if (memcmp(orow, nrow, rowBytes) != 0 &&
             VdpEditWrite(VDPEDIT_VDP2, VdpEditBitmapAddr(b, 0, y), orow, nrow, rowBytes) >= 0)
            st[0]++;
      }
      free(orow); free(nrow); free(codes);
   } else {
      u32 *done = (u32 *)malloc(sizeof(u32) * 16384);
      int ndone = 0;
      if (!done) { free(cur); return -1; }
      for (y = 0; y < H; y += b->csize)
         for (x = 0; x < W; x += b->csize) {
            VdpEditCell c;
            int k, seen = 0;
            if (v2RegionEq(argb, cur, W, x, y, b->csize)) continue;
            if (VdpEditBgCellAt(b, x, y, &c) != 0) continue;
            for (k = 0; k < ndone; k++) if (done[k] == c.charAddr) { seen = 1; break; }
            if (!seen) {
               u8 ob[0x400], nb[0x400];
               u32 codes[256];
               int i, j;
               if (ndone < 16384) done[ndone++] = c.charAddr;
               VdpEditReadVdp2(c.charAddr, ob, c.charBytes);
               VdpEditCharDecode(b, ob, codes);
               for (j = 0; j < b->csize; j++)
                  for (i = 0; i < b->csize; i++) {
                     const int ix = (c.flip & 1) ? b->csize - 1 - i : i;
                     const int iy = (c.flip & 2) ? b->csize - 1 - j : j;
                     const u32 p = argb[(size_t)(y + j) * W + x + i];
                     if (!v2PixEq(p, cur[(size_t)(y + j) * W + x + i]))
                        codes[iy * b->csize + ix] = v2Nearest(b, c.palBase, p, &st[3]);
                  }
               memcpy(nb, ob, c.charBytes);
               VdpEditCharEncode(b, codes, nb);
               if (memcmp(ob, nb, c.charBytes) != 0 &&
                   VdpEditWrite(VDPEDIT_VDP2, c.charAddr, ob, nb, c.charBytes) >= 0)
                  st[0]++;
            } else {
               /* character already rewritten from another place of the map:
                  a conflict only if, seen from here (flip, palette), it does
                  not give the expected picture */
               u32 tmp[256];
               int i, j, ok = 1;
               v2RenderChar(b, &c, tmp, b->csize, 0, 0);
               for (j = 0; j < b->csize && ok; j++)
                  for (i = 0; i < b->csize; i++)
                     if (!v2PixEq(tmp[j * b->csize + i], argb[(size_t)(y + j) * W + x + i])) { ok = 0; break; }
               if (!ok) st[1]++;
            }
         }
      free(done);
   }
   free(cur);

   after = VdpEditBgRender(b, &W, &H);
   if (after) {
      for (y = 0; y + 8 <= H; y += 8)
         for (x = 0; x + 8 <= W; x += 8)
            if (!v2RegionEq(argb, after, W, x, y, 8)) st[2]++;
      free(after);
   }
   if (stats) memcpy(stats, st, sizeof(st));
   return 0;
}
