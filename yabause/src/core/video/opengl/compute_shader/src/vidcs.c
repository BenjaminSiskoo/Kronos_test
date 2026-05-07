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
static int Vdp2CheckCharAccessPenalty(int char_access, int ptn_access);
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

static void finishRbgQueue(void);


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

extern vdp2rotationparameter_struct  Vdp1ParaA;

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

  // Screen culling
  if (tile.vertices[0] >= _Ygl->rwidth || tile.vertices[1] >= _Ygl->rheight || tile.vertices[2] < 0 || tile.vertices[5] < 0)
  {
  	return;
  }
  if (   tile.vertices[0] >= _Ygl->rwidth   // tile à droite de l'écran
      || tile.vertices[2] < 0               // tile à gauche de l'écran
      || tile.vertices[1] >= _Ygl->rheight  // tile en dessous de l'écran
      || tile.vertices[5] < 0)              // tile au-dessus de l'écran
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
    if (rbg->ctrl.regs->RPMD != 0) rbg->useb = 1;

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
  if (vdp2busy == 1) {
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
  }
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
   * Use MAX(1, .) to defend against a CMDSIZE field of 0 - VDP1 §6.4
   * says the dimension fields encode 1..63 cells of 8 pixels each, but
   * games occasionally upload a stale 0 during list construction. */
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
 
  for (i = 0; i < yabsys.VBlankLineCount; i++) {
    ctrl.info.display[i] = isEnabled(NBG0, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    ctrl.info.alpha_per_line[i] = (u8)(((~Vdp2Lines[i].CCRNA & 0x1F) * 255) / 31);
  }
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
 
  /* VDP2 Manual §4.2 CHCTLA bits 6-4 : NBG0 color number. */
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
    for (int k = 0; k < yabsys.VBlankLineCount; k++) {
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
        if (((Vdp2Lines[k].RAMCTL >> (charAddrBk << 1)) & 0x3) != 0x0) {
          needUpdate = 1;
          ctrl.info.display[k] = 0;
        }
      }
    }
    if (needUpdate != 0) {
      ctrl.info.enable = 0;
      for (int k = 0; k < yabsys.VBlankLineCount; k++) {
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
 
  ReadLineScrollData(&ctrl.info, ctrl.regs->SCRCTL & 0xFF, ctrl.regs->LSTA0.all);
  ctrl.info.lineinfo = lineNBG0;
  Vdp2GenLineinfo(&ctrl.info);
 
  if (ctrl.regs->SCRCTL & 1) {
    ctrl.info.isverticalscroll = 1;
    ctrl.info.verticalscrolltbl = (ctrl.regs->VCSTA.all & 0x7FFFE) << 1;
    ctrl.info.verticalscrollinc = (ctrl.regs->SCRCTL & 0x100) ? 8 : 4;
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
          Vdp2CheckCharAccessPenalty(char_access, ptn_access) != 0) {
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
    /* BGON: N1ON = bit 1. Also check RBG enable bits that suppress NBG1
     * (VDP2 §4.1 Table 4.1: when R0ON(4)+R1ON(5) both set, NBG screens off). */
    if ((a->BGON & 0x32) != (b->BGON & 0x32)) return 0;
 
    /* CHCTLA bits 15-9: N1CHSZ(15-14), N1BMEN(9), N1CHCN(13-12).
     * Color depth, bitmap mode and bitmap size for NBG1. */
    if ((a->CHCTLA & 0xFE00) != (b->CHCTLA & 0xFE00)) return 0;
 
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

  for (int i=0; i<yabsys.VBlankLineCount; i++) {
    ctrl.info.display[i] = isEnabled(NBG1, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    ctrl.info.alpha_per_line[i] = (u8)(((~(Vdp2Lines[i].CCRNA >> 8) & 0x1F) * 255) / 31);
  }

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
    ctrl.info.specialfunction = 0;
    ctrl.info.specialcolorfunction = (ctrl.regs->BMPNA & 0x1000) >> 4;

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
    for (int i=0; i<yabsys.VBlankLineCount; i++) {
      /* VDP2 Manual §4.1 BGON p.49: NBG1 enable bit is N1ON = BGON bit 1
       * (mask 0x02), NOT R0ON (mask 0x10).  Same copy-paste bug fixed in
       * NBG0; the per-line RAMCTL-conflict scan must key off the layer
       * actually being drawn. */
      if ((Vdp2Lines[i].BGON & 0x2)!=0) {
        if(((Vdp2Lines[i].RAMCTL>>(charAddrBk<<1))&0x3) != 0x0){
          needUpdate = 1;
          ctrl.info.display[i] = 0;
        }
      }
    }
    if (needUpdate != 0) {
      ctrl.info.enable = 0;
      for (int i=0; i<yabsys.VBlankLineCount; i++) ctrl.info.enable |= ctrl.info.display[i];
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

  /* Reduction Enable Register ZMCTL §5.2 p.129 : bits 9-8 for NBG1
   * (N1ZMQT/N1ZMHF — same encoding as bits 1-0 for NBG0).
   *   00 = no reduction              (maxzoom = 1.0)
   *   01 = up to 1/2                 (maxzoom = 0.5)
   *   10/11 = up to 1/4              (maxzoom = 0.25)
   *
   * The previous switch lacked a 'case 0' branch, leaving maxzoom
   * uninitialised on the local Vdp2Ctrl whenever NBG1 had no
   * reduction enabled — which is the most common case.  The down-
   * stream clamp `coordincx = max(coordincx, maxzoom)` then read a
   * stack-garbage value, occasionally producing visible zoom
   * artefacts on NBG1 backgrounds (especially in titles that share
   * the bitmap path between zones because the clamp ran on every
   * pixel via Vdp2DrawBitmapCoordinateInc).
   *
   * Mirror NBG0's switch which already handles case 0 explicitly. */

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
  else ctrl.info.coordincy = (float)65536 / (ctrl.regs->ZMYN1.all & 0x7FF00);

  ctrl.info.priority = (ctrl.regs->PRINA >> 8) & 0x7;

  ctrl.info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG1PlaneAddr;

  /* VDP2 Manual §4.1 p.61 (Note after color count tables):
   *   "When NBG0 is set at 16,770,000 colors, NBG1 to NBG3 can no longer
   *    be displayed."
   *
   * Only the 16,770,000 color mode (CHCTLA N0CHCN[2:0] = 100B = 4) suppresses
   * NBG1.  The 32,768 color mode (colornumber == 3 = RGB-15) does NOT
   * suppress NBG1 — the spec is explicit: NBG1 vanishes only at colornumber
   * == 4 ("32K Direct Color Mode" / 16,770,000 colors).
   *
   * The previous threshold ">= 3" wrongly killed NBG1 whenever NBG0 ran in
   * the standard 32K RGB mode (very common: Saturn games using NBG0 as a
   * full-color background plus NBG1 for HUD/text).  Symptom: NBG1 layer
   * (typically the score / HUD on RGB backgrounds) silently disappears.
   *
   * Correct threshold: == 4  (only the 16.7M color mode disables NBG1). */

  if ((ctrl.info.priority == 0) ||
      ((ctrl.regs->BGON & 0x1) && (((ctrl.regs->CHCTLA & 0x70) >> 4) == 4))) {
    return;
  }

  ReadLineScrollData(&ctrl.info, ctrl.regs->SCRCTL >> 8, ctrl.regs->LSTA1.all);
  ctrl.info.lineinfo = lineNBG1;
  Vdp2GenLineinfo(&ctrl.info);

  if (ctrl.regs->SCRCTL & 0x100) {
    ctrl.info.isverticalscroll = 1;
    if (ctrl.regs->SCRCTL & 0x1) {
      ctrl.info.verticalscrolltbl = 4 + ((ctrl.regs->VCSTA.all & 0x7FFFE) << 1);
      ctrl.info.verticalscrollinc = 8;
    }
    else {
      ctrl.info.verticalscrolltbl = (ctrl.regs->VCSTA.all & 0x7FFFE) << 1;
      ctrl.info.verticalscrollinc = 4;
    }
  }
  else ctrl.info.isverticalscroll = 0;

  /* Precompute the screen pixel rows for this zone.
   * All bitmap and linescroll paths below must draw only within
   * [screenY1, screenY2) so zones don't overwrite each other.
   * VDP2 Manual §5.1: the display area is split into segments where
   * each segment uses the register snapshot of its first scanline. */
  const int screenY1 = (_Ygl->rheight * startLine) / yabsys.VBlankLineCount;
  const int screenY2 = (_Ygl->rheight * endLine)   / yabsys.VBlankLineCount;

  if (ctrl.info.isbitmap)
  {
    if (ctrl.info.coordincx != 1.0f || ctrl.info.coordincy != 1.0f || VDPLINE_SZ(ctrl.info.islinescroll)) {
      /* Bitmap + CoordinateInc (zoom) or line-zoom path.
       * Was: hardcoded vertices [0, rheight]. Now: zone slice [screenY1, screenY2].
       * Vdp2DrawBitmapCoordinateInc() reads ctrl.info.startLine/endLine to
       * restrict its pixel-by-pixel loop to the same vertical range. */
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
        /* Bitmap + linescroll path.
         * Was: hardcoded vertices [0, rheight] and Vdp2DrawBitmapLineScroll
         * over the full screen. Now: zone slice [screenY1, screenY2].
         * Vdp2DrawBitmapLineScroll iterates 'height' rows starting from
         * vertex[1]; passing (screenY2 - screenY1) restricts it to the zone. */
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
        /* Bitmap tile path (no zoom, no linescroll).
         * Clamp the tile iteration to [screenY1, screenY2).
         * ctrl.info.y is the negative scroll offset (always <= 0 here).
         * We start yy at the first tile row that overlaps screenY1,
         * and stop as soon as yy >= screenY2. */
        int cellh = ctrl.info.cellh;
        yy = ctrl.info.y;
        /* Advance past tile rows that are entirely above screenY1. */
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
      /* Tile + linescroll path.
       * Was: hardcoded [0, rheight]. Now: zone slice [screenY1, screenY2].
       * Vdp2DrawMapPerLine iterates _Ygl->rheight rows in its inner loop;
       * the tile/pixel culling in Vdp2DrawPatternPos (vertex screen-cull)
       * discards tiles outside the visible range, but we also bound the
       * quad geometry to the zone so the GPU doesn't sample outside it. */
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
      /* Tile + standard scroll path.
       * Vdp2DrawMapTest iterates tiles over the full logical scroll space;
       * Vdp2DrawPatternPos screen-culls via vertex bounds, so tiles outside
       * [screenY1, screenY2) are naturally discarded. No geometry change
       * needed here — the tile positions are computed from scroll registers
       * and the screen culling in Vdp2DrawPatternPos handles the rest.
       * ctrl.info.startLine/endLine are set above for getPriority() lookups. */
      int delayed = 0;
      if (((ptn_access & 0x1)==0) && Vdp2CheckCharAccessPenalty(char_access, ptn_access) != 0)
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


  for (int i=0; i<yabsys.VBlankLineCount; i++) {
    ctrl.info.display[i] = isEnabled(NBG2, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    /* VDP2 Manual §12.1 CCRNB (18010AH) bits 4-0 = N2CCRT[4:0]:
     * Same encoding as NBG0/NBG1. Full 0-255 mapping. */
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
  ctrl.info.priority = ctrl.regs->PRINB & 0x7;
  ctrl.info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG2PlaneAddr;

	/* VDP2 Manual §4.1 Table 4.1 (color count) and §5.2 Table 5.2
	 * (reduction enable) jointly govern when NBG2 is hidden:
	 *
	 * Table 4.1 — NBG0 colornumber >= 010B (>= 2048 colors) -> NBG2 off
	 * Table 4.1 — NBG0 colornumber == 100B (16,770,000 colors) -> NBG1..NBG3 off
	 * Table 5.2 — reduction setting on NBG0 also restricts NBG2:
	 *
	 *   NBG0 16  colors  + NBG0 reduction up to 1/4   -> NBG2 cannot display
	 *   NBG0 256 colors  + NBG0 reduction up to 1/2   -> NBG2 cannot display
	 *   NBG0 256 colors  + NBG0 reduction up to 1/4   -> NBG2 cannot display
	 *
	 * ZMCTL bits 0/1 (N0ZMHF / N0ZMQT) encode the NBG0 reduction:
	 *   00 = none (no restriction)
	 *   01 = up to 1/2  -> kills NBG2 only when NBG0 has 256 colors
	 *   1x = up to 1/4  -> kills NBG2 for both 16- and 256-color NBG0
	 *
	 * The previous check only handled the colornumber>=2 case from Table
	 * 4.1.  Games that drove NBG0 in 16-color mode with N0ZMQT=1 (heavy
	 * horizontal reduction) would still see NBG2 rendered, which is
	 * forbidden by Table 5.2 and produces visual conflicts on real
	 * hardware (NBG2 is supposed to vanish when NBG0 reduces past these
	 * thresholds because of the VRAM access bandwidth budget). */
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
    // Setting miss of cycle patten need to plus 8 dot vertical
    if (Vdp2CheckCharAccessPenalty(char_access, ptn_access) != 0) {
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
     // Shift the draw origin so Vdp2DrawMapTest starts at the zone top.
     // The map draws from (info.y + v) where v iterates in tile steps;
     // we adjust info.y by subtracting the pixel rows above this zone.
     // The screen Y of each tile is (v - charx_equiv); we pass startLine
     // via ctrl.info.startLine so Vdp2DrawPatternPos can screen-cull tiles
     // outside [screenY1, screenY2).
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
    /* BGON: N3ON = bit 3. Also RBG suppression bits. */
    if ((a->BGON & 0x38) != (b->BGON & 0x38)) return 0;
 
    /* CHCTLB bits 5,4: N3CHCN(5)=colornumber, N3PNB(4)=pattern name size. */
    if ((a->CHCTLB & 0x0030) != (b->CHCTLB & 0x0030)) return 0;
    /* Disable conditions from §4.1 Table 4.1:
     *   NBG0 colornumber >= 2 disables NBG3. */
    if (((a->CHCTLA & 0x0070) >> 4) != ((b->CHCTLA & 0x0070) >> 4)) return 0;
    /*   NBG1 colornumber >= 2 disables NBG3. */
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

    /* MPOFN bits 14-12: N3MP[8:6] — NBG3 map offset.
     * VDP2 Manual §4 'Map Offset Register' p.85.  Same rationale as the
     * NBG2 MPOFN check — required to keep render zones in sync with
     * mid-frame bank swaps. */
    if ((a->MPOFN & 0x7000) != (b->MPOFN & 0x7000)) return 0;

    /* PLSZ bits 15-6: NBG3 plane size (bits 15-12 V, bits 11-8 not used by NBG3;
     * VDP2 §4 PLSZ: N3PLSH(7-6), N3PLSV not present — NBG3 uses bits 7-6). */
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

    /* ZMCTL bits 9-8: N1ZMQT/N1ZMHF — NBG1 horizontal reduction.
     * VDP2 Manual §5.2 Table 5.2: NBG1 reduction settings can disable
     * NBG3 (16 colors + 1/4 reduction, 256 colors + any reduction).
     * Mirror of the NBG2/NBG0 ZMCTL check — keep the zonal optimiser
     * in sync with mid-frame ZMCTL writes. */
    if ((a->ZMCTL & 0x0300) != (b->ZMCTL & 0x0300)) return 0;

    /* MZCTL bits 15-8 + bit 3: VDP2 §6.6.
     *   bit 3 = N3MZE (NBG3 mosaic enable)
     *   bits 15-8 = shared mosaic size. */
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
 
  for (int i=0; i<yabsys.VBlankLineCount; i++) {
    ctrl.info.display[i] = isEnabled(NBG3, &Vdp2Lines[i]);
    ctrl.info.enable |= ctrl.info.display[i];
    /* VDP2 Manual §12.1 CCRNB (18010AH) bits 12-8 = N3CCRT[4:0]:
     * Same encoding as NBG2. Shift right 8 to isolate, full 0-255 mapping. */
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
 
  ctrl.info.priority = (ctrl.regs->PRINB >> 8) & 0x7;
  ctrl.info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG3PlaneAddr;
 
  /* VDP2 Manual ST-058-R2:
   *   §4.1 Table 4.1 — NBG3 hidden when NBG0 colornumber >= 2 OR
   *                                     NBG1 colornumber >= 2
   *   §5.2 Table 5.2 — NBG3 also hidden by NBG1 reduction settings:
   *     NBG1 16  colors  + reduction 1/4   -> NBG3 cannot display
   *     NBG1 256 colors  + reduction 1/2   -> NBG3 cannot display
   *     NBG1 256 colors  + reduction 1/4   -> NBG3 cannot display
   *
   *   ZMCTL bit 8 = N1ZMHF (NBG1 horizontal half),
   *   ZMCTL bit 9 = N1ZMQT (NBG1 horizontal quarter).
   *
   * The previous check enforced the colornumber>=2 rules from Table 4.1
   * but ignored Table 5.2.  Mirror the NBG2 fix exactly, but on the
   * NBG1 side:
   *   - 16-color NBG1 with N1ZMQT=1  -> kill NBG3
   *   - 256-color NBG1 with any reduction -> kill NBG3 */
  if (ctrl.info.priority == 0) return;

  if (ctrl.regs->BGON & 0x1) {
    /* VDP2 Manual ST-58-R2 p.61 :
     *   « When NBG0 is set at 16,770,000 colors, NBG1 to NBG3
     *     can no longer be displayed. »
     * CHCTLA bits 6-4 (N0CHCN) :
     *   000b = 16    couleurs
     *   001b = 256   couleurs
     *   010b = 2048  couleurs   <-- désactive NBG2 seulement
     *   011b = 32768 couleurs   <-- désactive NBG2 seulement
     *   100b = 16,770,000       <-- désactive NBG1, NBG2 ET NBG3
     * Le seuil correct pour NBG3 est donc N0CHCN >= 4, pas >= 2.
     */
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
    if (Vdp2CheckCharAccessPenalty(char_access, ptn_access) != 0) delayed = 1;
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

  Vdp2ReadRotationTable(0, &rbg->paraA, rbg->ctrl.regs, Vdp2Ram);
  Vdp2ReadRotationTable(1, &rbg->paraB, rbg->ctrl.regs, Vdp2Ram);

  //rbg->paraA.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
  //rbg->paraB.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
  rbg->paraA.charaddr = (rbg->ctrl.regs->MPOFR & 0x7) * 0x20000;
  rbg->paraB.charaddr = (rbg->ctrl.regs->MPOFR & 0x70) * 0x2000;
  ReadPlaneSizeR(&rbg->paraA, rbg->ctrl.regs->PLSZ >> 8);
  ReadPlaneSizeR(&rbg->paraB, rbg->ctrl.regs->PLSZ >> 12);

  /* VDP2 Manual ST-058-R2 §6.2 'Rotation Parameter Mode Register'
   * (RPMD @ 1800B0H, p.166):
   *   bits 1-0 = RPMD[1:0]   ;  bits 15-2 are reserved (read-undefined)
   *   00B  Mode 0 - parameter A only
   *   01B  Mode 1 - parameter B only
   *   10B  Mode 2 - A/B selected by coefficient data MSB
   *   11B  Mode 3 - A/B selected by rotation parameter window
   *
   * Always mask 0x3 — the upper bits are reserved, and on real Saturn
   * hardware their value is undefined.  The previous direct equality
   * 'RPMD == 0x03' would silently miss mode 3 if any reserved bit
   * happened to read as 1.  Same reasoning applies to the
   * 'RPMD != 0' fast-path elsewhere — both have been switched to the
   * masked form. */
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
	  
	// APRÈS (spec §6.3 p.160) :
	// mode 0 = repeat → over_pattern_name ignoré, laisser à 0
	// mode 1 = pattern depuis registre OVPNRA/OVPNRB
	// mode 2 = transparent → sentinelle 0xFFFF
	// mode 3 = zone 512 transparente → sentinelle 0xFFFF (MaxH/MaxV déjà forcés à 512)
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
		// VDP2 Manual §6.3 RPMD=10B: "The rotation parameter is switched
		// between A and B for each dot based on the coefficient data of
		// rotation parameter A." ParaA's coefficient table (KTCTL bit 0 = RAKTE)
		// is read per-dot; if its MSB is set, the dot uses paraB instead.
		// ParaB does NOT have its own coefficient table in this mode.
		// The shader (prg_rbg_rpmd2_2w) implements: read paraA coef → if MSB=1
		// fallback to paraB. This requires paraA.coefenab=1 and paraB.coefenab=0.
		info->rotatenum = 0;
		info->PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
		// RAKTE (KTCTL bit 0): enables paraA coefficient table read
		rbg->paraA.coefenab = (rbg->ctrl.regs->KTCTL & 0x01) ? 1 : 0;
		// ParaB must NOT enable its own coef table in RPMD=2 (§6.3 explicit prohibition)
		rbg->paraB.coefenab = 0;
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

// APRÈS :
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

  Vdp2DrawRotation(rbg);
}


static void Vdp2DrawRBG0()
{
  int nbZone = 1;
  int lastLine = 0;
  int line;
  int max = (yabsys.VBlankLineCount >= 270)?270:yabsys.VBlankLineCount;
  RBGDrawInfo* rbg = NULL;
  for (line = 2; line<max; line++) {
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
    int line_shift = 0;
    if (_Ygl->rheight > 256) {
        line_shift = 1;
    } else {
        line_shift = 0;
    }

    u32 * linebuf = YglGetPerlineBuf();
    for (int line = 0; line < _Ygl->rheight; line++) {
        Vdp2 * lVdp2Regs = &Vdp2Lines[line >> line_shift];

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
        int msb_shadow_enabled = (!spwinen) && (sptype >= 2) && (sptype <= 7);
        int spcccs  = (lVdp2Regs->SPCTL >> 12) & 3;
        int spccn   = (lVdp2Regs->SPCTL >> 8) & 7;
        int spccen  = (lVdp2Regs->CCCTL >> 6) & 1;

        /* VDP2 Manual §9.2: RGB sprite data always selects priority register 0.
         * SPCLMD = SPCTL bit 5: 1 = RGB format, 0 = palette format. */
        int spclmd = (lVdp2Regs->SPCTL >> 5) & 1;
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
        /* VDP2 Manual §9.2: encode spcccs (bits 25-24), spccn (bits 28-26),
         * spccen (bit 29) dans les bits 31-24 du slot SPRITE.
         * Le shader lira cramindex du pixel courant et calculera
         * color_data_msb lui-même pour SPCCCS=3. */
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
  /* VDP1 Manual §6.7 p.105: local coordinate values are 11-bit signed;
   * bits 15-11 of CMDXA/CMDYA are sign-extension of bit 10, and the
   * hardware ignores them, sign-extending from bit 10 regardless.
   *
   * CMDXA/YA were read from VRAM as u16 (Vdp1ReadCommand) and stored
   * in the s32 struct field without sign extension. For negative
   * origins (e.g. -16 = 0xFFF0 in VRAM) the raw value becomes
   * +65520 here, and every downstream sprite coordinate is pushed
   * far off-screen. Sign-extend from bit 10 to recover the value. */
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

  if (Vdp2Regs->TVMD & 0x8000) {
    VIDCSVdp2DrawScreens();
    screenDirty = 1;
    vdp2busy = 1;
  } else {
    VIDCSVdp2DispOff();
    if (screenDirty != 0)
      vdp2busy = 1;
    screenDirty = 0;
  }

  /* It would be better to reset manualchange in a Vdp1SwapFrameBuffer
  function that would be called here and during a manual change */
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

  if (Vdp1Regs->TVMR & 0x02) {
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

  // Vertical Resolution
	/* VDP2 User's Manual ST-058-R2 §2.1 (TVMD VRESO[1:0], bits 5-4):
	 *   00  224 lines    NTSC or PAL format TV
	 *   01  240 lines    NTSC or PAL format TV
	 *   10  256 lines    PAL format TV only
	 *   11  Not Allowed  (reserved)
	 */

	switch ((TVMD >> 4) & 0x3)
	{
	case 0:
	  height = 224;
	  break;
	case 1:
		/* §2.1: 240 lines — same on NTSC and PAL.  Do NOT branch on
		 * yabsys.IsPal here; the previous '256/240' fork came from a
		 * misreading of the table (the 256-line entry is VRESO=10,
		 * not VRESO=01). */
		height = 240;
		break;
	case 2:
	  /* §2.1: VRESO=10 - 256 lines, PAL format TV ONLY.
	   * On NTSC hardware this setting is undefined, but every real
	   * BIOS-tested behaviour returns 256 because the line counter
	   * uses the VRESO field directly.  Returning 256 unconditionally
	   * is the safest cross-region choice and matches every other
	   * reasonably accurate Saturn emulator. */
	  height = 256;
	  break;
	case 3:
	  /* §2.1: VRESO=11 'Not Allowed'.
	   * Some Japanese homebrew/test ROMs poke this value to validate
	   * emulator robustness.  Pick a safe non-zero fallback so the
	   * GL pipeline never gets a zero height.  Use the region default
	   * (224 NTSC / 256 PAL) - it matches what most real Saturn boards
	   * produce by latching the previous valid VRESO setting at boot. */
	  height = yabsys.IsPal ? 256 : 224;
	  break;
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

  /* VDP1 User's Manual ST-013-R3 §4.1 Table 4.2 'Screen Modes':
   *   TVM[2:0]  Mode                Bit width  FB size     Interlace
   *   000       Normal              16 bpp     512 H 256 V single/dbl
   *   001       High Resolution      8 bpp    1024 H 256 V single/dbl
   *   010       Rotation 16         16 bpp     512 H 256 V single only
   *   011       Rotation 8           8 bpp     512 H 512 V single only
   *   100       HDTV                16 bpp     512 H 256 V non-interlace
   *
   * TVMR bit 0 (the bit-depth selector inside TVM) selects 8 vs 16 bpp.
   * High Resolution (TVM=001) widens the frame buffer to 1024 H => density
   * x2.  Rotation 8 (TVM=011) keeps 512 H but doubles the height to 512 V
   * => density x2 in the vertical axis. */
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
    /* VDP1 §4.1 Table 4.2: Rotation 8 frame buffer is 512 H x 512 V.
     * Treating it as 512 H x 256 V (the previous default) caused the
     * lower half of the FB to be sampled at half-resolution, visible as
     * vertical squashing of rotation backgrounds in titles that use
     * TVM=011 (notably for 8 bpp rotation effects). */
    vdp1w = 1;
    vdp1h = 2;
    break;
  default:
    /* Reserved TVM values (5,6,7) - 'Setting not allowed' per §4.1.
     * Fall back to the safest non-disruptive defaults. */
    vdp1w = 1;
    vdp1h = 1;
    break;
  }

  /* VDP1 §4.2 FBCR DIE bit (FBCR & 0x8): double-density interlace.
   * Per §4.1 Table 4.2 double-interlace is *only* permitted for
   * TVM=000 and TVM=001 — when active in those modes the host display
   * doubles the vertical density. */
  if (Vdp1Regs->FBCR & 0x8) {
    vdp1h = 2;
    vdp1_interlace = (Vdp1Regs->FBCR & 0x4) ? 2 : 1;
  }
  else {
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
    // VDP2 Manual §3.4: mode 0 = 1024 colors, CRAM mirrored (index wraps at 0x3FF)
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
    //Line color offset from rotation table might be applicable here
    if (offset != 0) colorindex = (colorindex&flag) | (offset&0x7F);
    colorindex <<= 1;
    tmp = T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
    return SAT2YAB1(alpha, tmp);
  }
  // APRÈS — vérifier que tmp1 fournit bien le composant R (bits 7-0 de tmp1)
  // VDP2 Manual §4.4 : mot0 = [msb|0|R(7-0)|0|G(7-3)], mot1 = [G(2-0)|B(7-0)|0...]
  // SAT2YAB2 doit recevoir les deux mots dans l'ordre correct
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

	/* VDP2 Manual §14.1 Figure 14.3 (Normal Shadow):
	 * The Normal Shadow code is the value where every DC (Dot Color)
	 * bit is set to 1 except bit 0.  Each sprite type exposes a
	 * different number of DC bits in the frame-buffer word — see
	 * Figure 9.1 in §9.1 'Sprite Data':
	 *
	 *   Type 0,1,2,3,5  : 11 DC bits (DC10..DC0)  -> shadow = 0x7FE
	 *   Type 4,6        : 10 DC bits (DC9..DC0)   -> shadow = 0x3FE
	 *   Type 7          :  9 DC bits (DC8..DC0)   -> shadow = 0x1FE
	 *   Type 8          :  7 DC bits (DC6..DC0)   -> shadow = 0x07E
	 *   Type 9,A,B      :  6 DC bits (DC5..DC0)   -> shadow = 0x03E
	 *   Type C,D,E,F    :  6 DC bits (DC5..DC0) on the FB itself.
	 *                     DC6..DC7 exist as 'shared' bits that re-use
	 *                     the priority/CC slots; they DO NOT count for
	 *                     Normal Shadow detection because the FB only
	 *                     stores 6 DC bits for these types (the shared
	 *                     bits live in CRAM, not in the per-pixel FB).
	 *                     -> shadow = 0x03E
	 *
	 * So the dc_mask is the FB-resident DC width, not the maximum
	 * theoretical DC width via shared bits — only the bits actually
	 * stored in the frame buffer participate in Normal Shadow detection.
	 */

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
		case 9: case 10: case 11: /* 9, A, B : 6 DC bits */
		case 12: case 13: case 14: case 15: /* C, D, E, F : 6 DC bits in FB */
			dc_mask = 0x3F; /* 6 DC bits */
			break;
		default:
			return 0;
		}

		/* Normal shadow: all DC bits = 1 except bit 0 = 0 */
		u32 shadow_val = dc_mask & ~1u; /* e.g. 0x7FE for 11-bit */
		return (dc_bits & dc_mask) == shadow_val;
	}
	
 /* Vdp2GetSpriteShadowBit — VDP2 Manual §14.1 MSB Shadow / §9.1 Figure 9.1
  * Returns the SD (shadow) bit from a raw 16-bit sprite frame-buffer word.
  * SD is at bit 15 for types 2-7 (16-bit pixel types with shadow support).
   * Types 0,1 and 8-F have no SD bit; returns 0.
  * When SPWINEN=1, bit 15 is sprite-window, not shadow; MSB shadow disabled. */
  
static INLINE int Vdp2GetSpriteShadowBit(u16 sprite_word, int sptype, int spwinen) {
	  if (spwinen)                    return 0;  /* bit 15 = sprite window */
	  if (sptype >= 2 && sptype <= 7) return (sprite_word >> 15) & 1;
	  return 0;
	}
	
 /* Vdp2GetSpriteCCEnable — VDP2 Manual §9.2
  * @param priority_number  3-bit sprite character priority (from PR bits)
  * @param color_data_msb   MSB of CRAM color entry (1=enable for SPCCCS=3)
  * @param spcccs           SPCTL bits 13-12: condition selector
  * @param spccn            SPCTL bits 10-8: condition number (0-7)
  * @param spccen           CCCTL bit 6: master CC enable for sprites */
  
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

/* Vdp2ApplyMSBShadow — apply half-luminance to a scroll-screen RGB pixel.
 * Called when MSB shadow condition is met (SD=1 and scroll MSB=1).
 * VDP2 §14.1: processing order is after CC and after color offset. */
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

  // VDP2 Manual §8: Win_op bit 0=OR, 1=AND
  // Vdp2CheckWindowRange: use_and = (Win_op[id] != 0)  ← correct
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


  for (int i=0; i<enBGMAX+1; i++) {
    if (Win0[i] != _Ygl->Win0[i]) _Ygl->needWinUpdate |= 1;
    if (Win1[i] != _Ygl->Win1[i]) _Ygl->needWinUpdate |= 1;
    if (WinS[i] != _Ygl->WinS[i]) _Ygl->needWinUpdate |= 1;
    if (Win0_mode[i] != _Ygl->Win0_mode[i]) _Ygl->needWinUpdate |= 1;
    if (Win1_mode[i] != _Ygl->Win1_mode[i]) _Ygl->needWinUpdate |= 1;
    if (WinS_mode[i] != _Ygl->WinS_mode[i]) _Ygl->needWinUpdate |= 1;
    if (Win_op[i] != _Ygl->Win_op[i]) _Ygl->needWinUpdate |= 1;
  #ifdef WINDOW_DEBUG
    //DEBUG
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
    // start address
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
    // Parameter Mode
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
    // start address
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
    // Parameter Mode
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

  // if (_Ygl->interlace == DOUBLE_INTERLACE) y >>= 1;

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
    // outside
  }
  else {
    if (win[y] == 0) return 1;
    if (x < upLx) return 1;
    if (x > upRx) return 1;
    return 0;
  }
  return 0;
}

// Helper : le segment [x, x+w] intersecte-t-il la fenêtre sur la ligne ly ?
static INLINE int Vdp2CheckWindowLine(vdp2draw_struct *info,
                                       int x, int ly, int w,
                                       int area, u32 *win)
{
  if (ly < 0 || ly >= _Ygl->rheight) return 0;

  int wl = win[ly] & 0xFFFF;
  int wr = (win[ly] >> 16) & 0xFFFF;

  // win[ly] == 0x000000FF means END < START → window invalid/empty
  if (win[ly] == 0x000000FF) {
    // Fenêtre invalide (END < START)
    // WA_INSIDE → rien n'est dans une fenêtre vide → 0
    // WA_OUTSIDE → tout est hors d'une fenêtre vide → 1
    return (area == WA_OUTSIDE) ? 1 : 0;
  }

  if (area == WA_INSIDE) {
    // Le tile doit avoir au moins un pixel DANS la fenêtre pour être visible
    return (x <= wr && (x + w - 1) >= wl);
  } else {
    // WA_OUTSIDE : le tile doit avoir au moins un pixel EN DEHORS de la fenêtre
    return (x < wl || (x + w - 1) > wr);
  }
}


/*
 * Vdp2CheckSpriteWindow - check if a VDP2 screen coordinate is inside/outside
 * the sprite window (WinS), which is defined by the MSB of VDP1 framebuffer pixels.
 *
 * VDP2 Manual §8.2: The sprite window uses the MSB of each VDP1 sprite pixel.
 * A pixel belongs to the sprite window when its VDP1 framebuffer MSB = 1.
 *
 * Returns 1 if the pixel satisfies the window condition (inside or outside
 * depending on WinS_mode), 0 otherwise.
 */
static INLINE int Vdp2CheckSpriteWindow(int id, int vdp2x, int vdp2y)
{
    /* WinS not enabled for this layer */
    if (_Ygl->WinS[id] == 0) return 0;

    /* Map VDP2 screen coordinate → VDP1 framebuffer coordinate.
     * The same ratio used by the compositor shader:
     *   vdp1x = vdp2x * (vdp1ratio * vdp1wdensity / vdp2wdensity) * (rwidth / vdp1width)
     * Simplified using the pre-computed densities stored in _Ygl.
     */
    float ratioX = _Ygl->vdp1ratio * _Ygl->vdp1wdensity / _Ygl->vdp2wdensity
                   * (float)_Ygl->rwidth  / (float)_Ygl->vdp1width;
    float ratioY = _Ygl->vdp1ratio * _Ygl->vdp1hdensity / _Ygl->vdp2hdensity
                   * (float)_Ygl->rheight / (float)_Ygl->vdp1height;

    int fbx = (int)(vdp2x * ratioX);
    int fby = (int)(vdp2y * ratioY);

    /* Clamp to VDP1 framebuffer bounds */
    if (fbx < 0) fbx = 0;
    if (fby < 0) fby = 0;
    if (fbx >= _Ygl->vdp1width)  fbx = _Ygl->vdp1width  - 1;
    if (fby >= _Ygl->vdp1height) fby = _Ygl->vdp1height - 1;

    /* Read from the CPU-side VDP1 framebuffer copy.
     * vdp1fb_read_buf[readframe] is populated by vdp1_read() / vdp1_read_gl().
     * Each pixel is RGBA u8: R=color&0xFF, G=(color>>8)&0xFF, B=0, A=0.
     * MSB of the 16-bit VDP1 color = bit 15 = bit 7 of the G byte.
     */
    u32 *fb = _Ygl->vdp1fb_read_buf[_Ygl->readframe];
    if (fb == NULL) return 0;

    u32 pixel = fb[fby * _Ygl->vdp1width + fbx];
    /* Extract G channel (bits 15-8 of the original 16-bit color) */
    u8 g = (pixel >> 8) & 0xFF;
    int msb = (g >> 7) & 1;   /* bit 15 of the original VDP1 color */

    /* msb=1 → pixel is inside the sprite window */
    int inside = msb;

    if (_Ygl->WinS_mode[id] == WA_INSIDE) {
        /* Drawing inside: visible when inside the sprite window */
        return inside;
    } else {
        /* Drawing outside: visible when outside the sprite window */
        return !inside;
    }
}


/* vidcs.c — Vdp2CheckWindowRange()
 * VDP2 Manual §8.1: Window area is applied per-dot. In line window mode
 * (LWTA0/LWTA1 bit 15 = 1, §8.2 LWTA register), horizontal bounds change
 * each line. Testing only 4 tile corners misses cases where a tile is
 * entirely inside or outside the window on intermediate lines.
 *
 * Fix: sample all lines of the tile (y, y+1, ..., y+h-1) at tile left and
 * right edges, and return 1 (draw) if any sample satisfies the window
 * condition. This is accurate for both rectangular and line window modes.
 *
 * VDP2 Manual §8.1: "If the start coordinate of either the horizontal or
 * vertical direction is larger than the end coordinate, the whole screen
 * is considered outside the window."
 */
static int FASTCALL Vdp2CheckWindowRange(Vdp2Ctrl *ctrl, int x, int y, int w, int h)
{
    int id = ctrl->info.idScreen;
    int useW0 = (_Ygl->Win0[id] != 0);
    int useW1 = (_Ygl->Win1[id] != 0);
    int useWS = (_Ygl->WinS[id] != 0);

    if (!useW0 && !useW1 && !useWS) return 0;

    int use_and = (_Ygl->Win_op[id] != 0);

    /* VDP2 Manual §8.2 line window: sample every line of the tile at
     * left (x) and right (x+w) edges to handle per-line window bounds. */
    for (int ly = y; ly < y + h; ly++) {
        /* Test left and right edge of tile at this screen line */
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

/* vidcs.c — Vdp2GenLineinfo()
 * VDP2 Manual §5.3 Figure 5.4/5.5: line scroll table entry layout.
 * Each enabled field occupies exactly 4 bytes (integer word + fractional word).
 * Fields are stored contiguously in the order: H-scroll, V-scroll, H-coord-inc.
 * 'bound' = total bytes per table entry = 4 × (number of enabled fields).
 * The table index for screen line i is: floor(i / lineinc) × bound
 * (lineinc = patternpixelwh when in tile mode, 1 when per-line).
 */
static void Vdp2GenLineinfo(vdp2draw_struct *info)
{
    int bound = 0;
    int i;
    u16 val1, val2;
    int index = 0;
    if (info->lineinc == 0 || info->islinescroll == 0) return;

    /* §5.3 Figure 5.4: each active field = 4 bytes (2 + 2) */
    if (VDPLINE_SX(info->islinescroll)) bound += 4;
    if (VDPLINE_SY(info->islinescroll)) bound += 4;
    if (VDPLINE_SZ(info->islinescroll)) bound += 4;

    int height = _Ygl->rheight;

    for (i = 0; i < height; i++) {
        /* §5.3 Figure 5.5: table entry index advances once per lineinc lines */
        int table_entry = i / info->lineinc;
        int byte_offset = table_entry * bound;
        int field_off = 0;
        index = 0;

        if (VDPLINE_SX(info->islinescroll)) {
            /* §5.3 H-scroll: 11-bit integer (+0H) + 8-bit fractional (+2H).
             * Read both, sign-extend the integer at bit 10, apply the
             * fractional part as a half-pixel rounding bias (see header
             * comment for rationale). */
             val1 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off);
             val2 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off + 2);
             s32 ival = (s32)(val1 & 0x07FF);

             if (val1 & 0x0400) ival -= 0x0800;     /* sign-extend bit 10 */
            /* val2 bits 15-8 = fractional; round half-up in the direction
             * of the sign so positive 0.5 -> +1, negative 0.5 -> -1. */
             if ((val2 & 0xFF00) >= 0x8000) ival += (ival >= 0) ? 1 : -1;
             info->lineinfo[i].LineScrollValH = (s16)ival;
            field_off += 4;
        } else {
            info->lineinfo[i].LineScrollValH = 0;
        }

        if (VDPLINE_SY(info->islinescroll)) {
            /* §5.3 V-scroll: same layout as H-scroll, read+round identically. */
            val1 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off);
            val2 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off + 2);
            s32 ival = (s32)(val1 & 0x07FF);
            if (val1 & 0x0400) ival -= 0x0800;     /* sign-extend bit 10 */
            if ((val2 & 0xFF00) >= 0x8000) ival += (ival >= 0) ? 1 : -1;
            info->lineinfo[i].LineScrollValV = (s16)ival;
            field_off += 4;
        } else {
            info->lineinfo[i].LineScrollValV = 0;
        }

        if (VDPLINE_SZ(info->islinescroll)) {
            /* §5.3 H coord-increment: 3-bit integer (+0H bits 2-0) plus
             * 8-bit fractional (+2H bits 15-8) packed as 3.8 fixed-point.
             * raw 0x0100 = 1.0 = no zoom; raw > 0x0100 = reduction;
             * raw < 0x0100 = expansion. Used downstream as
             *   coordincx = 1.0 / (raw / 256.0)
             * to invert into the renderer's "UV-per-screen-pixel" form. */
            val1 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off);
            val2 = Vdp2RamReadWord(NULL, Vdp2Ram, info->linescrolltbl + byte_offset + field_off + 2);
            info->lineinfo[i].CoordinateIncH = (((int)(val1 & 0x07) << 8) | (int)(val2 >> 8));


            field_off += 4;
        } else {
            info->lineinfo[i].CoordinateIncH = 0x0100;
			info->lineinfo[i].CoordinateIncH = 0x0100;  /* 1.0 = no zoom */
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
		// VDP2 Manual §9.4: Color Calculation Window (CCW) restricts color
		// calculation to specific screen regions. CCW data is in WCTLD bits 15-8.
		// CCW index in window arrays is SPRITE+1.
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
		  /* VDP2 §9.4: CC not performed INSIDE the CCW — return 1 = "do CC here" */
		  return !in_ccw;
	}

static INLINE u32 Vdp2GetCCOn(Vdp2Ctrl *ctrl, u8 dot, u32 cramindex) {
  /* VDP2 Manual §12.1 CCCTL (1800ECH) bit 8 = CCMD:
   *   CCMD=0  "Rate mode": out = top*(ratio/32) + 2nd*((32-ratio)/32)
   *   CCMD=1  "Add mode":  out = saturate(top + 2nd)
   * Both modes use the same dot-level enable logic (§12.3 SFCCMD/SFSEL).
   * cc=0 → do NOT color-calculate this dot (write top pixel as-is). */
  int cc = 1;
  switch (ctrl->info.specialcolormode) {
  case 0: /* always CC */ break;
  case 1:
    /* VDP2 §12.3 mode 1: CC only when pattern-name special-CC bit = 1 */
    if (ctrl->info.specialcolorfunction == 0) cc = 0;
    break;
  case 2:
    /* VDP2 §12.3 mode 2: special-CC bit AND special code matches nibble */
    if (ctrl->info.specialcolorfunction == 0) {
      cc = 0;
    } else if ((ctrl->info.specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
      cc = 0;
    }
    break;
  case 3:
    /* VDP2 §12.3 mode 3: CRAM MSB is CC-enable flag.
     * Only valid for palette format (colornumber<3). For RGB pixels,
     * cramindex is a direct color value, NOT a CRAM address — skip read. */
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

/* Vdp2GetPixel16bppbmp - 16-bit RGB-555 bitmap pixel fetch
 *
 * VDP2 User's Manual ST-058-R2:
 *   §4.3 Table 4.3 - RGB format dot bit layout:
 *      bit 15      = transparent / opaque flag (0 = transparent dot,
 *                    1 = displayed dot when SPD-equivalent is active)
 *      bits 14-10  = 5-bit Blue
 *      bits  9- 5  = 5-bit Green
 *      bits  4- 0  = 5-bit Red
 *   §12.3  Special Color Calculation Function, mode 3:
 *      "the most significant bit of color RAM data becomes the color
 *       calculation enable bit"
 *      For RGB-format pixels there is no CRAM lookup, but the spec
 *      generalises this to "the most significant bit of the COLOR
 *      DATA" - i.e. dot bit 15.  This is exactly the same physical
 *      bit as the transparency flag, used for a different purpose
 *      depending on whether the pixel is transparent.
 *
 * Special Color Calculation mode 2 inspects the lower nibble of the
 * dot value (specialcode bitmap).  For RGB pixels the lower 4 bits
 * are the red LSBs; passing them through preserves the documented
 * mode-2 behaviour (and matches the previous implementation).
 */


static INLINE u32 Vdp2GetPixel16bppbmp(Vdp2Ctrl *ctrl, u32 addr) {
  u32 color;
  u16 dot = Vdp2RamReadWord(NULL, Vdp2Ram, addr);

  /* VDP2 §4.3 Table 4.3: RGB format transparent when bit 15 = 0 */

  if (!(dot & 0x8000) && ctrl->info.transparencyenable) return 0x00000000;

  /* §12.3 mode 3: for RGB pixels the CC-enable flag is dot bit 15 itself.
   * Vdp2GetCCOn() takes a 'cramindex' argument and feeds it to
   * Vdp2ColorRamGetColorRaw() in mode 3.  We can't supply a meaningful
   * CRAM index for an RGB pixel, but the previous code passed 0, which
   * makes mode 3 always read CRAM[0] (a value unrelated to the actual
   * pixel).  Short-circuit the result here using the dot's own MSB:
   *
   *   - mode 0/1/2 : Vdp2GetCCOn handles correctly via the 'dot' arg
   *   - mode 3     : we override with the bit 15 of dot
   *
   * Color number passed unchanged to Vdp2GetCCOn so its colornumber-based
   * path discriminates correctly. */
  int cc;
  if (ctrl->info.specialcolormode == 3) {
    /* Direct bit-15 test - matches §12.3 wording 'MSB of color data' */
    cc = (dot & 0x8000) ? 1 : 0;
  } else {
    cc = Vdp2GetCCOn(ctrl, (u8)(dot & 0xF), 0);
  }
  color = VDP2COLOR(ctrl->info.idScreen, ctrl->info.alpha,
                    ctrl->info.priority, cc, RGB555_TO_RGB24(dot));
  return color;
}

/* Vdp2GetPixel32bppbmp - 32-bit RGB-888 bitmap pixel fetch.
 *
 * VDP2 User's Manual ST-058-R2 §4.4 RGB Format Dot Data:
 *   "16,770,000 colors are designated by RGB 8-bit; 32 bits are used
 *    (only the MSB and the lower 24 bits are used)."
 *
 * 32-bit dot layout (the two consecutive u16 words 'dot1' / 'dot2'):
 *   word @ +0 (dot1):  bit 15      = transparent flag (0=transparent dot)
 *                      bits 14- 8  = reserved (don't care)
 *                      bits  7- 0  = R (8 bits)
 *   word @ +2 (dot2):  bits 15- 8  = G (8 bits)
 *                      bits  7- 0  = B (8 bits)
 *
 * Special Color Calculation mode 3 (§12.3):
 *   For palette pixels the CC-enable flag is the MSB of CRAM data.
 *   For RGB pixels there is no CRAM lookup; the spec generalises to
 *   "MSB of color data" — for 32-bit RGB that is dot1 bit 15 (the
 *   same physical bit as the transparency flag, used differently).
 *
 * Mode 0/1/2 Vdp2GetCCOn() expects a meaningful 'dot' nibble for its
 * mode-2 specialcode bitmap test.  For RGB-888 there is no useful
 * nibble; pass the LSBs of B (dot2 & 0xF) which preserves the
 * documented mode-2 semantics on the LSBs of the color code.
 */

static INLINE u32 Vdp2GetPixel32bppbmp(Vdp2Ctrl *ctrl, u32 addr) {
  u32 color;
  u16 dot1, dot2;
  int cc;
  dot1 = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
  dot2 = Vdp2RamReadWord(NULL, Vdp2Ram, addr+2);

  cc = Vdp2GetCCOn(ctrl, 0, 0);
  
  /* §4.4: transparent when dot1 bit 15 = 0 */
  if (!(dot1 & 0x8000) && ctrl->info.transparencyenable) return 0x00000000;

  /* §12.3 mode 3: short-circuit using dot1 bit 15.  Modes 0/1/2 use
   * the standard Vdp2GetCCOn() path with the B LSB nibble as the
   * specialcode-test dot value (mirrors the 16bpp helper). */
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
  if ((idx>>shift) > yabsys.VBlankLineCount) idx = yabsys.VBlankLineCount<<shift;
  return info->alpha_per_line[idx>>shift];
}

static INLINE int isVramAccessible(Vdp2Ctrl *ctrl, u32 addr) {
    /* VDP2 Manual ST-058-R2 §3.1 'VRAM Mode Bit (VRBMD, VRAMD)' p.45:
     *   RAMCTL bit 8  = VRAMD (VRAM-A bank partition select)
     *   RAMCTL bit 9  = VRBMD (VRAM-B bank partition select)
     *
     *   00B = single bank (256 KB), full bank as one slot
     *   01B = two banks  (128 KB each, A0+A1 or B0+B1)
     *
     * AC_VRAM[bank][timeslot] uses the same partitioning.
     *
     * Previous code read VRBMD from bit 12 (which is CRMD0, the
     * color-RAM-mode bit).  When CRMD0 happened to be 1 (color RAM
     * mode 1, 2048-color RGB) the function would treat VRAM-B as
     * partitioned even when VRBMD was 0; conversely, when VRBMD was 1
     * but CRMD0 was 0, the function would miss the partition entirely.
     *
     * Effect on the renderer: bank-index lookups for VRAM-B were keyed
     * off the wrong bit.  In titles using both color-RAM mode 1 AND a
     * partitioned VRAM-B, the bug was self-cancelling and invisible;
     * in the more common case of color-RAM mode 0 or 2 with VRBMD=1,
     * the bug reported the wrong bank for any address >= 0x40000,
     * causing AC_VRAM lookups to consult the wrong bank's cycle
     * pattern — manifesting as occasional 'wrong tiles in the right-
     * hand half of NBG2/NBG3' when those layers' character/pattern
     * data lived in VRAM-B1 (= addresses 0x60000-0x7FFFF). */
    int vrama_split = (ctrl->regs->RAMCTL >> 8) & 0x1; /* §3.1 VRAMD */
    int vramb_split = (ctrl->regs->RAMCTL >> 9) & 0x1; /* §3.1 VRBMD */

    addr &= 0x7FFFF;

    int bank;
    if (addr < 0x40000) {
        /* VRAMA : 0x00000–0x3FFFF */
        if (vrama_split) {
            /* partitionnée : A0=0x00000–0x1FFFF, A1=0x20000–0x3FFFF */
            bank = (addr < 0x20000) ? 0 : 1;
        } else {
            /* non partitionnée : une seule banque A = index 0 */
            bank = 0;
        }
    } else {
        /* VRAMB : 0x40000–0x7FFFF */
        if (vramb_split) {
            /* partitionnée : B0=0x40000–0x5FFFF, B1=0x60000–0x7FFFF */
            bank = (addr < 0x60000) ? 2 : 3;
        } else {
            /* non partitionnée : une seule banque B = index 2 */
            bank = 2;
        }
    }

    if (bank > 3) return 0;
    return ctrl->info.char_bank[bank];
}

static void FASTCALL Vdp2DrawCell_in_sync(Vdp2Ctrl *ctrl)
{
  int i, j;
	//   if ((vdp2_interlace == 1) && (_Ygl->rheight > 448)) {
	//     // Weird... Partly fix True Pinball in case of interlace only but it is breaking Zen Nihon Pro Wres, so use the bad test of the height
	//     Vdp2DrawCellInterlace(info, texture, ctrl->regs);
	//     return;
	//   }
  /* Wrap et accessibilité VRAM : uniquement en mode bitmap.
   * En mode tile, le filtrage est fait en amont dans Vdp2DrawMapTest
   * via char_bank[]. Appliquer isVramAccessible aux tiles causerait
   * des faux positifs (tiles dont le charaddr tombe dans une banque
   * non déclarée accessible pour ce layer mais valide via pattern name). */
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

/* Vdp2DrawBitmapLineScroll — render a bitmap-format NBG with per-line scroll.
 *
 * VDP2 User's Manual ST-058-R2:
 *   §4.9  Bitmap mode (cellw/cellh = 256/512 powers of two, p.93-95)
 *   §5.3  Line scroll function (p.131-133)
 *   §5.1  Screen Scroll Function (p.124-126, scroll wrap behaviour)
 *
 * The renderer splits the screen into vertical "zones" — contiguous spans
 * of scanlines over which all VDP2 registers relevant to the layer are
 * unchanged.  Each zone is drawn by a single call into this function.
 *
 * 'i' in the loop is the row offset WITHIN THE ZONE (0..height-1),
 * but the zone may start somewhere other than line 0.  Two state
 * lookups must therefore be re-anchored on the absolute screen line:
 *
 *   (a) lineinfo[] is filled by Vdp2GenLineinfo() over [0, _Ygl->rheight)
 *       — always indexed by absolute screen line.
 *   (b) The "no V-scroll" fallback for sv must use the absolute line,
 *       because §5.1 says vertical scroll is screen-line-relative.
 *
 * The previous implementation read lineinfo[i] and used 'i' as the
 * vertical delta, so any zone that did NOT start at line 0 would
 * sample the wrong line-scroll values and the wrong vertical offset.
 * Symptoms: visible "mid-screen jump" of NBG0/NBG1 bitmap layers in
 * titles whose line-scroll register set changes mid-frame (Sega Rally
 * road horizon, Panzer Dragoon Saga water, Burning Rangers fire).
 */

static void FASTCALL Vdp2DrawBitmapLineScroll(Vdp2Ctrl *ctrl, int width, int height)
{
  int i, j;
  int shift = 0;
  if (_Ygl->interlace == DOUBLE_INTERLACE) shift = 1;

  /* Anchor the zone on its absolute screen line.  startLine == 0 for the
   * legacy single-zone case — backward-compatible. */
  const int zone_start = ctrl->info.startLine;



  for (i = 0; i < height; i++)
  {
    int sh, sv;
    u32 baseaddr;
    vdp2Lineinfo * line;
    const int absline = zone_start + i;

    /* alpha_per_line is also indexed by absolute screen line, see
     * VIDCSReadColorOffset() which fills it over the full frame. */
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
      /* §5.1: vertical scroll = base SCYIN value + screen line.
       * Use absline instead of 'i' so the zone's vertical offset is
       * preserved across split points.  Without this, every new zone
       * resets the vertical mapping to line 0. */
        sv = absline + ctrl->info.sv;

     /* §5.1 wrap: bitmap dimensions are powers of 2 (cellw, cellh ∈
     * {256, 512, 1024}), so a bit-AND with (size-1) is equivalent to
     * a positive modulo for two's-complement integers — the wrap is
     * correct even for negative sh/sv (which can happen when
     * LineScrollValH/V is sign-extended at bit 10).
     *
     * The previous code carried an additional adjustment:
     *   if (LineScrollValH >= 0 && LineScrollValH < sh && sv > 0)
     *       sv -= 1;
     * which has no basis in the manual — H scroll value should not
     * influence V scroll mapping.  It was an empirical fix for a
     * 1-pixel mis-alignment that no longer manifests once the zone
     * indexing above is corrected; remove it. */
    sv &= (ctrl->info.cellh - 1);
    sh &= (ctrl->info.cellw - 1);

    switch (ctrl->info.colornumber) {
    case 0:
      baseaddr += (((sh + sv * ctrl->info.cellw) >> 2) << 1);
      for (j = 0; j < width; j += 4)
      {
        Vdp2GetPixel4bpp(ctrl, baseaddr);
        baseaddr += 2;
      }
      break;
    case 1:
      baseaddr += sh + sv * ctrl->info.cellw;
      for (j = 0; j < width; j += 2)
      {
        Vdp2GetPixel8bpp(ctrl, baseaddr);
        baseaddr += 2;
      }
      break;
    case 2:
      baseaddr += ((sh + sv * ctrl->info.cellw) << 1);
      for (j = 0; j < width; j++)
      {
        *ctrl->texture.textdata++ = Vdp2GetPixel16bpp(ctrl, baseaddr);
        baseaddr += 2;

      }
      break;
    case 3:
      baseaddr += ((sh + sv * ctrl->info.cellw) << 1);
      for (j = 0; j < width; j++)
      {
        *ctrl->texture.textdata++ = Vdp2GetPixel16bppbmp(ctrl, baseaddr);
        baseaddr += 2;
      }
      break;
    case 4:
      baseaddr += ((sh + sv * ctrl->info.cellw) << 2);
      for (j = 0; j < width; j++)
      {
        *ctrl->texture.textdata++ = Vdp2GetPixel32bppbmp(ctrl, baseaddr);
        baseaddr += 4;
      }
      break;
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
 
  /* Constantes de wrap bitmap (cellw et cellh sont puissances de 2, §4.3). */
  const int cellw = ctrl->info.cellw;
  const int cellh = ctrl->info.cellh;
  const int cellw_mask = cellw - 1;
  const int cellh_mask = cellh - 1;
 
#ifdef SHELLSHOCK_DEBUG
  /* Log une fois par appel zone pour confirmer les paramètres effectifs. */
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
    /* VDP2 Manual §5.3 Figure 5.5 'Line Scroll Table' (p.133):
     * Vdp2GenLineinfo() fills lineinfo[] over [0, _Ygl->rheight),
     * indexed by ABSOLUTE screen line, and applies the per-line
     * /lineinc grouping internally (line at absline reads
     * table_entry = absline / lineinc).
     *
     * The previous code divided 'i' by lineinc here too, producing
     * a DOUBLE division and reading lineinfo[i/lineinc] — i.e. the
     * data that lineinfo[i/lineinc] was supposed to mirror, not the
     * data for screen line i.  For lineinc > 1 the read was off by
     * up to (lineinc-1)*N lines and could even go out of bounds for
     * upscaled rheights (e.g. i=400 with lineinc=2 -> index 200,
     * which is fine for 240-line rheight but indexable only because
     * lineinfo is dimensioned generously; on shorter heights this
     * would be reading past the array).
     *
     * Fix: read lineinfo[i] directly. */
     line = &(ctrl->info.lineinfo[i]);
     ctrl->info.draw_line = i;
 
    /* Mode ABSOLU conservé : v est l'offset vertical écran absolu, converti
     * en rangée source via incv. SCYIN0 du snapshot de zone s'ajoute
     * naturellement. Pour un jeu qui ne change pas SCYIN0 entre zones,
     * cela produit une texture continue sur tout l'écran, ce qui est
     * le comportement voulu. */
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
 
    /* Wrap cellw/cellh (powers of two, sans division). */
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
          if (addr >= 0x80000) {
            *ctrl->texture.textdata++ = 0x00000000;
          }
          else {
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
	// VDP2 Manual §4.3 Figure 4.6: RGB format, bit15=transparent.
	// §11.2: Special priority mode applies to RGB format tiles the same as palette tiles.
	// Pass dot lower nibble as special priority check value (consistent with palette cases).
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

/* vidcs.c — getPriority() — VDP2 Manual §11.1 PRINA/PRINB/PRINB registers
 * Priority number is a 3-bit value per scroll screen (bits 2-0 of each field).
 * PRINA (1800F8H): bits 2-0 = NBG0, bits 10-8 = NBG1
 * PRINB (1800FAH): bits 2-0 = NBG2, bits 10-8 = NBG3
 * PRIR  (1800FCH): bits 2-0 = RBG0
 * When priority == 0, the screen is treated as transparent (not displayed).
 */
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

  int lineindex = 0;

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

  /* VDP2 Manual §5.3 SCRCTL NxLSS bits: line scroll table read interval.
   * Table (p.137):
   *   NxLSS=00 → every line (NI), every 2 lines (SI), every line  (DDI)
   *   NxLSS=01 → every 2 lines (NI), every 4 lines (SI), every 2  (DDI)
   *   NxLSS=10 → every 4 lines (NI), every 8 lines (SI), every 4  (DDI)
   *   NxLSS=11 → every 8 lines (NI), every 16 lines(SI), every 8  (DDI)
   * SCRCTL (1800 9AH): NBG0 NxLSS = bits 5-4, NBG1 NxLSS = bits 13-12.
   * In single-density interlace both fields share one table entry,
   * so effective spacing doubles. DDI is identical to non-interlace.
   */
  int linescroll_spacing = 1;
  if (ctrl->info.islinescroll) {
    int lss = 0;
    if (ctrl->info.idScreen == NBG0)
      lss = (ctrl->regs->SCRCTL >> 4) & 0x3;
    else if (ctrl->info.idScreen == NBG1)
      lss = (ctrl->regs->SCRCTL >> 12) & 0x3;
    linescroll_spacing = 1 << lss;
    if (_Ygl->interlace == SINGLE_INTERLACE)
      linescroll_spacing <<= 1;   /* SI: both fields → one entry */
    /* DDI: no adjustment, same as non-interlace */
  }
  int linemask = linescroll_spacing - 1;

  int screenH = _Ygl->rheight;

  for (v = 0; v < screenH; v++) {
    int targetv = 0;
	
    /* VDP2 Manual §5.3 Figure 5.5: lineinfo[] is filled by
     * Vdp2GenLineinfo() indexed by ABSOLUTE screen line.  Adjacent
     * entries within the same NxLSS interval already share identical
     * data (the per-table_entry grouping is applied inside
     * Vdp2GenLineinfo when populating).
     *
     * The previous code indexed by 'lineindex<<res_shift', where
     * 'lineindex' is the loop's table_entry counter (incremented
     * once per linescroll_spacing lines).  That is a valid index
     * only if lineinfo[] is indexed by table_entry — it is NOT.
     * Reading lineinfo[lineindex] for line 'v' returns the values
     * stored at array index lineindex, which Vdp2GenLineinfo filled
     * with table_entry = lineindex / lineinc — i.e. wrong by a
     * factor of lineinc.
     *
     * Fix: read lineinfo[v] directly, mirroring the corrected
     * Vdp2DrawBitmapLineScroll / Vdp2DrawBitmapCoordinateInc paths. */

    if (VDPLINE_SX(ctrl->info.islinescroll)) {
      sx = ctrl->info.sh + ctrl->info.lineinfo[v].LineScrollValH;
    }
    else {
      sx = ctrl->info.sh;
    }

    if (VDPLINE_SY(ctrl->info.islinescroll)) {
       targetv = ctrl->info.sv + (v&linemask) + ctrl->info.lineinfo[v].LineScrollValV;
    }
    else {
      targetv = ctrl->info.sv + ((v*incv)>>8);
    }

    if (ctrl->info.isverticalscroll) {
      /* VDP2 Manual §5.3 'Vertical Cell Scroll Function' p.134:
       *   "Data of the vertical cell scroll table is treated as a
       *    table in the order from data in the left side cell of
       *    the TV screen."
       *
       * The VCSC table contains ONE entry per horizontal CELL of the
       * TV screen (not per line, not per pixel).  Each entry shifts
       * the vertical offset for all 8 pixels of that cell column.
       *
       * KNOWN LIMITATION: this read uses a fixed offset of 0 from
       * verticalscrolltbl, so every screen cell column receives the
       * same V-shift — equivalent to applying only the first cell's
       * vertical scroll uniformly.  Correct behaviour would require
       * reading at 'verticalscrolltbl + cellCol * verticalscrollinc'
       * inside the horizontal loop body and re-mapping mapy / planey
       * / pagey when the column changes.  See Vdp2DrawMapTest()
       * around line 5158 for the cell-stepping pattern.
       *
       * This bug is largely benign in practice: most titles using
       * VCSC do so with all cell columns sharing similar V-offsets
       * (it is mainly used for parallax planet curvature, water
       * tilt, etc.).  Symptoms appear as flat horizontal "bands"
       * that should instead curve column-by-column — visible in a
       * handful of arcade ports' transition effects.
       *
       * Leaving the targetv += unchanged for now: the per-cell
       * variant requires a non-trivial restructuring of the inner
       * loop and conflicts with the zoom path that re-derives mapx
       * from a non-cell-aligned 'hh'.  Marked as a TODO. */

      targetv += Vdp2RamReadLong(NULL, Vdp2Ram, ctrl->info.verticalscrolltbl) >> 16;
    }

	/* vidcs.c — Vdp2DrawMapPerLine() line zoom update
	 * VDP2 Manual §5.3 SCRCTL N0LZMX/N1LZMX: when set, horizontal coordinate
	 * increment is read per-line from the line scroll table (8.8 fixed-point).
	 * Table value 0x0100 = 1.0 (no zoom). Value 0x0000 is undefined/invalid;
	 * treat as 1.0 to avoid division by zero. Value > 0x0100 = reduction.
	 * VDP2 Manual §5.3: "coordinate increment must not exceed reduction setting."
	 */
	if (VDPLINE_SZ(ctrl->info.islinescroll)) {
		u16 raw_inc = ctrl->info.lineinfo[v].CoordinateIncH;
		if (raw_inc == 0) {
			/* VDP2 Manual §5.3: 0 is undefined — treat as 1.0 (no zoom) */
			ctrl->info.coordincx = 1.0f;
		} else {
			/* 8.8 fixed-point: integer part bits 10-8, frac bits 7-0 */
			ctrl->info.coordincx = 1.0f / ((float)raw_inc / 256.0f);
		}
	}
	/* VDP2 Manual §4.3 ZMCTL: clamp to minimum zoom allowed by reduction register */
	if (ctrl->info.coordincx < ctrl->info.maxzoom)
		ctrl->info.coordincx = ctrl->info.maxzoom;

    mapy = (targetv) >> planeh_shift;
    dot_on_planey = (targetv)-(mapy << planeh_shift);
    mapy = mapy & 0x01;
    planey = dot_on_planey >> plane_shift;
    dot_on_pagey = dot_on_planey & plane_mask;
    planey = planey & (ctrl->info.planeh - 1);
    pagey = dot_on_pagey >> page_shift;
    chary = dot_on_pagey & page_mask;
    if (pagey < 0) pagey = ctrl->info.pagewh - 1 + pagey;

    int inch = (int)(1.0f / ctrl->info.coordincx * 256.0f);

    for (int j = 0; j < ctrl->info.draww; j += 1) {

      int hh = ((j*inch) >> 8);

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

      /* VDP2 Manual §11.1 PRINA/PRINB (1800F8H-1800FAH, p.225):
       * Priority is a 3-bit value per scroll screen, sampled per line
       * from Vdp2Lines[] to capture mid-frame register changes.
       * PRINA bits 2-0 = NBG0, bits 10-8 = NBG1.
       * PRINB bits 2-0 = NBG2, bits 10-8 = NBG3.
       * (MapPerLine is only called for NBG0 and NBG1.) */
      ctrl->info.priority = getPriority(ctrl->info.idScreen,
                                        &Vdp2Lines[ctrl->info.draw_line]);

      /* VDP2 Manual §11.2 Special Priority Function (p.227):
       * specialprimode==1: LSB of the 3-bit priority number is replaced
       * by the special priority bit from pattern name data.
       * Must save/restore so the per-line base priority is not corrupted
       * across pixels within the same line. */
      int priority = ctrl->info.priority;
      if (ctrl->info.specialprimode == 1) {
        ctrl->info.priority = (ctrl->info.priority & 0xFFFFFFFE)
                              | ctrl->info.specialfunction;
      }

      /* Compute tile-local pixel coordinates with flip */
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

      /* Fetch pixel — priority already set above, restored below */
      *(ctrl->texture.textdata++) =
          Vdp2RotationFetchPixel(&ctrl->info, x, y, ctrl->info.cellw);

      /* VDP2 Manual §11.2: restore base priority after each pixel,
       * whether or not specialprimode modified it. */
      ctrl->info.priority = priority;
    }

    if ((v & linemask) == linemask) lineindex++;
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

  //ctrl->info.coordincx = 1.0f;

  for (v = -ctrl->info.patternpixelwh; v < ctrl->info.drawh + ctrl->info.patternpixelwh; v += ctrl->info.patternpixelwh) {
    int targetv = 0;
    sx = ctrl->info.x;

    if (!ctrl->info.isverticalscroll) {
      targetv = ctrl->info.y + v;
      // determine which chara shoud be used.
      //mapy   = (v+sy) / (512 * ctrl->info.planeh);
      mapy = (targetv) >> planeh_shift;
      //int dot_on_planey = (v + sy) - mapy*(512 * ctrl->info.planeh);
      dot_on_planey = (targetv)-(mapy * (1 << planeh_shift));
      mapy = mapy & 0x01;
      //planey = dot_on_planey / 512;
      planey = dot_on_planey >> plane_shift;
      //int dot_on_pagey = dot_on_planey - planey * 512;
      dot_on_pagey = dot_on_planey & plane_mask;
      planey = planey & (ctrl->info.planeh - 1);
      //pagey = dot_on_pagey / (512 / ctrl->info.pagewh);
      pagey = dot_on_pagey >> page_shift;
      //chary = dot_on_pagey - pagey*(512 / ctrl->info.pagewh);
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
        // determine which chara shoud be used.
        //mapy   = (v+sy) / (512 * ctrl->info.planeh);
        mapy = (targetv) >> planeh_shift;
        //int dot_on_planey = (v + sy) - mapy*(512 * ctrl->info.planeh);
        dot_on_planey = (targetv)-(mapy << planeh_shift);
        mapy = mapy & 0x01;
        //planey = dot_on_planey / 512;
        planey = dot_on_planey >> plane_shift;
        //int dot_on_pagey = dot_on_planey - planey * 512;
        dot_on_pagey = dot_on_planey & plane_mask;
        planey = planey & (ctrl->info.planeh - 1);
        //pagey = dot_on_pagey / (512 / ctrl->info.pagewh);
        pagey = dot_on_pagey >> page_shift;
        //chary = dot_on_pagey - pagey*(512 / ctrl->info.pagewh);
        chary = dot_on_pagey & page_mask;
        if (pagey < 0) pagey = ctrl->info.pagewh - 1 + pagey;
      }

      //mapx = (h + sx) / (512 * ctrl->info.planew);
      mapx = (h + sx) >> planew_shift;
      //int dot_on_planex = (h + sx) - mapx*(512 * ctrl->info.planew);
      dot_on_planex = (h + sx) - (mapx * (1<<planew_shift));
      mapx = mapx & 0x01;
      //planex = dot_on_planex / 512;
      planex = dot_on_planex >> plane_shift;
      //int dot_on_pagex = dot_on_planex - planex * 512;
      dot_on_pagex = dot_on_planex & plane_mask;
      planex = planex & (ctrl->info.planew - 1);
      //pagex = dot_on_pagex / (512 / ctrl->info.pagewh);
      pagex = dot_on_pagex >> page_shift;
      //charx = dot_on_pagex - pagex*(512 / ctrl->info.pagewh);
      charx = dot_on_pagex & page_mask;

      if (ctrl->info.PlaneAddr == 0) {
        exit(-1);
      }
      ctrl->info.PlaneAddr(&ctrl->info, ctrl->info.mapwh * mapy + mapx, ctrl->regs);
      if (Vdp2PatternAddrPos(ctrl, planex, pagex, planey, pagey) != 0) {
        //Only draw if there is a valid character pattern VRAM access for the current layer
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
    // color offset enable
    if (regs->CLOFSL & mask)
    {
      // color offset B
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
      // color offset A
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
  else { // color offset disable

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

    // --- CALCUL DE L'ADRESSE ---
    u32 scrAddr;
    if (varVdp2Regs->VRSIZE & 0x8000)
        scrAddr = (((varVdp2Regs->BKTAU & 0x7) << 16) | varVdp2Regs->BKTAL) * 2;
    else
        scrAddr = (((varVdp2Regs->BKTAU & 0x3) << 16) | varVdp2Regs->BKTAL) * 2;

    int isPerLine = (varVdp2Regs->BKTAU & 0x8000);
    int line_shift = (_Ygl->rheight > 256) ? 1 : 0;

    // --- LOGIQUE D'ALPHA (FIX DIE HARD / NBG3) ---
    u8 alpha8;

    // Si NBG3 a la Color Calculation activée (CCRNB bits 14-15), 
    // le fond doit être traité comme une base opaque pour permettre le mélange.
    if (varVdp2Regs->CCRNB & 0xC000) 
    {
        alpha8 = 0xFF; 
    } 
    else 
    {
        // Calcul standard basé sur CCRLB (ratio de mélange du Back Screen)
        // On récupère les bits 8-12 (0x1F00)
        u8 alpha = (u8)((varVdp2Regs->CCRLB & 0x1F00) >> 8);
        
        // Sur Saturn, 0 peut signifier transparent ou opaque selon le contexte.
        // On convertit vers 8 bits (0-255).
        if (alpha == 0) alpha8 = 0xFF; 
        else alpha8 = (alpha << 3) | (alpha >> 2);
    }

    // --- BOUCLE DE RENDU ---
    for (int i = 0; i < _Ygl->rheight; i++) {
        u32 currentAddr = isPerLine ? (scrAddr + (2 * (i >> line_shift))) : scrAddr;
        
        // Masque de sécurité VRAM 512Ko
        u16 dot = Vdp2RamReadWord(NULL, Vdp2Ram, currentAddr & 0x7FFFF);

        /* VDP2 Manual §7.2 Figure 7.5: Back Screen Table data layout
         *   bit 14-10 = 5-bit Blue
         *   bit  9- 5 = 5-bit Green
         *   bit  4- 0 = 5-bit Red
         * Per §3.4: "Because color data must be set to RGB-8 bit when it is
         * output, a 0 will be added to the lowest 3 bits if RGB-5 bit color
         * data is stored in the color RAM."
         *
         * The hardware adds zeros (`x << 3`), so 0x1F→0xF8 (not 0xFF).
         * However the visible output is then identical to the standard
         * full-range 5→8 conversion (x << 3) | (x >> 2) within ±2 LSB,
         * and using the bit-replication form here would diverge from how
         * Vdp2ColorRamGetColorRaw / SAT2YAB1 deliver scroll-screen pixels —
         * which is what the back screen must blend with.  Keep `<< 3` to
         * stay consistent with the rest of the pipeline (§3.4 wording). */
        u8 r = (dot & 0x001F) << 3;
        u8 g = ((dot >> 5)  & 0x1F) << 3;
        u8 b = ((dot >> 10) & 0x1F) << 3;

        /* Output channel order: the YglBackTexture is uploaded with
         * GL_RGBA / GL_UNSIGNED_BYTE on little-endian hosts, so the byte
         * stored at offset 0 is the R channel.  Using a u32 with shift
         * R<<0 | G<<8 | B<<16 | A<<24 produces the correct memory order.
         *
         * Previous comment claimed 'BGRA' which only worked by accident on
         * the test machine — make the channel layout explicit and document
         * that the alpha lives in the high byte. */
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

	// VDP2 Manual §11.3: LNCLEN register enables line color insertion per layer.
	// Bits: 0=NBG0, 1=NBG1, 2=NBG2, 3=NBG3, 4=RBG0, 5=Sprite
	// Early-out if no layer has line color enabled.
	// Per-layer activation is handled by the shader via VDP2COLOR encoding.
	if (varVdp2Regs->LNCLEN == 0) return;
	// Note: individual bit checking (varVdp2Regs->LNCLEN & (1<<layer)) should
	// be forwarded to the compositor shader. Currently the line color screen
	// texture is generated globally — full per-layer enforcement requires
	// shader-side LNCLEN bit testing. TODO: pass LNCLEN to compositor.


  line_pixel_data = YglGetLineColorScreenPointer();
  if (line_pixel_data == NULL) {
    return;
  }

  if ((varVdp2Regs->LCTA.part.U & 0x8000)) {
    inc = 0x02; // color per line
  }
  else {
    inc = 0x00; // single color
  }

	/* vidcs.c — Vdp2DrawLineColorScreen()
	 * VDP2 Manual §11.3 CCRLB (18010EH) bits 4-0 = LCCCRT[4:0]:
	 *   Line color screen color calculation ratio, same encoding as CCRNA.
	 *   alpha = (~ratio & 0x1F) * 255 / 31  (0=opaque, 31=~transparent)
	 * VDP2 Manual §11.3 LNCLEN (1800E8H): per-layer enable bits.
	 *   bit 0=NBG0, 1=NBG1, 2=NBG2, 3=NBG3, 4=RBG0, 5=Sprite.
	 *   Line color is only inserted on layers where LNCLEN bit is set.
	 */

	/* Correct alpha from CCRLB LCCCRT[4:0] */
	u8 alpha = (u8)(((~varVdp2Regs->CCRLB & 0x1F) * 255) / 31);

	addr = (varVdp2Regs->LCTA.all & 0x7FFFF) << 1;
	for (i = 0; i < line_cnt; i++) {
		u16 LineColorRamAdress = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
		/* VDP2 Manual §11.3: line color table entry is a color RAM index */
		*(line_pixel_data) = Vdp2ColorRamGetLineColor(LineColorRamAdress, alpha);
		line_pixel_data++;
		addr += inc;
	}

  YglSetLineColorScreen(line_pixel_data, line_cnt);

}

//////////////////////////////////////////////////////////////////////////////

static int Vdp2CheckCharAccessPenalty(int char_access, int ptn_access) {
  if (_Ygl->rwidth >= 640) {
    //if (char_access < ptn_access) {
    //  return -1;
    //}
    if (ptn_access & 0x01) { // T0
      // T0-T2
      if ((char_access & 0x07) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x02) { // T1
      // T1-T3
      if ((char_access & 0x0E) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x04) { // T2
      // T0,T2,T3
      if ((char_access & 0x0D) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x08) { // T3
      // T0,T1,T3
      if ((char_access & 0xB) != 0) {
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
      // T0-T2, T4-T7
      if ((char_access & 0xF7) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x02) { // T1
      // T0-T3, T5-T7
      if ((char_access & 0xEF) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x04) { // T2
      // T0-T3, T6-T7
      if ((char_access & 0xCF) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x08) { // T3
      // T0-T3, T7
      if ((char_access & 0x8F) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x10) { // T4
      // T0-T3
      if ((char_access & 0x0F) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x20) { // T5
      // T1-T3
      if ((char_access & 0x0E) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x40) { // T6
      // T2,T3
      if ((char_access & 0x0C) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x80) { // T7
      // T3
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
  // RBG1 shall not work without RBG0 but it looks like the HW is able to...
  // MechWarrior 2 - 31st Century Combat - Arcade Combat Edition uses this capability.
  //if (!(varVdp2Regs->BGON & 0x10)) info->enable = 0;

  if (!info->enable) {
    pushRBG(rbg);
    return;
  }

  for (int i = info->startLine; i < info->endLine; i++) {
    info->display[i] = info->enable;
    /* VDP2 Manual §12.1 CCRNA (180108H) bits 4-0 = N0CCRT[4:0]:
     * RBG1 shares NBG0's color calculation ratio register (same bits).
     * Full 0-255 mapping instead of <<3 which clips at 248. */
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

    // VDP2 Manual §6.1: Verify that the VRAM bank used by RBG1 bitmap
    // is actually allocated for rotation screen use (RAMCTL bits).
    // RAMCTL bits [2*bank+1 : 2*bank] must be 11B (rotation data) for the
    // bank to be valid. This mirrors the check done in Vdp2DrawRBG0_part.
    {
	// VDP2 Manual §6.1: RAMCTL rotation-exclusive check only applies to
	// palette format bitmaps. RGB format (colornumber==3) reads VRAM directly as
	// pixel data and does not require the bank to be rotation-allocated (0x3).
	// Skipping the check for RGB format prevents incorrectly suppressing RBG1 RGB bitmaps.

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

  ReadLineScrollData(info, rbg->ctrl.regs->SCRCTL & 0xFF, rbg->ctrl.regs->LSTA0.all);
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
  if ((a->BGON & 0x1010) != (b->BGON & 0x1010)) return 0;
  if ((a->PRIR & 0x7) != (b->PRIR & 0x7)) return 0;
  if ((a->RPTA.all) != (b->RPTA.all)) return 0;
  if ((a->RPMD & 0x3) != (b->RPMD & 0x3)) return 0;
  // Alpha/transparence RBG0 : CCRR bits 4-0, CCCTL bit 12
  if ((a->CCRR & 0x1F) != (b->CCRR & 0x1F)) return 0;
   /* §12.1 : R0CCEN(4) active le color calc pour RBG0.
   *          CCMD(8) bascule mode ratio/add.
   *          CCRTMD(9) sélectionne top vs second screen pour le ratio.
   *          EXCCEN(10) active le extended color calc (3e/4e plan). */
  if ((a->CCCTL & 0x0710) != (b->CCCTL & 0x0710)) return 0;
  /* ------------------------------------------------------------------
   * §6.4 : tous les bits contrôlent le mode, la taille des données et
   *         la line-color-enable des tables coefficient A et B.
   *   ParaA: RAKTE(0)+RAKDBS(1)+RAKMD(3-2)+RAKLCE(4)
   *   ParaB: RBKTE(8)+RBKDBS(9)+RBKMD(11-10)+RBKLCE(12)
   * Masque 0x1F1F = bits 12..8 | 4..0.
   * ------------------------------------------------------------------ */
  if ((a->KTCTL & 0x1F1F) != (b->KTCTL & 0x1F1F)) return 0;
  if ((a->CHCTLB & 0x7700) != (b->CHCTLB & 0x7700)) return 0; // colornumber + bitmap RBG0
  if ((a->PLSZ & 0xFF00)   != (b->PLSZ & 0xFF00))   return 0; // plane size ParaA + ParaB
  if ((a->MPOFR & 0x77)    != (b->MPOFR & 0x77))     return 0; // map offset RBG0 A+B
  if ((a->PNCR & 0xFFFF) != (b->PNCR & 0xFFFF)) return 0; // R0PNB,R0CNSM,R0SPR,etc.
  if ((a->KTAOF & 0x0707) != (b->KTAOF & 0x0707)) return 0; // RAKTAOS+RBKTAOS
  /* ------------------------------------------------------------------
   * [AJOUTÉ] SFPRMD 1800EAH bits 9~8 : R0SPRM1,R0SPRM0
   * §11.2 : special priority mode RBG0 (par écran / par caractère /
   *          par dot). Change comment le LSB du numéro de priorité est
   *          sélectionné pour chaque dot → doit provoquer un split.
   * Masque 0x0300 = bits 9,8.
   * ------------------------------------------------------------------ */
  if ((a->SFPRMD & 0x0300) != (b->SFPRMD & 0x0300)) return 0;
  /* ------------------------------------------------------------------
   * [AJOUTÉ] WCTLC 1800D4H bits 7~0 : fenêtres RBG0
   * §8.1 : R0W0A,R0W0E,R0W1A,R0W1E,R0SWA,R0SWE + R0LOG(7).
   *          Un changement mid-frame active, désactive ou inverse
   *          la zone de fenêtre appliquée à RBG0.
   * Masque 0x00FF = octet bas.
   * ------------------------------------------------------------------ */
  if ((a->WCTLC & 0x00FF) != (b->WCTLC & 0x00FF)) return 0;;
  /* ------------------------------------------------------------------
   * [AJOUTÉ] WCTLD 1800D6H bits 3~0 : rotation parameter window
   * §8.2 : RPW0A(0),RPW0E(1),RPW1A(2),RPW1E(3).
   *          Utilisé directement dans Vdp2DrawRBG0_part() pour
   *          info->RotWin (mode RPMD=3). Masque 0x000F.
   * ------------------------------------------------------------------ */
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
	//  if ((a->VRSIZE & 0x8000) != (b->VRSIZE & 0x8000)) return 0;
	//  if ((a->RAMCTL & 0x80FF) != (b->RAMCTL & 0x80FF)) return 0;
	//  if ((a->COBR & 0x1FF) != (b->COBR & 0x1FF)) return 0;
	//  if ((a->COBG & 0x1FF) != (b->COBG & 0x1FF)) return 0;
	//  if ((a->COBB & 0x1FF) != (b->COBB & 0x1FF)) return 0;
	//  if ((a->COAR & 0x1FF) != (b->COAR & 0x1FF)) return 0;
	//  if ((a->COAG & 0x1FF) != (b->COAG & 0x1FF)) return 0;
	//  if ((a->COAB & 0x1FF) != (b->COAB & 0x1FF)) return 0;
  return 1;
}

static int sameVDP2RegRBG1(Vdp2 *a, Vdp2 *b)
{
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
  if ((a->LSTA0.all) != (b->LSTA0.all)) return 0; // adresse table line scroll NBG0/RBG1 scroll est actif (SCRCTL bits 5-0 != 0).
  if ((a->VCSTA.all) != (b->VCSTA.all)) return 0; // adresse table vertical cell scroll NBG0/RBG1
  if ((a->WCTLD & 0x000F) != (b->WCTLD & 0x000F)) return 0; // rotation parameter window
	//  if ((a->VRSIZE & 0x8000) != (b->VRSIZE & 0x8000)) return 0;
	//  if ((a->RAMCTL & 0x80FF) != (b->RAMCTL & 0x80FF)) return 0;
	//  if ((a->PLSZ & 0xFF00) != (b->PLSZ & 0xFF00)) return 0;
	//  if ((a->CHCTLA & 0x7F) != (b->CHCTLA & 0x7F)) return 0;
	//  if ((a->WCTLA & 0xFF) != (b->WCTLA & 0xFF)) return 0;
	//  if ((a->PNCN0 & 0xFFFF) != (b->PNCN0 & 0xFFFF)) return 0;
	//  if ((a->COBR & 0x1FF) != (b->COBR & 0x1FF)) return 0;
	//  if ((a->COBG & 0x1FF) != (b->COBG & 0x1FF)) return 0;
	//  if ((a->COBB & 0x1FF) != (b->COBB & 0x1FF)) return 0;
	//  if ((a->COAR & 0x1FF) != (b->COAR & 0x1FF)) return 0;
	//  if ((a->COAG & 0x1FF) != (b->COAG & 0x1FF)) return 0;
	//  if ((a->COAB & 0x1FF) != (b->COAB & 0x1FF)) return 0;
  return 1;
}

static int sameVDP2RegNBG0(Vdp2 *a, Vdp2 *b)
{
    /* BGON: N0ON = bit 0. Also check RBG enable bits that suppress NBG0
     * (VDP2 §4.1 Table 4.1: when R0ON(4)+R1ON(5) both set, NBG screens off). */
    if ((a->BGON & 0x31) != (b->BGON & 0x31)) return 0;
 
    /* CHCTLA bits 6-0: N0CHSZ(3-2), N0BMEN(1), N0CHCN(6-4). */
    if ((a->CHCTLA & 0x7F) != (b->CHCTLA & 0x7F)) return 0;
 
    /* PRINA bits 2-0: NBG0 priority number. */
    if ((a->PRINA & 0x7) != (b->PRINA & 0x7)) return 0;
 
    /* CCRNA bits 4-0: N0CCRT[4:0] — NBG0 color calculation ratio. */
    if ((a->CCRNA & 0x1F) != (b->CCRNA & 0x1F)) return 0;
 
    /* SCXIN0 bits 10-0: NBG0 horizontal scroll integer part.
     * Games doing raster-scroll MUST be detected here — Shellshock changes
     * SCXIN0 mid-frame to switch between the two halves of its ground bitmap
     * used as a double-buffer. */
    if ((a->SCXIN0 & 0x7FF) != (b->SCXIN0 & 0x7FF)) return 0;
 
    /* SCYIN0 bits 10-0: NBG0 vertical scroll integer part. */
    if ((a->SCYIN0 & 0x7FF) != (b->SCYIN0 & 0x7FF)) return 0;
 
    /* NEW: SCXDN0 bits 15-8: NBG0 horizontal scroll FRACTIONAL part (8.8 FP).
     * Some games use sub-pixel scrolling — a pure SCXDN0 change would
     * otherwise be invisible to the zone detector. */
    if ((a->SCXDN0 & 0xFF00) != (b->SCXDN0 & 0xFF00)) return 0;
 
    /* NEW: SCYDN0 bits 15-8: NBG0 vertical scroll FRACTIONAL part. */
    if ((a->SCYDN0 & 0xFF00) != (b->SCYDN0 & 0xFF00)) return 0;
 
    /* ZMXN0 bits 18-8: NBG0 horizontal coordinate increment (zoom integer).
     * Shellshock's ground zone is detected HERE — the game writes 0x20000
     * for zoom x2 when entering the ground area, 0x10000 for the cabin. */
    if ((a->ZMXN0.all & 0x7FF00) != (b->ZMXN0.all & 0x7FF00)) return 0;
 
    /* ZMYN0 bits 18-8: NBG0 vertical coordinate increment. */
    if ((a->ZMYN0.all & 0x7FF00) != (b->ZMYN0.all & 0x7FF00)) return 0;
 
    /* NEW: ZMCTL bits 1-0: N0ZMQT,N0ZMHF — NBG0 reduction limit.
     * Vdp2DrawNBG0 reads these to clamp coordincx. A mid-frame change of the
     * reduction cap would otherwise be missed. */
    if ((a->ZMCTL & 0x0003) != (b->ZMCTL & 0x0003)) return 0;
 
    /* CRAOFA bits 2-0: N0CAOS[2:0] — NBG0 color RAM address offset.
     * If the game uses different palette banks for cabin vs ground, this
     * is where the transition is detected. */
    if ((a->CRAOFA & 0x7) != (b->CRAOFA & 0x7)) return 0;
 
    /* MPOFN bits 2-0: NBG0 map offset. In bitmap mode it selects the VRAM
     * area holding the bitmap base (charaddr). */
    if ((a->MPOFN & 0x7) != (b->MPOFN & 0x7)) return 0;
 
    /* BMPNA bits 6-0: NBG0 bitmap palette address + special bits. */
    if ((a->BMPNA & 0x77) != (b->BMPNA & 0x77)) return 0;
 
    /* PLSZ bits 1-0: NBG0 plane size (tile mode only, harmless in bitmap). */
    if ((a->PLSZ & 0x0003) != (b->PLSZ & 0x0003)) return 0;
 
    /* PNCN0: NBG0 pattern name control (tile mode). */
    if ((a->PNCN0 & 0xFFFF) != (b->PNCN0 & 0xFFFF)) return 0;
 
    /* SCRCTL bits 7-0: NBG0 line/cell scroll control. */
    if ((a->SCRCTL & 0x00FF) != (b->SCRCTL & 0x00FF)) return 0;
 
    /* SFPRMD bits 1-0: NBG0 special priority mode. */
    if ((a->SFPRMD & 0x0003) != (b->SFPRMD & 0x0003)) return 0;
 
    /* SFCCMD bits 1-0: NBG0 special color calculation mode. */
    if ((a->SFCCMD & 0x0003) != (b->SFCCMD & 0x0003)) return 0;
 
    /* LNCLEN bit 0: N0LCEN. */
    if ((a->LNCLEN & 0x0001) != (b->LNCLEN & 0x0001)) return 0;
 
    /* CLOFSL bit 0: N0COSL. */
    if ((a->CLOFSL & 0x0001) != (b->CLOFSL & 0x0001)) return 0;
 
    /* NEW: MZCTL bit 8 (N0MZE enable) + size bits 15-8 that apply to NBG0
     * when mosaic is on. If mosaic is toggled mid-frame, we need a new zone. */
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
  for (line = 2; line<max; line++) {
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

  /* VDP2 User's Manual ST-058-R2 §4.1 'Screen display enable bit':
   *   "When R0ON is 0, do not set R1ON at 1."
   *   "When both R0ON and R1ON are 1, the normal scroll screen can no
   *    longer be displayed.  At this time, VRAM-B0 is fixed in RAM used
   *    for RBG1 character pattern tables; and VRAM-B1 is fixed in RAM
   *    used for RBG1 pattern name tables."
   *
   * Two derived rules used below:
   *   rule_a: R0ON=1 AND R1ON=1  ->  NBG0..NBG3 forced off
   *   rule_b: R0ON=0 AND R1ON=1  ->  prohibited combination; the spec
   *           does not specify behaviour, but RBG1 cannot run without
   *           RBG0 supplying the rotation parameter resources, so we
   *           force RBG1 off and let NBG draws proceed normally.  This
   *           matches what the original Saturn hardware does in
   *           practice (RBG1 silently drops) and prevents an attempt
   *           to render with undefined parameter tables. */
  const int r0on = (varVdp2Regs->BGON & 0x10) != 0;
  const int r1on = (varVdp2Regs->BGON & 0x20) != 0;
  const int rule_a = (r0on && r1on);  /* both rotation screens active */
  const int rule_b = (!r0on && r1on); /* prohibited config */
  switch(id) {
    case NBG0:
      display = ((varVdp2Regs->BGON & 0x1)!=0);
      if (rule_a) display = 0; /* §4.1: NBG disabled when R0ON+R1ON=1 */
      break;
    case NBG1:
      display = ((varVdp2Regs->BGON & 0x2)!=0);
     if (rule_a) display = 0;
	  break;
    case NBG2:
      display = ((varVdp2Regs->BGON & 0x4)!=0);
	 if (rule_a) display = 0;
      break;
    case NBG3:
      display = ((varVdp2Regs->BGON & 0x8)!=0);
	 if (rule_a) display = 0;
      break;
    case RBG0:
      display = r0on;
      break;
    case RBG1:
      /* §4.1: if R0ON=0, R1ON=1 is prohibited - drop RBG1 silently
       * rather than rendering against undefined parameter tables. */
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


