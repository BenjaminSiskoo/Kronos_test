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

/*  vdpedit -- graphics edits made from the debugger (VDP1 sprites, VDP2
    characters / bitmaps, color RAM), kept alive while the game runs.

    An edit is a pair (original bytes, new bytes) at an address of one of the
    three memories. At the start of every VDP1 draw and at every V-blank, each
    edit is checked: when the memory holds the ORIGINAL bytes again (the game
    reloaded its graphic), the new bytes are written back; any other content
    at that address (another animation frame, another graphic) is left alone.
    Several edits may therefore share one address, one per original content:
    every frame of an animation streamed to the same VRAM area can be edited.

    The same pairs are what the .kmod pack stores and what the disc patcher
    looks for in the image (see port/qt/EditsPatcher.cpp).

    Nothing here runs unless an edit exists: the per-frame hooks return at
    once when the list is empty.

    Byte order: VDP1/VDP2 VRAM bytes are in Saturn order (T1 memory). Color
    RAM bytes are stored as big-endian 16-bit words, i.e. as the SH-2 sees
    them, whatever the host order.

    VDP2 layer decoding (VdpEditBg*) follows the renderer of this fork
    (core/video/opengl/common/vidshared.[ch], compute_shader/vidcs.c), whose
    comments cite the VDP2 User's Manual ST-058-R2. */

#ifndef VDPEDIT_H
#define VDPEDIT_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Edit registry                                                            */
/* ------------------------------------------------------------------------ */

#define VDPEDIT_VDP1 0   /* VDP1 VRAM, 512 KB, byte address        */
#define VDPEDIT_VDP2 1   /* VDP2 VRAM, 512 KB / 1 MB, byte address */
#define VDPEDIT_CRAM 2   /* color RAM, 4 KB, byte address (even)   */

/* Exchange format of an edit (pack / disc patcher). For type 2 (CRAM) the
   historical packs stored a color INDEX in addr with len = 2; such records
   are still accepted by VdpEditRestore (see vdpedit.c). */
typedef struct EditRecord_s {
   int type;          /* VDPEDIT_VDP1 / VDPEDIT_VDP2 / VDPEDIT_CRAM */
   u32 addr, len;
   u8 *orig, *repl;
} EditRecord;

/* Records an edit and writes it at once when the memory holds 'orig' (or the
   true original of an earlier edit made on top of it). 'orig' is what the
   caller read before editing.
   Returns 1 = written now, 0 = recorded, written when the game loads the
   original data again, 2 = edit undone (repl == true original), <0 = error. */
int  VdpEditWrite(int space, u32 addr, const u8 *orig, const u8 *repl, u32 len);

/* Same, for a record read from a pack: written only if the memory holds the
   original bytes right now. Same return values. */
int  VdpEditRestore(const EditRecord *r);

/* Puts the original bytes back and forgets the edits that intersect
   [addr, addr + len) of 'space'. Returns the number of edits removed. */
int  VdpEditRevertRange(int space, u32 addr, u32 len);
/* Puts every original back and empties the list. */
void VdpEditRevertAll(void);
/* Empties the list without touching memory (emulator shutdown). */
void VdpEditClearAll(void);

/* Per-frame work: VDP1 draw start and V-blank. */
void VdpEditApplyVdp1(void);
void VdpEditApplyAll(void);

/* Hooks through which vdp1.c / vdp2.c reach the functions above. They stay
   NULL until the first edit is recorded: the core does not depend on this
   file, so a build without vdpedit.c (libretro) links unchanged.
   VdpEditHookVdp1 is defined in vdp1.c, the two others in vdp2.c. */
extern void (*VdpEditHookVdp1)(void);    /* VdpEditApplyVdp1, VDP1 draw start */
extern void (*VdpEditHookVblank)(void);  /* VdpEditApplyAll, V-blank IN      */
extern void (*VdpEditHookDeInit)(void);  /* VdpEditClearAll, Vdp2DeInit      */

/* Number of edits of a space, or of all spaces with space < 0. */
int  VdpEditCount(int space);
/* Incremented on every change of the list (UI refresh). */
u32  VdpEditGeneration(void);
/* 1 if an edit of 'space' intersects [addr, addr + len). */
int  VdpEditIsEdited(int space, u32 addr, u32 len);

/* Copies of every edit (orig / repl malloc'ed, free with EditsFree).
   Returns the number written. */
int  VdpEditCollect(EditRecord *out, int max);
void EditsFree(EditRecord *list, int n);

/* Color RAM block for the disc patcher: n entries from firstIdx as
   big-endian words, with the edited colors replaced by their original
   (origBytes) or new (replBytes) value. 0 = ok. */
int  VdpEditCramBlock(u32 firstIdx, int n, u8 *origBytes, u8 *replBytes);

/* ------------------------------------------------------------------------ */
/* Color RAM helpers                                                        */
/* ------------------------------------------------------------------------ */

/* Number of colors of the current color RAM mode (1024 or 2048). */
int  VdpEditCramColorCount(void);
/* Byte address of color 'idx' in the current mode. */
u32  VdpEditCramAddr(u32 idx);
/* Bytes per color in the current mode (2, or 4 in 24-bit mode). */
int  VdpEditCramBytes(void);
/* Color 'idx' as 0xAARRGGBB (A = 0xFF). */
u32  VdpEditCramArgb(u32 idx);
/* Sets color 'idx' from 8-bit R, G, B (rounded to 5 bits outside 24-bit
   mode; the MSB of the stored word is kept). Persistent. 0 = ok. */
int  VdpEditSetCramColor(u32 idx, int r, int g, int b);

/* Saturn RGB555 word (MSB set) <-> 0xAARRGGBB. */
u32  VdpEditRgb555ToArgb(u16 w);
u16  VdpEditArgbToRgb555(u32 argb);

/* ------------------------------------------------------------------------ */
/* VDP1 sprites                                                             */
/* ------------------------------------------------------------------------ */

/* The command table words that define what a sprite looks like. Kept by
   value so a sprite seen in an earlier frame can still be edited after the
   game rebuilt its command table. */
typedef struct {
   u16 CMDCTRL, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE;
} VdpEditSprite;

/* Reads the sprite command at cmdAddr of 'ram' (NULL = memory the VDP1
   debugger reads: frame snapshot or live VRAM). 0 = ok, -3 = not a drawn
   sprite (draw end, skipped, not types 0-3, invalid color mode). */
int  VdpEditSpriteFromCmd(const u8 *ram, u32 cmdAddr, VdpEditSprite *s);

/* Size in dots, color mode (CMDPMOD bits 5-3), texture address and size. */
void VdpEditSpriteGeometry(const VdpEditSprite *s, int *w, int *h, int *mode,
                           u32 *addr, u32 *bytes);
/* Number of palette codes of the sprite (16, 64, 128, 256; 0 for RGB). */
int  VdpEditSpritePaletteSize(const VdpEditSprite *s);
/* VDP1 VRAM address of the 16-entry LUT (color mode 1), else 0xFFFFFFFF. */
u32  VdpEditSpriteLutAddr(const VdpEditSprite *s);

/* Frame-buffer data that the VDP1 writes for a texel code (bank mode:
   CMDCOLR | code, LUT: the table entry, RGB: the code itself). 'lut' = the
   16 words of the LUT as Saturn-order bytes, NULL to read VRAM. */
u32  VdpEditSpriteCodeData(const VdpEditSprite *s, const u8 *lut, u32 code);
/* How the VDP2 shows a frame-buffer word (SPCTL type, SPCLMD, SPCAOS):
   0xAARRGGBB, and the color RAM index it reads (-1 = RGB data). Same rules
   as the VDP1 debugger preview (vdp1.c, Vdp1DebugSpriteDataColor). */
u32  VdpEditSpriteDataArgb(u32 data);
int  VdpEditSpriteDataCram(u32 data);
/* 1 if a code is transparent (code 0 / MSB 0 in RGB, SPD = 0), 2 if it is
   the end code (ECD = 0), else 0. */
int  VdpEditSpriteCodeKind(const VdpEditSprite *s, u32 code);

/* Texture bytes (Saturn order) <-> w*h codes, stored layout. */
void VdpEditSpriteDecode(int mode, const u8 *bytes, int w, int h, u32 *codes);
void VdpEditSpriteEncode(int mode, const u32 *codes, int w, int h, u8 *bytes);

/* ------------------------------------------------------------------------ */
/* VDP2 layers                                                              */
/* ------------------------------------------------------------------------ */

enum {
   VDPEDIT_NBG0 = 0, VDPEDIT_NBG1, VDPEDIT_NBG2, VDPEDIT_NBG3,
   VDPEDIT_RBG0,     /* rotation parameter selected by RPMD (A for 0, 2, 3) */
   VDPEDIT_RBG0B,    /* RBG0 read through rotation parameter B              */
   VDPEDIT_RBG1,     /* parameter B, NBG0 character registers               */
   VDPEDIT_LAYERS
};

/* Color formats (CHCTLx CHCN): 0 = 16 palette, 1 = 256 palette,
   2 = 2048 palette, 3 = RGB 15-bit, 4 = RGB 24-bit. */
typedef struct {
   int layer;
   int enabled;        /* BGON bit of the layer                         */
   int bitmap;         /* 1 = bitmap mode                               */
   int cn;             /* color format, see above                       */
   int tpon;           /* transparent display OFF (code 0 is opaque)    */
   u32 coff;           /* color RAM offset, in colors                   */
   /* cell mode */
   int pw;             /* character size: 1 = 1x1 cell, 2 = 2x2 cells   */
   int csize;          /* character size in dots (8 or 16)              */
   int pdsize;         /* pattern name: 1 or 2 words                    */
   int auxmode, supp;  /* PNCx: character number mode, supplement data  */
   int planew, planeh; /* plane size in pages                           */
   int planesX, planesY; /* planes in the map: 2x2 (NBG) or 4x4 (RBG)   */
   u32 planeAddr[16];  /* VRAM address of each plane                    */
   /* bitmap mode */
   u32 bmBase;         /* VRAM address of the bitmap                    */
   int bmW, bmH;
   u32 bmPal;          /* palette number (already shifted, x16 colors)  */
   /* whole map / bitmap, in dots */
   int mapW, mapH;
} VdpEditBg;

typedef struct {
   u32 pnAddr;         /* VRAM address of the pattern name              */
   u32 charAddr;       /* VRAM address of the character                 */
   u32 charBytes;      /* bytes of the character                        */
   int flip;           /* bit 0 horizontal, bit 1 vertical              */
   u32 palBase;        /* palette number x16 (16 / 256 colors)          */
} VdpEditCell;

/* Reads the registers of a layer. 0 = ok (enabled), -3 = layer off,
   -1 = invalid. 'b' is filled in both 0 and -3 cases. */
int  VdpEditBgGet(int layer, VdpEditBg *b);
/* Cell at map dot (x, y) (cell mode only). 0 = ok. */
int  VdpEditBgCellAt(const VdpEditBg *b, int x, int y, VdpEditCell *c);
/* Whole map (cell mode) or bitmap as 0xAARRGGBB, A = 0 for transparent dots.
   No scroll, zoom, window or color calculation. free() the buffer. */
u32 *VdpEditBgRender(const VdpEditBg *b, int *w, int *h);

/* Number of codes of a color format (16 / 256 / 2048, 0 for RGB). */
int  VdpEditBgPaletteSize(int cn);
/* Color of a dot code with a palette (palBase as in VdpEditCell).
   Codes are dot values: 4/8/11-bit palette codes, RGB555 words (MSB =
   bit 15) or RGB888 longs (MSB = bit 31). Transparent -> 0. */
u32  VdpEditBgCodeArgb(const VdpEditBg *b, u32 palBase, u32 code);
/* Color RAM index of a palette code (-1 for RGB formats). */
int  VdpEditBgCodeCramIndex(const VdpEditBg *b, u32 palBase, u32 code);

/* Dots of a character in stored orientation, csize*csize codes.
   'fromBytes' != NULL decodes those bytes instead of VRAM. */
int  VdpEditCharDecode(const VdpEditBg *b, const u8 *bytes, u32 *codes);
int  VdpEditCharEncode(const VdpEditBg *b, const u32 *codes, u8 *bytes);
/* Bitmap row segment <-> codes. 'bytes' covers the dots [x, x + n) of the
   row and must start on a byte boundary (x even for 16 colors). */
u32  VdpEditBitmapRowBytes(const VdpEditBg *b, int n);
u32  VdpEditBitmapAddr(const VdpEditBg *b, int x, int y);
void VdpEditBitmapDecode(const VdpEditBg *b, const u8 *bytes, int n, u32 *codes);
void VdpEditBitmapEncode(const VdpEditBg *b, const u32 *codes, int n, u8 *bytes);

/* VDP2 VRAM read helper (honours VRSIZE). */
void VdpEditReadVdp2(u32 addr, u8 *out, u32 len);
void VdpEditReadVdp1(u32 addr, u8 *out, u32 len);

/* Re-imports an edited copy of VdpEditBgRender() (same size, 0xAARRGGBB).
   Only the characters / bitmap rows whose picture differs from the current
   one are rewritten, each color mapped to the nearest code of the palette
   of its cell. stats: [0] characters / rows changed, [1] conflicts (shared
   character edited two different ways), [2] 8x8 blocks still different
   after import, [3] dots mapped to the nearest color.
   0 = ok, -1 invalid, -2 size differs, -3 layer off. */
int  VdpEditBgImport(const VdpEditBg *b, const u32 *argb, int w, int h, int *stats);

#ifdef __cplusplus
}
#endif

#endif
