/* Copyright 2003-2006 Guillaume Duhamel
    Copyright 2004 Lawrence Sebald
    Copyright 2004-2007 Theo Berkau

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

/*! \file vidcs.c
    \brief OpenGL video renderer
*/
#if defined(HAVE_LIBGL) || defined(__ANDROID__) || defined(IOS)

#include <math.h>
#define EPSILON (1e-10 )

#include "vidcs.h"
#include "vidshared.h"
#include "debug.h"
#include "vdp2.h"
#include "yabause.h"
#include "ygl.h"
#include "yui.h"
#include "vdp1_compute.h"

#define Y_MAX(a, b) ((a) > (b) ? (a) : (b))
#define Y_MIN(a, b) ((a) < (b) ? (a) : (b))

#define LOG_AREA
#define LOG_CMD

static int renderer_started = 0;
static Vdp2 baseVdp2Regs;
static int drawcell_run = 0;

static int isEnabled(int id, Vdp2* varVdp2Regs);
static void VIDCSVdp2DrawScreens(void);
static void Vdp2SetResolution(u16 TVMD);

extern GLuint GetCSVDP1fb(int id);

// Correction : Double déclaration de baseVdp2Regs supprimée ici.

static int vdp1_interlace = 0;

int GlWidth = 320;
int GlHeight = 224;

int vdp1cor = 0;
int vdp1cog = 0;
int vdp1cob = 0;

static int vdp2busy = 0;
static int screenDirty = 0;

vdp2rotationparameter_struct  Vdp1ParaA;

// Correction : (void) pour éviter l'avertissement de liste de paramètres indéterminée
static void Vdp2DrawRBG0(void);
static void Vdp2DrawRBG1(void);

static u32 Vdp2ColorRamGetLineColor(u32 colorindex, int alpha);
static int Vdp2PatternAddrPos(Vdp2Ctrl *ctrl, int planex, int x, int planey, int y);
static void Vdp2DrawPatternPos(Vdp2Ctrl *ctrl, int x, int y, int cx, int cy, int lines);
static INLINE void ReadVdp2ColorOffset(Vdp2 * regs, vdp2draw_struct *info, int mask);
static INLINE u16 Vdp2ColorRamGetColorRaw(u32 colorindex);
static void FASTCALL Vdp2DrawRotation(RBGDrawInfo * rbg);

// Correction : Prototype mis en conformité avec l'argument RBGDrawInfo*
static void Vdp2DrawRotation_in_sync(RBGDrawInfo * rbg);

static int FASTCALL Vdp2CheckWindowRange(Vdp2Ctrl *ctrl, int x, int y, int w, int h);
static void requestDrawCell(Vdp2Ctrl * ctrl);
static void requestDrawCellQuad(Vdp2Ctrl *ctrl);
static INLINE u32 Vdp2RotationFetchPixel(vdp2draw_struct *info, int x, int y, int cellw);
static u32 Vdp2ColorRamGetLineColorOffset(u32 colorindex, int alpha, int offset);
static void FASTCALL Vdp2DrawBitmapCoordinateInc(Vdp2Ctrl *ctrl);
static void FASTCALL Vdp2DrawBitmapLineScroll(Vdp2Ctrl *ctrl, int width, int height);
static void Vdp2DrawMapPerLine(Vdp2Ctrl *ctrl);
static void Vdp2DrawMapTest(Vdp2Ctrl *ctrl, int delayed);
static int Vdp2CheckCharAccessPenalty(int char_access, int ptn_access, int char_size_2x2);
static int sameVDP2Reg(int id, Vdp2 *a, Vdp2 *b);

static void Vdp2GenLineinfo(vdp2draw_struct *info);
static void Vdp2DrawBackScreen(Vdp2 *varVdp2Regs);
static void Vdp2DrawLineColorScreen(Vdp2 *varVdp2Regs);
static void Vdp1SetTextureRatio(int vdp2widthratio, int vdp2heightratio);

// Correction : Prototypes mis à jour avec les 3 arguments (Vdp2*, int, int)
static void Vdp2DrawNBG0(Vdp2* varVdp2Regs, int startLine, int endLine);
static void Vdp2DrawNBG1(Vdp2* varVdp2Regs, int startLine, int endLine);
static void Vdp2DrawNBG2(Vdp2* varVdp2Regs, int startLine, int endLine);
static void Vdp2DrawNBG3(Vdp2* varVdp2Regs, int startLine, int endLine);

/* Correction : prototype 'finishRbgQueue' supprimé — la fonction
 * n'était ni définie ni appelée nulle part (déclaration morte). */

static pixel_t *VIDCSGetVdp2ScreenExtract(u32 screen, int * w, int * h);

static vdp2Lineinfo lineNBG0[512];
static vdp2Lineinfo lineNBG1[512];

#define WA_INSIDE (0)
#define WA_OUTSIDE (1)

extern void YglGenReset();

int VIDCSInit(void);
void VIDCSDeInit(void);
void VIDCSResize(int, int, unsigned int, unsigned int, int);
void VIDCSGetScale(float *, float *, int *, int *);
int VIDCSIsFullscreen(void);
int VIDCSVdp1Reset(void);

/* Correction : déclaration 'extern Vdp1ParaA' redondante supprimée ici.
 * La variable est déjà définie dans ce même fichier (voir plus haut :
 * 'vdp2rotationparameter_struct Vdp1ParaA;'). Un 'extern' local au
 * fichier de définition est trompeur et inutile. */

void VIDCSVdp1Draw();
void VIDCSVdp1NormalSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1ScaledSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1DistortedSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1PolygonDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1PolylineDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1LineDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1UserClipping(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1SystemClipping(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1NormalSpriteDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1ScaledSpriteDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1DistortedSpriteDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1PolygonDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1PolylineDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1LineDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1UserClippingUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1SystemClippingUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
void VIDCSVdp1DrawFB(void);
void VIDCSReadColorOffset(void);
void VIDCSVdp1LocalCoordinate(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
extern void VIDCSRender(Vdp2 *varVdp2Regs);
extern void VIDCSRenderVDP1(void);
extern void VIDCSFinsihDraw(void);

extern void startVdp1RenderUpscale();
extern void endVdp1RenderUpscale();
extern void startVdp1Render();
extern void endVdp1Render();

 int VIDCSVdp2Reset(void);
 void VIDCSVdp2Draw(void);
extern void VIDCSGetGlSize(int *width, int *height);
extern void VIDCSSetSettingValueMode(int type, int value);
extern void VIDCSSync();
extern void VIDCSVdp2DispOff(void);
extern int VIDCSGenFrameBuffer();

static void VIDCSSetupVdp1Scale(int scale);

static void VIDCSStartVdp1Render(void);
static void VIDCSStartVdp1RenderUpscale(void);
static void VIDCSEndVdp1Render(void);
static void VIDCSEndVdp1RenderUpscale(void);


VideoInterface_struct VIDCS = {
VIDCORE_CS,
"Compute Shader Video Interface",
VIDCSInit,
VIDCSDeInit,
VIDCSResize,
VIDCSGetScale,
VIDCSIsFullscreen,
VIDCSVdp1Reset,
VIDCSVdp1Draw,
VIDCSVdp1NormalSpriteDraw,
VIDCSVdp1ScaledSpriteDraw,
VIDCSVdp1DistortedSpriteDraw,
VIDCSVdp1PolygonDraw,
VIDCSVdp1PolylineDraw,
VIDCSVdp1LineDraw,
VIDCSVdp1UserClipping,
VIDCSVdp1SystemClipping,
VIDCSVdp1LocalCoordinate,
VIDCSEraseWriteVdp1,
VIDCSFrameChangeVdp1,
NULL,
VIDCSVdp2Reset,
VIDCSVdp2Draw,
VIDCSGetGlSize,
VIDCSSetSettingValueMode,
VIDCSSync,
VIDCSVdp2DispOff,
VIDCSRender,
VIDCSRenderVDP1,
VIDCSGenFrameBuffer,
VIDCSFinsihDraw,
VIDCSVdp1DrawFB,
VIDCSGetVdp2ScreenExtract,
VIDCSSetupVdp1Scale,
VIDCSStartVdp1Render,
VIDCSEndVdp1Render
};


#define LOG_ASYN

static void FASTCALL Vdp2DrawCell_in_sync(Vdp2Ctrl *ctrl);

#define NB_MSG 256

YabMutex * Vdp2CtrlLock = NULL;

YabEventQueue *VDP2CtrlStack = NULL;
YabEventQueue *RBGStack = NULL;

Vdp2Ctrl* popCtrl() {
  return (Vdp2Ctrl *)YabWaitEventQueue(VDP2CtrlStack);
}

void pushCtrl(Vdp2Ctrl* val) {
  YabAddEventQueue(VDP2CtrlStack,val);
}

RBGDrawInfo* popRBG() {
  return (RBGDrawInfo *)YabWaitEventQueue(RBGStack);
}

void pushRBG(RBGDrawInfo* val) {
  YabAddEventQueue(RBGStack,val);
}

static void VIDCSSetupVdp1Scale(int scale) {
  if (scale == 1) {
    VIDCS.Vdp1NormalSpriteDraw = VIDCSVdp1NormalSpriteDraw;
    VIDCS.Vdp1ScaledSpriteDraw = VIDCSVdp1ScaledSpriteDraw;
    VIDCS.Vdp1DistortedSpriteDraw = VIDCSVdp1DistortedSpriteDraw;
    VIDCS.Vdp1PolygonDraw = VIDCSVdp1PolygonDraw;
    VIDCS.Vdp1PolylineDraw = VIDCSVdp1PolylineDraw;
    VIDCS.Vdp1LineDraw = VIDCSVdp1LineDraw;
    VIDCS.Vdp1UserClipping = VIDCSVdp1UserClipping;
    VIDCS.Vdp1SystemClipping = VIDCSVdp1SystemClipping;
    VIDCS.endVdp1Render = VIDCSEndVdp1Render;
    VIDCS.startVdp1Render = VIDCSStartVdp1Render;
  } else {
    VIDCS.Vdp1NormalSpriteDraw = VIDCSVdp1NormalSpriteDrawUpscale;
    VIDCS.Vdp1ScaledSpriteDraw = VIDCSVdp1ScaledSpriteDrawUpscale;
    VIDCS.Vdp1DistortedSpriteDraw = VIDCSVdp1DistortedSpriteDrawUpscale;
    VIDCS.Vdp1PolygonDraw = VIDCSVdp1PolygonDrawUpscale;
    VIDCS.Vdp1PolylineDraw = VIDCSVdp1PolylineDrawUpscale;
    VIDCS.Vdp1LineDraw = VIDCSVdp1LineDrawUpscale;
    VIDCS.Vdp1UserClipping = VIDCSVdp1UserClippingUpscale;
    VIDCS.Vdp1SystemClipping = VIDCSVdp1SystemClippingUpscale;
    VIDCS.endVdp1Render = VIDCSEndVdp1RenderUpscale;
    VIDCS.startVdp1Render = VIDCSStartVdp1RenderUpscale;
  }
}

static void VIDCSStartVdp1Render(void) {
 startVdp1Render();
}
static void VIDCSStartVdp1RenderUpscale(void) {
  startVdp1RenderUpscale();
}

static void VIDCSEndVdp1Render(void) {
  endVdp1Render();
}
static void VIDCSEndVdp1RenderUpscale(void) {
  endVdp1RenderUpscale();
}

#define CELL_SINGLE 0x1
#define CELL_QUAD   0x2

static void Vdp2DrawPatternPos(Vdp2Ctrl *ctrl, int x, int y, int cx, int cy, int lines)
{
  u64 cacheaddr = (ctrl->info.paladdr << 20) | ctrl->info.charaddr | ctrl->info.transparencyenable |
    ((ctrl->info.patternpixelwh >> 4) << 1) | (((u64)(ctrl->info.coloroffset >> 8) & 0x07) << 32) | (((u64)(ctrl->info.idScreen) & 0x07) << 39)
    | ((u32)(ctrl->info.alpha_per_line[y] >> 3) << 27);
  int priority = ctrl->info.priority;
  YglCache c;
  vdp2draw_struct tile = ctrl->info;
  int winmode = 0;

  tile.dst = 0;
  tile.colornumber = ctrl->info.colornumber;
  tile.mosaicxmask = ctrl->info.mosaicxmask;
  tile.mosaicymask = ctrl->info.mosaicymask;
  tile.idScreen = ctrl->info.idScreen;

  tile.cellw = tile.cellh = ctrl->info.patternpixelwh;
  tile.flipfunction = ctrl->info.flipfunction;

  if (ctrl->info.specialprimode == 1) {
    ctrl->info.priority = (ctrl->info.priority & 0xFFFFFFFE) | ctrl->info.specialfunction;
  }

  cacheaddr |= ((u64)(ctrl->info.priority) & 0x07) << 42;

  tile.priority = ctrl->info.priority;

  tile.vertices[0] = x;
  tile.vertices[1] = y;
  tile.vertices[2] = (x + tile.cellw);
  tile.vertices[3] = y;
  tile.vertices[4] = (x + tile.cellh);
  tile.vertices[5] = (y + lines /*(float)ctrl->info.lineinc*/);
  tile.vertices[6] = x;
  tile.vertices[7] = (y + lines/*(float)ctrl->info.lineinc*/ );

  /* Screen culling. `x`/`y` (and therefore tile.vertices[]) are expressed
   * in pre-zoom "source" units whenever coordincx/coordincy != 1.0 --
   * Vdp2DrawMapTest's draww/drawh loop bounds are computed as
   * rwidth/coordincx and rheight/coordincy, so the h/v (and thus x/y)
   * values handed to this function already live in that scaled space.
   * The actual on-screen position is only produced later, at draw time,
   * by YglQuadOffset(..., coordincx, coordincy, ...).
   *
   * Comparing the raw (unscaled) vertices directly against
   * _Ygl->rwidth/rheight is only correct when coordincx == coordincy == 1.0
   * (no zoom). As soon as an NBG layer has a non-1.0 coordinate increment
   * (any zoom in or out, e.g. Golden Axe: The Duel's magnified background),
   * tiles that are still legitimately on-screen once scaled were being
   * culled here, leaving the rest of the screen undrawn (black). */
  if ((tile.vertices[0] * ctrl->info.coordincx)  >= _Ygl->rwidth    // tile à droite de l'écran
      || (tile.vertices[2] * ctrl->info.coordincx) < 0              // tile à gauche de l'écran
      || (tile.vertices[1] * ctrl->info.coordincy) >= _Ygl->rheight // tile en dessous de l'écran
      || (tile.vertices[5] * ctrl->info.coordincy) < 0)             // tile au-dessus de l'écran
  {
      return;
  }

  if ((_Ygl->Win0[ctrl->info.idScreen] != 0 || _Ygl->Win1[ctrl->info.idScreen] != 0) && ctrl->info.coordincx == 1.0f && ctrl->info.coordincy == 1.0f)
  {                                                 // coordinate inc is not supported yet.
    winmode = Vdp2CheckWindowRange(ctrl, x - cx, y - cy, tile.cellw, ctrl->info.lineinc);
    if (winmode == 0) // all outside, no need to draw
    {
      return;
    }
  }

  tile.cor = ctrl->info.cor;
  tile.cog = ctrl->info.cog;
  tile.cob = ctrl->info.cob;

  if (1 == YglIsCached(YglTM_vdp2, cacheaddr, &c))
  {
    YglCachedQuadOffset(&tile, &c, cx, cy, ctrl->info.coordincx, ctrl->info.coordincy, YglTM_vdp2);
    return;
  }

  YglQuadOffset(&tile, &ctrl->texture, &c, cx, cy, ctrl->info.coordincx, ctrl->info.coordincy, YglTM_vdp2);
  YglCacheAdd(YglTM_vdp2, cacheaddr, &c);
  switch (ctrl->info.patternwh)
  {
  case 1:
    requestDrawCell(ctrl);
    break;
  case 2:
    ctrl->texture.w += 8;
    requestDrawCellQuad(ctrl);
    break;
  }
  ctrl->info.priority = priority;
}


//////////////////////////////////////////////////////////////////////////////

static int Vdp2PatternAddrPos(Vdp2Ctrl *ctrl, int planex, int x, int planey, int y)
{

  u32 addr = ctrl->info.addr +
    (ctrl->info.pagewh*ctrl->info.pagewh*ctrl->info.planew*planey +
      ctrl->info.pagewh*ctrl->info.pagewh*planex +
      ctrl->info.pagewh*y +
      x)*ctrl->info.patterndatasize * 2;

  int ptnAddrBk = (((addr >> 16)& 0xF) >> ((ctrl->regs->VRSIZE >> 15)&0x1)) >> 1;
  if (ctrl->info.pname_bank[ptnAddrBk] == 0) return 0;

  switch (ctrl->info.patterndatasize)
  {
  case 1:
  {
    u16 tmp = Vdp2RamReadWord(NULL, Vdp2Ram, addr);

    ctrl->info.specialfunction = (ctrl->info.supplementdata >> 9) & 0x1;
    ctrl->info.specialcolorfunction = (ctrl->info.supplementdata >> 8) & 0x1;

    switch (ctrl->info.colornumber)
    {
    case 0: // in 16 colors
      ctrl->info.paladdr = ((tmp & 0xF000) >> 12) | ((ctrl->info.supplementdata & 0xE0) >> 1);
      break;
    default: // not in 16 colors
      ctrl->info.paladdr = (tmp & 0x7000) >> 8;
      break;
    }

    switch (ctrl->info.auxmode)
    {
    case 0:
      ctrl->info.flipfunction = (tmp & 0xC00) >> 10;

      switch (ctrl->info.patternwh)
      {
      case 1:
        ctrl->info.charaddr = (tmp & 0x3FF) | ((ctrl->info.supplementdata & 0x1F) << 10);
        break;
      case 2:
        ctrl->info.charaddr = ((tmp & 0x3FF) << 2) | (ctrl->info.supplementdata & 0x3) | ((ctrl->info.supplementdata & 0x1C) << 10);
        break;
      }
      break;
    case 1:
      ctrl->info.flipfunction = 0;

      switch (ctrl->info.patternwh)
      {
      case 1:
        ctrl->info.charaddr = (tmp & 0xFFF) | ((ctrl->info.supplementdata & 0x1C) << 10);
        break;
      case 2:
        ctrl->info.charaddr = ((tmp & 0xFFF) << 2) | (ctrl->info.supplementdata & 0x3) | ((ctrl->info.supplementdata & 0x10) << 10);
        break;
      }
      break;
    }

    break;
  }
  case 2: {
    u16 tmp1 = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
    u16 tmp2 = Vdp2RamReadWord(NULL, Vdp2Ram, addr + 2);
    ctrl->info.charaddr = tmp2 & 0x7FFF;
    ctrl->info.flipfunction = (tmp1 & 0xC000) >> 14;
    switch (ctrl->info.colornumber) {
    case 0:
      ctrl->info.paladdr = (tmp1 & 0x7F);
      break;
    default:
      ctrl->info.paladdr = (tmp1 & 0x70);
      break;
    }
    ctrl->info.specialfunction = (tmp1 & 0x2000) >> 13;
    ctrl->info.specialcolorfunction = (tmp1 & 0x1000) >> 12;
    break;
  }
  }

  if (!(ctrl->regs->VRSIZE & 0x8000))
    ctrl->info.charaddr &= 0x3FFF;

  ctrl->info.charaddr *= 0x20; // thanks Runik

  return 1;
}

static void Vdp2DrawRotation_in_sync(RBGDrawInfo * rbg)
{
    if (rbg == NULL) return;

    // Seules ces variables sont réellement utilisées par le code actuel
    int cellw, cellh;
    vdp2draw_struct *info = &rbg->ctrl.info;

    cellw = rbg->ctrl.info.cellw;
    cellh = rbg->ctrl.info.cellh;

    if (rbg->rbg_type == 0)
    {
        rbg->paraA.dx = rbg->paraA.A * rbg->paraA.deltaX + rbg->paraA.B * rbg->paraA.deltaY;
        rbg->paraA.dy = rbg->paraA.D * rbg->paraA.deltaX + rbg->paraA.E * rbg->paraA.deltaY;
        rbg->paraA.Xp = rbg->paraA.A * (rbg->paraA.Px - rbg->paraA.Cx) +
            rbg->paraA.B * (rbg->paraA.Py - rbg->paraA.Cy) +
            rbg->paraA.C * (rbg->paraA.Pz - rbg->paraA.Cz) + rbg->paraA.Cx + rbg->paraA.Mx;
        rbg->paraA.Yp = rbg->paraA.D * (rbg->paraA.Px - rbg->paraA.Cx) +
            rbg->paraA.E * (rbg->paraA.Py - rbg->paraA.Cy) +
            rbg->paraA.F * (rbg->paraA.Pz - rbg->paraA.Cz) + rbg->paraA.Cy + rbg->paraA.My;
    }

    if (rbg->useb)
    {
        rbg->paraB.dx = rbg->paraB.A * rbg->paraB.deltaX + rbg->paraB.B * rbg->paraB.deltaY;
        rbg->paraB.dy = rbg->paraB.D * rbg->paraB.deltaX + rbg->paraB.E * rbg->paraB.deltaY;
        rbg->paraB.Xp = rbg->paraB.A * (rbg->paraB.Px - rbg->paraB.Cx) + rbg->paraB.B * (rbg->paraB.Py - rbg->paraB.Cy)
            + rbg->paraB.C * (rbg->paraB.Pz - rbg->paraB.Cz) + rbg->paraB.Cx + rbg->paraB.Mx;
        rbg->paraB.Yp = rbg->paraB.D * (rbg->paraB.Px - rbg->paraB.Cx) + rbg->paraB.E * (rbg->paraB.Py - rbg->paraB.Cy)
            + rbg->paraB.F * (rbg->paraB.Pz - rbg->paraB.Cz) + rbg->paraB.Cy + rbg->paraB.My;
    }

    rbg->paraA.over_pattern_name = rbg->ctrl.regs->OVPNRA;
    rbg->paraB.over_pattern_name = rbg->ctrl.regs->OVPNRB;

    rbg->ctrl.info.cellw = rbg->hres;
    rbg->ctrl.info.cellh = (rbg->vres * (info->endLine - info->startLine)) / yabsys.VBlankLineCount;

    if (info->isbitmap) {
        rbg->ctrl.info.cellw = cellw;
        rbg->ctrl.info.cellh = cellh;
    }

    // Appel au moteur de rendu OpenGL (Ygl)
    YglQuadRbg0(rbg, NULL, &rbg->c, rbg->rbg_type, YglTM_vdp2, rbg->ctrl.regs);

    // Mise à jour des offsets de couleur de ligne
    _Ygl->useLineColorOffset[0] = ((rbg->ctrl.regs->KTCTL & 0x0010) != 0) ? _Ygl->linecolorcoef_tex[0] : 0;
    _Ygl->useLineColorOffset[1] = ((rbg->ctrl.regs->KTCTL & 0x1000) != 0) ? _Ygl->linecolorcoef_tex[1] : 0;
}

  /*------------------------------------------------------------------------------
   Rotate Screen drawing
   ------------------------------------------------------------------------------*/
static void FASTCALL Vdp2DrawRotation(RBGDrawInfo * rbg)
  {
    vdp2draw_struct *info = &rbg->ctrl.info;
    YglTexture *texture = &rbg->ctrl.texture;

    int x, y;
    int oldcellx = -1, oldcelly = -1;
    int screenHeight = _Ygl->rheight;
    int screenWidth  = _Ygl->rwidth;

      rbg->vres = _Ygl->rheight;
      if (_Ygl->rwidth >= 640) rbg->hres = (_Ygl->rwidth >> 1); else rbg->hres = _Ygl->rwidth;

    rbg->hres *= _Ygl->widthRatio;
    rbg->vres *= _Ygl->heightRatio;

    RBGGenerator_init(_Ygl->width, _Ygl->height);
    info->vertices[0] = 0;
    info->vertices[1] = (screenHeight * info->startLine)/yabsys.VBlankLineCount;
    info->vertices[2] = screenWidth;
    info->vertices[3] = (screenHeight * info->startLine)/yabsys.VBlankLineCount;
    info->vertices[4] = screenWidth;
    info->vertices[5] = (screenHeight * info->endLine)/yabsys.VBlankLineCount;
    info->vertices[6] = 0;
    info->vertices[7] = (screenHeight * info->endLine)/yabsys.VBlankLineCount;
    info->flipfunction = 0;
    info->cor = 0x00;
    info->cog = 0x00;
    info->cob = 0x00;
	
    /* VDP2 §6.2: only RPMD bits 1-0 are defined; mask them explicitly
     * so the 'use parameter B' fast-path is robust against undefined
     * upper bits returned by the host on register reads. */
    if ((rbg->ctrl.regs->RPMD & 0x3) != 0) rbg->useb = 1;

    if (!info->isbitmap)
    {
      oldcellx = -1;
      oldcelly = -1;
      rbg->pagesize = info->pagewh*info->pagewh;
      rbg->patternshift = (2 + info->patternwh);
    }
    else
    {
      oldcellx = 0;
      oldcelly = 0;
      rbg->pagesize = 0;
      rbg->patternshift = 0;
    }

      u64 cacheaddr = 0x90000000BAD;

      rbg->vdp2_sync_flg = -1;

      Vdp2DrawRotation_in_sync(rbg);
      pushRBG(rbg);
  }

#ifdef CELL_ASYNC

YabEventQueue *cellq = NULL;
YabEventQueue *cellq_end = NULL;

typedef struct {
  Vdp2Ctrl *ctrl;
} drawCellTask;

static int nbLoop = 0;
static int nbClear = 0;

void* Vdp2DrawCell_in_async(void *p)
{
   while(drawcell_run != 0){
     Vdp2Ctrl *ctrl = (Vdp2Ctrl *)YabWaitEventQueue(cellq);
     if (ctrl != NULL) {
       if (ctrl->order == CELL_SINGLE) {
         Vdp2DrawCell_in_sync(ctrl);
       } else {
         Vdp2DrawCell_in_sync(ctrl);
         ctrl->texture.textdata -= (ctrl->texture.w + 8) * 8 - 8;
         Vdp2DrawCell_in_sync(ctrl);

         ctrl->texture.textdata -= 8;
         ctrl->info.draw_line += 8;
         Vdp2DrawCell_in_sync(ctrl);
         ctrl->texture.textdata -= (ctrl->texture.w + 8) * 8 - 8;
         Vdp2DrawCell_in_sync(ctrl);

       }
       pushCtrl(ctrl);
     }
     YabWaitEventQueue(cellq_end);
   }
   return NULL;
}

static void FASTCALL Vdp2DrawCell(Vdp2Ctrl *ctrl) {

   if (drawcell_run == 0) {
     drawcell_run = 1;
     cellq = YabThreadCreateQueue(NB_MSG);
     cellq_end = YabThreadCreateQueue(NB_MSG);
     YabThreadStart(YAB_THREAD_VDP2_NBG0, Vdp2DrawCell_in_async, 0);
   }
   Vdp2Ctrl *ctrl_toExec = popCtrl();
   memcpy(ctrl_toExec, ctrl, sizeof(Vdp2Ctrl));
   YabAddEventQueue(cellq_end, NULL);
   YabAddEventQueue(cellq, ctrl_toExec);
   // YabThreadYield();
}

static void requestDrawCell(Vdp2Ctrl *ctrl) {
#ifdef CELL_ASYNC
   ctrl->order = CELL_SINGLE;
   Vdp2DrawCell(ctrl);
#else
   Vdp2DrawCell_in_sync(ctrl);
#endif
}

static void requestDrawCellQuad(Vdp2Ctrl *ctrl) {
#ifdef CELL_ASYNC
   ctrl->order = CELL_QUAD;
   Vdp2DrawCell(ctrl);
#else
   Vdp2DrawCell_in_sync(ctrl);
   ctrl->texture.textdata -= (ctrl->texture.w + 8) * 8 - 8;
   Vdp2DrawCell_in_sync(ctrl);
   ctrl->texture.textdata -= 8;
   Vdp2DrawCell_in_sync(ctrl);
   ctrl->texture.textdata -= (ctrl->texture.w + 8) * 8 - 8;
   Vdp2DrawCell_in_sync(ctrl);
#endif
}
#endif

//////////////////////////////////////////////////////////////////////////////

int VIDCSInit(void)
{
  if(renderer_started)
    return -1;

  if (YglInit(2048, 1024, 8) != 0)
    return -1;

  for (int i=0; i<SPRITE; i++)
    YglReset(_Ygl->vdp2levels[i]);

  memset(_Ygl->last_back_color, 0x0, 4*sizeof(float));
  _Ygl->vdp1ratio = 1.0;
  if (VIDCore->SetupVdp1Scale) VIDCore->SetupVdp1Scale((int)_Ygl->vdp1ratio);

  _Ygl->vdp1wdensity = 1.0;
  _Ygl->vdp1hdensity = 1.0;

  _Ygl->vdp2wdensity = 1.0;
  _Ygl->vdp2hdensity = 1.0;
  YglChangeResolution(320, 224);

  VDP2CtrlStack = YabThreadCreateQueue(NB_MSG);
  for (int i=0; i<NB_MSG; i++) pushCtrl((Vdp2Ctrl*)calloc(sizeof(Vdp2Ctrl),1));

  RBGStack = YabThreadCreateQueue(NB_MSG);
  for (int i=0; i<NB_MSG; i++) pushRBG((RBGDrawInfo*)calloc(sizeof(RBGDrawInfo),1));

  renderer_started = 1;
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSDeInit(void)
{
  if(!renderer_started)
    return;
#ifdef CELL_ASYNC
  if (drawcell_run == 1) {
    drawcell_run = 0;
    for (int i=0; i<4; i++) {
      YabAddEventQueue(cellq_end, NULL);
      YabAddEventQueue(cellq, NULL);
    }
    YabThreadWait(YAB_THREAD_VDP2_NBG0);
    YabThreadWait(YAB_THREAD_VDP2_NBG1);
    YabThreadWait(YAB_THREAD_VDP2_NBG2);
    YabThreadWait(YAB_THREAD_VDP2_NBG3);
  }
#endif
  YglGenReset();
  YglDeInit();

  renderer_started = 0;
}

int WaitVdp2Async(int sync) {
  int empty = 0;
  /* Fix: this whole block used to be gated behind `if (vdp2busy == 1)`.
   * vdp2busy is a one-shot latch: the first WaitVdp2Async() call in a
   * frame that finds the async cell-draw queue empty resets it to 0,
   * and every LATER call in the SAME frame then short-circuits here,
   * even if new async work (e.g. from NBG0's tile cache filling in
   * YglTMRealloc) was queued after that reset. That let callers like
   * YglTMRealloc() -- which relies on this function to guarantee the
   * async worker thread is done before the texture atlas buffer is
   * unmapped/replaced -- silently skip the wait, unmapping/deleting a
   * buffer the worker thread was still writing into (and likewise
   * skip RBGGenerator_onFinish() at the wrong time). This was mostly
   * invisible before because NBG0 rarely queued enough real tile
   * fills to trigger a mid-frame atlas realloc; now that it does,
   * the gap became reachable. Always run the check -- it's already
   * cheap and safe to call repeatedly (YaGetQueueSize on an empty
   * queue just returns 0 immediately). */
#ifdef CELL_ASYNC
  if (cellq_end != NULL) {
    empty = 1;
    while (((empty = YaGetQueueSize(cellq_end))!=0) && (sync == 1))
    {
      YabThreadYield();
    }
  }
#endif
  RBGGenerator_onFinish();
  if (empty == 0) vdp2busy = 0;
  return empty;
}

void waitVdp2DrawScreensEnd(int sync) {
  YglCheckFBSwitch(0);
  if (vdp2busy == 1) {
    WaitVdp2Async(sync);
    YglTmPush(YglTM_vdp2);
    if (VIDCore != NULL) {
      VIDCSReadColorOffset();
      VIDCore->composeFB(&Vdp2Lines[0]);
    }
  }
}

void addCSCommands(vdp1cmd_struct* cmd, int type)
{
  //Test game: Sega rally : The aileron at the start
  int ADx = (cmd->CMDXD - cmd->CMDXA);
  int ADy = (cmd->CMDYD - cmd->CMDYA);
  int BCx = (cmd->CMDXC - cmd->CMDXB);
  int BCy = (cmd->CMDYC - cmd->CMDYB);

  int nbStepAD = sqrt(ADx*ADx + ADy*ADy);
  int nbStepBC = sqrt(BCx*BCx + BCy*BCy);

  int nbStep = MAX(nbStepAD, nbStepBC);

  cmd->type = type;

  cmd->nbStep = nbStep;
  if(cmd->nbStep  != 0) {
    // Ici faut voir encore les Ax doivent faire un de plus.
    cmd->uAstepx = (float)ADx/(float)nbStep;
    cmd->uAstepy = (float)ADy/(float)nbStep;
    cmd->uBstepx = (float)BCx/(float)nbStep;
    cmd->uBstepy = (float)BCy/(float)nbStep;
  } else {
    cmd->uAstepx = 0.0;
    cmd->uAstepy = 0.0;
    cmd->uBstepx = 0.0;
    cmd->uBstepy = 0.0;
  }
#ifdef DEBUG_VDP1_CMD
  YuiMsg("Add Distorted\n");
  YuiMsg("\t[%d,%d]\n", cmd->CMDXA, cmd->CMDYA);
  YuiMsg("\t[%d,%d]\n", cmd->CMDXB, cmd->CMDYB);
  YuiMsg("\t[%d,%d]\n", cmd->CMDXC, cmd->CMDYC);
  YuiMsg("\t[%d,%d]\n", cmd->CMDXD, cmd->CMDYD);
  YuiMsg("\n\n");
  YuiMsg("=> %d (%d %d %d %d => %d %d) %f %f %f %f\n", cmd->nbStep, ADx, ADy, BCx, BCy, nbStepAD, nbStepBC, cmd->uAstepx, cmd->uAstepy, cmd->uBstepx, cmd->uBstepy);
  YuiMsg("==============\n");
#endif
  vdp1_add_upscale(cmd,0);
}

//////////////////////////////////////////////////////////////////////////////
void VIDCSVdp1Draw()
{
  int i;
  int line = 0;
  Vdp2 *varVdp2Regs = &Vdp2Lines[yabsys.LineCount];
  _Ygl->vpd1_running = 1;

  _Ygl->msb_shadow_count_[_Ygl->drawframe] = 0;

  Vdp1DrawCommands(Vdp1Ram, Vdp1Regs);

  _Ygl->vpd1_running = 0;
}


//////////////////////////////////////////////////////////////////////////////
void VIDCSVdp1NormalSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);
  /* VDP1 §6.3 SPD (CMDPMOD bit 6): transparent pixel disable.
   * When SPD=0: transparent code (mode 0-4: pixel==0; mode 5: pixel==0x0000)
   *             is not drawn.
   * When SPD=1: transparent code is drawn as a normal pixel (black in RGB).
   * For RGB mode (mode 5): transparent test is (dot == 0x0000), NOT (MSB==0).
   * MSB=0 with non-zero data is a palette bank code, not transparent in VDP1 FB. */

  /* VDP1 User's Manual ST-013-R3 §4.4 p.53 + §7.4 p.118
   * (Normal Sprite Draw command):
   *   Vertex A is the top-left of the sprite at (CMDXA, CMDYA).
   *   The other three vertices are derived from A and the character size
   *   stored in CMDSIZE (cmd->w, cmd->h):
   *     B = (XA + w-1, YA      )      top-right
   *     C = (XA + w-1, YA + h-1)      bottom-right
   *     D = (XA      , YA + h-1)      bottom-left
   *
   * Without these explicit vertices, the downstream geometry path falls
   * back on whatever leftover B/C/D values are still in the command
   * struct (the VDP1 hardware re-derives them from CMDSIZE in real time;
   * a software emulator must do it explicitly).  The Upscale variant
   * already does this calculation - the non-upscale path was missing it,
   * which sometimes manifested as 1-pixel sprite seams or stale
   * dimensions when the same vdp1cmd_struct slot was reused.
   *
   * Use MAX(1, .) to defend against a CMDSIZE field of 0.
   * VDP1 Manual ST-013-R3 §6.6 p.104 (CMDSIZE) :
   *   bits 13-8 : Character size X / 8  (1..63 → 8..504 px,  0 prohibited)
   *   bits  7-0 : Character size Y      (1..255 px,          0 prohibited)
   * Games occasionally upload a stale 0 during list construction
   * even though both axes prohibit it ; clamp to 1 to keep the
   * geometry well-formed. */

  cmd->CMDXB = cmd->CMDXA + MAX(1, cmd->w) - 1;
  cmd->CMDYB = cmd->CMDYA;
  cmd->CMDXC = cmd->CMDXA + MAX(1, cmd->w) - 1;
  cmd->CMDYC = cmd->CMDYA + MAX(1, cmd->h) - 1;
  cmd->CMDXD = cmd->CMDXA;
  cmd->CMDYD = cmd->CMDYA + MAX(1, cmd->h) - 1;
   
  /* VDP2 Manual §9.2: For RGB sprite data, priority register 0 is always
   * selected (bits 2~0 of CCRSA = PRISA bits 2~0).
   * Do NOT modify CCRSA globally — just set a flag on the command so the
   * shader uses priority register 0. Setting cmd type is sufficient.
     Remove the cclist masking entirely — it corrupts global state
	  if (((cmd->CMDPMOD >> 3) & 0x7u) == 5) {
		// hard/vdp2/hon/p09_20.htm#no9_21
		u32 *cclist = (u32 *)&(Vdp2Lines[0].CCRSA);
		cclist[0] &= 0x1Fu;
	  }*/
  cmd->type = QUAD;

  vdp1_add(cmd,0);

  LOG_CMD("%d\n", __LINE__);
}

void VIDCSVdp1ScaledSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{

  /* VDP2 Manual §9.2: For RGB sprite data, priority register 0 is always
   * selected (bits 2~0 of CCRSA = PRISA bits 2~0).
   * Do NOT modify CCRSA globally — just set a flag on the command so the
   * shader uses priority register 0. Setting cmd type is sufficient.
     Remove the cclist masking entirely — it corrupts global state
	  if (((cmd->CMDPMOD >> 3) & 0x7u) == 5) {
		// hard/vdp2/hon/p09_20.htm#no9_21
		u32 *cclist = (u32 *)&(Vdp2Lines[0].CCRSA);
		cclist[0] &= 0x1Fu;
	  }*/

  cmd->type = QUAD;
  vdp1_add(cmd,0);

  LOG_CMD("%d\n", __LINE__);
}

void VIDCSVdp1DistortedSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);

  /* VDP2 Manual §9.2: For RGB sprite data, priority register 0 is always
   * selected (bits 2~0 of CCRSA = PRISA bits 2~0).
   * Do NOT modify CCRSA globally — just set a flag on the command so the
   * shader uses priority register 0. Setting cmd type is sufficient.
     Remove the cclist masking entirely — it corrupts global state
	  if (((cmd->CMDPMOD >> 3) & 0x7u) == 5) {
		// hard/vdp2/hon/p09_20.htm#no9_21
		u32 *cclist = (u32 *)&(Vdp2Lines[0].CCRSA);
		cclist[0] &= 0x1Fu;
	  }*/

  cmd->type = DISTORTED;
  vdp1_add(cmd,0);

  return;
}

// Dans VIDCSVdp1PolygonDraw — vérifier qu'on ne masque pas CMDPMOD
void VIDCSVdp1PolygonDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
   /* VDP1 Manual §6.3 CMDPMOD bits 2-0 — Color Calculation mode table:
   *  000 Replace:           write sprite pixel as-is, no FB read
   * Shadow (001) and Half-transparent (011):
   *   - Read FB pixel at draw coordinate.
   *   - If FB_MSB = 0: replace (shadow) or replace (half-transp) → no blend.
   *   - If FB_MSB = 1: shadow → FB_rgb >>= 1 (preserve MSB).
   *                    half-transp → (sprite + FB) / 2 (preserve MSB).
   *   Shadow does NOT modify the MSB bit (VDP1 §6.3 explicit requirement).
   *  010 Half-luminance:    sprite_out = sprite_in >> 1 (each channel)
   *  011 Half-transparent:  if FB MSB=1 → (sprite+FB)/2; else replace
   *  100 Gouraud:           sprite + Gouraud interpolated offset
   *  101 Prohibited:        do not use
   *  110 Gouraud+Half-lum:  Gouraud then >> 1
   *  111 Gouraud+Half-transp: Gouraud result, MSB-dep half-transparency
   *
   * CMDPMOD bits 2-0 MUST reach the compute shader intact via cmd->CMDPMOD.
   * Do NOT mask or clear these bits before calling vdp1_add().
   * Mesh (CMDPMOD bit 8):
   *   Draw pixel when: (fb_x & 1) XOR (fb_y & 1) == 0  (i.e. (fb_x+fb_y) even)
   *   Skip pixel otherwise.  Uses FRAME BUFFER coordinates, not screen coords.
   *   MSB-ON (MON=1) still applies to drawn pixels in mesh mode. */
  cmd->type = POLYGON;
  vdp1_add(cmd, 0);
  return;
}

void VIDCSVdp1PolylineDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);

  cmd->type = POLYLINE;

  vdp1_add(cmd,0);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1LineDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);

  cmd->type = LINE;

  vdp1_add(cmd,0);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1UserClipping(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
    /* VDP1 Manual §7.2 p.112-113: this command sets the user clip
     * RECTANGLE only.  Per §7.2 p.113: "whether the inside or the
     * outside of the area is clipped is determined by the draw mode
     * of the draw command for the part" — i.e. Clip (CMDPMOD bit 10)
     * and Cmod (CMDPMOD bit 9) are read PER-DRAW, not here. */
    // VDP1 Manual §7.2: "Operation cannot be guaranteed if XC < XA or YC < YA"
    // The hardware does NOT reset localX/Y — it simply produces undefined results.
    // We skip the command silently to avoid rendering artifacts.
    if (  ((s16)cmd->CMDXC < (s16)cmd->CMDXA)
       || ((s16)cmd->CMDYC < (s16)cmd->CMDYA)
    ) {
        // Invalid clip rectangle: skip command, do not modify local coordinates
        return;
    }
    cmd->type = USER_CLIPPING;
    regs->userclipX1 = cmd->CMDXA;
    regs->userclipY1 = cmd->CMDYA;
    regs->userclipX2 = cmd->CMDXC;
    regs->userclipY2 = cmd->CMDYC;
    /* userclipMode is now refreshed in vdp1_add() from the draw cmd
     * itself — see Patch 04. */
    vdp1_add(cmd, 1);
}

//////////////////////////////////////////////////////////////////////////////

 void VIDCSVdp1SystemClipping(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
 {
  /* VDP1 §7.1: "Operation cannot be ensured if XC < 0 or YC < 0." */
   if ((s16)cmd->CMDXC < 0 || (s16)cmd->CMDYC < 0) return;
   if (((cmd->CMDXC) == regs->systemclipX2) && (regs->systemclipY2 == (cmd->CMDYC))) return;
   cmd->type = SYSTEM_CLIPPING;
   regs->systemclipX2 = cmd->CMDXC;
   regs->systemclipY2 = cmd->CMDYC;
   vdp1_add(cmd,1);
 }

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1NormalSpriteDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);
  
  /* VDP1 §5.3 Gouraud: correction = gouraud_table_5bit - 0x10.
   * Shader must compute: out_ch = clamp(src_ch + (gtab_ch - 0x10), 0, 0x1F)
   * for each of R,G,B.  Only valid when color mode = RGB (mode 5) or LUT
   * with RGB entries (mode 1).  Gouraud on palette bank codes = undefined.
     VDP1 §5.2 mode 1 (lookup table): write 16-bit LUT entry verbatim to FB.
   * Do NOT mask MSB — VDP2 uses MSB to determine palette vs. RGB format.
   * Color calculation only valid when LUT entry is RGB (MSB=1).
   * Prohibited to use RGB LUT entries in 8-bit/pixel frame buffer mode. */

  /* VDP1 Manual §4.4 p.53 + §7.4 p.118 (Normal Sprite Draw):
   * The four vertices A,B,C,D describe a w×h pixel rectangle where
   * B = A + (w-1, 0), C = A + (w-1, h-1), D = A + (0, h-1).
   * Using +cmd->w (without -1) creates a (w+1)×(h+1) rectangle, which
   * makes upscaled sprites overlap by one pixel against the non-upscale
   * draw path (vdp1.c Vdp1NormalSpriteDraw uses MAX(1,w)-1) and breaks
   * exact 1:1 tiled UI sprites (Sega Rally HUD, Panzer Dragoon menus). */
  cmd->CMDXB = cmd->CMDXA + MAX(1,cmd->w) - 1;
  cmd->CMDYB = cmd->CMDYA;
  cmd->CMDXC = cmd->CMDXA + MAX(1,cmd->w) - 1;
  cmd->CMDYC = cmd->CMDYA + MAX(1,cmd->h) - 1;
  cmd->CMDXD = cmd->CMDXA;
  cmd->CMDYD = cmd->CMDYA + MAX(1,cmd->h) - 1;

  /* VDP2 Manual §9.2: For RGB sprite data, priority register 0 is always
   * selected (bits 2~0 of CCRSA = PRISA bits 2~0).
   * Do NOT modify CCRSA globally — just set a flag on the command so the
   * shader uses priority register 0. Setting cmd type is sufficient.
     Remove the cclist masking entirely — it corrupts global state
	  if (((cmd->CMDPMOD >> 3) & 0x7u) == 5) {
		// hard/vdp2/hon/p09_20.htm#no9_21
		u32 *cclist = (u32 *)&(Vdp2Lines[0].CCRSA);
		cclist[0] &= 0x1Fu;
	  }*/
  cmd->type = QUAD;

  vdp1_add_upscale(cmd,0);

  LOG_CMD("%d\n", __LINE__);
}

void VIDCSVdp1ScaledSpriteDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{

  // Setup Zoom Point
  switch ((cmd->CMDCTRL & 0xF00) >> 8)
  {
    case 0x0: // Only two coordinates
      if ((s16)cmd->CMDXC > (s16)cmd->CMDXA){ cmd->CMDXB += 1; cmd->CMDXC += 1;} else { cmd->CMDXA += 1; cmd->CMDXD += 1;}
      /* VDP1 Manual §4.4 p.53: A=top-left, B=top-right, C=bottom-right,
       * D=bottom-left. Symmetric to the X case above:
       *   YC > YA (normal): bottom edge = {C,D} -> YC+=1, YD+=1
       *   else (flipped V): top edge    = {A,B} -> YA+=1, YB+=1
       * Previous code incremented YD in both branches (copy-paste
       * from first branch), breaking vertically-flipped scaled sprites. */
      if ((s16)cmd->CMDYC > (s16)cmd->CMDYA){ cmd->CMDYC += 1; cmd->CMDYD += 1;} else { cmd->CMDYA += 1; cmd->CMDYB += 1;}
      break;
    case 0x5: // Upper-left
    case 0x6: // Upper-Center
    case 0x7: // Upper-Right
    case 0x9: // Center-left
    case 0xA: // Center-center
    case 0xB: // Center-right
    case 0xD: // Lower-left
    case 0xE: // Lower-center
    case 0xF: // Lower-right
      cmd->CMDXB += 1;
      cmd->CMDXC += 1;
      cmd->CMDYC += 1;
      cmd->CMDYD += 1;
      break;
    default: break;
  }
  /* VDP2 Manual §9.2: For RGB sprite data, priority register 0 is always
   * selected (bits 2~0 of CCRSA = PRISA bits 2~0).
   * Do NOT modify CCRSA globally — just set a flag on the command so the
   * shader uses priority register 0. Setting cmd type is sufficient.
     Remove the cclist masking entirely — it corrupts global state
	  if (((cmd->CMDPMOD >> 3) & 0x7u) == 5) {
		// hard/vdp2/hon/p09_20.htm#no9_21
		u32 *cclist = (u32 *)&(Vdp2Lines[0].CCRSA);
		cclist[0] &= 0x1Fu;
	  }*/

  cmd->type = QUAD;
  vdp1_add_upscale(cmd,0);

  LOG_CMD("%d\n", __LINE__);
}

void VIDCSVdp1DistortedSpriteDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);

  /* VDP2 Manual §9.2 p.204-206 / §12.3 p.272-276:
   * CCRSA is a global VDP2 color-calculation-ratio register indexed by
   * priority register (0 or 1). Mutating Vdp2Lines[0].CCRSA here
   * corrupts per-line register state and biases the CC ratio for every
   * subsequent sprite that uses the same priority register.
   *
   * Priority-register selection for RGB sprite data (color mode 5) is
   * already handled by the shader via cmd type. The non-upscale paths
   * and the other upscale paths dropped this mutation; align distorted. */

  addCSCommands(cmd, DISTORTED);

  return;
}

void VIDCSVdp1PolygonDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  // cmd->type = POLYGON;

  addCSCommands(cmd,POLYGON);
  return;
}

void VIDCSVdp1PolylineDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);

  cmd->type = POLYLINE;

  vdp1_add_upscale(cmd,0);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1LineDrawUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  LOG_CMD("%d\n", __LINE__);

  cmd->type = LINE;

  vdp1_add_upscale(cmd,0);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1UserClippingUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
    // VDP1 Manual §7.2: "Operation cannot be guaranteed if XC < XA or YC < YA"
    // The hardware does NOT reset localX/Y — it simply produces undefined results.
    // We skip the command silently to avoid rendering artifacts.
    if (  ((s16)cmd->CMDXC < (s16)cmd->CMDXA)
       || ((s16)cmd->CMDYC < (s16)cmd->CMDYA)
    ) {
        // Invalid clip rectangle: skip command, do not modify local coordinates
        return;
    }
    cmd->type = USER_CLIPPING;
    regs->userclipX1 = cmd->CMDXA;
    regs->userclipY1 = cmd->CMDYA;
    regs->userclipX2 = cmd->CMDXC;
    regs->userclipY2 = cmd->CMDYC;
    // VDP1 Manual §6.3 Cmod bit 9: outside drawing mode when =1
    regs->userclipMode = (cmd->CMDPMOD >> 9) & 0x1;
    /* VDP1 §7.2: User clipping coordinates are VDP1 framebuffer coordinates,
     * not display coordinates.  The upscale path must NOT scale them;
     * use vdp1_add (not vdp1_add_upscale) to forward raw register values. */
	vdp1_add(cmd, 1);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1SystemClippingUpscale(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  /* VDP1 Manual §7.1: "Operation cannot be ensured if XC < 0 or YC < 0." */
  if ((s16)cmd->CMDXC < 0 || (s16)cmd->CMDYC < 0) return;
  if (((cmd->CMDXC) == regs->systemclipX2) && (regs->systemclipY2 == (cmd->CMDYC))) return;
  cmd->type = SYSTEM_CLIPPING;
  regs->systemclipX2 = cmd->CMDXC;
  regs->systemclipY2 = cmd->CMDYC;
  vdp1_add_upscale(cmd,1);
}

void VIDCSVdp1DrawFB(void) {
  VIDCSRenderVDP1();
  vdp1_write();
}

/* VDP2 §6.2 p.149 : si le rotation scroll screen est
 * affiche et qu'une bank est attribuee a RBG0 (RDBS != 00b),
 * son VRAM cycle pattern est ignore par le hardware. */
static INLINE int Vdp2BankOwnedByRBG0(Vdp2 *regs, int bank)
{
    if (((regs->BGON >> 4) & 0x1) == 0) return 0;   /* R0ON */
    int rdbs = (regs->RAMCTL >> (bank * 2)) & 0x3;
    return (rdbs != 0);
}

/* §3.3 p.32 + §6.2 p.149 : un timeslot T0..T7 d'une bank
 * n'est exploitable par un NBG que si :
 *   - la bank existe (partition active, §3.2)
 *   - la bank n'est pas reservee a RBG0 (cycle pattern ignore, §6.2 p.149)
 * Une access command pointant une bank sans donnee ne lit rien (§3.3 p.32). */
static INLINE int Vdp2TimeslotUsable(Vdp2 *regs, int bank)
{
    const int a_split = (regs->RAMCTL >> 8) & 0x1;   /* VRAMD */
    const int b_split = (regs->RAMCTL >> 9) & 0x1;   /* VRBMD */
    const int bank_used[4] = { 1, a_split, 1, b_split };
    if (!bank_used[bank]) return 0;
    if (Vdp2BankOwnedByRBG0(regs, bank)) return 0;
    return 1;
}

/* VDP2 §3.3 p.35 : l'acces a la table Vertical Cell Scroll
 * n'est reellement effectue que si un access command "Vertical Cell
 * Scroll Table Data Read" est present dans le VRAM cycle pattern
 * register, dans un timing autorise :
 *   - NBG0 : T0 ou T1
 *   - NBG1 : T0, T1 ou T2
 * Access command VCSC (§3.3) : NBG0 = 1100b (0xC), NBG1 = 1101b (0xD).
 *
 * Les registres de cycle pattern sont des u16 simples (cf. vdp2.h) :
 *   CYCA0L/U, CYCA1L/U, CYCB0L/U, CYCB1L/U.
 * On compose (U << 16) | L ; le mot U porte T0-T3 (T0 en bits 31-28),
 * le mot L porte T4-T7. §3.3 fig.3.2 : l'acces commence a T0.
 *
 * NB : en Hi-Res / Exclusive monitor seuls T0-T3 sont valides
 * (§3.3 p.32) ; ici maxT vaut au plus 3, donc pas d'impact, mais
 * tout parseur de cycle pattern plus general doit borner a T0-T3. */
/* VDP2 §3.3 p.35 : l'acces a la table Vertical Cell Scroll n'est
 * reellement effectue que si un access command VCSC est present dans
 * un timing autorise (NBG0 : T0-T1 ; NBG1 : T0-T2), dans une bank
 * exploitable :
 *   - B1 : bank existante (A1/B1 ignorees si VRAM non partitionnee)
 *          + meme bank pour NBG0/NBG1, NBG0 avant NBG1
 *   - E1 : bank non reservee a RBG0 (cycle pattern ignore, §6.2 p.149)
 * Access command VCSC : NBG0 = 1100b (0xC), NBG1 = 1101b (0xD). */
static int Vdp2VCSCAccessValid(Vdp2 *regs, int nbg)
{
    const u32 cyc[4] = {
        ((u32)regs->CYCA0U << 16) | regs->CYCA0L,
        ((u32)regs->CYCA1U << 16) | regs->CYCA1L,
        ((u32)regs->CYCB0U << 16) | regs->CYCB0L,
        ((u32)regs->CYCB1U << 16) | regs->CYCB1L,
    };
    /* §3.2 : bit 9 = VRBMD, bit 8 = VRAMD (1 = partitionnee) */
    const int a_split = (regs->RAMCTL >> 8) & 0x1;
    const int b_split = (regs->RAMCTL >> 9) & 0x1;
    const int bank_used[4] = { 1, a_split, 1, b_split };

    const u8 want = (nbg == NBG0) ? 0xC : 0xD;
    const int maxT = (nbg == NBG0) ? 2 : 3;   /* T0-T1 / T0-T2 */

    for (int b = 0; b < 4; b++) {
        if (!bank_used[b]) continue;          /* A1/B1 ignores si non split */
        for (int t = 0; t < maxT; t++) {
            if (((cyc[b] >> (28 - t * 4)) & 0xF) == want) {
                /* §3.3 p.35 : si NBG1 demande aussi la VCSC, elle doit
                 * etre dans la MEME bank et NBG0 doit venir AVANT. */
                if (nbg == NBG1) {
                    int n0t = -1;
                    for (int tt = 0; tt < t; tt++)
                        if (((cyc[b] >> (28 - tt*4)) & 0xF) == 0xC) { n0t = tt; break; }
                    /* si NBG0 fait de la VCSC, elle doit etre dans cette
                     * bank et a un timing < t ; sinon NBG1 invalide. */
                    int n0_anywhere = 0;
                    for (int bb = 0; bb < 4 && !n0_anywhere; bb++) {
                        if (!bank_used[bb]) continue;
                        for (int tt = 0; tt < 2; tt++)
                            if (((cyc[bb] >> (28 - tt*4)) & 0xF) == 0xC) n0_anywhere = 1;
                    }
                    if (n0_anywhere && n0t < 0) continue; /* mauvaise bank/ordre */
                }
                return 1;
            }
        }
    }
    return 0;
}

static void Vdp2DrawNBG0_zones(void)
{
  int lastLine = 0;
  int line;
  int max = (yabsys.VBlankLineCount >= 270) ? 270 : yabsys.VBlankLineCount;
 
  for (line = 1; line < max; line++) {
    if (!sameVDP2Reg(NBG0, &Vdp2Lines[line-1], &Vdp2Lines[line])) {
      Vdp2DrawNBG0(&Vdp2Lines[lastLine], lastLine, line);
      lastLine = line;
    }
  }
  Vdp2DrawNBG0(&Vdp2Lines[lastLine], lastLine, max);
}
 
static void Vdp2DrawNBG0(Vdp2* varVdp2Regs, int startLine, int endLine)
{
  YglCache tmpc;
  u32 char_access = 0;
  u32 ptn_access = 0;
  Vdp2Ctrl ctrl;
  int i;
 
  ctrl.regs = varVdp2Regs;
  ctrl.info.dst = 0;
  ctrl.info.idScreen = NBG0;
  ctrl.info.coordincx = 1.0f;
  ctrl.info.coordincy = 1.0f;
  ctrl.info.coordincy_raw = 0x100;   /* 1.0 en point fixe .8 - defaut sur */
  ctrl.info.cor = 0;
  ctrl.info.cog = 0;
  ctrl.info.cob = 0;
 
  ctrl.info.enable = 0;
  /* Segmented line rendering (§5.1 p.125) : scroll registers can change
   * mid-frame via H-blank writes. Each zone covers the screen rows where
   * the register snapshot is constant. */
  ctrl.info.startLine = startLine;
  ctrl.info.endLine   = endLine;
  ctrl.info.cellh = 256;
  if (_Ygl->interlace == DOUBLE_INTERLACE) ctrl.info.cellh = ctrl.info.cellh << 1;
  ctrl.info.specialcolorfunction = 0;
  ctrl.info.bitmap_base      = 0;
  ctrl.info.bitmap_wrap_size = 0;
 
  /* [P2] §2.1 Table 2.1 : VBlankLineCount peut dépasser 270 pendant les
   * transitions de mode vidéo. display[]/alpha_per_line[] (vdp2draw_struct)
   * et Vdp2Lines[] (dimensionné [270], cf. vdp2.h) doivent être bornés à
   * 270 pour éviter un débordement de tableau. Identique à Vdp2DrawNBG1. */
  const int line_max = (yabsys.VBlankLineCount >= 270)
                       ? 270 : yabsys.VBlankLineCount;

  for (int i = 0; i < line_max; i++) {
    ctrl.info.display[i] = isEnabled(NBG0, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    ctrl.info.alpha_per_line[i] = (u8)(((~Vdp2Lines[i].CCRNA & 0x1F) * 255) / 31);
  }
  /* [P2 v2] No zero-init pass beyond line_max: vdp2draw_struct may
   * dimension display[]/alpha_per_line[] smaller than 512, and writing
   * past line_max risks overwriting neighbouring struct fields.
   * The consumer-side clamp in [P3] getAlpha() already prevents reads
   * beyond line_max-1. */

  /* enable is the OR of display[i] over the same range as the
   * fill loop above (clamped to 270 lines, see line_max). */
  if (!ctrl.info.enable) return;

 
  for (int b = 0; b < 4; b++) {
    ctrl.info.char_bank[b] = 0;
    ctrl.info.pname_bank[b] = 0;
    for (int j = 0; j < 8; j++) {
      if (Vdp2External.AC_VRAM[b][j] == 0x04) {
        ctrl.info.char_bank[b] = 1;
        char_access |= 1 << j;
      }
      if (Vdp2External.AC_VRAM[b][j] == 0x00) {
        ctrl.info.pname_bank[b] = 1;
        ptn_access |= (1 << j);
      }
    }
  }
 

  if (char_access == 0) return;
 
  /* VDP2 Manual §4.2 CHCTLA bits 6-4 : NBG0 color number.
   * Must be assigned BEFORE the isbitmap branch below, which switches on
   * ctrl.info.colornumber for the wrap-size computation, and before
   * Vdp2DrawCell_in_sync()'s per-pixel switch (case 0..4) that decides how
   * many bytes/pixel to read. This line was accidentally dropped during a
   * manual file reconstruction -- leaving colornumber as uninitialised
   * stack garbage, which matched none of the switch cases and silently
   * skipped writing any pixel data at all (root cause of the missing
   * NBG0 bitmap: title logo + background never drawn). */
  ctrl.info.colornumber = (ctrl.regs->CHCTLA & 0x70) >> 4;
 
  if ((ctrl.info.isbitmap = ctrl.regs->CHCTLA & 0x2) != 0)
  {
    /* ------ Bitmap mode (§4.9 p.93-95) ------ */
    ReadBitmapSize(&ctrl.info, ctrl.regs->CHCTLA >> 2, 0x3);
 
    /* §5.1 : scroll is a positive coordinate into the bitmap; the display
     * area wraps when exceeded. Modulo by bitmap dimensions. */
    ctrl.info.x = -((ctrl.regs->SCXIN0 & 0x7FF) % ctrl.info.cellw);
    ctrl.info.y = -((ctrl.regs->SCYIN0 & 0x7FF) % ctrl.info.cellh);
 
    /* charaddr must be assigned BEFORE bitmap_base. */
    ctrl.info.charaddr = (ctrl.regs->MPOFN & 0x7) * 0x20000;
    ctrl.info.paladdr = (ctrl.regs->BMPNA & 0x7) << 4;
    ctrl.info.flipfunction = 0;
    ctrl.info.specialcolorfunction = (ctrl.regs->BMPNA & 0x10) >> 4;
    ctrl.info.specialfunction = (ctrl.regs->BMPNA >> 5) & 0x01;
 
    /* [FIX 3] VDP2 Manual p.95 Table 4.11 — wrap size depends on color depth
     * AND bitmap dimensions. Compute wrap FIRST, then assign bitmap_base,
     * same ordering as Vdp2DrawNBG1 for consistency and so that any
     * downstream reader sees both fields set atomically. */
    switch (ctrl.info.colornumber) {
      case 0: /* 4bpp, 16 colors */
        ctrl.info.bitmap_wrap_size = (ctrl.info.cellw * ctrl.info.cellh) / 2;
        break;
      case 1: /* 8bpp, 256 colors */
        ctrl.info.bitmap_wrap_size = ctrl.info.cellw * ctrl.info.cellh;
        break;
      case 2: /* 16bpp palette (2048 colors) */
      case 3: /* 16bpp RGB (32768 colors) */
        ctrl.info.bitmap_wrap_size = ctrl.info.cellw * ctrl.info.cellh * 2;
        break;
      case 4: /* 32bpp RGB (16.7M colors) */
        ctrl.info.bitmap_wrap_size = ctrl.info.cellw * ctrl.info.cellh * 4;
        break;
      default:
        ctrl.info.bitmap_wrap_size = 0;
        break;
    }
    ctrl.info.bitmap_base = ctrl.info.charaddr;
 
    /* VDP2 Manual §3.1 : VRSIZE bit 15 determines VRAM size
     *   0 = 4 Mbit (512 KB), 1 = 8 Mbit (1 MB).
     * The bank index must account for this to select the correct RAMCTL
     * cycle pattern bits.
     *
     * [FIX 5] Apply the restriction to ALL color numbers, like NBG1
     * (l.1619-1627). §3.1 does not limit this check to colornumber < 3 ;
     * any bitmap mode can be disabled by a RAMCTL cycle-pattern conflict. */
    int charAddrBk = (((ctrl.info.charaddr >> 16) & 0xF)
                      >> ((ctrl.regs->VRSIZE >> 15) & 0x1)) >> 1;
    int needUpdate = 0;
    const int line_max = (yabsys.VBlankLineCount >= 270) ? 270 : yabsys.VBlankLineCount;
    for (int k = 0; k < line_max; k++) {
      /* VDP2 Manual §4.1 BGON p.49: bits map to scroll screens as:
       *   bit 0 = N0ON (NBG0)
       *   bit 1 = N1ON (NBG1)
       *   bit 4 = R0ON (RBG0)
       *
       * The previous code tested BGON & 0x10 (R0ON / RBG0) inside the
       * NBG0 disable scan — a copy-paste bug from a path that was
       * checking the rotation screen.  In NBG0 the per-line BGON test
       * must match N0ON (bit 0).
       *
       * Symptom of the original bug: when NBG0 was disabled but RBG0
       * was active on a given line, the loop incorrectly applied the
       * RAMCTL-conflict mask to NBG0's display[k] flag — and conversely,
       * when NBG0 was active but RBG0 was not, the conflict was missed
       * and the layer rendered against an unsafe RAMCTL cycle pattern.
       * Either way the per-line decision was keyed off the wrong layer. */
      if ((Vdp2Lines[k].BGON & 0x1) != 0) {
        if (Vdp2BankOwnedByRBG0(&Vdp2Lines[k], charAddrBk)) {
          needUpdate = 1;
          ctrl.info.display[k] = 0;
        }
      }
    }
	if (needUpdate != 0) {
		ctrl.info.enable = 0;
		/* Re-évaluer 'enable' sur la même plage que le scan initial.
		 * VDP2 Manual §3.1 + §4.1: garder la cohérence entre la boucle
		 * de remplissage display[] (lignes ~1180) et la re-évaluation. */
		for (int k = 0; k < line_max; k++) {
			ctrl.info.enable |= ctrl.info.display[k];
		}
		if (!ctrl.info.enable) return;
	}
  }
  else
  {
    /* ------ Tile / cell mode ------ */
    if (ptn_access == 0) return;
    ctrl.info.mapwh = 2;
    ReadPlaneSize(&ctrl.info, ctrl.regs->PLSZ);
    ctrl.info.x = -((ctrl.regs->SCXIN0 & 0x7FF) % (512 * ctrl.info.planew));
    ctrl.info.y = -((ctrl.regs->SCYIN0 & 0x7FF) % (512 * ctrl.info.planeh));
    ReadPatternData(&ctrl.info, ctrl.regs->PNCN0, ctrl.regs->CHCTLA & 0x1);
  }
 
  /* ------ Zoom (§5.2 p.126-130) ------ */
  /* Coordinate Increment Register : integer bits 18-8, fractional 7-0.
   * Mask 0x7FF00 extracts the 11-bit integer + upper fractional portion used
   * to form the 16.16 reciprocal. A value of 0 disables the screen. */
  if ((ctrl.regs->ZMXN0.all & 0x7FF00) == 0) return;
  ctrl.info.coordincx = (float)65536 / (ctrl.regs->ZMXN0.all & 0x7FF00);
 
  /* Reduction Enable Register ZMCTL §5.2 p.129 : bits 1-0 for NBG0.
   *   00 = no reduction           (coordincx in [0, 1])
   *   01 = up to 1/2              (coordincx in [0, 2], clamp at 0.5)
   *   10/11 = up to 1/4           (coordincx in [0, 4], clamp at 0.25)
   */
 
  switch (ctrl.regs->ZMCTL & 0x03)
  {
    case 0:
      ctrl.info.maxzoom = 1.0f;
      break;
    case 1:
      ctrl.info.maxzoom = 0.5f;
      if (ctrl.info.coordincx < 0.5f) ctrl.info.coordincx = 0.5f;
      break;
    case 2:
    case 3:
      ctrl.info.maxzoom = 0.25f;
      if (ctrl.info.coordincx < 0.25f) ctrl.info.coordincx = 0.25f;
      break;
  }
 
  if ((ctrl.regs->ZMYN0.all & 0x7FF00) == 0) return;
  ctrl.info.coordincy_raw = ctrl.regs->ZMYN0.all & 0x7FF00;
  ctrl.info.coordincy = (float)65536 / (ctrl.regs->ZMYN0.all & 0x7FF00);
 
  ctrl.info.PlaneAddr = (void (FASTCALL*)(void *, int, Vdp2*))&Vdp2NBG0PlaneAddr;
 
  ReadMosaicData(&ctrl.info, 0x1, ctrl.regs);
 
  ctrl.info.transparencyenable = !(ctrl.regs->BGON & 0x100);
  ctrl.info.specialprimode   = ctrl.regs->SFPRMD & 0x3;
  ctrl.info.specialcolormode = ctrl.regs->SFCCMD & 0x3;
  ctrl.info.specialcode      = (ctrl.regs->SFSEL & 0x1)
                                ? (ctrl.regs->SFCODE >> 8)
                                : (ctrl.regs->SFCODE & 0xFF);
  ctrl.info.coloroffset      = (ctrl.regs->CRAOFA & 0x7) << 8;
  ctrl.info.linecheck_mask   = 0x01;
  ctrl.info.priority         = ctrl.regs->PRINA & 0x7;
 
  if (ctrl.info.priority == 0) return;
 
  ReadLineScrollData(&ctrl.info, ctrl.regs->SCRCTL & 0xFF, ctrl.regs->LSTA0.all, ctrl.regs);
  ctrl.info.lineinfo = lineNBG0;
  Vdp2GenLineinfo(&ctrl.info);
 
  /* §3.3 p.35 : la VCSC n'est active que si SCRCTL le
   * demande ET qu'un access command VCSC est reserve dans un timing
   * valide du cycle pattern register. Sinon le hardware ne lit pas
   * la table -> ne pas appliquer de V-shift fantome. */
  if ((ctrl.regs->SCRCTL & 1) && Vdp2VCSCAccessValid(ctrl.regs, NBG0)) {
    ctrl.info.isverticalscroll = 1;
    ctrl.info.verticalscrolltbl = (ctrl.regs->VCSTA.all & 0x7FFFE) << 1;
	/* §5.3 Figure 5.8 p.136 : pas NBG0 = 8 UNIQUEMENT si
	 * NBG1 fait reellement de la VCSC (enable + access command valide).
	 * Sinon la table n'est pas entrelacee et le pas reste 4. */
	int nbg1_vcs_real = (ctrl.regs->SCRCTL & 0x100)
						&& Vdp2VCSCAccessValid(ctrl.regs, NBG1);
	ctrl.info.verticalscrollinc = nbg1_vcs_real ? 8 : 4;
  }
  else {
    ctrl.info.isverticalscroll = 0;
  }
 
  /* Precompute the screen pixel rows for this zone. All bitmap and
   * linescroll paths below must draw only within [screenY1, screenY2)
   * so zones don't overwrite each other. */
  const int screenY1 = (_Ygl->rheight * ctrl.info.startLine) / yabsys.VBlankLineCount;
  const int screenY2 = (_Ygl->rheight * ctrl.info.endLine)   / yabsys.VBlankLineCount;
 
  if (ctrl.info.isbitmap)
  {
    const int is_full_screen_zone =
        (ctrl.info.startLine == 0) &&
        (ctrl.info.endLine   >= yabsys.VBlankLineCount);
 
    /* [FIX 2] has_coord_inc covers zoom ONLY. Linescroll is handled by its
     * own branch below (previously dead code because VDPLINE_SZ was folded
     * into has_coord_inc). */
    const int has_coord_inc =
        (ctrl.info.coordincx != 1.0f) ||
        (ctrl.info.coordincy != 1.0f);
 
    if (has_coord_inc) {
      /* Zoom / line-zoom path — handles zone-relative rendering via
       * ctrl.info.startLine/endLine inside Vdp2DrawBitmapCoordinateInc. */
      ctrl.info.sh = (ctrl.regs->SCXIN0 & 0x7FF);
      ctrl.info.sv = (ctrl.regs->SCYIN0 & 0x7FF);
      ctrl.info.x = 0;
      ctrl.info.y = 0;
      ctrl.info.vertices[0] = 0;                   ctrl.info.vertices[1] = (float)screenY1;
      ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
      ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
      ctrl.info.vertices[6] = 0;                   ctrl.info.vertices[7] = (float)screenY2;
 
      vdp2draw_struct infotmp = ctrl.info;
      infotmp.cellw = _Ygl->rwidth;
      infotmp.cellh = screenY2 - screenY1;
      YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
      Vdp2DrawBitmapCoordinateInc(&ctrl);
    }
    else if (ctrl.info.islinescroll) {
      /* Pure linescroll path (no zoom). */
      ctrl.info.sh = (ctrl.regs->SCXIN0 & 0x7FF);
      ctrl.info.sv = (ctrl.regs->SCYIN0 & 0x7FF);
      ctrl.info.x = 0;
      ctrl.info.y = 0;
      ctrl.info.vertices[0] = 0;                   ctrl.info.vertices[1] = (float)screenY1;
      ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
      ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
      ctrl.info.vertices[6] = 0;                   ctrl.info.vertices[7] = (float)screenY2;
 
      vdp2draw_struct infotmp = ctrl.info;
      infotmp.cellw = _Ygl->rwidth;
      infotmp.cellh = screenY2 - screenY1;
      YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
      Vdp2DrawBitmapLineScroll(&ctrl, _Ygl->rwidth, screenY2 - screenY1);
    }
    else if (!is_full_screen_zone) {
      /* Partial zone, no zoom, no linescroll.
       * The tile-local loop below would draw from y=0 and miss the zone
       * start; route through Vdp2DrawBitmapCoordinateInc with
       * coordinc = 1.0 which honors startLine/endLine via the vertex rect.
       * Minor per-pixel overhead, guaranteed correctness. */
      ctrl.info.coordincx = 1.0f;
      ctrl.info.coordincy = 1.0f;
      ctrl.info.coordincy_raw = 0x100;
      ctrl.info.sh = (ctrl.regs->SCXIN0 & 0x7FF);
      ctrl.info.sv = (ctrl.regs->SCYIN0 & 0x7FF);
      ctrl.info.x = 0;
      ctrl.info.y = 0;
      ctrl.info.vertices[0] = 0;                   ctrl.info.vertices[1] = (float)screenY1;
      ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
      ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
      ctrl.info.vertices[6] = 0;                   ctrl.info.vertices[7] = (float)screenY2;
 
      vdp2draw_struct infotmp = ctrl.info;
      infotmp.cellw = _Ygl->rwidth;
      infotmp.cellh = screenY2 - screenY1;
      YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
      Vdp2DrawBitmapCoordinateInc(&ctrl);
    }
    else {
      /* Full-screen zone, no zoom, no linescroll → tile-local loop. */
      int cellh_local = ctrl.info.cellh;
      int xx, yy;
      int isCachedLocal = 0;
 
      yy = ctrl.info.y;
      /* Skip tile rows entirely above screenY1 (== 0 here). */
      while (yy + cellh_local <= 0) yy += cellh_local;
 
      while (yy < _Ygl->rheight) {
        ctrl.info.draw_line = yy;
        xx = ctrl.info.x;
        while (xx < _Ygl->rwidth) {
          ctrl.info.vertices[0] = (float)xx;                     ctrl.info.vertices[1] = (float)yy;
          ctrl.info.vertices[2] = (float)(xx + ctrl.info.cellw); ctrl.info.vertices[3] = (float)yy;
          ctrl.info.vertices[4] = (float)(xx + ctrl.info.cellw); ctrl.info.vertices[5] = (float)(yy + cellh_local);
          ctrl.info.vertices[6] = (float)xx;                     ctrl.info.vertices[7] = (float)(yy + cellh_local);
 
          if (isCachedLocal == 0) {
            YglQuad(&ctrl.info, &ctrl.texture, &tmpc, YglTM_vdp2);
            requestDrawCell(&ctrl);
            isCachedLocal = 1;
          }
          else {
            YglCachedQuad(&ctrl.info, &tmpc, YglTM_vdp2);
          }
          xx += ctrl.info.cellw;
        }
        yy += cellh_local;
      }
    }
  }
  else
  {
    /* ------ Tilemap mode ------ */
    if (ctrl.info.islinescroll) {
      /* Tile + linescroll : zone-clipped quad, per-line draw. */
      ctrl.info.sh = (ctrl.regs->SCXIN0 & 0x7FF);
      ctrl.info.sv = (ctrl.regs->SCYIN0 & 0x7FF);
      ctrl.info.x = 0;
      ctrl.info.y = 0;
      ctrl.info.vertices[0] = 0;                   ctrl.info.vertices[1] = (float)screenY1;
      ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
      ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
      ctrl.info.vertices[6] = 0;                   ctrl.info.vertices[7] = (float)screenY2;
 
      vdp2draw_struct infotmp = ctrl.info;
      infotmp.cellw = _Ygl->rwidth;
      infotmp.cellh = screenY2 - screenY1;
      infotmp.flipfunction = 0;
      YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
      Vdp2DrawMapPerLine(&ctrl);
    }
    else {
      /* Tile + standard scroll. Vdp2DrawPatternPos does vertex-based
       * screen culling inside the zone using ctrl.info.startLine/endLine. */
      int delayed = 0;
      if (((ptn_access & 0x1) == 0) &&
          Vdp2CheckCharAccessPenalty(char_access, ptn_access, (ctrl.info.patternwh == 2)) != 0) {
        delayed = 1;
      }
 
      /* [FIX 1] Vdp2DrawMapTest expects the POSITIVE scroll coordinates
       * (§5.2 formula : display = coordinate_increment × counter + scroll).
       * NBG1 (l.1818-1820) and NBG3 (l.2184-2186) do the same re-assignment
       * here. The negative offset computed earlier was only useful for the
       * bitmap tile-local loop. */
      ctrl.info.x = ctrl.regs->SCXIN0 & 0x7FF;
      ctrl.info.y = ctrl.regs->SCYIN0 & 0x7FF;
      Vdp2DrawMapTest(&ctrl, delayed);
    }
  }
#ifdef CELL_ASYNC
  YabThreadYield();
#endif
}

//////////////////////////////////////////////////////////////////////////////

static int sameVDP2RegNBG1(Vdp2 *a, Vdp2 *b)
{
    /* WPSX0/WPEX0/WPSY0/WPEY0 (180020H..180026H) :
     * Window 0 boundaries.  VDP2 Manual ST-58-R2 §8.1 p.181.
     * Mid-frame moves of W0 (spotlight effect) must split zones. */
    if (a->WPSX0 != b->WPSX0) return 0;
    if (a->WPEX0 != b->WPEX0) return 0;
    if (a->WPSY0 != b->WPSY0) return 0;
    if (a->WPEY0 != b->WPEY0) return 0;
    /* W1 : 180028H..18002EH */
    if (a->WPSX1 != b->WPSX1) return 0;
    if (a->WPEX1 != b->WPEX1) return 0;
    if (a->WPSY1 != b->WPSY1) return 0;
    if (a->WPEY1 != b->WPEY1) return 0;

    /* LWTA0/LWTA1 (1800D8H/1800DCH) : line window table
     * addresses + W0LWE/W1LWE bit 15. VDP2 Manual ST-58-R2
     * p.186. Mid-frame rewrite must split zones. */
    if (a->LWTA0.all != b->LWTA0.all) return 0;
    if (a->LWTA1.all != b->LWTA1.all) return 0;
	
	/* RAMCTL bits 15 (CRKTE) + 11..0 (bank type assignments).
     * VDP2 Manual ST-58-R2 §3.2 p.29-30. The bank-conflict
     * scans inside Vdp2DrawNBG0..3 and Vdp2DrawRBG0_part read
     * RAMCTL ; mid-frame rewrites must invalidate zones. */
    if ((a->RAMCTL & 0x8FFF) != (b->RAMCTL & 0x8FFF)) return 0;
	
    /* BGON: N1ON = bit 1. Also check RBG enable bits that suppress NBG1
     * (VDP2 §4.1 Table 4.1: when R0ON(4)+R1ON(5) both set, NBG screens off). */
    if ((a->BGON & 0x32) != (b->BGON & 0x32)) return 0;
 
    /* CHCTLA bits 15-9: N1CHSZ(15-14), N1BMEN(9), N1CHCN(13-12).
     * Color depth, bitmap mode and bitmap size for NBG1. */
    if ((a->CHCTLA & 0xFE00) != (b->CHCTLA & 0xFE00)) return 0;
	
    /* CHCTLA bits 6-4: N0CHCN — NBG0 color number.
     * VDP2 Manual ST-58-R2 p.61 : when NBG0 = 16,770,000 colors
     * (N0CHCN == 4), NBG1 cannot be displayed.  Vdp2DrawNBG1 honors
     * this rule (l.1836) but the zonal optimiser must also detect
     * the threshold crossing — otherwise NBG1 stays visible across
     * a mid-frame switch into 16M-color mode on NBG0. */
    if ((a->CHCTLA & 0x0070) != (b->CHCTLA & 0x0070)) return 0;
 
    /* PRINA bits 10-8: NBG1 priority number (3-bit). */
    if ((a->PRINA & 0x0700) != (b->PRINA & 0x0700)) return 0;
 
    /* CCRNA bits 12-8: N1CCRT[4:0] — NBG1 color calculation ratio. */
    if ((a->CCRNA & 0x1F00) != (b->CCRNA & 0x1F00)) return 0;
 
    /* SCXIN1 bits 10-0: NBG1 horizontal scroll integer part.
     * Note: fractional part (SCXDN1) is rarely changed mid-frame;
     * include if games use sub-pixel scrolling changes. */
    if ((a->SCXIN1 & 0x7FF) != (b->SCXIN1 & 0x7FF)) return 0;
 
    /* SCYIN1 bits 10-0: NBG1 vertical scroll integer part. */
    if ((a->SCYIN1 & 0x7FF) != (b->SCYIN1 & 0x7FF)) return 0;
 
    /* ZMXN1 bits 18-8: NBG1 horizontal coordinate increment (zoom). */
    if ((a->ZMXN1.all & 0x7FF00) != (b->ZMXN1.all & 0x7FF00)) return 0;
 
    /* ZMYN1 bits 18-8: NBG1 vertical coordinate increment (zoom). */
    if ((a->ZMYN1.all & 0x7FF00) != (b->ZMYN1.all & 0x7FF00)) return 0;

    /* ZMCTL bits 9-8: N1ZMQT, N1ZMHF — NBG1 reduction limit.
     * VDP2 Manual ST-58-R2 §5.2 p.129 (180098H).  Vdp2DrawNBG1
     * reads these to clamp coordincx ; mid-frame rewrite must
     * trigger a new zone (mirror of the NBG0 ZMCTL & 0x0003
     * comparison done in sameVDP2RegNBG0). */
    if ((a->ZMCTL & 0x0300) != (b->ZMCTL & 0x0300)) return 0;
 
    /* CRAOFA bits 6-4: N1CAOS[2:0] — NBG1 color RAM address offset. */
    if ((a->CRAOFA & 0x0070) != (b->CRAOFA & 0x0070)) return 0;
 
    /* MPOFN bits 6-4: NBG1 map offset (affects which VRAM area holds the map). */
    if ((a->MPOFN & 0x0070) != (b->MPOFN & 0x0070)) return 0;
 
    /* BMPNA bits 10-8: NBG1 bitmap palette address (bitmap mode only).
     * Also bits 12,13: N1BMCC, N1BMPR. */
    if ((a->BMPNA & 0x3700) != (b->BMPNA & 0x3700)) return 0;
 
    /* SCRCTL bits 15-8: NBG1 line scroll / vertical cell scroll control.
     * N1LSS[1:0](13:12), N1LZMX(11), N1LSCY(10), N1LSCX(9), N1VCSC(8). */
    if ((a->SCRCTL & 0xFF00) != (b->SCRCTL & 0xFF00)) return 0;

    /* LSTA1 (1800A4H..1800A6H): NBG1 line scroll table address.
     * VDP2 Manual ST-58-R2 p.140. */
    if (a->LSTA1.all != b->LSTA1.all) return 0;

    /* VCSTA (18009CH..18009EH): shared with NBG0.
     * VDP2 Manual ST-58-R2 p.141. */
    if (a->VCSTA.all != b->VCSTA.all) return 0;
 
    /* PNCN1: NBG1 pattern name control — affects character address decoding. */
    if ((a->PNCN1 & 0xFFFF) != (b->PNCN1 & 0xFFFF)) return 0;
 
    /* PLSZ bits 3-2: NBG1 plane size (H and V). Affects map wrapping. */
    if ((a->PLSZ & 0x000C) != (b->PLSZ & 0x000C)) return 0;
 
    /* SFPRMD bits 3-2: NBG1 special priority mode. */
    if ((a->SFPRMD & 0x000C) != (b->SFPRMD & 0x000C)) return 0;
 
    /* SFCCMD bits 3-2: NBG1 special color calculation mode. */
    if ((a->SFCCMD & 0x000C) != (b->SFCCMD & 0x000C)) return 0;
 
    /* LNCLEN bit 1: N1LCEN — line color insertion enable for NBG1. */
    if ((a->LNCLEN & 0x0002) != (b->LNCLEN & 0x0002)) return 0;
 
    /* CLOFSL bit 1: N1COSL — color offset A/B select for NBG1. */
    if ((a->CLOFSL & 0x0002) != (b->CLOFSL & 0x0002)) return 0;

    /* WCTLA byte 1 (bits 15-8): N1LOG, N1SWE, N1SWA, N1W1E, N1W1A,
     * N1W0E, N1W0A.  VDP2 Manual ST-58-R2 §8 p.193 (1800D0H). */
    if ((a->WCTLA & 0xFF00) != (b->WCTLA & 0xFF00)) return 0;
	
    /* CLOFEN bit 1: N1COEN — color offset enable for NBG1.
     * VDP2 Manual ST-58-R2 p.250 (180110H). */
    if ((a->CLOFEN & 0x0002) != (b->CLOFEN & 0x0002)) return 0;

   /* MZCTL bits 15-8 + bit 1: VDP2 Manual §6.6 Mosaic Function p.196.
     *   bits 15-12 = MZSZV (vertical mosaic size, shared across layers)
     *   bits 11-8  = MZSZH (horizontal mosaic size, shared)
     *   bit  1     = N1MZE (NBG1 mosaic enable)
     * Re-emit a draw zone if any of these flips mid-frame. */
    if ((a->MZCTL & 0xFF02) != (b->MZCTL & 0xFF02)) return 0;

    return 1;
}

static void Vdp2DrawNBG1_zones(void)
{
    int lastLine = 0;
    int line;
    int max = (yabsys.VBlankLineCount >= 270) ? 270 : yabsys.VBlankLineCount;
 
    for (line = 1; line < max; line++) {
        if (!sameVDP2RegNBG1(&Vdp2Lines[line - 1], &Vdp2Lines[line])) {
            Vdp2DrawNBG1(&Vdp2Lines[lastLine], lastLine, line);
            lastLine = line;
        }
    }
    Vdp2DrawNBG1(&Vdp2Lines[lastLine], lastLine, max);
}

static void Vdp2DrawNBG1(Vdp2* varVdp2Regs, int startLine, int endLine)
{
  YglCache tmpc;
  u32 char_access = 0;
  u32 ptn_access = 0;
  Vdp2Ctrl ctrl;
  ctrl.regs = varVdp2Regs;
  ctrl.info.dst = 0;
  ctrl.info.idScreen = NBG1;
  ctrl.info.cor = 0;
  ctrl.info.cog = 0;
  ctrl.info.cob = 0;
  ctrl.info.specialcolorfunction = 0;
  ctrl.info.enable = 0;
  /* Segmented line rendering: restrict this draw call to [startLine, endLine).
   * VDP2 Manual §5.1 p.125: registers can change mid-frame via H-blank writes.
   * Each zone covers the screen rows where the register snapshot is constant. */
  ctrl.info.startLine = startLine;
  ctrl.info.endLine   = endLine;

  /* Initialiser bitmap_base et bitmap_wrap_size à 0 par défaut (mode tile) */
  ctrl.info.bitmap_base      = 0;
  ctrl.info.bitmap_wrap_size = 0;
  
  /* [P2] §2.1 Table 2.1 : VBlankLineCount peut dépasser 270 pendant les
   * transitions de mode vidéo. display[]/alpha_per_line[] et Vdp2Lines[]
   * (dimensionné [270], cf. vdp2.h) doivent être bornés. Identique à NBG0. */
  const int line_max = (yabsys.VBlankLineCount >= 270)
                       ? 270 : yabsys.VBlankLineCount;
					   
  for (int i = 0; i < line_max; i++) {
    ctrl.info.display[i] = isEnabled(NBG1, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    ctrl.info.alpha_per_line[i] = (u8)(((~(Vdp2Lines[i].CCRNA >> 8) & 0x1F) * 255) / 31);
  }

  /* enable is the OR of display[i] over the same range as the
   * fill loop above (clamped to 270 lines, see line_max). */
  if (!ctrl.info.enable) return;

  for (int i=0; i < 4; i++) {
    ctrl.info.char_bank[i] = 0;
    ctrl.info.pname_bank[i] = 0;
    for (int j=0; j < 8; j++) {
      if (Vdp2External.AC_VRAM[i][j] == 0x05) {
        ctrl.info.char_bank[i] = 1;
        char_access |= 1<<j;
      }
      if (Vdp2External.AC_VRAM[i][j] == 0x01) {
        ctrl.info.pname_bank[i] = 1;
        ptn_access |= (1 << j);
      }
    }
  }

  ctrl.info.transparencyenable = !(ctrl.regs->BGON & 0x200);
  ctrl.info.specialprimode = (ctrl.regs->SFPRMD >> 2) & 0x3;

  /* VDP2 Manual §4.2 CHCTLA bits 13-12 : colornumber NBG1
   * Doit être assigné AVANT ReadBitmapSize et le calcul du wrap. */
  ctrl.info.colornumber = (ctrl.regs->CHCTLA & 0x3000) >> 12;

  if (char_access == 0) return;

  if ((ctrl.info.isbitmap = ctrl.regs->CHCTLA & 0x200) != 0)
  {
    ReadBitmapSize(&ctrl.info, ctrl.regs->CHCTLA >> 10, 0x3);

    ctrl.info.x = -((ctrl.regs->SCXIN1 & 0x7FF) % ctrl.info.cellw);
    ctrl.info.y = -((ctrl.regs->SCYIN1 & 0x7FF) % ctrl.info.cellh);

    /* charaddr doit être assigné AVANT bitmap_base */
    ctrl.info.charaddr = ((ctrl.regs->MPOFN & 0x70) >> 4) * 0x20000;
    ctrl.info.paladdr = (ctrl.regs->BMPNA & 0x700) >> 4;
    ctrl.info.flipfunction = 0;
    /* VDP2 Manual ST-58-R2 §11.2 p.228 : en mode bitmap, le bit de
     * priorité spéciale (special priority mode 1/2) provient de BMPNA
     * (N1BMPR = bit 13), PAS du pattern name data. NBG0 lit déjà
     * N0BMPR (bit 5) dans sa branche bitmap. */
    ctrl.info.specialfunction = (ctrl.regs->BMPNA >> 13) & 0x01;
    /* VDP2 ST-058-R2 p.112 : N1BMCC = BMPNA bit 12. Normaliser en 0/1
     * (>>12), pas >>4 qui laissait la valeur a 256. */
    ctrl.info.specialcolorfunction = (ctrl.regs->BMPNA >> 12) & 0x1;

    /* VDP2 Manual p.95 : wrap bitmap.
     * Calculé APRÈS charaddr (bitmap_base) ET colornumber ET cellw/cellh. */
    switch (ctrl.info.colornumber) {
      case 0: /* 4bpp */
        ctrl.info.bitmap_wrap_size = (ctrl.info.cellw * ctrl.info.cellh) / 2;
        break;
      case 1: /* 8bpp */
        ctrl.info.bitmap_wrap_size = ctrl.info.cellw * ctrl.info.cellh;
        break;
      case 2: /* 16bpp palette */
      case 3: /* 16bpp RGB */
        ctrl.info.bitmap_wrap_size = ctrl.info.cellw * ctrl.info.cellh * 2;
        break;
      case 4: /* 32bpp */
        ctrl.info.bitmap_wrap_size = ctrl.info.cellw * ctrl.info.cellh * 4;
        break;
      default:
        ctrl.info.bitmap_wrap_size = 0;
        break;
    }
    ctrl.info.bitmap_base = ctrl.info.charaddr;

    int charAddrBk = (((ctrl.info.charaddr >> 16)& 0xF) >> ((ctrl.regs->VRSIZE >> 15)&0x1)) >> 1;
    int needUpdate = 0;
    for (int i = 0; i < line_max; i++) {
      /* VDP2 Manual §4.1 BGON p.49: NBG1 enable bit is N1ON = BGON bit 1
       * (mask 0x02), NOT R0ON (mask 0x10).  Same copy-paste bug fixed in
       * NBG0; the per-line RAMCTL-conflict scan must key off the layer
       * actually being drawn. */
      if ((Vdp2Lines[i].BGON & 0x2)!=0) {
        /* §6.2 p.149 : bank du character pattern de NBG1
         * reservee a RBG0 -> cycle pattern ignore, calque invalide. */
        if (Vdp2BankOwnedByRBG0(&Vdp2Lines[i], charAddrBk)) {
          needUpdate = 1;
          ctrl.info.display[i] = 0;
        }
      }
    }
    if (needUpdate != 0) {
      ctrl.info.enable = 0;
      for (int i = 0; i < line_max; i++) ctrl.info.enable |= ctrl.info.display[i];
      if (!ctrl.info.enable) return;
    }
  }
  else
  {
    // Tile Mode — bitmap_base et bitmap_wrap_size restent à 0
    if (ptn_access == 0) return;
    ctrl.info.mapwh = 2;
    ReadPlaneSize(&ctrl.info, ctrl.regs->PLSZ >> 2);
    ctrl.info.x = -((ctrl.regs->SCXIN1 & 0x7FF) % (512 * ctrl.info.planew));
    ctrl.info.y = -((ctrl.regs->SCYIN1 & 0x7FF) % (512 * ctrl.info.planeh));
    ReadPatternData(&ctrl.info, ctrl.regs->PNCN1, ctrl.regs->CHCTLA & 0x100);
  }

  ctrl.info.specialcolormode = (ctrl.regs->SFCCMD >> 2) & 0x3;

  if (ctrl.regs->SFSEL & 0x2)
    ctrl.info.specialcode = ctrl.regs->SFCODE >> 8;
  else
    ctrl.info.specialcode = ctrl.regs->SFCODE & 0xFF;

  ReadMosaicData(&ctrl.info, 0x2, ctrl.regs);


  ctrl.info.coloroffset = (ctrl.regs->CRAOFA & 0x70) << 4;
  ctrl.info.linecheck_mask = 0x02;

  if ((ctrl.regs->ZMXN1.all & 0x7FF00) == 0) return;
  else ctrl.info.coordincx = (float)65536 / (ctrl.regs->ZMXN1.all & 0x7FF00);

  switch ((ctrl.regs->ZMCTL >> 8) & 0x03)
  {
  case 0:
    ctrl.info.maxzoom = 1.0f;
    break;
  case 1:
    ctrl.info.maxzoom = 0.5f;
    if (ctrl.info.coordincx < 0.5f) ctrl.info.coordincx = 0.5f;
    break;
  case 2:
  case 3:
    ctrl.info.maxzoom = 0.25f;
    if (ctrl.info.coordincx < 0.25f) ctrl.info.coordincx = 0.25f;
    break;
  }

  if ((ctrl.regs->ZMYN1.all & 0x7FF00) == 0) return;
  ctrl.info.coordincy_raw = ctrl.regs->ZMYN1.all & 0x7FF00;
  ctrl.info.coordincy = (float)65536 / (ctrl.regs->ZMYN1.all & 0x7FF00);

  ctrl.info.priority = (ctrl.regs->PRINA >> 8) & 0x7;

  ctrl.info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG1PlaneAddr;

  if ((ctrl.info.priority == 0) ||
      ((ctrl.regs->BGON & 0x1) && (((ctrl.regs->CHCTLA & 0x70) >> 4) == 4))) {
    return;
  }

  ReadLineScrollData(&ctrl.info, ctrl.regs->SCRCTL >> 8, ctrl.regs->LSTA1.all, ctrl.regs);
  ctrl.info.lineinfo = lineNBG1;
  Vdp2GenLineinfo(&ctrl.info);

  if ((ctrl.regs->SCRCTL & 0x100) && Vdp2VCSCAccessValid(ctrl.regs, NBG1)) {
    ctrl.info.isverticalscroll = 1;
    if ((ctrl.regs->SCRCTL & 0x1) && Vdp2VCSCAccessValid(ctrl.regs, NBG0)) {
      ctrl.info.verticalscrolltbl = 4 + ((ctrl.regs->VCSTA.all & 0x7FFFE) << 1);
      ctrl.info.verticalscrollinc = 8;
    }
    else {
      ctrl.info.verticalscrolltbl = (ctrl.regs->VCSTA.all & 0x7FFFE) << 1;
      ctrl.info.verticalscrollinc = 4;
    }
  }
  else ctrl.info.isverticalscroll = 0;

  const int screenY1 = (_Ygl->rheight * startLine) / yabsys.VBlankLineCount;
  const int screenY2 = (_Ygl->rheight * endLine)   / yabsys.VBlankLineCount;

  if (ctrl.info.isbitmap)
  {
    if (ctrl.info.coordincx != 1.0f || ctrl.info.coordincy != 1.0f || VDPLINE_SZ(ctrl.info.islinescroll)) {
      ctrl.info.sh = (ctrl.regs->SCXIN1 & 0x7FF);
      ctrl.info.sv = (ctrl.regs->SCYIN1 & 0x7FF);
      ctrl.info.x = 0; ctrl.info.y = 0;
      ctrl.info.vertices[0] = 0;                  ctrl.info.vertices[1] = (float)screenY1;
      ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
      ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
      ctrl.info.vertices[6] = 0;                  ctrl.info.vertices[7] = (float)screenY2;
      vdp2draw_struct infotmp = ctrl.info;
      infotmp.cellw = _Ygl->rwidth;
      infotmp.cellh = screenY2 - screenY1;
      YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
      Vdp2DrawBitmapCoordinateInc(&ctrl);
    }
    else {
      int xx, yy;
      int isCached = 0;
      if (ctrl.info.islinescroll) {
        ctrl.info.sh = (ctrl.regs->SCXIN1 & 0x7FF);
        ctrl.info.sv = (ctrl.regs->SCYIN1 & 0x7FF);
        ctrl.info.x = 0; ctrl.info.y = 0;
        ctrl.info.vertices[0] = 0;                  ctrl.info.vertices[1] = (float)screenY1;
        ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
        ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
        ctrl.info.vertices[6] = 0;                  ctrl.info.vertices[7] = (float)screenY2;
        vdp2draw_struct infotmp = ctrl.info;
        infotmp.cellw = _Ygl->rwidth;
        infotmp.cellh = screenY2 - screenY1;
        YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
        Vdp2DrawBitmapLineScroll(&ctrl, _Ygl->rwidth, screenY2 - screenY1);
      }
      else {
        int cellh = ctrl.info.cellh;
        yy = ctrl.info.y;
        while (yy + cellh <= screenY1) yy += cellh;
        while (yy < screenY2) {
          ctrl.info.draw_line = yy;
          xx = ctrl.info.x;
          while (xx + ctrl.info.x < _Ygl->rwidth) {
            ctrl.info.vertices[0] = xx;                       ctrl.info.vertices[1] = (float)yy;
            ctrl.info.vertices[2] = (float)(xx + ctrl.info.cellw); ctrl.info.vertices[3] = (float)yy;
            ctrl.info.vertices[4] = (float)(xx + ctrl.info.cellw); ctrl.info.vertices[5] = (float)(yy + cellh);
            ctrl.info.vertices[6] = xx;                       ctrl.info.vertices[7] = (float)(yy + cellh);
            if (isCached == 0) {
              YglQuad(&ctrl.info, &ctrl.texture, &tmpc, YglTM_vdp2);
              requestDrawCell(&ctrl);
              isCached = 1;
            }
            else YglCachedQuad(&ctrl.info, &tmpc, YglTM_vdp2);
            xx += ctrl.info.cellw;
          }
          yy += cellh;
        }
      }
    }
  }
  else {
    if (ctrl.info.islinescroll) {
      if (char_access == 0) return;
      ctrl.info.sh = (ctrl.regs->SCXIN1 & 0x7FF);
      ctrl.info.sv = (ctrl.regs->SCYIN1 & 0x7FF);
      ctrl.info.x = 0; ctrl.info.y = 0;
      ctrl.info.vertices[0] = 0;                  ctrl.info.vertices[1] = (float)screenY1;
      ctrl.info.vertices[2] = (float)_Ygl->rwidth; ctrl.info.vertices[3] = (float)screenY1;
      ctrl.info.vertices[4] = (float)_Ygl->rwidth; ctrl.info.vertices[5] = (float)screenY2;
      ctrl.info.vertices[6] = 0;                  ctrl.info.vertices[7] = (float)screenY2;
      vdp2draw_struct infotmp = ctrl.info;
      infotmp.cellw = _Ygl->rwidth;
      infotmp.cellh = screenY2 - screenY1;
      infotmp.flipfunction = 0;
      YglQuad(&infotmp, &ctrl.texture, &tmpc, YglTM_vdp2);
      Vdp2DrawMapPerLine(&ctrl);
    }
    else {
      int delayed = 0;
      if (((ptn_access & 0x1)==0) && Vdp2CheckCharAccessPenalty(char_access, ptn_access, (ctrl.info.patternwh == 2)) != 0)
        delayed = 1;
      ctrl.info.x = ctrl.regs->SCXIN1 & 0x7FF;
      ctrl.info.y = ctrl.regs->SCYIN1 & 0x7FF;
      Vdp2DrawMapTest(&ctrl, delayed);
    }
  }
#ifdef CELL_ASYNC
  YabThreadYield();
#endif
}

//////////////////////////////////////////////////////////////////////////////

/*
 * sameVDP2RegNBG2 — compare NBG2-relevant registers between two scan lines.
 *
 * NBG2 has no zoom (coordincx = coordincy = 1 always), no bitmap mode,
 * no line scroll. Relevant registers are fewer than NBG0/NBG1.
 * (VDP2 Manual §4.1 Table 4.1, §4 CHCTLB)
 */
static int sameVDP2RegNBG2(Vdp2 *a, Vdp2 *b)
{
    /* WPSX0/WPEX0/WPSY0/WPEY0 (180020H..180026H) :
     * Window 0 boundaries.  VDP2 Manual ST-58-R2 §8.1 p.181.
     * Mid-frame moves of W0 (spotlight effect) must split zones. */
    if (a->WPSX0 != b->WPSX0) return 0;
    if (a->WPEX0 != b->WPEX0) return 0;
    if (a->WPSY0 != b->WPSY0) return 0;
    if (a->WPEY0 != b->WPEY0) return 0;
    /* W1 : 180028H..18002EH */
    if (a->WPSX1 != b->WPSX1) return 0;
    if (a->WPEX1 != b->WPEX1) return 0;
    if (a->WPSY1 != b->WPSY1) return 0;
    if (a->WPEY1 != b->WPEY1) return 0;

    /* LWTA0/LWTA1 (1800D8H/1800DCH) : line window table
     * addresses + W0LWE/W1LWE bit 15. VDP2 Manual ST-58-R2
     * p.186. Mid-frame rewrite must split zones. */
    if (a->LWTA0.all != b->LWTA0.all) return 0;
    if (a->LWTA1.all != b->LWTA1.all) return 0;
	
    /* RAMCTL bits 15 (CRKTE) + 11..0 (bank type assignments).
     * VDP2 Manual ST-58-R2 §3.2 p.29-30. The bank-conflict
     * scans inside Vdp2DrawNBG0..3 and Vdp2DrawRBG0_part read
     * RAMCTL ; mid-frame rewrites must invalidate zones. */
    if ((a->RAMCTL & 0x8FFF) != (b->RAMCTL & 0x8FFF)) return 0;
	
    /* BGON: N2ON = bit 2. Also RBG suppression bits 4,5. */
    if ((a->BGON & 0x34) != (b->BGON & 0x34)) return 0;
 
    /* CHCTLB bits 2-0: N2CHCN(1)=colornumber, N2PNB(0)=pattern name size.
     * Also check N0CHCN(6-4) for NBG2 disable condition (§4.1 Table 4.1). */
    if ((a->CHCTLB & 0x0003) != (b->CHCTLB & 0x0003)) return 0;
    /* Disable condition check: if NBG0 colornumber >= 2, NBG2 is off.
     * A change in CHCTLA bits 6-4 that crosses the >=2 threshold matters. */
    if (((a->CHCTLA & 0x0070) >> 4) != ((b->CHCTLA & 0x0070) >> 4)) return 0;
 
    /* PRINB bits 2-0: NBG2 priority number. */
    if ((a->PRINB & 0x0007) != (b->PRINB & 0x0007)) return 0;
 
    /* CCRNB bits 4-0: N2CCRT[4:0] — NBG2 color calculation ratio. */
    if ((a->CCRNB & 0x001F) != (b->CCRNB & 0x001F)) return 0;
 
    /* SCXN2 bits 10-0: NBG2 horizontal scroll (integer only, no fractional). */
    if ((a->SCXN2 & 0x07FF) != (b->SCXN2 & 0x07FF)) return 0;
 
    /* SCYN2 bits 10-0: NBG2 vertical scroll. */
    if ((a->SCYN2 & 0x07FF) != (b->SCYN2 & 0x07FF)) return 0;
 
    /* CRAOFA bits 10-8: N2CAOS[2:0] — NBG2 color RAM address offset. */
    if ((a->CRAOFA & 0x0700) != (b->CRAOFA & 0x0700)) return 0;

    /* MPOFN bits 10-8: N2MP[8:6] — NBG2 map offset.
     * VDP2 Manual §4 'Map Offset Register' p.85.  Without this comparison,
     * a mid-frame change to the NBG2 map offset (rare but legal — some
     * games swap layer banks at a horizontal split point) would not
     * trigger a new render zone, and the layer would render with the
     * wrong character base address until the next register-change event
     * picked up by another field. */
    if ((a->MPOFN & 0x0700) != (b->MPOFN & 0x0700)) return 0;

    /* PLSZ bits 7-4: NBG2 plane size. Affects map wrapping calculations. */
    if ((a->PLSZ & 0x00F0) != (b->PLSZ & 0x00F0)) return 0;
 
    /* PNCN2: NBG2 pattern name control. */
    if ((a->PNCN2 & 0xFFFF) != (b->PNCN2 & 0xFFFF)) return 0;
 
    /* SFPRMD bits 5-4: NBG2 special priority mode. */
    if ((a->SFPRMD & 0x0030) != (b->SFPRMD & 0x0030)) return 0;
 
    /* SFCCMD bits 5-4: NBG2 special color calculation mode. */
    if ((a->SFCCMD & 0x0030) != (b->SFCCMD & 0x0030)) return 0;
 
    /* LNCLEN bit 2: N2LCEN. */
    if ((a->LNCLEN & 0x0004) != (b->LNCLEN & 0x0004)) return 0;
 
    /* CLOFSL bit 2: N2COSL. */
    if ((a->CLOFSL & 0x0004) != (b->CLOFSL & 0x0004)) return 0;
	
    /* WCTLB byte 0 (bits 7-0): N2LOG, N2SWE, N2SWA, N2W1E, N2W1A,
     * N2W0E, N2W0A.  VDP2 Manual ST-58-R2 §8 p.193 (1800D2H). */
    if ((a->WCTLB & 0x00FF) != (b->WCTLB & 0x00FF)) return 0;

    /* CLOFEN bit 2: N2COEN — color offset enable for NBG2.
     * VDP2 Manual ST-58-R2 p.250 (180110H).
     * Mirror of the NBG3 fix : zonal optimiser must invalidate
     * when N2COEN flips mid-frame. */
    if ((a->CLOFEN & 0x0004) != (b->CLOFEN & 0x0004)) return 0;

    /* ZMCTL bits 1-0: N0ZMQT/N0ZMHF — NBG0 horizontal reduction.
     * VDP2 Manual §5.2 Table 5.2: NBG0 reduction settings can disable
     * NBG2 (16 colors + 1/4 reduction, 256 colors + any reduction).
     * If a title rewrites ZMCTL mid-frame (e.g. an effect that
     * progressively zooms NBG0), the NBG2 disable threshold can flip,
     * so NBG2 must be re-rendered at that boundary.  Without this
     * check the zonal optimiser keeps the previous decision and the
     * lower zone renders against the wrong policy. */
    if ((a->ZMCTL & 0x0003) != (b->ZMCTL & 0x0003)) return 0;

    /* MZCTL bits 15-8 + bit 2: VDP2 §6.6.
     *   bit 2 = N2MZE (NBG2 mosaic enable)
     *   bits 15-8 = shared mosaic size. */
    if ((a->MZCTL & 0xFF04) != (b->MZCTL & 0xFF04)) return 0;

    return 1;
}
static void Vdp2DrawNBG2_zones(void)
{
    int lastLine = 0;
    int line;
    int max = (yabsys.VBlankLineCount >= 270) ? 270 : yabsys.VBlankLineCount;
 
    for (line = 1; line < max; line++) {
        if (!sameVDP2RegNBG2(&Vdp2Lines[line - 1], &Vdp2Lines[line])) {
            Vdp2DrawNBG2(&Vdp2Lines[lastLine], lastLine, line);
            lastLine = line;
        }
    }
    Vdp2DrawNBG2(&Vdp2Lines[lastLine], lastLine, max);
}

static void Vdp2DrawNBG2(Vdp2* varVdp2Regs, int startLine, int endLine)
{
  Vdp2Ctrl ctrl;
  ctrl.regs = varVdp2Regs;
  ctrl.info.startLine = startLine;
  ctrl.info.endLine = endLine;
  ctrl.info.dst = 0;
  ctrl.info.idScreen = NBG2;
  ctrl.info.cor = 0;
  ctrl.info.cog = 0;
  ctrl.info.cob = 0;
  ctrl.info.specialcolorfunction = 0;
  ctrl.info.enable = 0;

  const int line_max = (yabsys.VBlankLineCount >= 270)
                       ? 270 : yabsys.VBlankLineCount;
  for (int i = 0; i < line_max; i++) {
    ctrl.info.display[i] = isEnabled(NBG2, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    ctrl.info.alpha_per_line[i] = (u8)(((~Vdp2Lines[i].CCRNB & 0x1F) * 255) / 31);
  }
  
  if (!ctrl.info.enable) {
    return;
  }

  ctrl.info.transparencyenable = !(ctrl.regs->BGON & 0x400);
  ctrl.info.specialprimode = (ctrl.regs->SFPRMD >> 4) & 0x3;

  ctrl.info.colornumber = (ctrl.regs->CHCTLB & 0x2) >> 1;
  ctrl.info.mapwh = 2;

  ReadPlaneSize(&ctrl.info, ctrl.regs->PLSZ >> 4);
  ctrl.info.x = -((ctrl.regs->SCXN2 & 0x7FF) % (512 * ctrl.info.planew));
  ctrl.info.y = -((ctrl.regs->SCYN2 & 0x7FF) % (512 * ctrl.info.planeh));
  ReadPatternData(&ctrl.info, ctrl.regs->PNCN2, ctrl.regs->CHCTLB & 0x1);

  ReadMosaicData(&ctrl.info, 0x4, ctrl.regs);

  ctrl.info.specialcolormode = (ctrl.regs->SFCCMD >> 4) & 0x3;

  if (ctrl.regs->SFSEL & 0x4)
    ctrl.info.specialcode = ctrl.regs->SFCODE >> 8;
  else
    ctrl.info.specialcode = ctrl.regs->SFCODE & 0xFF;


  ctrl.info.coloroffset = (ctrl.regs->CRAOFA & 0x700);

  ctrl.info.linecheck_mask = 0x04;
  ctrl.info.coordincx = ctrl.info.coordincy = 1;
  ctrl.info.coordincy_raw = 0x100;
  ctrl.info.priority = ctrl.regs->PRINB & 0x7;
  ctrl.info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG2PlaneAddr;

	if (ctrl.info.priority == 0) return;
	if (ctrl.regs->BGON & 0x1) {
		const int n0_color = (ctrl.regs->CHCTLA & 0x70) >> 4;
		const int n0_zmhf  = (ctrl.regs->ZMCTL >> 0) & 1; /* up to 1/2 */
		const int n0_zmqt  = (ctrl.regs->ZMCTL >> 1) & 1; /* up to 1/4 */

		/* Table 4.1: NBG0 >= 2048 colors disables NBG2 */
		if (n0_color >= 2) return;

		/* Table 5.2: 256 colors + any reduction disables NBG2 */
		if (n0_color == 1 && (n0_zmhf || n0_zmqt)) return;

		/* Table 5.2: 16 colors + 1/4 reduction disables NBG2 */
		if (n0_color == 0 && n0_zmqt) return;
	}

  ctrl.info.islinescroll = 0;
  ctrl.info.linescrolltbl = 0;
  ctrl.info.lineinc = 0;
  ctrl.info.isverticalscroll = 0;

  int delayed = 0;
  {
    int char_access = 0;
    int ptn_access = 0;

    for (int i = 0; i < 4; i++) {
      ctrl.info.char_bank[i] = 0;
      ctrl.info.pname_bank[i] = 0;
      for (int j = 0; j < 8; j++) {
        if (Vdp2External.AC_VRAM[i][j] == 0x06) {
          ctrl.info.char_bank[i] = 1;
          char_access |= (1 << j);
        }
        if (Vdp2External.AC_VRAM[i][j] == 0x02) {
          ctrl.info.pname_bank[i] = 1;
          ptn_access |= (1 << j);
        }
      }
    }
    if (char_access == 0) {
      return;
    }
    if (ptn_access == 0) {
      return;
    }
     if (Vdp2CheckCharAccessPenalty(char_access, ptn_access, (ctrl.info.patternwh == 2)) != 0) {
       delayed = 1;

    }
  }


  ctrl.info.x = ctrl.regs->SCXN2 & 0x7FF;
  ctrl.info.y = ctrl.regs->SCYN2 & 0x7FF;

   {
     int screenY1 = (_Ygl->rheight * startLine) / yabsys.VBlankLineCount;
     int screenY2 = (_Ygl->rheight * endLine)   / yabsys.VBlankLineCount;
     ctrl.info.x = ctrl.regs->SCXN2 & 0x7FF;
     ctrl.info.y = ctrl.regs->SCYN2 & 0x7FF;
     Vdp2DrawMapTest(&ctrl, delayed);
   }

#ifdef CELL_ASYNC
    YabThreadYield();
#endif
}

//////////////////////////////////////////////////////////////////////////////

/*
 * sameVDP2RegNBG3 — compare NBG3-relevant registers between two scan lines.
 *
 * NBG3 has no zoom, no bitmap mode, no line scroll (same as NBG2).
 * Disabled when NBG0 >= 2048 colors OR NBG1 >= 2048 colors (§4.1 Table 4.1).
 */
static int sameVDP2RegNBG3(Vdp2 *a, Vdp2 *b)
{

    /* WPSX0/WPEX0/WPSY0/WPEY0 (180020H..180026H) :
     * Window 0 boundaries.  VDP2 Manual ST-58-R2 §8.1 p.181.
     * Mid-frame moves of W0 (spotlight effect) must split zones. */
    if (a->WPSX0 != b->WPSX0) return 0;
    if (a->WPEX0 != b->WPEX0) return 0;
    if (a->WPSY0 != b->WPSY0) return 0;
    if (a->WPEY0 != b->WPEY0) return 0;
    /* W1 : 180028H..18002EH */
    if (a->WPSX1 != b->WPSX1) return 0;
    if (a->WPEX1 != b->WPEX1) return 0;
    if (a->WPSY1 != b->WPSY1) return 0;
    if (a->WPEY1 != b->WPEY1) return 0;

    /* LWTA0/LWTA1 (1800D8H/1800DCH) : line window table
     * addresses + W0LWE/W1LWE bit 15. VDP2 Manual ST-58-R2
     * p.186. Mid-frame rewrite must split zones. */
    if (a->LWTA0.all != b->LWTA0.all) return 0;
    if (a->LWTA1.all != b->LWTA1.all) return 0;
	
    /* RAMCTL bits 15 (CRKTE) + 11..0 (bank type assignments).
     * VDP2 Manual ST-58-R2 §3.2 p.29-30. The bank-conflict
     * scans inside Vdp2DrawNBG0..3 and Vdp2DrawRBG0_part read
     * RAMCTL ; mid-frame rewrites must invalidate zones. */
    if ((a->RAMCTL & 0x8FFF) != (b->RAMCTL & 0x8FFF)) return 0;
	
    /* BGON: N3ON = bit 3. Also RBG suppression bits. */
    if ((a->BGON & 0x38) != (b->BGON & 0x38)) return 0;
 
    if ((a->CHCTLB & 0x0030) != (b->CHCTLB & 0x0030)) return 0;

    if (((a->CHCTLA & 0x0070) >> 4) != ((b->CHCTLA & 0x0070) >> 4)) return 0;

    if (((a->CHCTLA & 0x3000) >> 12) != ((b->CHCTLA & 0x3000) >> 12)) return 0;
 
    /* PRINB bits 10-8: NBG3 priority number. */
    if ((a->PRINB & 0x0700) != (b->PRINB & 0x0700)) return 0;
 
    /* CCRNB bits 12-8: N3CCRT[4:0] — NBG3 color calculation ratio. */
    if ((a->CCRNB & 0x1F00) != (b->CCRNB & 0x1F00)) return 0;
 
    /* SCXN3 bits 10-0: NBG3 horizontal scroll. */
    if ((a->SCXN3 & 0x07FF) != (b->SCXN3 & 0x07FF)) return 0;
 
    /* SCYN3 bits 10-0: NBG3 vertical scroll. */
    if ((a->SCYN3 & 0x07FF) != (b->SCYN3 & 0x07FF)) return 0;
 
    /* CRAOFA bits 14-12: N3CAOS[2:0] — NBG3 color RAM address offset. */
    if ((a->CRAOFA & 0x7000) != (b->CRAOFA & 0x7000)) return 0;

    /* MPOFN bits 14-12: N3MP[8:6] — NBG3 map offset. */
    if ((a->MPOFN & 0x7000) != (b->MPOFN & 0x7000)) return 0;

    if ((a->PLSZ & 0x00C0) != (b->PLSZ & 0x00C0)) return 0;
 
    /* PNCN3: NBG3 pattern name control. */
    if ((a->PNCN3 & 0xFFFF) != (b->PNCN3 & 0xFFFF)) return 0;
 
    /* SFPRMD bits 7-6: NBG3 special priority mode. */
    if ((a->SFPRMD & 0x00C0) != (b->SFPRMD & 0x00C0)) return 0;
 
    /* SFCCMD bits 7-6: NBG3 special color calculation mode. */
    if ((a->SFCCMD & 0x00C0) != (b->SFCCMD & 0x00C0)) return 0;
 
    /* LNCLEN bit 3: N3LCEN. */
    if ((a->LNCLEN & 0x0008) != (b->LNCLEN & 0x0008)) return 0;
 
    /* CLOFSL bit 3: N3COSL. */
    if ((a->CLOFSL & 0x0008) != (b->CLOFSL & 0x0008)) return 0;

    /* WCTLB byte 1 (bits 15-8): N3LOG, N3SWE, N3SWA, N3W1E, N3W1A,
     * N3W0E, N3W0A.  VDP2 Manual ST-58-R2 §8 p.193 (1800D2H). */
    if ((a->WCTLB & 0xFF00) != (b->WCTLB & 0xFF00)) return 0;
	
    /* CLOFEN bit 3: N3COEN — color offset enable for NBG3. */
    if ((a->CLOFEN & 0x0008) != (b->CLOFEN & 0x0008)) return 0;


    /* ZMCTL bits 9-8: N1ZMQT/N1ZMHF — NBG1 horizontal reduction. */
    if ((a->ZMCTL & 0x0300) != (b->ZMCTL & 0x0300)) return 0;

    /* MZCTL bits 15-8 + bit 3: VDP2 §6.6. */
    if ((a->MZCTL & 0xFF08) != (b->MZCTL & 0xFF08)) return 0;

    return 1;
}

static void Vdp2DrawNBG3_zones(void)
{
    int lastLine = 0;
    int line;
    int max = (yabsys.VBlankLineCount >= 270) ? 270 : yabsys.VBlankLineCount;
 
    for (line = 1; line < max; line++) {
        if (!sameVDP2RegNBG3(&Vdp2Lines[line - 1], &Vdp2Lines[line])) {
            Vdp2DrawNBG3(&Vdp2Lines[lastLine], lastLine, line);
            lastLine = line;
        }
    }
    Vdp2DrawNBG3(&Vdp2Lines[lastLine], lastLine, max);
}

static void Vdp2DrawNBG3(Vdp2* varVdp2Regs, int startLine, int endLine)
{
  Vdp2Ctrl ctrl;
  ctrl.regs = varVdp2Regs;
  ctrl.info.idScreen = NBG3;
  ctrl.info.dst = 0;
  ctrl.info.cor = 0;
  ctrl.info.cog = 0;
  ctrl.info.cob = 0;
  ctrl.info.specialcolorfunction = 0;
  ctrl.info.enable = 0;
  /* Segmented line rendering — zone [startLine, endLine). */
  ctrl.info.startLine = startLine;
  ctrl.info.endLine   = endLine;
 
  const int line_max = (yabsys.VBlankLineCount >= 270)
                       ? 270 : yabsys.VBlankLineCount;
  for (int i = 0; i < line_max; i++) {
    ctrl.info.display[i] = isEnabled(NBG3, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    ctrl.info.alpha_per_line[i] = (u8)(((~(Vdp2Lines[i].CCRNB >> 8) & 0x1F) * 255) / 31);
  }
 
  if (!ctrl.info.enable) {
    return;
  }
  ctrl.info.transparencyenable = !(ctrl.regs->BGON & 0x800);
  ctrl.info.specialprimode = (ctrl.regs->SFPRMD >> 6) & 0x3;
 
  ctrl.info.colornumber = (ctrl.regs->CHCTLB & 0x20) >> 5;
 
  ctrl.info.mapwh = 2;
 
  ReadPlaneSize(&ctrl.info, ctrl.regs->PLSZ >> 6);
  ctrl.info.x = -((ctrl.regs->SCXN3 & 0x7FF) % (512 * ctrl.info.planew));
  ctrl.info.y = -((ctrl.regs->SCYN3 & 0x7FF) % (512 * ctrl.info.planeh));
  ReadPatternData(&ctrl.info, ctrl.regs->PNCN3, ctrl.regs->CHCTLB & 0x10);
 
  ReadMosaicData(&ctrl.info, 0x8, ctrl.regs);

 
  ctrl.info.specialcolormode = (ctrl.regs->SFCCMD >> 6) & 0x03;
  if (ctrl.regs->SFSEL & 0x8)
    ctrl.info.specialcode = ctrl.regs->SFCODE >> 8;
  else
    ctrl.info.specialcode = ctrl.regs->SFCODE & 0xFF;
 
  ctrl.info.coloroffset = ((ctrl.regs->CRAOFA & 0x7000) >> 4);
 
  ctrl.info.linecheck_mask = 0x08;
  ctrl.info.coordincx = ctrl.info.coordincy = 1;
  ctrl.info.coordincy_raw = 0x100;
 
  ctrl.info.priority = (ctrl.regs->PRINB >> 8) & 0x7;
  ctrl.info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG3PlaneAddr;
 
  if (ctrl.info.priority == 0) return;

  if (ctrl.regs->BGON & 0x1) {
    if (((ctrl.regs->CHCTLA & 0x70) >> 4) >= 4) return;
  }

  if (ctrl.regs->BGON & 0x2) {
    const int n1_color = (ctrl.regs->CHCTLA & 0x3000) >> 12;
    const int n1_zmhf  = (ctrl.regs->ZMCTL >> 8) & 1;
    const int n1_zmqt  = (ctrl.regs->ZMCTL >> 9) & 1;

    /* Table 4.1: NBG1 >= 2048 colors disables NBG3 */
    if (n1_color >= 2) return;

    /* Table 5.2: 256 colors + any reduction disables NBG3 */
    if (n1_color == 1 && (n1_zmhf || n1_zmqt)) return;

    /* Table 5.2: 16 colors + 1/4 reduction disables NBG3 */
    if (n1_color == 0 && n1_zmqt) return;
  }
 
  ctrl.info.islinescroll = 0;
  ctrl.info.linescrolltbl = 0;
  ctrl.info.lineinc = 0;
  ctrl.info.isverticalscroll = 0;
 
  int delayed = 0;
  {
    int char_access = 0;
    int ptn_access = 0;
    for (int i = 0; i < 4; i++) {
      ctrl.info.char_bank[i] = 0;
      ctrl.info.pname_bank[i] = 0;
      for (int j = 0; j < 8; j++) {
        if (Vdp2External.AC_VRAM[i][j] == 0x07) {
          ctrl.info.char_bank[i] = 1;
          char_access |= (1 << j);
        }
        if (Vdp2External.AC_VRAM[i][j] == 0x03) {
          ctrl.info.pname_bank[i] = 1;
          ptn_access |= (1 << j);
        }
      }
    }
    if (char_access == 0) return;
    if (ptn_access == 0) return;
    if (Vdp2CheckCharAccessPenalty(char_access, ptn_access, (ctrl.info.patternwh == 2)) != 0) delayed = 1;
  }
 
  ctrl.info.x = ctrl.regs->SCXN3 & 0x7FF;
  ctrl.info.y = ctrl.regs->SCYN3 & 0x7FF;
  Vdp2DrawMapTest(&ctrl, delayed);
#ifdef CELL_ASYNC
  YabThreadYield();
#endif
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawRBG0_part( RBGDrawInfo *rbg)
{
  vdp2draw_struct* info = &rbg->ctrl.info;

  info->dst = 0;
  info->idScreen = RBG0;
  info->cor = 0;
  info->cog = 0;
  info->cob = 0;
  info->specialcolorfunction = 0;
  info->enable = 0;
  info->RotWin = NULL;
  info->RotWinMode = 0;

  info->enable = ((rbg->ctrl.regs->BGON & 0x10)!=0);
  if (!info->enable) {
    pushRBG(rbg);
    return;
  }
  // //If no VRAM access is granted to RBG0, just abort.
  if ((rbg->ctrl.regs->RAMCTL & 0xFF) == 0) {
    LOG("No RAMCTL for RBG0\n");
    pushRBG(rbg);
    return;
  }

  for (int i=info->startLine; i<info->endLine; i++) {
    info->display[i] = info->enable;
    /* VDP2 Manual §12.1 CCRR (18010CH) bits 4-0 = R0CCRT[4:0]:
     * Same encoding as NBG screens. Full 0-255 mapping. */
    rbg->alpha[i] = (u8)(((~Vdp2Lines[i].CCRR & 0x1F) * 255) / 31);
    info->alpha_per_line[i] = rbg->alpha[i];
  }

  info->priority = rbg->ctrl.regs->PRIR & 0x7;

  LOG_AREA("RGB0 prio = %d\n", info->priority);

  if (info->priority == 0) {
    pushRBG(rbg);
    return;
  }

  info->transparencyenable = !(rbg->ctrl.regs->BGON & 0x1000);

  info->specialprimode = (rbg->ctrl.regs->SFPRMD >> 8) & 0x3;

  info->colornumber = (rbg->ctrl.regs->CHCTLB & 0x7000) >> 12;

  LOG_AREA("RGB0 colornumber = %d\n", info->colornumber);

  info->islinescroll = 0;
  info->linescrolltbl = 0;
  info->lineinc = 0;
  info->coordincy_raw = 0x100;

  Vdp2ReadRotationTable(0, &rbg->paraA, rbg->ctrl.regs, Vdp2Ram);
  Vdp2ReadRotationTable(1, &rbg->paraB, rbg->ctrl.regs, Vdp2Ram);

  rbg->paraA.charaddr = (rbg->ctrl.regs->MPOFR & 0x7) * 0x20000;
  rbg->paraB.charaddr = (rbg->ctrl.regs->MPOFR & 0x70) * 0x2000;
  ReadPlaneSizeR(&rbg->paraA, rbg->ctrl.regs->PLSZ >> 8);
  ReadPlaneSizeR(&rbg->paraB, rbg->ctrl.regs->PLSZ >> 12);

  if ((rbg->ctrl.regs->RPMD & 0x3) == 0x03)
  {
    //printf("RPMD 0x3\n");
    // Enable Window0(RPW0E)?
    if (((rbg->ctrl.regs->WCTLD >> 1) & 0x01) == 0x01)
    {
      info->RotWin = _Ygl->win[0];
      // RPW0A( inside = 0, outside = 1 )
      info->RotWinMode = (rbg->ctrl.regs->WCTLD & 0x01);
      // Enable Window1(RPW1E)?
    }
    else if (((rbg->ctrl.regs->WCTLD >> 3) & 0x01) == 0x01)
    {
      info->RotWin = _Ygl->win[1];
      // RPW1A( inside = 0, outside = 1 )
      info->RotWinMode = ((rbg->ctrl.regs->WCTLD >> 2) & 0x01);
      // Bad Setting Both Window is disabled
    }
  }

  rbg->paraA.screenover = (rbg->ctrl.regs->PLSZ >> 10) & 0x03;
  rbg->paraB.screenover = (rbg->ctrl.regs->PLSZ >> 14) & 0x03;
	  
	if (rbg->paraA.screenover == 1)
		rbg->paraA.over_pattern_name = rbg->ctrl.regs->OVPNRA;
	else if (rbg->paraA.screenover >= 2)
		rbg->paraA.over_pattern_name = 0xFFFF;

	if (rbg->paraB.screenover == 1)
		rbg->paraB.over_pattern_name = rbg->ctrl.regs->OVPNRB;
	else if (rbg->paraB.screenover >= 2)
		rbg->paraB.over_pattern_name = 0xFFFF;

  // Figure out which Rotation Parameter we're uqrt
  switch (rbg->ctrl.regs->RPMD & 0x3)
  {
  case 0:
    // Parameter A
    info->rotatenum = 0;
    info->PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
    break;
  case 1:
    // Parameter B
    info->rotatenum = 1;
    info->PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
    break;
	case 2:
		info->rotatenum = 0;
		info->PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
		rbg->paraA.coefenab = rbg->ctrl.regs->KTCTL & 0x01;   // RAKTE (bit 0)
		rbg->paraB.coefenab = rbg->ctrl.regs->KTCTL & 0x100;  // RBKTE (bit 8)
		rbg->useb = 1;
		break;
  case 3:
  default:
    // Parameter A+B switched via rotation parameter window
    // FIX ME(need to figure out which Parameter is being used)
    info->rotatenum = 0;
    info->PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
    break;
  }

  info->isbitmap = ((rbg->ctrl.regs->CHCTLB & 0x200) != 0);

  if (info->isbitmap)
  {

    // Bitmap Mode
    ReadBitmapSize(info, rbg->ctrl.regs->CHCTLB >> 10, 0x1);
    if (info->rotatenum == 0)
      // Parameter A
      info->charaddr = (rbg->ctrl.regs->MPOFR & 0x7) * 0x20000;
    else
      // Parameter B
      info->charaddr = (rbg->ctrl.regs->MPOFR & 0x70) * 0x2000;

    //If no VRAM access is granted to RBG0 character pattern table , just abort.
      int charAddrBk = (((info->charaddr >> 16)& 0xF) >> ((rbg->ctrl.regs->VRSIZE >> 15)&0x1)) >> 1;
      if ((((rbg->ctrl.regs->RAMCTL>>(charAddrBk<<1))&0x3) != 0x3) && ((rbg->ctrl.regs->RPMD & 0x3) < 0x2)) {
        pushRBG(rbg);
        return;
      }

    info->paladdr = (rbg->ctrl.regs->BMPNB & 0x7) << 4;
    info->flipfunction = 0;
    info->specialfunction = 0;
  }
  else
  {
    int i;
    // Tile Mode
    info->mapwh = 4;

    if (info->rotatenum == 0)
      // Parameter A
      ReadPlaneSize(info, rbg->ctrl.regs->PLSZ >> 8);
    else
      // Parameter B
      ReadPlaneSize(info, rbg->ctrl.regs->PLSZ >> 12);

    ReadPatternData(info, rbg->ctrl.regs->PNCR, rbg->ctrl.regs->CHCTLB & 0x100);

    rbg->paraA.ShiftPaneX = 8 + rbg->paraA.planew;
    rbg->paraA.ShiftPaneY = 8 + rbg->paraA.planeh;
    rbg->paraB.ShiftPaneX = 8 + rbg->paraB.planew;
    rbg->paraB.ShiftPaneY = 8 + rbg->paraB.planeh;

    rbg->paraA.MskH = (8 * 64 * rbg->paraA.planew) - 1;
    rbg->paraA.MskV = (8 * 64 * rbg->paraA.planeh) - 1;
    rbg->paraB.MskH = (8 * 64 * rbg->paraB.planew) - 1;
    rbg->paraB.MskV = (8 * 64 * rbg->paraB.planeh) - 1;

    rbg->paraA.MaxH = 8 * 64 * rbg->paraA.planew * 4;
    rbg->paraA.MaxV = 8 * 64 * rbg->paraA.planeh * 4;
    rbg->paraB.MaxH = 8 * 64 * rbg->paraB.planew * 4;
    rbg->paraB.MaxV = 8 * 64 * rbg->paraB.planeh * 4;

    if (rbg->paraA.screenover == OVERMODE_512)
    {
      rbg->paraA.MaxH = 512;
      rbg->paraA.MaxV = 512;
    }

    if (rbg->paraB.screenover == OVERMODE_512)
    {
      rbg->paraB.MaxH = 512;
      rbg->paraB.MaxV = 512;
    }

// Sauvegarder les valeurs de ParaA et substituer celles de ParaB pour la boucle B
int saved_planew = info->planew;
int saved_planeh = info->planeh;
int saved_mapwh  = info->mapwh;

for (i = 0; i < 16; i++) {
    Vdp2ParameterAPlaneAddr(info, i, rbg->ctrl.regs);
    rbg->paraA.PlaneAddrv[i] = info->addr;
}

// Restaurer les dimensions de ParaB pour Vdp2ParameterBPlaneAddr
info->planew = rbg->paraB.planew;
info->planeh = rbg->paraB.planeh;
info->mapwh  = 4; // RBG0 toujours 4x4

for (i = 0; i < 16; i++) {
    Vdp2ParameterBPlaneAddr(info, i, rbg->ctrl.regs);
    rbg->paraB.PlaneAddrv[i] = info->addr;
}

// Restaurer les valeurs de ParaA (info est utilisé ensuite pour le rendu)
info->planew = saved_planew;
info->planeh = saved_planeh;
info->mapwh  = saved_mapwh;
  }
  

  ReadMosaicData(info, 0x10, rbg->ctrl.regs);

  info->specialcolormode = (rbg->ctrl.regs->SFCCMD >> 8) & 0x03;
  if (rbg->ctrl.regs->SFSEL & 0x10)
    info->specialcode = rbg->ctrl.regs->SFCODE >> 8;
  else
    info->specialcode = rbg->ctrl.regs->SFCODE & 0xFF;

  info->coloroffset = (rbg->ctrl.regs->CRAOFB & 0x7) << 8;

  info->linecheck_mask = 0x10;
  info->coordincx = info->coordincy = 1;
  info->coordincy_raw = 0x100;

  Vdp2DrawRotation(rbg);
}

/* VDP2 §6.2 p.148 RAMCTL RDBS
 * En mode rotation, le type d'usage de chaque bank VRAM pour RBG0
 * est donné UNIQUEMENT par RDBSx1/RDBSx0 du RAM control register :
 *   00 = non utilisé par RBG0
 *   01 = coefficient table
 *   10 = pattern name table
 *   11 = character pattern / bitmap pattern
 * Le VRAM cycle pattern register de la bank est ignoré (§3.3 p.31). */
enum { RDBS_UNUSED = 0, RDBS_COEF = 1, RDBS_PNAME = 2, RDBS_CHAR = 3 };

static int Vdp2RBG0BankType(Vdp2 *regs, int bank)   /* bank : 0=A0 1=A1 2=B0 3=B1 */
{
    return (regs->RAMCTL >> (bank * 2)) & 0x3;
}

/* Validation : l'adresse character/pattern de RBG0 doit tomber dans
 * une bank dont le RDBS correspond. Sinon le hardware ne lit rien
 * d'utile (§3.3 p.31 "access won't be done and the correct screen
 * will not be displayed"). */
static int Vdp2RBG0CharBankValid(Vdp2 *regs, u32 charaddr)
{
    int bank = Vdp2GetBank(regs, charaddr);
    int t = Vdp2RBG0BankType(regs, bank);
    return (t == RDBS_CHAR);
}

static void Vdp2DrawRBG0()
{
  int nbZone = 1;
  int lastLine = 0;
  int line;
  int max = (yabsys.VBlankLineCount >= 270)?270:yabsys.VBlankLineCount;
  RBGDrawInfo* rbg = NULL;
   /* Detection starts at line 1 like NBG0..NBG3 — comparing
    * Vdp2Lines[0] vs Vdp2Lines[1] catches register writes done
    * during the first H-blank.  Previous start at line=2 silently
    * dropped this boundary. */
    for (line = 1; line<max; line++) {

    if (!sameVDP2Reg(RBG0, &Vdp2Lines[line-1], &Vdp2Lines[line])) {
      rbg = popRBG();
      rbg->rbg_type = 0x0;
      rbg->ctrl.info.startLine = lastLine;
      rbg->ctrl.info.endLine = line;
      rbg->ctrl.regs = &Vdp2Lines[rbg->ctrl.info.startLine];
      lastLine = line;
      LOG_AREA("RBG0 Draw from %d to %d %x\n", rbg->ctrl.info.startLine, rbg->ctrl.info.endLine, rbg->ctrl.regs->BGON);
      Vdp2DrawRBG0_part(rbg);
    }
  }
  rbg = popRBG();
  rbg->rbg_type = 0x0;
  rbg->ctrl.info.startLine = lastLine;
  rbg->ctrl.info.endLine = line;
  rbg->ctrl.regs = &Vdp2Lines[rbg->ctrl.info.startLine];
  LOG_AREA("RBG0 Draw from %d to %d %x\n", rbg->ctrl.info.startLine, rbg->ctrl.info.endLine, rbg->ctrl.regs->BGON);
  Vdp2DrawRBG0_part(rbg);

}

#define ceilf(a) ((a)+0.99999f)


//////////////////////////////////////////////////////////////////////////////

static int _VIDCSIsFullscreen;

void VIDCSResize(int originx, int originy, unsigned int w, unsigned int h, int on)
{
  if ((originx == _Ygl->originx)&&
     (originy == _Ygl->originy)&&
     (w == GlWidth)&&
     (h == GlHeight)&&
     (_VIDCSIsFullscreen == on)) return;

  _VIDCSIsFullscreen = on;

  GlWidth = w;
  GlHeight = h;

  YglChangeResolution(_Ygl->rwidth, _Ygl->rheight);

  _Ygl->originx = originx;
  _Ygl->originy = originy;

  glViewport(originx, originy, GlWidth, GlHeight);

}

void VIDCSGetScale(float *xRatio, float *yRatio, int *xUp, int *yUp) {
  double w = 0;
  double h = 0;
  int x,y = 0;
  double dar = (double)GlWidth/(double)GlHeight;
  double par = 4.0/3.0;

  int width = (_Ygl->interlace == SINGLE_INTERLACE)?_Ygl->width*2:_Ygl->width;
  int Intw = (int)(floor((float)GlWidth/(float)width));
  int Inth = (int)(floor((float)GlHeight/(256.0 * _Ygl->vdp1ratio)));
  if (_Ygl->interlace != NORMAL_INTERLACE) Inth >>= 1;
  int Int  = 1<<(_Ygl->interlace == NORMAL_INTERLACE);
  RATIOMODE modeScreen = _Ygl->stretch;
  #ifndef __LIBRETRO__
  if (yabsys.isRotated) par = 1.0/par;
  #endif
  if (Intw == 0) {
    modeScreen = 0;
    Intw = 1;
  }
  if (Inth == 0) {
    modeScreen = 0;
    Inth = 1;
  }

  switch(modeScreen) {
    case ORIGINAL_RATIO:
      w = (dar>par)?(double)GlHeight*par:GlWidth;
      h = (dar>par)?(double)GlHeight:(double)GlWidth/par;
      x = (GlWidth-w)/2;
      y = (GlHeight-h)/2;
      break;
    case STRETCH_RATIO:
      w = GlWidth;
      h = GlHeight;
      x = 0;
      y = 0;
      break;
    case INTEGER_RATIO:
    case INTEGER_RATIO_FULL:
      w = Int * _Ygl->width;
      h = Int * _Ygl->height;
      x = (GlWidth-w)/2;
      y = (GlHeight-h)/2;
      break;
    default:
       break;
   }
   if (modeScreen == INTEGER_RATIO_FULL)  Int = (Inth<Intw)?Inth:Intw;

  *xRatio = w / _Ygl->rwidth;
  *yRatio = h / _Ygl->rheight;
  *xUp = x;
  *yUp = y;
}
//////////////////////////////////////////////////////////////////////////////

int VIDCSIsFullscreen(void) {
  return _VIDCSIsFullscreen;
}

//////////////////////////////////////////////////////////////////////////////

int VIDCSVdp1Reset(void)
{
  return 0;
}

// VDP2 Manual §13.1: Color offset range is -256 to +255 (9-bit two's complement).
// We encode into 8-bit centered at 128 for the shader.
// Clamp to [-128,+127] since RGB is 5-bit (0-31) and offsets beyond ±128
// would saturate anyway. This preserves full precision within useful range.
static inline int encodeColorOffset(int v) {
    if (v < -128) v = -128;
    if (v >  127) v =  127;
    return (v + 128) & 0xFF; // neutral=128, shader decodes: (x/255.0-0.5)*2→[-1,+1]
}

void VIDCSReadColorOffset(void) {
    u8 offset[enBGMAX+1] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x1, 0x40, 0x20};
    /* Linear mapping physical pixel row → logical scan line.
     * Replaces the `line >> line_shift` shortcut which only
     * worked for exact 1×/2× ratios. Robust for 480-line
     * exclusive (rheight=480, VBL=240), DDI (rheight=512,
     * VBL=256), and any future mode. */
    const int phys_h = _Ygl->rheight;
    const int log_h  = (yabsys.VBlankLineCount >= 270)
                       ? 270 : yabsys.VBlankLineCount;
    u32 * linebuf = YglGetPerlineBuf();
    for (int line = 0; line < phys_h; line++) {
        const int li = (line * log_h) / phys_h;
        Vdp2 * lVdp2Regs = &Vdp2Lines[li];


	// VDP2 Manual §13.1: Color offset registers are 9-bit two's complement,
	// range -256 to +255, added directly to RGB components.
	// No division needed — map [-256,+255] to the 8-bit centered encoding
	// used by encodeColorOffset(). encodeColorOffset clamps to [-128,+127]
	// which covers the practical range since RGB components are 5-bit (0-31).
	// Division by 2 was incorrectly halving the effective range.
	int a_cor = lVdp2Regs->COAR & 0x1FF;
	if (a_cor & 0x100) a_cor |= ~0x1FF; // sign-extend 9→32 bits
	// No >>1: keep full [-256,+255] range, encodeColorOffset clamps to [-128,+127]

	int a_cog = lVdp2Regs->COAG & 0x1FF;
	if (a_cog & 0x100) a_cog |= ~0x1FF;

	int a_cob = lVdp2Regs->COAB & 0x1FF;
	if (a_cob & 0x100) a_cob |= ~0x1FF;

	int b_cor = lVdp2Regs->COBR & 0x1FF;
	if (b_cor & 0x100) b_cor |= ~0x1FF;

	int b_cog = lVdp2Regs->COBG & 0x1FF;
	if (b_cog & 0x100) b_cog |= ~0x1FF;

	int b_cob = lVdp2Regs->COBB & 0x1FF;
	if (b_cob & 0x100) b_cob |= ~0x1FF;

        // Encodage sur 8 bits centré 128 — suppression du /2
        int colOffB =
             (encodeColorOffset(b_cob) << 16)
           | (encodeColorOffset(b_cog) << 8)
           | (encodeColorOffset(b_cor) << 0);
        int colOffA =
             (encodeColorOffset(a_cob) << 16)
           | (encodeColorOffset(a_cog) << 8)
           | (encodeColorOffset(a_cor) << 0);

		/* VDP2 Manual §14.1: MSB shadow active when:
		 * - SPWINEN = 0 (sprite window disabled)
		 * - sprite type is 2~7
		 * - sprite FB pixel MSB = 1
		 * - dot color data != Normal Shadow pattern
		 * This info must reach the compositor shader. */

		// Encoder dans linebuf un flag shadow par ligne :
        int sptype  = lVdp2Regs->SPCTL & 0xF;
        int spwinen = (lVdp2Regs->SPCTL >> 4) & 1;
        int spclmd  = (lVdp2Regs->SPCTL >> 5) & 1;
        int msb_shadow_enabled = (!spwinen) && (!spclmd) && (sptype >= 2) && (sptype <= 7);
        int spcccs  = (lVdp2Regs->SPCTL >> 12) & 3;
        int spccn   = (lVdp2Regs->SPCTL >> 8) & 7;
        int spccen  = (lVdp2Regs->CCCTL >> 6) & 1;

        /* VDP2 Manual §9.2: RGB sprite data always selects priority register 0. */
        int sprite_rgb_priority = lVdp2Regs->PRISA & 0x7; /* register 0 */
        _Ygl->sprite_rgb_priority_per_line[line] = spclmd ? sprite_rgb_priority : -1;

        /* VDP2 Manual §14.1: MSB shadow requires SPWINEN=0 and sprite types 2~7. */
        _Ygl->msb_shadow_enabled_per_line[line] = (u8)msb_shadow_enabled;

for (int id = 0; id < enBGMAX+1; id++) {
    u32 col;
    if (isEnabled(id, lVdp2Regs) == 0) {
        col = 0x00808080;
    } else {
        if (lVdp2Regs->CLOFEN & offset[id]) {
            if (lVdp2Regs->CLOFSL & offset[id])
                col = colOffB;
            else
                col = colOffA;
        } else {
            col = 0x00808080;
        }
    }

    if (id == SPRITE) {
        u32 sp_ctrl_bits = ((u32)(spccen & 0x1) << 29)
                         | ((u32)(spccn  & 0x7) << 26)
                         | ((u32)(spcccs & 0x3) << 24);
        linebuf[line + 512*id] = sp_ctrl_bits | (col & 0x00FFFFFFu);
    } else {
        linebuf[line + 512*id] = col;
    }
}
    }
    YglSetPerlineBuf(linebuf);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp1LocalCoordinate(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs)
{
  s32 lx = cmd->CMDXA & 0x7FF;
  s32 ly = cmd->CMDYA & 0x7FF;
  regs->localX = (lx & 0x400) ? (lx | ~0x7FF) : lx;
  regs->localY = (ly & 0x400) ? (ly | ~0x7FF) : ly;
}

//////////////////////////////////////////////////////////////////////////////

int VIDCSVdp2Reset(void)
{
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSVdp2Draw(void)
{
  YglCheckFBSwitch(1);
  //varVdp2Regs = Vdp2RestoreRegs(0, Vdp2Lines);
  //if (varVdp2Regs == NULL) varVdp2Regs = Vdp2Regs;
  Vdp2SetResolution(Vdp2Lines[0].TVMD);
  if (_Ygl->rwidth > YglTM_vdp2->width) {
    int new_width = _Ygl->rwidth;
    int new_height = YglTM_vdp2->height;
    YglTMDeInit(&YglTM_vdp2);
    YglTM_vdp2 = YglTMInit(new_width, new_height);
  }
  YglTmPull(YglTM_vdp2, 0);

  if (Vdp2Lines[0].TVMD & 0x8000) {

    VIDCSVdp2DrawScreens();
    screenDirty = 1;
    vdp2busy = 1;
  } else {
    VIDCSVdp2DispOff();
    if (screenDirty != 0)
      vdp2busy = 1;
    screenDirty = 0;
  }

  //Vdp1External.manualchange = 0;
}

//////////////////////////////////////////////////////////////////////////////

#define VDP2_DRAW_LINE 0
extern u8 Vdp2Ram_Updated;
static void VIDCSVdp2DrawScreens(void)
{
  u64 before;
  u64 now;
  u32 difftime;
  char str[64];
LOG_ASYN("===================================\n");

  _Ygl->useLineColorOffset[0] = 0;
  _Ygl->useLineColorOffset[1] = 0;

  Vdp2GenerateWindowInfo(&Vdp2Lines[VDP2_DRAW_LINE]);

	const int vdp1_tvm = Vdp1Regs->TVMR & 0x7;
	if (vdp1_tvm == 2 || vdp1_tvm == 3) {
		Vdp2ReadRotationTable(0, &Vdp1ParaA, &Vdp2Lines[VDP2_DRAW_LINE], Vdp2Ram);
	}
  Vdp2DrawBackScreen(&Vdp2Lines[VDP2_DRAW_LINE]);
  Vdp2DrawLineColorScreen(&Vdp2Lines[VDP2_DRAW_LINE]);

  Vdp2DrawRBG0();
  Vdp2DrawNBG3_zones();
  Vdp2DrawNBG2_zones();
  Vdp2DrawNBG1_zones();
  Vdp2DrawNBG0_zones();
  Vdp2DrawRBG1();

LOG_ASYN("*********************************\n");

  Vdp2Ram_Updated = 0;
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2SetResolution(u16 TVMD)
{
  int width = 1, height = 1;
  int wratio = 1, hratio = 1;
  InterlaceMode old_simple_interlace = _Ygl->interlace;

  // Horizontal Resolution
  switch (TVMD & 0x7)
  {
  case 0:
    width = 320;
    wratio = 1;
    break;
  case 1:
    width = 352;
    wratio = 1;
    break;
  case 2:
    width = 640;
    wratio = 2;
    break;
  case 3:
    width = 704;
      wratio = 2;
    break;
  case 4:
    width = 320;
    wratio = 1;
    break;
  case 5:
    width = 352;
    wratio = 1;
    break;
  case 6:
    width = 640;
    wratio = 2;
    break;
  case 7:
    width = 704;
    wratio = 2;
    break;
  }

	switch ((TVMD >> 4) & 0x3)
	{
	case 0:
	  height = 224;
	  break;
	case 1:
		height = 240;
		break;
	case 2:
	  height = 256;
	  break;
	case 3:
	  height = yabsys.IsPal ? 256 : 224;
	  break;
	}

  if (TVMD & 0x4) {
    height = 480;
  }

  hratio = 1;

  _Ygl->interlace = NORMAL_INTERLACE;
  // Check for interlace
  switch ((TVMD >> 6) & 0x3)
  {
  case 3: // Double-density Interlace
    hratio = 2;
    _Ygl->interlace = DOUBLE_INTERLACE;
    break;
  case 2: // Single-density Interlace
    _Ygl->interlace = SINGLE_INTERLACE;
    break;
  case 0: // Non-interlace
  default:
    break;
  }

  float oldvdp1wd = _Ygl->vdp1wdensity;
  float oldvdp1hd = _Ygl->vdp1hdensity;

  float oldvdp2wd = _Ygl->vdp2wdensity;
  float oldvdp2hd = _Ygl->vdp2hdensity;

  Vdp1SetTextureRatio(wratio, hratio);

  int change = 0;
  change |= (old_simple_interlace != _Ygl->interlace);
  change |= (oldvdp1wd != _Ygl->vdp1wdensity);
  change |= (oldvdp1hd != _Ygl->vdp1hdensity);
  change |= (oldvdp2wd != _Ygl->vdp2wdensity);
  change |= (oldvdp2hd != _Ygl->vdp2hdensity);

  change |= (width != _Ygl->rwidth);
  if (_Ygl->interlace == DOUBLE_INTERLACE) {
    change |= ((height*2) != _Ygl->rheight);
  } else {
    change |= (height != _Ygl->rheight);
  }
  if (change != 0)YglChangeResolution(width, height);
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSGetGlSize(int *width, int *height)
{
  *width = GlWidth;
  *height = GlHeight;
}

void VIDCSVdp2DispOff()
{
    // TVMD bit 15 = 0 : forcer le background à noir
    YglSetBackColor(0.0f, 0.0f, 0.0f);
}

void updateMeshMode(MESHMODE value) {
  if (_Ygl->meshmode != value){
    _Ygl->meshmode = value;
    vdp1_update_mesh();
  }
}
void updateBandingMode(BANDINGMODE value) {
  if (_Ygl->bandingmode != value){
    _Ygl->bandingmode = value;
    vdp1_update_banding();
  }
}
void VIDCSSetSettingValueMode(int type, int value) {

  switch (type) {
  case VDP_SETTING_FILTERMODE:
    _Ygl->aamode = (AAMODE)value;
    break;
  case VDP_SETTING_UPSCALMODE:
    _Ygl->upmode = (UPMODE)value;
    break;
  case VDP_SETTING_RESOLUTION_MODE:
    if (_Ygl->resolution_mode != (RESOLUTION_MODE)value) {
       _Ygl->resolution_mode = (RESOLUTION_MODE)value;
       YglChangeResolution(_Ygl->rwidth, _Ygl->rheight);
    }
    break;
  case VDP_SETTING_ASPECT_RATIO:
    _Ygl->stretch = (RATIOMODE)value;
  break;
  case VDP_SETTING_WIREFRAME:
    _Ygl->wireframe_mode = value;
  break;
  case VDP_SETTING_MESH_MODE:
    updateMeshMode((MESHMODE)value);
  break;
  case VDP_SETTING_BANDING_MODE:
    updateBandingMode((BANDINGMODE)value);
  break;
  default:
  return;
  }
}

//////////////////////////////////////////////////////////////////////////////
static void Vdp1SetTextureRatio(int vdp2widthratio, int vdp2heightratio)
{
  int vdp1w = 1;
  int vdp1h = 1;

  if (Vdp1Regs->TVMR & 0x1) VDP1_MASK = 0xFF;     /* 8 bpp pixel mask  */
  else VDP1_MASK = 0xFFFF;                        /* 16 bpp pixel mask */

  /* Figure out which vdp1 screen mode to use */
  switch (Vdp1Regs->TVMR & 7)
  {
  case 0: /* Normal NTSC/PAL  - 512 H x 256 V */
  case 2: /* Rotation 16      - 512 H x 256 V */
  case 4: /* HDTV / 31KC      - 512 H x 256 V */
    vdp1w = 1;
    vdp1h = 1;
    break;
  case 1: /* High Resolution  - 1024 H x 256 V */
    vdp1w = 2;
    vdp1h = 1;
    break;
  case 3:
    vdp1w = 1;
    vdp1h = 2;
    break;
  default:
    vdp1w = 1;
    vdp1h = 1;
    break;
  }

  const int tvm = Vdp1Regs->TVMR & 0x7;
  const int die_legal = (tvm == 0) || (tvm == 1);
  if (die_legal && (Vdp1Regs->FBCR & 0x8)) {
    vdp1h = 2;
    vdp1_interlace = (Vdp1Regs->FBCR & 0x4) ? 2 : 1;
  } else {
    vdp1_interlace = 0;
  }

  _Ygl->vdp1wdensity = vdp1w;
  _Ygl->vdp1hdensity = vdp1h;

  _Ygl->vdp2wdensity = vdp2widthratio;
  _Ygl->vdp2hdensity = vdp2heightratio;
}

//////////////////////////////////////////////////////////////////////////////
static u16 Vdp2ColorRamGetColorRaw(u32 colorindex) {
  switch (Vdp2Internal.ColorMode)
  {
  case 0:
    colorindex &= 0x3FF; // mirror: N and N+1024 are identical
    colorindex <<= 1;
    return T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
  case 1:
  {
    colorindex <<= 1;
    return T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
  }
  case 2:
  {
    colorindex <<= 2;
    colorindex &= 0xFFF;
    return T2ReadWord(Vdp2ColorRam, colorindex);
  }
  default: break;
  }
  return 0;
}

static u32 Vdp2ColorRamGetLineColorOffset(u32 colorindex, int alpha, int offset)
{
  int flag = 0xFFF;
  switch (Vdp2Internal.ColorMode)
  {
  case 0:
    flag &= 0x380;
  case 1:
  {
    u32 tmp;
    flag &= 0x780;
    if (offset != 0) colorindex = (colorindex&flag) | (offset&0x7F);
    colorindex <<= 1;
    tmp = T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
    return SAT2YAB1(alpha, tmp);
  }
  case 2:
  {
    u32 tmp1, tmp2;
    colorindex <<= 2;
    colorindex &= 0xFFF;
    tmp1 = T2ReadWord(Vdp2ColorRam, colorindex);          // mot haut : MSB + R
    tmp2 = T2ReadWord(Vdp2ColorRam, (colorindex + 2) & 0xFFF); // mot bas : G + B
    return SAT2YAB2(alpha, tmp1, tmp2);
  }
  default: break;
  }
  return 0;
}

static u32 Vdp2ColorRamGetLineColor(u32 colorindex, int alpha) {
  return Vdp2ColorRamGetLineColorOffset(colorindex, alpha,0);
}

static INLINE int Vdp2IsNormalShadow(u32 cramindex, int sptype) {
		u32 dc_mask;
		u32 dc_bits = cramindex; /* cramindex IS the dot color data for palette types */

		switch (sptype) {
		case 0: case 1: case 2: case 3: case 5:
			dc_mask = 0x7FF; /* 11 DC bits */
			break;
		case 4: case 6:
			dc_mask = 0x3FF; /* 10 DC bits */
			break;
		case 7:
			dc_mask = 0x1FF; /* 9 DC bits */
			break;
		case 8: /* type 8: 7 DC bits */
			dc_mask = 0x7F;
			break;
		case 9: case 10: case 11: /* 9, A, B : 6 DC bits (DC5~DC0) */
			dc_mask = 0x3F;
			break;
		case 12: case 13: case 14: case 15:
			dc_mask = 0xFF;
			break;
		default:
			return 0;
		}

		u32 shadow_val = dc_mask & ~1u; /* e.g. 0x7FE for 11-bit */
		return (dc_bits & dc_mask) == shadow_val;
	}
	
static INLINE int Vdp2GetSpriteShadowBit(u16 sprite_word, int sptype, int spwinen) {
	  if (spwinen)                    return 0;  /* bit 15 = sprite window */
	  if (sptype >= 2 && sptype <= 7) return (sprite_word >> 15) & 1;
	  return 0;
	}
	
static INLINE int Vdp2GetSpriteCCEnable(int priority_number, int color_data_msb, int spcccs, int spccn, int spccen) {
	  if (!spccen) return 0;
	  switch (spcccs) {
	  case 0: return (priority_number <= spccn);
	  case 1: return (priority_number == spccn);
	  case 2: return (priority_number >= spccn);
	  case 3: return (color_data_msb != 0);
	  default: return 0;
	  }
	}

static INLINE u32 Vdp2ApplyMSBShadow(u32 scroll_rgb32) {
	  u8 r = ((scroll_rgb32 >> 16) & 0xFF) >> 1;
	  u8 g = ((scroll_rgb32 >>  8) & 0xFF) >> 1;
	  u8 b = ((scroll_rgb32      ) & 0xFF) >> 1;
	  return (scroll_rgb32 & 0xFF000000) | ((u32)r << 16) | ((u32)g << 8) | b;
	}

//////////////////////////////////////////////////////////////////////////////
// Window
static int useRotWin = 0;
int WinS[enBGMAX+1];
int WinS_mode[enBGMAX+1];

void Vdp2GenerateWindowInfo(Vdp2 *varVdp2Regs)
{
  int HShift;
  int v = 0;
  int p = 0;
  u32 LineWinAddr;
  int upWindow = 0;
  u32 val = 0;

  int Win0[enBGMAX+1];
  int Win0_mode[enBGMAX+1];
  int Win1[enBGMAX+1];
  int Win1_mode[enBGMAX+1];
  int Win_op[enBGMAX+1];

  if (((varVdp2Regs->WCTLD & 0xA)!=0x0) != useRotWin) {
    useRotWin = ((varVdp2Regs->WCTLD & 0xA)!=0x0);
    _Ygl->needWinUpdate |= 1;
  }

  Win0[NBG0] = (varVdp2Regs->WCTLA >> 1) & 0x01;
  Win1[NBG0] = (varVdp2Regs->WCTLA >> 3) & 0x01;
  WinS[NBG0] = (varVdp2Regs->WCTLA >> 5) & 0x01;
  Win0[NBG1] = (varVdp2Regs->WCTLA >> 9) & 0x01;
  Win1[NBG1] = (varVdp2Regs->WCTLA >> 11) & 0x01;
  WinS[NBG1] = (varVdp2Regs->WCTLA >> 13) & 0x01;

  Win0[NBG2] = (varVdp2Regs->WCTLB >> 1) & 0x01;
  Win1[NBG2] = (varVdp2Regs->WCTLB >> 3) & 0x01;
  WinS[NBG2] = (varVdp2Regs->WCTLB >> 5) & 0x01;
  Win0[NBG3] = (varVdp2Regs->WCTLB >> 9) & 0x01;
  Win1[NBG3] = (varVdp2Regs->WCTLB >> 11) & 0x01;
  WinS[NBG3] = (varVdp2Regs->WCTLB >> 13) & 0x01;

  Win0[RBG0] = (varVdp2Regs->WCTLC >> 1) & 0x01;
  Win1[RBG0] = (varVdp2Regs->WCTLC >> 3) & 0x01;
  WinS[RBG0] = (varVdp2Regs->WCTLC >> 5) & 0x01;
  Win0[SPRITE] = (varVdp2Regs->WCTLC >> 9) & 0x01;
  Win1[SPRITE] = (varVdp2Regs->WCTLC >> 11) & 0x01;
  WinS[SPRITE] = (varVdp2Regs->WCTLC >> 13) & 0x01;

  Win0_mode[NBG0] = (varVdp2Regs->WCTLA) & 0x01;
  Win1_mode[NBG0] = (varVdp2Regs->WCTLA >> 2) & 0x01;
  WinS_mode[NBG0] = (varVdp2Regs->WCTLA >> 4) & 0x01;
  Win0_mode[NBG1] = (varVdp2Regs->WCTLA >> 8) & 0x01;
  Win1_mode[NBG1] = (varVdp2Regs->WCTLA >> 10) & 0x01;
  WinS_mode[NBG1] = (varVdp2Regs->WCTLA >> 12) & 0x01;

  Win0_mode[NBG2] = (varVdp2Regs->WCTLB) & 0x01;
  Win1_mode[NBG2] = (varVdp2Regs->WCTLB >> 2) & 0x01;
  WinS_mode[NBG2] = (varVdp2Regs->WCTLB >> 4) & 0x01;
  Win0_mode[NBG3] = (varVdp2Regs->WCTLB >> 8) & 0x01;
  Win1_mode[NBG3] = (varVdp2Regs->WCTLB >> 10) & 0x01;
  WinS_mode[NBG3] = (varVdp2Regs->WCTLB >> 12) & 0x01;

  Win0_mode[RBG0] = (varVdp2Regs->WCTLC) & 0x01;
  Win1_mode[RBG0] = (varVdp2Regs->WCTLC >> 2) & 0x01;
  WinS_mode[RBG0] = (varVdp2Regs->WCTLC >> 4) & 0x01;
  Win0_mode[SPRITE] = (varVdp2Regs->WCTLC >> 8) & 0x01;
  Win1_mode[SPRITE] = (varVdp2Regs->WCTLC >> 10) & 0x01;
  WinS_mode[SPRITE] = (varVdp2Regs->WCTLC >> 12) & 0x01;

  Win_op[NBG0] = (varVdp2Regs->WCTLA >> 7) & 0x01;  // correct: bit 7
  Win_op[NBG1] = (varVdp2Regs->WCTLA >> 15) & 0x01; // correct: bit 15
  Win_op[NBG2] = (varVdp2Regs->WCTLB >> 7) & 0x01;
  Win_op[NBG3] = (varVdp2Regs->WCTLB >> 15) & 0x01;
  Win_op[RBG0] = (varVdp2Regs->WCTLC >> 7) & 0x01;
  Win_op[SPRITE] = (varVdp2Regs->WCTLC >> 15) & 0x01;

  Win0_mode[SPRITE+1] = (varVdp2Regs->WCTLD >> 8) & 0x01;
  Win0[SPRITE+1] = (varVdp2Regs->WCTLD >> 9) & 0x01;
  Win1_mode[SPRITE+1] = (varVdp2Regs->WCTLD >> 10) & 0x01;
  Win1[SPRITE+1] = (varVdp2Regs->WCTLD >> 11) & 0x01;
  WinS_mode[SPRITE+1] = (varVdp2Regs->WCTLD >> 12) & 0x01;
  WinS[SPRITE+1] = (varVdp2Regs->WCTLD >> 13) & 0x01;
  Win_op[SPRITE+1] = (varVdp2Regs->WCTLD >> 15) & 0x01;

  Win0[RBG1] = Win0[NBG0];
  Win0_mode[RBG1] = Win0_mode[NBG0];
  Win1[RBG1] = Win1[NBG0];
  Win1_mode[RBG1] = Win1_mode[NBG0];
  WinS[RBG1] = WinS[NBG0];
  WinS_mode[RBG1] = WinS_mode[NBG0];
  Win_op[RBG1] = Win_op[NBG0];

  {
    int sptype  =  varVdp2Regs->SPCTL & 0xF;
    int spwinen = (varVdp2Regs->SPCTL >> 4) & 1;
    int spclmd  = (varVdp2Regs->SPCTL >> 5) & 1;
    int sw_available = spwinen && !spclmd && (sptype >= 2) && (sptype <= 7);
    if (!sw_available)
      for (int i = 0; i < enBGMAX+1; i++) WinS[i] = 0;
  }


  for (int i=0; i<enBGMAX+1; i++) {
    if (Win0[i] != _Ygl->Win0[i]) _Ygl->needWinUpdate |= 1;
    if (Win1[i] != _Ygl->Win1[i]) _Ygl->needWinUpdate |= 1;
    if (WinS[i] != _Ygl->WinS[i]) _Ygl->needWinUpdate |= 1;
    if (Win0_mode[i] != _Ygl->Win0_mode[i]) _Ygl->needWinUpdate |= 1;
    if (Win1_mode[i] != _Ygl->Win1_mode[i]) _Ygl->needWinUpdate |= 1;
    if (WinS_mode[i] != _Ygl->WinS_mode[i]) _Ygl->needWinUpdate |= 1;
    if (Win_op[i] != _Ygl->Win_op[i]) _Ygl->needWinUpdate |= 1;
  #ifdef WINDOW_DEBUG
    if ((Win0[i] == 1) || (Win1[i] == 1) || (WinS[i] == 1))
      YuiMsg("Windows are used on layer %d (WO:%d, W1:%d, WS:%d, WS mode %s, WS op %s)\n", i, Win0[i], Win1[i], WinS[i], (WinS_mode[i]==0)?"INSIDE":"OUTSIDE", (Win_op[i]==0)?"OR":"AND");
    else
      YuiMsg("Windows are not used on layer %d\n", i);
  #endif
  }
  memcpy(&_Ygl->Win0[0], &Win0[0], (enBGMAX+1)*sizeof(int));
  memcpy(&_Ygl->Win1[0], &Win1[0], (enBGMAX+1)*sizeof(int));
  memcpy(&_Ygl->WinS[0], &WinS[0], (enBGMAX+1)*sizeof(int));
  memcpy(&_Ygl->Win0_mode[0], &Win0_mode[0], (enBGMAX+1)*sizeof(int));
  memcpy(&_Ygl->Win1_mode[0], &Win1_mode[0], (enBGMAX+1)*sizeof(int));
  memcpy(&_Ygl->WinS_mode[0], &WinS_mode[0], (enBGMAX+1)*sizeof(int));
  memcpy(&_Ygl->Win_op[0], &Win_op[0], (enBGMAX+1)*sizeof(int));

  if( _Ygl->win[0] == NULL ){
    _Ygl->win[0] = (u32*)malloc(512 * 4);
  }
  if( _Ygl->win[1] == NULL ){
    _Ygl->win[1] = (u32*)malloc(512 * 4);
  }

  HShift = 0;
  if (_Ygl->rwidth >= 640) HShift = 0; else HShift = 1;

  int step = 1;
  // Line Table mode
  if ((varVdp2Regs->LWTA0.part.U & 0x8000))
  {
    LineWinAddr = (u32)((((varVdp2Regs->LWTA0.part.U & 0x07) << 15) | (varVdp2Regs->LWTA0.part.L >> 1)) << 2);
    for (v = 0,p=0; p < _Ygl->rheight; v+=step, p++) {
      if (v >= varVdp2Regs->WPSY0 && v <= varVdp2Regs->WPEY0) {
        short HStart = Vdp2RamReadWord(NULL, Vdp2Ram, LineWinAddr + (v << 2)) & 0xFFFF;
        short HEnd = Vdp2RamReadWord(NULL, Vdp2Ram, LineWinAddr + (v << 2) + 2) & 0xFFFF;
        if ((HEnd < HStart) || (HEnd < 0)) val = 0x000000FF; //END < START
        else {
          val = (HStart>>HShift) | ((HEnd>>HShift) << 16);
        }
      } else {
        val = 0x000000FF; //END < START
      }
      if (val != _Ygl->win[0][p]) {
        _Ygl->win[0][p] = val;
        _Ygl->needWinUpdate = 1;
      }
    }
  }
  else {
    for (v = 0,p=0; p < _Ygl->rheight; v+=step, p++) {
      if (v >= varVdp2Regs->WPSY0 && v <= varVdp2Regs->WPEY0) {
        if (((short)varVdp2Regs->WPEY0 < (short)varVdp2Regs->WPSY0)  || ((short)varVdp2Regs->WPEY0 < 0)) val = 0x000000FF; //END < START
        else {
          val = (varVdp2Regs->WPSX0 >>HShift) | ((varVdp2Regs->WPEX0>>HShift) << 16);
        }
      } else {
        val = 0x000000FF; //END > START
      }
      if (val != _Ygl->win[0][p]) {
        _Ygl->win[0][p] = val;
        _Ygl->needWinUpdate = 1;
      }
    }
  }
  // Line Table mode
  if ((varVdp2Regs->LWTA1.part.U & 0x8000))
  {
    LineWinAddr = (u32)((((varVdp2Regs->LWTA1.part.U & 0x07) << 15) | (varVdp2Regs->LWTA1.part.L >> 1)) << 2);
    for (v = 0,p=0; p < _Ygl->rheight; v+=step, p++) {
      if (v >= varVdp2Regs->WPSY1 && v <= varVdp2Regs->WPEY1) {
        short HStart = Vdp2RamReadWord(NULL, Vdp2Ram, LineWinAddr + (v << 2)) & 0xFFFF;
        short HEnd = Vdp2RamReadWord(NULL, Vdp2Ram, LineWinAddr + (v << 2) + 2) & 0xFFFF;
        if ((HEnd < HStart) || (HEnd < 0)) val = 0x000000FF; //END < START
        else {
          val = (HStart>>HShift) | ((HEnd>>HShift) << 16);
        }
      } else {
        val = 0x000000FF; //END < START
      }
      if (val != _Ygl->win[1][p]) {
        _Ygl->win[1][p] = val;
        _Ygl->needWinUpdate = 1;
      }
    }
  }
  else {
    for (v = 0,p=0; p < _Ygl->rheight; v+=step, p++) {
      if (v >= varVdp2Regs->WPSY1 && v <= varVdp2Regs->WPEY1) {
        if (((short)varVdp2Regs->WPEY1 < (short)varVdp2Regs->WPSY1) || ((short)varVdp2Regs->WPEY1 < 0)) val = 0x000000FF; //END < START
        else {
          val = (varVdp2Regs->WPSX1 >>HShift) | ((varVdp2Regs->WPEX1>>HShift) << 16);
        }
      } else {
        val = 0x000000FF; //END < START
      }
      if (val != _Ygl->win[1][p]) {
        _Ygl->win[1][p] = val;
        _Ygl->needWinUpdate = 1;
      }
    }
  }
}

// 0 .. outside,1 .. inside
static INLINE int Vdp2CheckWindow(vdp2draw_struct *info, int x, int y, int area, u32* win)
{
  if (y < 0) return 0;

  if (y >= _Ygl->rheight) return 0;
  int upLx = win[y] & 0xFFFF;
  int upRx = (win[y] >> 16) & 0xFFFF;
  // inside
  if (area == 1)
  {
    if (win[y] == 0) return 0;
    if (x >= upLx && x <= upRx)
    {
      return 1;
    }
    else {
      return 0;
    }
  }
  else {
    if (win[y] == 0) return 1;
    if (x < upLx) return 1;
    if (x > upRx) return 1;
    return 0;
  }
  return 0;
}

static INLINE int Vdp2CheckWindowLine(vdp2draw_struct *info,
                                       int x, int ly, int w,
                                       int area, u32 *win)
{
  if (ly < 0 || ly >= _Ygl->rheight) return 0;

  int wl = win[ly] & 0xFFFF;
  int wr = (win[ly] >> 16) & 0xFFFF;

  if (win[ly] == 0x000000FF) {
    return (area == WA_OUTSIDE) ? 1 : 0;
  }

  if (area == WA_INSIDE) {
    return (x <= wr && (x + w - 1) >= wl);
  } else {
    return (x < wl || (x + w - 1) > wr);
  }
}


static INLINE int Vdp2CheckSpriteWindow(int id, int vdp2x, int vdp2y)
{
    if (_Ygl->WinS[id] == 0) return 0;

    float ratioX = _Ygl->vdp1ratio * _Ygl->vdp1wdensity / _Ygl->vdp2wdensity
                   * (float)_Ygl->rwidth  / (float)_Ygl->vdp1width;
    float ratioY = _Ygl->vdp1ratio * _Ygl->vdp1hdensity / _Ygl->vdp2hdensity
                   * (float)_Ygl->rheight / (float)_Ygl->vdp1height;

    int fbx = (int)(vdp2x * ratioX);
    int fby = (int)(vdp2y * ratioY);

    if (fbx < 0) fbx = 0;
    if (fby < 0) fby = 0;
    if (fbx >= _Ygl->vdp1width)  fbx = _Ygl->vdp1width  - 1;
    if (fby >= _Ygl->vdp1height) fby = _Ygl->vdp1height - 1;

    u32 *fb = _Ygl->vdp1fb_read_buf[_Ygl->readframe];
    if (fb == NULL) return 0;

    u32 pixel = fb[fby * _Ygl->vdp1width + fbx];
    u8 g = (pixel >> 8) & 0xFF;
    int msb = (g >> 7) & 1;   /* bit 15 of the original VDP1 color */

    int inside = msb;

    if (_Ygl->WinS_mode[id] == WA_INSIDE) {
        return inside;
    } else {
        return !inside;
    }
}


static int FASTCALL Vdp2CheckWindowRange(Vdp2Ctrl *ctrl, int x, int y, int w, int h)
{
    int id = ctrl->info.idScreen;
    int useW0 = (_Ygl->Win0[id] != 0);
    int useW1 = (_Ygl->Win1[id] != 0);
    int useWS = (_Ygl->WinS[id] != 0);

    if (!useW0 && !useW1 && !useWS) return 0;

    int use_and = (_Ygl->Win_op[id] != 0);

    for (int ly = y; ly < y + h; ly++) {
        int test_xs[2] = { x, x + w };
        for (int ei = 0; ei < 2; ei++) {
            int cx = test_xs[ei];
            int result;

            if (!use_and) {
                result = 0;
                if (useW0) result |= Vdp2CheckWindow(&ctrl->info, cx, ly,
                                                      _Ygl->Win0_mode[id], _Ygl->win[0]);
                if (useW1) result |= Vdp2CheckWindow(&ctrl->info, cx, ly,
                                                      _Ygl->Win1_mode[id], _Ygl->win[1]);
                if (useWS) result |= Vdp2CheckSpriteWindow(id, cx, ly);
            } else {
                result = 1;
                if (useW0) result &= Vdp2CheckWindow(&ctrl->info, cx, ly,
                                                      _Ygl->Win0_mode[id], _Ygl->win[0]);
                if (useW1) result &= Vdp2CheckWindow(&ctrl->info, cx, ly,
                                                      _Ygl->Win1_mode[id], _Ygl->win[1]);
                if (useWS) result &= Vdp2CheckSpriteWindow(id, cx, ly);
            }

            if (result) return 1;
        }
    }
    return 0;
}

static void Vdp2GenLineinfo(vdp2draw_struct *info)
{
    int bound = 0, i;
    u16 val1;
    if (info->lineinc == 0 || info->islinescroll == 0) return;

    if (VDPLINE_SX(info->islinescroll)) bound += 4;
    if (VDPLINE_SY(info->islinescroll)) bound += 4;
    if (VDPLINE_SZ(info->islinescroll)) bound += 4;

    int height = _Ygl->rheight;

    if (height > 512) height = 512;

    int delta_y_q8 = 0x100;   /* 1.0 par defaut */
    if (info->coordincy_raw != 0)
        delta_y_q8 = (int)(info->coordincy_raw >> 8);

    s16 last_sh = 0, last_sv = 0;
    int last_inc = 0x0100;

    for (i = 0; i < height; i++) {
        int sub_line    = i % info->lineinc;
        int table_entry = i / info->lineinc;
        int byte_offset = table_entry * bound;
        int field_off   = 0;

        if (sub_line == 0) {
            if (VDPLINE_SX(info->islinescroll)) {
                val1 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off);
                s32 ival = (s32)(val1 & 0x07FF);
                if (val1 & 0x0400) ival -= 0x0800;   /* sign-extend bit 10 */
                last_sh = (s16)ival;
                field_off += 4;
            } else {
                last_sh = 0;
            }

            if (VDPLINE_SY(info->islinescroll)) {
                val1 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off);
                s32 ival = (s32)(val1 & 0x07FF);
                if (val1 & 0x0400) ival -= 0x0800;   /* sign-extend bit 10 */
                last_sv = (s16)ival;
                field_off += 4;
            } else {
                last_sv = 0;
            }

            if (VDPLINE_SZ(info->islinescroll)) {
                u16 z1 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off);
                u16 z2 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off + 2);
                last_inc = ((int)(z1 & 0x07) << 8) | (int)(z2 >> 8);
            } else {
                last_inc = 0x0100;
            }
        }

        info->lineinfo[i].LineScrollValH = last_sh;
        info->lineinfo[i].CoordinateIncH = last_inc;

        if (VDPLINE_SY(info->islinescroll)) {
            s32 v = (s32)last_sv + ((sub_line * delta_y_q8) >> 8);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            info->lineinfo[i].LineScrollValV = (s16)v;
        } else {
            info->lineinfo[i].LineScrollValV = 0;
        }
    }
}

INLINE void Vdp2SetSpecialPriority(vdp2draw_struct *info, u8 dot, u32 *prio, u32 * cramindex ) {
  *prio = info->priority;
  if (info->specialprimode == 2) {
    *prio = info->priority & 0xE;
    if (info->specialfunction & 1) {
      if (PixelIsSpecialPriority(info->specialcode, dot))
      {
        *prio |= 1;
      }
    }
  }
}

static INLINE int Vdp2CheckCCWindow(int x, int y) {
		int idx = SPRITE + 1;
		if (_Ygl->Win0[idx] == 0 && _Ygl->Win1[idx] == 0) return 1; // no CCW → CC active everywhere

		  int have_w0 = (_Ygl->Win0[idx] != 0);
		  int have_w1 = (_Ygl->Win1[idx] != 0);
		  vdp2draw_struct dummy = {0};
		  int w0 = have_w0 ? Vdp2CheckWindow(&dummy, x, y,
					   (_Ygl->Win0_mode[idx] == WA_INSIDE) ? 1 : 0, _Ygl->win[0]) : 0;
		  int w1 = have_w1 ? Vdp2CheckWindow(&dummy, x, y,
					   (_Ygl->Win1_mode[idx] == WA_INSIDE) ? 1 : 0, _Ygl->win[1]) : 0;
		  int in_ccw;
		  if (_Ygl->Win_op[idx] == 0) {   /* OR */
			in_ccw = (have_w0 ? w0 : 0) | (have_w1 ? w1 : 0);
		  } else {                         /* AND */
			if (have_w0 && have_w1) in_ccw = w0 & w1;
			else if (have_w0)        in_ccw = w0;
			else                     in_ccw = w1;
		  }
		  return !in_ccw;
	}

static INLINE u32 Vdp2GetCCOn(Vdp2Ctrl *ctrl, u8 dot, u32 cramindex) {
  int cc = 1;
  switch (ctrl->info.specialcolormode) {
  case 0: /* always CC */ break;
  case 1:
    if (ctrl->info.specialcolorfunction == 0) cc = 0;
    break;
  case 2:
    if (ctrl->info.specialcolorfunction == 0) {
      cc = 0;
    } else if ((ctrl->info.specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
      cc = 0;
    }
    break;
  case 3:
    if (ctrl->info.colornumber < 3) {
      if ((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0) cc = 0;
    }
    break;
  }
  return cc;
}


static INLINE u32 Vdp2GetPixel4bpp(Vdp2Ctrl *ctrl, u32 addr) {

  u32 cramindex;
  u16 dotw = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
  u8 dot;
  u32 cc;
  u32 priority = 0;

  dot = (dotw & 0xF000) >> 12;
  if (!(dot & 0xF) && ctrl->info.transparencyenable) {
    *ctrl->texture.textdata++ = 0x00000000;
  } else {
    cramindex = (ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xF)));
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }

  cc = 1;
  dot = (dotw & 0xF00) >> 8;
  if (!(dot & 0xF) && ctrl->info.transparencyenable) {
    *ctrl->texture.textdata++ = 0x00000000;
  }
  else {
    cramindex = (ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xF)));
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }

  cc = 1;
  dot = (dotw & 0xF0) >> 4;
  if (!(dot & 0xF) && ctrl->info.transparencyenable) {
    *ctrl->texture.textdata++ = 0x00000000;
  }
  else {
    cramindex = (ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xF)));
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }

  cc = 1;
  dot = (dotw & 0xF);
  if (!(dot & 0xF) && ctrl->info.transparencyenable) {
    *ctrl->texture.textdata++ = 0x00000000;
  }
  else {
    cramindex = (ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xF)));
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }
  return 0;
}

static INLINE u32 Vdp2GetPixel8bpp(Vdp2Ctrl *ctrl, u32 addr) {

  u32 cramindex;
  u16 dotw = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
  u8 dot;
  u32 cc;
  u32 priority = 0;

  cc = 1;
  dot = (dotw & 0xFF00)>>8;
  if (!(dot & 0xFF) && ctrl->info.transparencyenable) *ctrl->texture.textdata++ = 0x00000000;
  else {
    cramindex = ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xFF));
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }
  cc = 1;
  dot = (dotw & 0xFF);
  if (!(dot & 0xFF) && ctrl->info.transparencyenable) *ctrl->texture.textdata++ = 0x00000000;
  else {
    cramindex = ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xFF));
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }
  return 0;
}


static INLINE u32 Vdp2GetPixel16bpp(Vdp2Ctrl *ctrl, u32 addr) {
  u32 cramindex;
  u8 cc;
  u16 dot = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
  u32 priority = 0;
  if ((dot == 0) && ctrl->info.transparencyenable) return 0x00000000;
  else {
    cramindex = ctrl->info.coloroffset + dot;
    Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
    cc = Vdp2GetCCOn(ctrl, dot, cramindex);
    return VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, priority, cc, cramindex);
  }
}

static INLINE u32 Vdp2GetPixel16bppbmp(Vdp2Ctrl *ctrl, u32 addr) {
  u32 color;
  u16 dot = Vdp2RamReadWord(NULL, Vdp2Ram, addr);

  if (!(dot & 0x8000) && ctrl->info.transparencyenable) return 0x00000000;

  int cc;
  if (ctrl->info.specialcolormode == 3) {
    cc = (dot & 0x8000) ? 1 : 0;
  } else {
    cc = Vdp2GetCCOn(ctrl, (u8)(dot & 0xF), 0);
  }
  color = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha,
                    ctrl->info.priority, cc, RGB555_TO_RGB24(dot));
  return color;
}

static INLINE u32 Vdp2GetPixel32bppbmp(Vdp2Ctrl *ctrl, u32 addr) {
  u32 color;
  u16 dot1, dot2;
  int cc;
  dot1 = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
  dot2 = Vdp2RamReadWord(NULL, Vdp2Ram, addr+2);

  cc = Vdp2GetCCOn(ctrl, 0, 0);
  
  if (!(dot1 & 0x8000) && ctrl->info.transparencyenable) return 0x00000000;

  if (ctrl->info.specialcolormode == 3) {
    cc = (dot1 & 0x8000) ? 1 : 0;
  } else {
    cc = Vdp2GetCCOn(ctrl, (u8)(dot2 & 0xF), 0);
  }

  color = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha, ctrl->info.priority,
                    cc,
                    (((u32)(dot1 & 0xFF) << 16) | ((u32)dot2 & 0xFF00) | ((u32)dot2 & 0xFF)));
	return color;
}

static u32 getAlpha(vdp2draw_struct *info, int id) {
    int shift = 0;
    if (_Ygl->interlace == DOUBLE_INTERLACE) shift = 1;
    int idx = info->draw_line + id;
    if (idx < 0) idx = 0;
    const int alpha_max = (yabsys.VBlankLineCount >= 270) ? 270 : yabsys.VBlankLineCount;
    int li = idx >> shift;
    if (li >= alpha_max) li = alpha_max - 1;
    return info->alpha_per_line[li];
}

static INLINE int isVramAccessible(Vdp2Ctrl *ctrl, u32 addr) {
    int vrama_split = (ctrl->regs->RAMCTL >> 8) & 0x1; /* §3.1 VRAMD */
    int vramb_split = (ctrl->regs->RAMCTL >> 9) & 0x1; /* §3.1 VRBMD */

    addr &= 0x7FFFF;

    int bank;
    if (addr < 0x40000) {
        if (vrama_split) {
            bank = (addr < 0x20000) ? 0 : 1;
        } else {
            bank = 0;
        }
    } else {
        if (vramb_split) {
            bank = (addr < 0x60000) ? 2 : 3;
        } else {
            bank = 2;
        }
    }

    if (bank > 3) return 0;
    return ctrl->info.char_bank[bank];
}

static void FASTCALL Vdp2DrawCell_in_sync(Vdp2Ctrl *ctrl)
{
  int i, j;
  u32 base      = ctrl->info.bitmap_base;
  u32 wrap_size = ctrl->info.bitmap_wrap_size;
  int is_bitmap = (ctrl->info.isbitmap != 0) && (wrap_size > 0);

#define BITMAP_ADDR(raw_addr) \
  ((is_bitmap && ((raw_addr) >= base + wrap_size)) \
   ? (base + ((raw_addr) - base) % wrap_size) \
   : (raw_addr))

#define BITMAP_ACCESSIBLE(raw_addr) \
  (!is_bitmap || isVramAccessible(ctrl, BITMAP_ADDR(raw_addr)))

  switch (ctrl->info.colornumber)
  {
  case 0: // 4 BPP
    for (i = 0; i < ctrl->info.cellh; i++) {
      ctrl->info.alpha = getAlpha(&ctrl->info, i);
      for (j = 0; j < ctrl->info.cellw; j += 4) {
        u32 eff_addr = BITMAP_ADDR(ctrl->info.charaddr);
        if (BITMAP_ACCESSIBLE(ctrl->info.charaddr)) {
          u32 save = ctrl->info.charaddr;
          ctrl->info.charaddr = eff_addr;
          Vdp2GetPixel4bpp(ctrl, ctrl->info.charaddr);
          ctrl->info.charaddr = save + 2;
        } else {
          *ctrl->texture.textdata++ = 0x00000000;
          *ctrl->texture.textdata++ = 0x00000000;
          *ctrl->texture.textdata++ = 0x00000000;
          *ctrl->texture.textdata++ = 0x00000000;
          ctrl->info.charaddr += 2;
        }
      }
      ctrl->texture.textdata += ctrl->texture.w;
    }
    break;
  case 1: // 8 BPP
    for (i = 0; i < ctrl->info.cellh; i++) {
      ctrl->info.alpha = getAlpha(&ctrl->info, i);
      for (j = 0; j < ctrl->info.cellw; j += 2) {
        u32 eff_addr = BITMAP_ADDR(ctrl->info.charaddr);
        if (BITMAP_ACCESSIBLE(ctrl->info.charaddr)) {
          u32 save = ctrl->info.charaddr;
          ctrl->info.charaddr = eff_addr;
          Vdp2GetPixel8bpp(ctrl, ctrl->info.charaddr);
          ctrl->info.charaddr = save + 2;
        } else {
          *ctrl->texture.textdata++ = 0x00000000;
          *ctrl->texture.textdata++ = 0x00000000;
          ctrl->info.charaddr += 2;
        }
      }
      ctrl->texture.textdata += ctrl->texture.w;
    }
    break;
  case 2: // 16 BPP(palette)
    for (i = 0; i < ctrl->info.cellh; i++) {
      ctrl->info.alpha = getAlpha(&ctrl->info, i);
      for (j = 0; j < ctrl->info.cellw; j++) {
        u32 eff_addr = BITMAP_ADDR(ctrl->info.charaddr);
        if (BITMAP_ACCESSIBLE(ctrl->info.charaddr)) {
          *ctrl->texture.textdata++ = Vdp2GetPixel16bpp(ctrl, eff_addr);
        } else {
          *ctrl->texture.textdata++ = 0x00000000;
        }
        ctrl->info.charaddr += 2;
      }
      ctrl->texture.textdata += ctrl->texture.w;
    }
    break;
  case 3: // 16 BPP(RGB)
    for (i = 0; i < ctrl->info.cellh; i++) {
      ctrl->info.alpha = getAlpha(&ctrl->info, i);
      for (j = 0; j < ctrl->info.cellw; j++) {
        u32 eff_addr = BITMAP_ADDR(ctrl->info.charaddr);
        if (BITMAP_ACCESSIBLE(ctrl->info.charaddr)) {
          *ctrl->texture.textdata++ = Vdp2GetPixel16bppbmp(ctrl, eff_addr);
        } else {
          *ctrl->texture.textdata++ = 0x00000000;
        }
        ctrl->info.charaddr += 2;
      }
      ctrl->texture.textdata += ctrl->texture.w;
    }
    break;
  case 4: // 32 BPP
    for (i = 0; i < ctrl->info.cellh; i++) {
      ctrl->info.alpha = getAlpha(&ctrl->info, i);
      for (j = 0; j < ctrl->info.cellw; j++) {
        u32 eff_addr = BITMAP_ADDR(ctrl->info.charaddr);
        if (BITMAP_ACCESSIBLE(ctrl->info.charaddr)) {
          *ctrl->texture.textdata++ = Vdp2GetPixel32bppbmp(ctrl, eff_addr);
        } else {
          *ctrl->texture.textdata++ = 0x00000000;
        }
        ctrl->info.charaddr += 4;
      }
      ctrl->texture.textdata += ctrl->texture.w;
    }
    break;
  }
#undef BITMAP_ADDR
#undef BITMAP_ACCESSIBLE
}

static void FASTCALL Vdp2DrawBitmapLineScroll(Vdp2Ctrl *ctrl, int width, int height)
{
  int i, j;
  int shift = 0;
  if (_Ygl->interlace == DOUBLE_INTERLACE) shift = 1;

  const int zone_start = ctrl->info.startLine;

  for (i = 0; i < height; i++)
  {
    int sh, sv;
    u32 baseaddr;
    vdp2Lineinfo * line;
    const int absline = zone_start + i;

    ctrl->info.draw_line = absline;
    ctrl->info.alpha = ctrl->info.alpha_per_line[absline >> shift];
    baseaddr = (u32)ctrl->info.charaddr;
    line = &(ctrl->info.lineinfo[absline]);

    if (VDPLINE_SX(ctrl->info.islinescroll))
      sh = line->LineScrollValH + ctrl->info.sh;
    else
      sh = ctrl->info.sh;

    if (VDPLINE_SY(ctrl->info.islinescroll))
      sv = line->LineScrollValV + ctrl->info.sv;
    else
        sv = absline + ctrl->info.sv;

    sv &= (ctrl->info.cellh - 1);
    sh &= (ctrl->info.cellw - 1);

    {
      const u32 bmpmask = (u32)(ctrl->info.cellw * ctrl->info.cellh) - 1;
      const u32 lin_base = (u32)sv * ctrl->info.cellw + (u32)sh;

      switch (ctrl->info.colornumber) {
      case 0: /* 4 bpp — §10.1 : pixel gauche = quartet haut */
        {
          for (j = 0; j < width; j++)
          {
            u32 p = (lin_base + (u32)j) & bmpmask;
            u8 dot = Vdp2RamReadByte(NULL, Vdp2Ram, baseaddr + (p >> 1));
            if (!(p & 0x01)) dot >>= 4;
            if (!(dot & 0xF) && ctrl->info.transparencyenable) {
              *ctrl->texture.textdata++ = 0x00000000;
            } else {
              u32 priority = 0;
              u32 cramindex = (ctrl->info.coloroffset +
                               ((ctrl->info.paladdr << 4) | (dot & 0xF)));
              Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
              u32 cc = Vdp2GetCCOn(ctrl, dot, cramindex);
              *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen,
                  ctrl->info.alpha, priority, cc, cramindex);
            }
          }
        }
        break;
      case 1: /* 8 bpp */
        {
          for (j = 0; j < width; j++)
          {
            u32 p = (lin_base + (u32)j) & bmpmask;
            u8 dot = Vdp2RamReadByte(NULL, Vdp2Ram, baseaddr + p);
            if (!(dot & 0xFF) && ctrl->info.transparencyenable) {
              *ctrl->texture.textdata++ = 0x00000000;
            } else {
              u32 priority = 0;
              u32 cramindex = ctrl->info.coloroffset +
                              ((ctrl->info.paladdr << 4) | (dot & 0xFF));
              Vdp2SetSpecialPriority(&ctrl->info, dot, &priority, &cramindex);
              u32 cc = Vdp2GetCCOn(ctrl, dot, cramindex);
              *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen,
                  ctrl->info.alpha, priority, cc, cramindex);
            }
          }
        }
        break;
      case 2: /* 16 bpp palette */
        {
          for (j = 0; j < width; j++)
          {
            u32 p = (lin_base + (u32)j) & bmpmask;
            *ctrl->texture.textdata++ =
                Vdp2GetPixel16bpp(ctrl, baseaddr + (p << 1));
          }
        }
        break;
      case 3: /* 16 bpp RGB */
        {
          for (j = 0; j < width; j++)
          {
            u32 p = (lin_base + (u32)j) & bmpmask;
            *ctrl->texture.textdata++ =
                Vdp2GetPixel16bppbmp(ctrl, baseaddr + (p << 1));
          }
        }
        break;
      case 4: /* 32 bpp */
        {
          for (j = 0; j < width; j++)
          {
            u32 p = (lin_base + (u32)j) & bmpmask;
            *ctrl->texture.textdata++ =
                Vdp2GetPixel32bppbmp(ctrl, baseaddr + (p << 2));
          }
        }
        break;
      }
    }

    ctrl->texture.textdata += ctrl->texture.w;
  }
}



static void FASTCALL Vdp2DrawBitmapCoordinateInc(Vdp2Ctrl *ctrl)
{
  u32 color;
  int i, j;
  int shift = 0;
  if (_Ygl->interlace == DOUBLE_INTERLACE) shift = 1;
 
  float incv_f = (1.0f / ctrl->info.coordincy) * 256.0f;
  float inch_f = (1.0f / ctrl->info.coordincx) * 256.0f;
  int incv = (int)(incv_f + 0.5f);
  int inch_base = (int)(inch_f + 0.5f);
 
  int screenY1 = (_Ygl->rheight * ctrl->info.startLine) / yabsys.VBlankLineCount;
  int screenY2 = (_Ygl->rheight * ctrl->info.endLine)   / yabsys.VBlankLineCount;
  if (screenY1 < 0) screenY1 = 0;
  if (screenY2 > _Ygl->rheight) screenY2 = _Ygl->rheight;
 
  const int cellw = ctrl->info.cellw;
  const int cellh = ctrl->info.cellh;
  const int cellw_mask = cellw - 1;
  const int cellh_mask = cellh - 1;
 
#ifdef SHELLSHOCK_DEBUG
  static int log_throttle = 0;
  if ((log_throttle++ & 0x3F) == 0) {
    YuiMsg("[NBG0 bitmap draw] zone=[%d..%d] sh=%d sv=%d cellw=%d cellh=%d "
           "coordincx=%.3f coordincy=%.3f charaddr=0x%X paladdr=0x%X\n",
           screenY1, screenY2, ctrl->info.sh, ctrl->info.sv, cellw, cellh,
           ctrl->info.coordincx, ctrl->info.coordincy,
           ctrl->info.charaddr, ctrl->info.paladdr);
  }
#endif
 
  for (i = screenY1; i < screenY2; i++)
  {
    int sh, sv;
    int v;
    u32 baseaddr;
    vdp2Lineinfo *line;
    int inch = inch_base;
 
    baseaddr = (u32)ctrl->info.charaddr;
     line = &(ctrl->info.lineinfo[i]);
     ctrl->info.draw_line = i;
 
    v = (i * incv) >> 8;
 
    if (VDPLINE_SZ(ctrl->info.islinescroll)) {
      u16 raw_inc = line->CoordinateIncH;
      if (raw_inc == 0) inch = 256;
      else              inch = raw_inc;
    }
    if (inch == 0) inch = 1;
 
    if (VDPLINE_SX(ctrl->info.islinescroll))
      sh = ctrl->info.sh + line->LineScrollValH;
    else
      sh = ctrl->info.sh;
 
    if (VDPLINE_SY(ctrl->info.islinescroll))
      sv = ctrl->info.sv + line->LineScrollValV;
    else
      sv = v + ctrl->info.sv;
 
    sv = sv & cellh_mask;
    sh = sh & cellw_mask;
 
    switch (ctrl->info.colornumber) {
    case 0: /* 4 bpp */
      {
        u32 row_base = baseaddr + sv * (cellw >> 1);
        for (j = 0; j < _Ygl->rwidth; j++)
        {
          int h = (sh + ((j * inch) >> 8)) & cellw_mask;
          u32 addr = row_base + (h >> 1);
          int cc = 1;
          u8 dot = Vdp2RamReadByte(NULL, Vdp2Ram, addr);
          u32 alpha = ctrl->info.alpha_per_line[ctrl->info.draw_line >> shift];
          if (!(h & 0x01)) dot = dot >> 4;
          if (!(dot & 0xF) && ctrl->info.transparencyenable) *ctrl->texture.textdata++ = 0x00000000;
          else {
            color = (ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xF)));
            switch (ctrl->info.specialcolormode) {
              case 1: if (ctrl->info.specialcolorfunction == 0) { cc = 0; } break;
              case 2:
                if (ctrl->info.specialcolorfunction == 0) { cc = 0; }
                else { if ((ctrl->info.specialcode & (1 << ((dot & 0xF) >> 1))) == 0) { cc = 0; } }
                break;
              case 3:
                if (((Vdp2ColorRamGetColorRaw(color) & 0x8000) == 0)) { cc = 0; }
                break;
            }
            *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, alpha, ctrl->info.priority, cc, color);
          }
        }
      }
      break;
 
    case 1: /* 8 bpp — cas Shellshock */
      {
        u32 row_base = baseaddr + sv * cellw;
        for (j = 0; j < _Ygl->rwidth; j++)
        {
          int h = (sh + ((j * inch) >> 8)) & cellw_mask;
          u32 alpha = ctrl->info.alpha_per_line[ctrl->info.draw_line >> shift];
          u8 dot = Vdp2RamReadByte(NULL, Vdp2Ram, row_base + h);
          if (!dot && ctrl->info.transparencyenable) {
            *ctrl->texture.textdata++ = 0;
            continue;
          }
          else {
            int cc = 1;
            color = ctrl->info.coloroffset + ((ctrl->info.paladdr << 4) | (dot & 0xFF));
            switch (ctrl->info.specialcolormode) {
              case 1: if (ctrl->info.specialcolorfunction == 0) { cc = 0; } break;
              case 2:
                if (ctrl->info.specialcolorfunction == 0) { cc = 0; }
                else { if ((ctrl->info.specialcode & (1 << ((dot & 0xF) >> 1))) == 0) { cc = 0; } }
                break;
              case 3:
                if (((Vdp2ColorRamGetColorRaw(color) & 0x8000) == 0)) { cc = 0; }
                break;
            }
            *ctrl->texture.textdata++ = VDP2COLOR(ctrl->info.idScreen, alpha, ctrl->info.priority, cc, color);
          }
        }
      }
      break;
 
    case 2: /* 16 bpp palette */
      {
        u32 row_base = baseaddr + (sv * cellw) * 2;
        for (j = 0; j < _Ygl->rwidth; j++)
        {
          int h = (sh + ((j * inch) >> 8)) & cellw_mask;
          *ctrl->texture.textdata++ = Vdp2GetPixel16bpp(ctrl, row_base + (h << 1));
        }
      }
      break;
 
    case 3: /* 16 bpp RGB */
      {
        u32 row_base = baseaddr + (sv * cellw) * 2;
        for (j = 0; j < _Ygl->rwidth; j++)
        {
          int h = (sh + ((j * inch) >> 8)) & cellw_mask;
          *ctrl->texture.textdata++ = Vdp2GetPixel16bppbmp(ctrl, row_base + (h << 1));
        }
      }
      break;
 
    case 4: /* 32 bpp */
      {
        u32 row_base = baseaddr + (sv * cellw) * 4;
        for (j = 0; j < _Ygl->rwidth; j++)
        {
          int h = (sh + ((j * inch) >> 8)) & cellw_mask;
          *ctrl->texture.textdata++ = Vdp2GetPixel32bppbmp(ctrl, row_base + (h << 2));
        }
      }
      break;
    }
 
    ctrl->texture.textdata += ctrl->texture.w;
  }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 Vdp2RotationFetchPixel(vdp2draw_struct *info, int x, int y, int cellw)
{
  u32 dot;
  u32 cramindex;
  int shift = 0;
  if (_Ygl->interlace == DOUBLE_INTERLACE) shift = 1;
  u32 alpha = info->alpha_per_line[info->draw_line>>shift];
  u8 lowdot = 0x00;
  u32 priority = 0;
  switch (info->colornumber)
  {
  case 0: // 4 BPP
    dot = Vdp2RamReadByte(NULL, Vdp2Ram, (info->charaddr + (((y * cellw) + x) >> 1) ));
    if (!(x & 0x1)) dot >>= 4;
    if (!(dot & 0xF) && info->transparencyenable) return 0x00000000;
    else {
      int cc = 1;
      cramindex = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
      Vdp2SetSpecialPriority(info, dot, &priority, &cramindex);
      switch (info->specialcolormode)
      {
      case 1: if (info->specialcolorfunction == 0) { cc = 0; } break;
      case 2:
        if (info->specialcolorfunction == 0) { cc = 0; }
        else { if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) { cc = 0; } }
        break;
      case 3:
        if (((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0)) { cc = 0; }
        break;
      }
      return   VDP2COLOR(info->idScreen, alpha, priority, cc, cramindex);
    }
  case 1: // 8 BPP
    dot = Vdp2RamReadByte(NULL, Vdp2Ram, (info->charaddr + (y * cellw) + x));
    if (!(dot & 0xFF) && info->transparencyenable) return 0x00000000;
    else {
      int cc = 1;
      cramindex = info->coloroffset + ((info->paladdr << 4) | (dot & 0xFF));
      Vdp2SetSpecialPriority(info, dot, &priority, &cramindex);
      switch (info->specialcolormode)
      {
      case 1: if (info->specialcolorfunction == 0) { cc = 0; } break;
      case 2:
        if (info->specialcolorfunction == 0) { cc = 0; }
        else { if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) { cc = 0; } }
        break;
      case 3:
        if (((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0)) { cc = 0; }
        break;
      }
      return   VDP2COLOR(info->idScreen, alpha, priority, cc, cramindex);
    }
  case 2: // 16 BPP(palette)
    dot = Vdp2RamReadWord(NULL, Vdp2Ram, (info->charaddr + ((y * cellw) + x) * 2));
    if ((dot == 0) && info->transparencyenable) return 0x00000000;
    else {
      int cc = 1;
      cramindex = (info->coloroffset + dot);
      Vdp2SetSpecialPriority(info, dot, &priority, &cramindex);
      switch (info->specialcolormode)
      {
      case 1: if (info->specialcolorfunction == 0) { cc = 0; } break;
      case 2:
        if (info->specialcolorfunction == 0) { cc = 0; }
        else { if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) { cc = 0; } }
        break;
      case 3:
        if (((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0)) { cc = 0; }
        break;
      }
      return   VDP2COLOR(info->idScreen, alpha, priority, cc, cramindex);
    }
	case 3: // 16 BPP(RGB)
	  dot = Vdp2RamReadWord(NULL, Vdp2Ram, (info->charaddr + ((y * cellw) + x) * 2));
	  if (!(dot & 0x8000) && info->transparencyenable) return 0x00000000;
	  else {
		Vdp2SetSpecialPriority(info, (u8)(dot & 0xF), &priority, &cramindex);
		return VDP2COLOR(info->idScreen, alpha, priority, 1, RGB555_TO_RGB24(dot & 0xFFFF));
	  }
  case 4: // 32 BPP
    dot = Vdp2RamReadLong(NULL, Vdp2Ram, (info->charaddr + ((y * cellw) + x) * 4));
    if (!(dot & 0x80000000) && info->transparencyenable) return 0x00000000;
    else return VDP2COLOR(info->idScreen, alpha, info->priority, 1, dot & 0xFFFFFF);
  default:
    return 0;
  }
}

static int getPriority(int id, Vdp2 *a) {
    switch (id) {
    case NBG0:  return  (a->PRINA)       & 0x7;
    case NBG1:  return  (a->PRINA >> 8)  & 0x7;
    case NBG2:  return  (a->PRINB)       & 0x7;
    case NBG3:  return  (a->PRINB >> 8)  & 0x7;
    case RBG0:  return  (a->PRIR)        & 0x7;
    default:    return  0;
    }
}
//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawMapPerLine(Vdp2Ctrl *ctrl) {

  int sx;
  int mapx, mapy;
  int planex, planey;
  int pagex, pagey;
  int charx, chary;
  int dot_on_planey;
  int dot_on_pagey;
  int dot_on_planex;
  int dot_on_pagex;
  int h, v;
  const int planeh_shift = 9 + (ctrl->info.planeh - 1);
  const int planew_shift = 9 + (ctrl->info.planew - 1);
  const int plane_shift = 9;
  const int plane_mask = 0x1FF;
  const int page_shift = 9 - 7 + (64 / ctrl->info.pagewh);
  const int page_mask = 0x0f >> ((ctrl->info.pagewh / 32) - 1);

  int preplanex = -1;
  int preplaney = -1;
  int prepagex = -1;
  int prepagey = -1;
  int mapid = 0;
  int premapid = -1;

  ctrl->info.patternpixelwh = 8 * ctrl->info.patternwh;
  ctrl->info.draww = _Ygl->rwidth;

  const int incv = (int)(256.0f / ctrl->info.coordincy + 0.5f);
  const int res_shift = 0;

  int screenH = _Ygl->rheight;

  for (v = 0; v < screenH; v++) {
    int targetv = 0;

    if (VDPLINE_SX(ctrl->info.islinescroll)) {
      sx = ctrl->info.sh + ctrl->info.lineinfo[v].LineScrollValH;
    }
    else {
      sx = ctrl->info.sh;
    }

    if (VDPLINE_SY(ctrl->info.islinescroll)) {
       targetv = ctrl->info.sv + ctrl->info.lineinfo[v].LineScrollValV;
    }
    else {
      targetv = ctrl->info.sv + ((v*incv)>>8);
    }

    const int base_targetv = targetv;

	if (VDPLINE_SZ(ctrl->info.islinescroll)) {
		u16 raw_inc = ctrl->info.lineinfo[v].CoordinateIncH;
		if (raw_inc == 0) {
			ctrl->info.coordincx = 1.0f;
		} else {
			ctrl->info.coordincx = 1.0f / ((float)raw_inc / 256.0f);
		}
	}
	if (ctrl->info.coordincx < ctrl->info.maxzoom)
		ctrl->info.coordincx = ctrl->info.maxzoom;

    if (!ctrl->info.isverticalscroll) {
      mapy = (base_targetv) >> planeh_shift;
      dot_on_planey = (base_targetv) - (mapy << planeh_shift);
      mapy = mapy & 0x01;
      planey = dot_on_planey >> plane_shift;
      dot_on_pagey = dot_on_planey & plane_mask;
      planey = planey & (ctrl->info.planeh - 1);
      pagey = dot_on_pagey >> page_shift;
      chary = dot_on_pagey & page_mask;
      if (pagey < 0) pagey = ctrl->info.pagewh - 1 + pagey;
    }

    int inch = (int)(1.0f / ctrl->info.coordincx * 256.0f);

    int vcsc_cell = -1;

    for (int j = 0; j < ctrl->info.draww; j += 1) {

      int hh = ((j*inch) >> 8);

      if (ctrl->info.isverticalscroll) {
        int cellCol = (hh + sx) >> 3;
        if (cellCol != vcsc_cell) {
          vcsc_cell = cellCol;
          s32 vshift = (s32)Vdp2RamReadLong(NULL, Vdp2Ram,
                          ctrl->info.verticalscrolltbl
                          + cellCol * ctrl->info.verticalscrollinc) >> 16;
          int tv = base_targetv + vshift;
          mapy = tv >> planeh_shift;
          dot_on_planey = tv - (mapy << planeh_shift);
          mapy = mapy & 0x01;
          planey = dot_on_planey >> plane_shift;
          dot_on_pagey = dot_on_planey & plane_mask;
          planey = planey & (ctrl->info.planeh - 1);
          pagey = dot_on_pagey >> page_shift;
          chary = dot_on_pagey & page_mask;
          if (pagey < 0) pagey = ctrl->info.pagewh - 1 + pagey;
        }
      }

      mapx = (hh + sx) >> planew_shift;
      dot_on_planex = (hh + sx) - (mapx << planew_shift);
      mapx = mapx & 0x01;

      mapid = ctrl->info.mapwh * mapy + mapx;
      if (mapid != premapid) {
        if (ctrl->info.PlaneAddr == 0) {
          exit(-1);
        }
        ctrl->info.PlaneAddr(&ctrl->info, mapid, ctrl->regs);
        premapid = mapid;
      }

      planex = dot_on_planex >> plane_shift;
      dot_on_pagex = dot_on_planex & plane_mask;
      planex = planex & (ctrl->info.planew - 1);
      pagex = dot_on_pagex >> page_shift;
      charx = dot_on_pagex & page_mask;
      if (pagex < 0) pagex = ctrl->info.pagewh - 1 + pagex;

      if (planex != preplanex || pagex != prepagex ||
          planey != preplaney || pagey != prepagey) {
        if (Vdp2PatternAddrPos(ctrl, planex, pagex, planey, pagey) == 0) continue;
        preplanex = planex;
        preplaney = planey;
        prepagex = pagex;
        prepagey = pagey;
      }

      ctrl->info.draw_line = v;
      if (_Ygl->interlace == DOUBLE_INTERLACE) ctrl->info.draw_line >>= 1;

      ctrl->info.priority = getPriority(ctrl->info.idScreen,
                                        &Vdp2Lines[ctrl->info.draw_line]);

      int priority = ctrl->info.priority;
      if (ctrl->info.specialprimode == 1) {
        ctrl->info.priority = (ctrl->info.priority & 0xFFFFFFFE)
                              | ctrl->info.specialfunction;
      }

      int x = charx;
      int y = chary;

      if (ctrl->info.patternwh == 1)
      {
        x &= 8 - 1;
        y &= 8 - 1;
        if (ctrl->info.flipfunction & 0x2) y = 8 - 1 - y;
        if (ctrl->info.flipfunction & 0x1) x = 8 - 1 - x;
      }
      else
      {
        if (ctrl->info.flipfunction)
        {
          y &= 16 - 1;
          if (ctrl->info.flipfunction & 0x2)
          {
            if (!(y & 8)) y = 8 - 1 - y + 16;
            else          y = 16 - 1 - y;
          }
          else if (y & 8)
            y += 8;

          if (ctrl->info.flipfunction & 0x1)
          {
            if (!(x & 8)) y += 8;
            x &= 8 - 1;
            x = 8 - 1 - x;
          }
          else if (x & 8)
          {
            y += 8;
            x &= 8 - 1;
          }
          else
            x &= 8 - 1;
        }
        else
        {
          y &= 16 - 1;
          if (y & 8) y += 8;
          if (x & 8) y += 8;
          x &= 8 - 1;
        }
      }

      *(ctrl->texture.textdata++) =
          Vdp2RotationFetchPixel(&ctrl->info, x, y, ctrl->info.cellw);

      ctrl->info.priority = priority;
    }

    ctrl->texture.textdata += ctrl->texture.w;
  }
}

static void Vdp2DrawMapTest(Vdp2Ctrl *ctrl, int delayed) {

  int lineindex = 0;

  int sx = 0; //, sy;
  int mapx = 0, mapy = 0;
  int planex = 0, planey = 0;
  int pagex = 0, pagey = 0;
  int charx = 0, chary = 0;
  int dot_on_planey = 0;
  int dot_on_pagey = 0;
  int dot_on_planex = 0;
  int dot_on_pagex = 0;
  int h, v;
  int cell_count = 0;

  const int planeh_shift = 9 + (ctrl->info.planeh - 1);
  const int planew_shift = 9 + (ctrl->info.planew - 1);
  const int plane_shift = 9;
  const int plane_mask = 0x1FF;
  const int page_shift = 9 - 7 + (64 / ctrl->info.pagewh);
  const int page_mask = 0x0f >> ((ctrl->info.pagewh / 32) - 1);

  ctrl->info.patternpixelwh = 8 * ctrl->info.patternwh;
  ctrl->info.draww = (int)((float)_Ygl->rwidth / ctrl->info.coordincx);
  ctrl->info.drawh = (int)((float)_Ygl->rheight / ctrl->info.coordincy);
  if (_Ygl->interlace == DOUBLE_INTERLACE) ctrl->info.drawh *= 2;
  ctrl->info.lineinc = ctrl->info.patternpixelwh;

  for (v = -ctrl->info.patternpixelwh; v < ctrl->info.drawh + ctrl->info.patternpixelwh; v += ctrl->info.patternpixelwh) {
    int targetv = 0;
    sx = ctrl->info.x;

    if (!ctrl->info.isverticalscroll) {
      targetv = ctrl->info.y + v;
      mapy = (targetv) >> planeh_shift;
      dot_on_planey = (targetv)-(mapy * (1 << planeh_shift));
      mapy = mapy & 0x01;
      planey = dot_on_planey >> plane_shift;
      dot_on_pagey = dot_on_planey & plane_mask;
      planey = planey & (ctrl->info.planeh - 1);
      pagey = dot_on_pagey >> page_shift;
      chary = dot_on_pagey & page_mask;
      if (pagey < 0) pagey = ctrl->info.pagewh - 1 + pagey;
    }
    else {
      cell_count = 0;
    }
    for (h = -ctrl->info.patternpixelwh; h < ctrl->info.draww + ctrl->info.patternpixelwh; h += ctrl->info.patternpixelwh) {

      if (ctrl->info.isverticalscroll) {
        targetv = ctrl->info.y + v + (Vdp2RamReadLong(NULL, Vdp2Ram, ctrl->info.verticalscrolltbl + cell_count) >> 16);
        cell_count += ctrl->info.verticalscrollinc;
        mapy = (targetv) >> planeh_shift;
        dot_on_planey = (targetv)-(mapy << planeh_shift);
        mapy = mapy & 0x01;
        planey = dot_on_planey >> plane_shift;
        dot_on_pagey = dot_on_planey & plane_mask;
        planey = planey & (ctrl->info.planeh - 1);
        pagey = dot_on_pagey >> page_shift;
        chary = dot_on_pagey & page_mask;
        if (pagey < 0) pagey = ctrl->info.pagewh - 1 + pagey;
      }

      mapx = (h + sx) >> planew_shift;
      dot_on_planex = (h + sx) - (mapx * (1<<planew_shift));
      mapx = mapx & 0x01;
      planex = dot_on_planex >> plane_shift;
      dot_on_pagex = dot_on_planex & plane_mask;
      planex = planex & (ctrl->info.planew - 1);
      pagex = dot_on_pagex >> page_shift;
      charx = dot_on_pagex & page_mask;

      if (ctrl->info.PlaneAddr == 0) {
        exit(-1);
      }
      ctrl->info.PlaneAddr(&ctrl->info, ctrl->info.mapwh * mapy + mapx, ctrl->regs);
      if (Vdp2PatternAddrPos(ctrl, planex, pagex, planey, pagey) != 0) {
        int charAddrBk = (((ctrl->info.charaddr >> 16)& 0xF) >> ((ctrl->regs->VRSIZE >> 15)&0x1)) >> 1;
        if (ctrl->info.char_bank[charAddrBk] == 1) {
          int x = h - charx;
          int y = v - chary;
          ctrl->info.draw_line =  y;
          if (delayed && (h == -ctrl->info.patternpixelwh)) continue;
          Vdp2DrawPatternPos(ctrl, x+delayed*8, y, 0, 0, ctrl->info.lineinc);
        }
      }
    }
    lineindex++;
  }

}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DoNothing(UNUSED void *info, u32 pixel)
{
  return pixel;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DoColorOffset(void *info, u32 pixel)
{
  return pixel;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void ReadVdp2ColorOffset(Vdp2 * regs, vdp2draw_struct *info, int mask)
{
  if (regs->CLOFEN & mask)
  {
    if (regs->CLOFSL & mask)
    {
      info->cor = regs->COBR & 0xFF;
      if (regs->COBR & 0x100)
        info->cor |= 0xFFFFFF00;

      info->cog = regs->COBG & 0xFF;
      if (regs->COBG & 0x100)
        info->cog |= 0xFFFFFF00;

      info->cob = regs->COBB & 0xFF;
      if (regs->COBB & 0x100)
        info->cob |= 0xFFFFFF00;
    }
    else
    {
      info->cor = regs->COAR & 0xFF;
      if (regs->COAR & 0x100)
        info->cor |= 0xFFFFFF00;

      info->cog = regs->COAG & 0xFF;
      if (regs->COAG & 0x100)
        info->cog |= 0xFFFFFF00;

      info->cob = regs->COAB & 0xFF;
      if (regs->COAB & 0x100)
        info->cob |= 0xFFFFFF00;
    }
    info->PostPixelFetchCalc = &DoColorOffset;
  }
  else {

    info->PostPixelFetchCalc = &DoNothing;
    info->cor = 0;
    info->cob = 0;
    info->cog = 0;

  }
}


static void Vdp2DrawBackScreen(Vdp2 *varVdp2Regs)
{
    u32* back_pixel_data = YglGetBackColorPointer();
    if (back_pixel_data == NULL) return;

    const int bk_line_shift = (_Ygl->interlace == SINGLE_INTERLACE) ? 1 : 0;

    const int phys_lines = _Ygl->rheight;
    const int logical_lines = (yabsys.VBlankLineCount >= 270)
                              ? 270 : yabsys.VBlankLineCount;

    for (int i = 0; i < phys_lines; i++) {
        const int li = (i * logical_lines) / phys_lines;
        const int table_line = li >> bk_line_shift;   /* §7.2 Fig.7.4 p.175 */
        const Vdp2 *L = &Vdp2Lines[li];

        const u8 bkccrt = (u8)((L->CCRLB >> 8) & 0x1F);   /* BKCCRT4-0 */
        const u8 alpha8 = (u8)(((~bkccrt & 0x1F) * 255) / 31);

        u32 base = (((L->BKTAU & 0x7) << 16) | L->BKTAL) * 2;
        const int isPerLineL = (L->BKTAU & 0x8000) != 0;
        u32 currentAddr = isPerLineL ? (base + 2 * table_line) : base;

        const u32 vram_mask = (L->VRSIZE & 0x8000) ? 0xFFFFF : 0x7FFFF;
        u16 dot = Vdp2RamReadWord(NULL, Vdp2Ram, currentAddr & vram_mask);

        u8 r = (dot & 0x001F) << 3;
        u8 g = ((dot >> 5)  & 0x1F) << 3;
        u8 b = ((dot >> 10) & 0x1F) << 3;

        back_pixel_data[i] =
              ((u32)alpha8 << 24)   /* A */
            | ((u32)b      << 16)   /* B */
            | ((u32)g      <<  8)   /* G */
            | ((u32)r      <<  0);  /* R */
    }

    YglSetBackTextureColor(_Ygl->rheight);
}

//////////////////////////////////////////////////////////////////////////////
// 11.3 Line Color insertion
//  7.1 Line Color Screen
static void Vdp2DrawLineColorScreen(Vdp2 *varVdp2Regs)
{

  u32 cacheaddr = 0xFFFFFFFF;
  int inc = 0;
  int line_cnt = _Ygl->rheight;
  int i;
  u32 * line_pixel_data;
  u32 addr;

	if (varVdp2Regs->LNCLEN == 0) return;


  line_pixel_data = YglGetLineColorScreenPointer();
  if (line_pixel_data == NULL) {
    return;
  }

  u32 * const line_pixel_data_base = line_pixel_data;

  if ((varVdp2Regs->LCTA.part.U & 0x8000)) {
    inc = 0x02; // color per line
  }
  else {
    inc = 0x00; // single color
  }

  const int phys_lines = _Ygl->rheight;
  const int logical_lines = (yabsys.VBlankLineCount >= 270)
                            ? 270 : yabsys.VBlankLineCount;
  for (i = 0; i < phys_lines; i++) {
    const int li = (i * logical_lines) / phys_lines;
    const Vdp2 *L = &Vdp2Lines[li];
    const u8 alpha = (u8)(((~L->CCRLB & 0x1F) * 255) / 31);
    u32 lineAddr = (L->LCTA.all & 0x7FFFF) << 1;
    if (L->LCTA.part.U & 0x8000) lineAddr += 2 * li;
    u16 LineColorRamAddress = Vdp2RamReadWord(NULL, Vdp2Ram, lineAddr);
    *(line_pixel_data++) = Vdp2ColorRamGetLineColor(LineColorRamAddress, alpha);
  }

  YglSetLineColorScreen(line_pixel_data_base, line_cnt);

}

//////////////////////////////////////////////////////////////////////////////

static int Vdp2CheckCharAccessPenalty(int char_access, int ptn_access, int char_size_2x2) {
  if (_Ygl->rwidth >= 640) {
    if (ptn_access & 0x01) { // T0
      if ((char_access & 0x07) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x02) { // T1
      if ((char_access & 0x0E) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x04) { // T2
      const int mask = char_size_2x2 ? 0x0C : 0x0D;
      if ((char_access & mask) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x08) { // T3
      const int mask = char_size_2x2 ? 0x08 : 0x0B;
      if ((char_access & mask) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }
    return -1;
  }
  else {

    if (ptn_access & 0x01) { // T0
      if ((char_access & 0xF7) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x02) { // T1
      if ((char_access & 0xEF) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x04) { // T2
      if ((char_access & 0xCF) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x08) { // T3
      if ((char_access & 0x8F) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x10) { // T4
      if ((char_access & 0x0F) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x20) { // T5
      if ((char_access & 0x0E) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x40) { // T6
      if ((char_access & 0x0C) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x80) { // T7
      if ((char_access & 0x08) != 0) {
        return 0;
      }
    }
    return -1;
  }
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawRBG1_part(RBGDrawInfo *rbg)
{
  YglTexture texture;
  YglCache tmpc;
  vdp2draw_struct* info = &rbg->ctrl.info;
  int shift = 0;
  if (_Ygl->interlace == DOUBLE_INTERLACE) shift = 1;

  info->dst = 0;
  info->idScreen = RBG1;
  info->cor = 0;
  info->cog = 0;
  info->cob = 0;
  info->specialcolorfunction = 0;

  int i;
  info->enable = 0;

  // RBG1 mode
  info->enable = ((rbg->ctrl.regs->BGON & 0x20)!=0);

  if (!info->enable) {
    pushRBG(rbg);
    return;
  }

  for (int i = info->startLine; i < info->endLine; i++) {
    info->display[i] = info->enable;
    rbg->alpha[i] = (u8)(((~Vdp2Lines[i].CCRNA & 0x1F) * 255) / 31);
    info->alpha_per_line[i] = rbg->alpha[i];
  }

  // Read in Parameter B
  Vdp2ReadRotationTable(1, &rbg->paraB, rbg->ctrl.regs, Vdp2Ram);

  if ((info->isbitmap = rbg->ctrl.regs->CHCTLA & 0x2) != 0)
  {
    // Bitmap Mode
    ReadBitmapSize(info, rbg->ctrl.regs->CHCTLA >> 2, 0x3);
    if (shift) info->cellh *= 2;

    info->charaddr = (rbg->ctrl.regs->MPOFR & 0x70) * 0x2000;

    {
	int charAddrBk = (((info->charaddr >> 16) & 0xF)
					  >> ((rbg->ctrl.regs->VRSIZE >> 15) & 0x1)) >> 1;
	if (info->colornumber != 3 &&  /* RGB format bypasses VRAM bank restriction */
		((rbg->ctrl.regs->RAMCTL >> (charAddrBk << 1)) & 0x3) != 0x3) {
		pushRBG(rbg);
		return;
	}
    }

    info->paladdr = (rbg->ctrl.regs->BMPNA & 0x7) << 4;
    info->flipfunction = 0;
    info->specialfunction = 0;
  }
  else
  {
    // Tile Mode
    info->mapwh = 4;
    ReadPlaneSize(info, rbg->ctrl.regs->PLSZ >> 12);
    ReadPatternData(info, rbg->ctrl.regs->PNCN0, rbg->ctrl.regs->CHCTLA & 0x1);

    rbg->paraB.ShiftPaneX = 8 + info->planew;
    rbg->paraB.ShiftPaneY = 8 + info->planeh;
    rbg->paraB.MskH = (8 * 64 * info->planew) - 1;
    rbg->paraB.MskV = (8 * 64 * info->planeh) - 1;
    rbg->paraB.MaxH = 8 * 64 * info->planew * 4;
    rbg->paraB.MaxV = 8 * 64 * info->planeh * 4;
  }

  info->rotatenum = 1;
  rbg->paraB.coefenab = rbg->ctrl.regs->KTCTL & 0x100;
  rbg->paraB.charaddr = (rbg->ctrl.regs->MPOFR & 0x70) * 0x2000;
  ReadPlaneSizeR(&rbg->paraB, rbg->ctrl.regs->PLSZ >> 12);

  for (i = 0; i < 16; i++)
  {
    Vdp2ParameterBPlaneAddr(info, i, rbg->ctrl.regs);
    rbg->paraB.PlaneAddrv[i] = info->addr;
  }

  ReadMosaicData(info, 0x1, rbg->ctrl.regs);

  info->transparencyenable = !(rbg->ctrl.regs->BGON & 0x100);
  info->specialprimode = rbg->ctrl.regs->SFPRMD & 0x3;
  info->specialcolormode = rbg->ctrl.regs->SFCCMD & 0x3;

  if (rbg->ctrl.regs->SFSEL & 0x1)
    info->specialcode = rbg->ctrl.regs->SFCODE >> 8;
  else
    info->specialcode = rbg->ctrl.regs->SFCODE & 0xFF;

  info->colornumber = (rbg->ctrl.regs->CHCTLA & 0x70) >> 4;

  int dest_alpha = ((rbg->ctrl.regs->CCCTL >> 9) & 0x01);

  info->coloroffset = (rbg->ctrl.regs->CRAOFA & 0x7) << 8;
  info->linecheck_mask = 0x01;
  info->priority = rbg->ctrl.regs->PRINA & 0x7;

  LOG_AREA("RGB1 prio = %d\n", info->priority);

  if (info->priority == 0) {
    pushRBG(rbg);
    return;
  }

  ReadLineScrollData(info, rbg->ctrl.regs->SCRCTL & 0xFF, rbg->ctrl.regs->LSTA0.all, rbg->ctrl.regs);
  info->lineinfo = lineNBG0;
  Vdp2GenLineinfo(info);

  if (rbg->ctrl.regs->SCRCTL & 1)
  {
    info->isverticalscroll = 1;
    info->verticalscrolltbl = (rbg->ctrl.regs->VCSTA.all & 0x7FFFE) << 1;
    if (rbg->ctrl.regs->SCRCTL & 0x100)
      info->verticalscrollinc = 8;
    else
      info->verticalscrollinc = 4;
  }
  else
    info->isverticalscroll = 0;

  // RBG1 draw
  Vdp2DrawRotation(rbg);
}

static int sameVDP2RegRBG0(Vdp2 *a, Vdp2 *b)
{

    if (a->WPSX0 != b->WPSX0) return 0;
    if (a->WPEX0 != b->WPEX0) return 0;
    if (a->WPSY0 != b->WPSY0) return 0;
    if (a->WPEY0 != b->WPEY0) return 0;
    if (a->WPSX1 != b->WPSX1) return 0;
    if (a->WPEX1 != b->WPEX1) return 0;
    if (a->WPSY1 != b->WPSY1) return 0;
    if (a->WPEY1 != b->WPEY1) return 0;
	
    if (a->LWTA0.all != b->LWTA0.all) return 0;
    if (a->LWTA1.all != b->LWTA1.all) return 0;
	
    if ((a->RAMCTL & 0x8FFF) != (b->RAMCTL & 0x8FFF)) return 0;
	
  if ((a->BGON & 0x1010) != (b->BGON & 0x1010)) return 0;
  if ((a->PRIR & 0x7) != (b->PRIR & 0x7)) return 0;
  if ((a->RPTA.all) != (b->RPTA.all)) return 0;
  if ((a->RPMD & 0x3) != (b->RPMD & 0x3)) return 0;
  if ((a->CCRR & 0x1F) != (b->CCRR & 0x1F)) return 0;
  if ((a->CCCTL & 0xF710) != (b->CCCTL & 0xF710)) return 0;
  if ((a->KTCTL & 0x1F1F) != (b->KTCTL & 0x1F1F)) return 0;
  if ((a->CHCTLB & 0x7700) != (b->CHCTLB & 0x7700)) return 0; // colornumber + bitmap RBG0
  if ((a->PLSZ & 0xFF00)   != (b->PLSZ & 0xFF00))   return 0; // plane size ParaA + ParaB
  if ((a->MPOFR & 0x77)    != (b->MPOFR & 0x77))     return 0; // map offset RBG0 A+B
  if ((a->PNCR & 0xFFFF) != (b->PNCR & 0xFFFF)) return 0; // R0PNB,R0CNSM,R0SPR,etc.
  if ((a->KTAOF & 0x0707) != (b->KTAOF & 0x0707)) return 0; // RAKTAOS+RBKTAOS

   if ((a->OVPNRA & 0xFFFF) != (b->OVPNRA & 0xFFFF)) return 0;
   if ((a->OVPNRB & 0xFFFF) != (b->OVPNRB & 0xFFFF)) return 0;

  if ((a->SFPRMD & 0x0300) != (b->SFPRMD & 0x0300)) return 0;
  if ((a->WCTLC & 0x00FF) != (b->WCTLC & 0x00FF)) return 0;
  if ((a->WCTLD & 0x000F) != (b->WCTLD & 0x000F)) return 0;
  if ((a->BMPNB & 0x0077) != (b->BMPNB & 0x0077)) return 0;
  if ((a->MZCTL & 0xFF10) != (b->MZCTL & 0xFF10)) return 0;
  if ((a->SFCCMD & 0x0300) != (b->SFCCMD & 0x0300)) return 0;
  if ((a->SFSEL & 0x0010) != (b->SFSEL & 0x0010)) return 0;
  if ((a->SFCODE & 0xFFFF) != (b->SFCODE & 0xFFFF)) return 0; 
  if ((a->LNCLEN & 0x0010) != (b->LNCLEN & 0x0010)) return 0;
  if ((a->LCTA.all) != (b->LCTA.all)) return 0;
  if ((a->CRAOFB & 0x0007) != (b->CRAOFB & 0x0007)) return 0;
  if ((a->CLOFSL & 0x0010) != (b->CLOFSL & 0x0010)) return 0;
  if ((a->CLOFEN & 0x0010) != (b->CLOFEN & 0x0010)) return 0;
  return 1;
}

static int sameVDP2RegRBG1(Vdp2 *a, Vdp2 *b)
{

    if (a->WPSX0 != b->WPSX0) return 0;
    if (a->WPEX0 != b->WPEX0) return 0;
    if (a->WPSY0 != b->WPSY0) return 0;
    if (a->WPEY0 != b->WPEY0) return 0;
    if (a->WPSX1 != b->WPSX1) return 0;
    if (a->WPEX1 != b->WPEX1) return 0;
    if (a->WPSY1 != b->WPSY1) return 0;
    if (a->WPEY1 != b->WPEY1) return 0;
	
    if (a->LWTA0.all != b->LWTA0.all) return 0;
    if (a->LWTA1.all != b->LWTA1.all) return 0;
	
    if ((a->RAMCTL & 0x8FFF) != (b->RAMCTL & 0x8FFF)) return 0;
	
  if ((a->BGON & 0x130) != (b->BGON & 0x130)) return 0;
  if ((a->PRINA & 0x7) != (b->PRINA & 0x7)) return 0;
  if ((a->CCRNA &0x1F) != (b->CCRNA &0x1F)) return 0;
  if ((a->SFCCMD &0x3) != (b->SFCCMD &0x3)) return 0;
  if ((a->RPTA.all) != (b->RPTA.all)) return 0;
  if ((a->CHCTLA & 0x7F)   != (b->CHCTLA & 0x7F))   return 0; // colornumber/bitmap NBG0=RBG1
  if ((a->PLSZ & 0xF000)   != (b->PLSZ & 0xF000))   return 0; // plane size ParaB
  if ((a->PNCN0 & 0xFFFF)  != (b->PNCN0 & 0xFFFF))  return 0; // pattern name control
  if ((a->CCCTL & 0x0301) != (b->CCCTL & 0x0301)) return 0; // N0CCEN(0)+CCMD(8)+CCRTMD(9)
  if ((a->MPOFR & 0x70) != (b->MPOFR & 0x70)) return 0; // charaddr RBG1 = (MPOFR&0x70)*0x2000
  if ((a->SCRCTL & 0x3F) != (b->SCRCTL & 0x3F)) return 0; // N0LSCE+N0LSSX/Y+N0LZMX+N0LSS
  if ((a->SFPRMD & 0x0003) != (b->SFPRMD & 0x0003)) return 0; // special priority mode NBG0/RBG1. Masque 0x0003
  if ((a->BMPNA & 0x0077) != (b->BMPNA & 0x0077)) return 0; // Actif uniquement en bitmap mode (N0BMEN=1 dans CHCTLA).
  if ((a->KTCTL & 0x1F00) != (b->KTCTL & 0x1F00)) return 0; // RBG1 utilise uniquement ParaB — seul l'octet haut compte.
  if ((a->MZCTL & 0xFF01) != (b->MZCTL & 0xFF01)) return 0; // N0MZE(0) enable, MZSZH(11-8)+MZSZV(15-12) taille commune.
  if ((a->SFSEL & 0x0001) != (b->SFSEL & 0x0001)) return 0;  // N0SFCS code A ou B pour NBG0/RBG1.
  if ((a->SFCODE & 0xFFFF) != (b->SFCODE & 0xFFFF)) return 0; // special function code A+B pertinent quand SFCCMD ou SFPRMD >= mode 2.  
  if ((a->LNCLEN & 0x0001) != (b->LNCLEN & 0x0001)) return 0; // N0LCEN line color insert NBG0/RBG1.
  if ((a->LCTA.all) != (b->LCTA.all)) return 0; // adresse table ligne couleur source des couleurs line-color.
  if ((a->CRAOFA & 0x0007) != (b->CRAOFA & 0x0007)) return 0; // N0CAOS2..0 color RAM address offset NBG0/RBG1.
  if ((a->CLOFSL & 0x0001) != (b->CLOFSL & 0x0001)) return 0; // N0COSL color offset A vs B pour NBG0/RBG1.
  if ((a->CLOFEN & 0x0001) != (b->CLOFEN & 0x0001)) return 0;  
  if ((a->LSTA0.all) != (b->LSTA0.all)) return 0; // adresse table line scroll NBG0/RBG1 scroll est actif (SCRCTL bits 5-0 != 0).
  if ((a->VCSTA.all) != (b->VCSTA.all)) return 0; // adresse table vertical cell scroll NBG0/RBG1
  if ((a->WCTLD & 0x000F) != (b->WCTLD & 0x000F)) return 0; // rotation parameter window
  return 1;
}

static int sameVDP2RegNBG0(Vdp2 *a, Vdp2 *b)
{
    if (a->WPSX0 != b->WPSX0) return 0;
    if (a->WPEX0 != b->WPEX0) return 0;
    if (a->WPSY0 != b->WPSY0) return 0;
    if (a->WPEY0 != b->WPEY0) return 0;
    if (a->WPSX1 != b->WPSX1) return 0;
    if (a->WPEX1 != b->WPEX1) return 0;
    if (a->WPSY1 != b->WPSY1) return 0;
    if (a->WPEY1 != b->WPEY1) return 0;
	
    if (a->LWTA0.all != b->LWTA0.all) return 0;
    if (a->LWTA1.all != b->LWTA1.all) return 0;
	
    if ((a->RAMCTL & 0x8FFF) != (b->RAMCTL & 0x8FFF)) return 0;

    if ((a->BGON & 0x131) != (b->BGON & 0x131)) return 0;
 
     if ((a->CHCTLA & 0x7F) != (b->CHCTLA & 0x7F)) return 0;
 
    if ((a->PRINA & 0x7) != (b->PRINA & 0x7)) return 0;
 
    if ((a->CCRNA & 0x1F) != (b->CCRNA & 0x1F)) return 0;
 
    if ((a->SCXIN0 & 0x7FF) != (b->SCXIN0 & 0x7FF)) return 0;
 
    if ((a->SCYIN0 & 0x7FF) != (b->SCYIN0 & 0x7FF)) return 0;
 
    if ((a->SCXDN0 & 0xFF00) != (b->SCXDN0 & 0xFF00)) return 0;
 
    if ((a->SCYDN0 & 0xFF00) != (b->SCYDN0 & 0xFF00)) return 0;
 
    if ((a->ZMXN0.all & 0x7FF00) != (b->ZMXN0.all & 0x7FF00)) return 0;
    if ((a->ZMYN0.all & 0x7FF00) != (b->ZMYN0.all & 0x7FF00)) return 0;
 
    if ((a->ZMCTL & 0x0003) != (b->ZMCTL & 0x0003)) return 0;
 
    if ((a->CRAOFA & 0x7) != (b->CRAOFA & 0x7)) return 0;
 
    if ((a->MPOFN & 0x7) != (b->MPOFN & 0x7)) return 0;
 
    if ((a->BMPNA & 0x37) != (b->BMPNA & 0x37)) return 0;
 
    if ((a->PLSZ & 0x0003) != (b->PLSZ & 0x0003)) return 0;
 
    if ((a->PNCN0 & 0xFFFF) != (b->PNCN0 & 0xFFFF)) return 0;
 
    if ((a->SCRCTL & 0x00FF) != (b->SCRCTL & 0x00FF)) return 0;

    if (a->LSTA0.all != b->LSTA0.all) return 0;

    if (a->VCSTA.all != b->VCSTA.all) return 0;
 
    if ((a->SFPRMD & 0x0003) != (b->SFPRMD & 0x0003)) return 0;
 
    if ((a->SFCCMD & 0x0003) != (b->SFCCMD & 0x0003)) return 0;
 
    if ((a->LNCLEN & 0x0001) != (b->LNCLEN & 0x0001)) return 0;
 
    if ((a->CLOFSL & 0x0001) != (b->CLOFSL & 0x0001)) return 0;

    if ((a->WCTLA & 0x00FF) != (b->WCTLA & 0x00FF)) return 0;

    if ((a->CLOFEN & 0x0001) != (b->CLOFEN & 0x0001)) return 0;

    if ((a->MZCTL & 0xFF01) != (b->MZCTL & 0xFF01)) return 0;
 
    return 1;
}

static void Vdp2DrawRBG1()
{
  int nbZone = 1;
  int lastLine = 0;
  int line;
  int max = (yabsys.VBlankLineCount >= 270)?270:yabsys.VBlankLineCount;
  RBGDrawInfo *rbg = NULL;
  for (line = 1; line<max; line++) {
    if (!sameVDP2Reg(RBG1, &Vdp2Lines[line-1], &Vdp2Lines[line])) {
      rbg = popRBG();
      rbg->rbg_type = 0x04;
      rbg->ctrl.info.startLine = lastLine;
      rbg->ctrl.info.endLine = line;
      rbg->ctrl.regs = &Vdp2Lines[rbg->ctrl.info.startLine];
      lastLine = line;
      LOG_AREA("RBG1 Draw from %d to %d %x\n", rbg->ctrl.info.startLine, rbg->ctrl.info.endLine, rbg->ctrl.regs->BGON);
      Vdp2DrawRBG1_part(rbg);
    }
  }
  rbg = popRBG();
  rbg->rbg_type = 0x04;
  rbg->ctrl.info.startLine = lastLine;
  rbg->ctrl.info.endLine = line;
  rbg->ctrl.regs = &Vdp2Lines[rbg->ctrl.info.startLine];
  LOG_AREA("RBG1 Draw from %d to %d %x\n", rbg->ctrl.info.startLine, rbg->ctrl.info.endLine, rbg->ctrl.regs->BGON);
  Vdp2DrawRBG1_part(rbg);
}

static int sameVDP2Reg(int id, Vdp2 *a, Vdp2 *b)
{
    switch (id) {
    case RBG0: return sameVDP2RegRBG0(a, b);
    case RBG1: return sameVDP2RegRBG1(a, b);
    case NBG0: return sameVDP2RegNBG0(a, b);
    case NBG1: return sameVDP2RegNBG1(a, b);
    case NBG2: return sameVDP2RegNBG2(a, b);
    case NBG3: return sameVDP2RegNBG3(a, b);
    default:   break;
    }
    return 1;
}

static int isEnabled(int id, Vdp2* varVdp2Regs) {
  int display = 1;

  const int r0on = (varVdp2Regs->BGON & 0x10) != 0;
  const int r1on = (varVdp2Regs->BGON & 0x20) != 0;
  const int rule_a = (r0on && r1on);  /* both rotation screens active */
  const int rule_b = (!r0on && r1on); /* prohibited config */

  const int n0on    = (varVdp2Regs->BGON & 0x1) != 0;
  const int n1on    = (varVdp2Regs->BGON & 0x2) != 0;
  const int n0color = (varVdp2Regs->CHCTLA & 0x70) >> 4;     /* N0CHCN */
  const int n1color = (varVdp2Regs->CHCTLA & 0x3000) >> 12;  /* N1CHCN */
  const int n0_kills_n2 = n0on && (n0color >= 2);
  const int n0_kills_n1n2n3 = n0on && (n0color >= 4);
  const int n1_kills_n3 = n1on && (n1color >= 2);

  switch(id) {
    case NBG0:
      display = ((varVdp2Regs->BGON & 0x1)!=0);
      if (rule_a) display = 0; /* §4.1: NBG disabled when R0ON+R1ON=1 */
      break;
    case NBG1:
      display = ((varVdp2Regs->BGON & 0x2)!=0);
     if (rule_a) display = 0;
	 if (n0_kills_n1n2n3) display = 0;   /* p.61 NBG0=16M */
	  break;
    case NBG2:
      display = ((varVdp2Regs->BGON & 0x4)!=0);
	 if (rule_a) display = 0;
     if (n0_kills_n2) display = 0;       /* p.61 NBG0≥2048 */
     if (n0_kills_n1n2n3) display = 0;   /* p.61 NBG0=16M */
      break;
    case NBG3:
      display = ((varVdp2Regs->BGON & 0x8)!=0);
	 if (rule_a) display = 0;
	 if (n0_kills_n1n2n3) display = 0;   /* p.61 NBG0=16M */
     if (n1_kills_n3) display = 0;       /* p.61 NBG1≥2048 */
      break;
    case RBG0:
      display = r0on;
      break;
    case RBG1:
      display = r1on && !rule_b;
      break;
    default:
      display = 1;
  }
  return display;
}

static pixel_t *VIDCSGetVdp2ScreenExtract(u32 screen, int * w, int * h)
{
  if ((screen >= NBG0) && (screen <= NBG3)) {
    pixel_t* pixels = (pixel_t*)malloc(_Ygl->rwidth*_Ygl->rheight * sizeof(pixel_t));
    *w = _Ygl->rwidth;
    *h = _Ygl->rheight;
    glBindTexture(GL_TEXTURE_2D, _Ygl->screen_fbotex[screen]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return pixels;
  }
  if ((screen == RBG0) || (screen == RBG1))
  {
    pixel_t* pixels = (pixel_t*)malloc(_Ygl->width*_Ygl->height * sizeof(pixel_t));
    *w = _Ygl->width;
    *h = _Ygl->height;
    glBindTexture(GL_TEXTURE_2D, _Ygl->rbg_compute_fbotex[screen-RBG0]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return pixels;
  }
  if (screen == SPRITE)
  {
    pixel_t* pixels = (pixel_t*)malloc(_Ygl->vdp1width*_Ygl->vdp1height * sizeof(pixel_t));
    *w = _Ygl->vdp1width;
    *h = _Ygl->vdp1height;
    glBindTexture(GL_TEXTURE_2D, GetCSVDP1fb(0));
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    for (int i = 0; i<_Ygl->vdp1width*_Ygl->vdp1height; i++) {
      if (pixels[i] != 0) pixels[i] |= 0xFF000000;
    }
    return pixels;
  }
  if (screen == 0xFF)
  {
    pixel_t* pixels = (pixel_t*)malloc(_Ygl->width * _Ygl->height * sizeof(pixel_t));
    *w = _Ygl->width;
    *h = _Ygl->height;
    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
    glReadPixels(0, 0, _Ygl->width, _Ygl->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return pixels;
  }  // Ecran inconnu
  *w = 0;
  *h = 0;
  return NULL;
}

#endif
