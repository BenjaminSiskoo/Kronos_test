/*  Copyright 2005-2006 Guillaume Duhamel
    Copyright 2005-2006 Theo Berkau
    Copyright 2011-2015 Shinya Miyamoto(devmiyax)

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


#include <stdlib.h>
#include <math.h>
#include "ygl.h"
#include "yui.h"
#include "vidshared.h"
#include "debug.h"
#include "error.h"
#include "vdp1_compute.h"
#include "perfetto_trace.h"
#include "mpegcard.h"

//////////////////////////////////////////////////////////////////////////////
// Video CD Card (EXPERIMENTAL, see mpegcard.h).
//
// On real hardware the card's picture is not drawn on top of anything: it
// is fed into VDP2's external video input (the EXBG pins, see the VDP2
// pinout in the service manual) and VDP2 composites it as the EXBG screen.
// VDP2 manual ST-058-R2 is explicit about what EXBG is (§1.3 Table 1.2
// "External Input Screen EXBG", and §2.5): the external screen data
// *becomes NBG1 screen data*, so NBG1 cannot be displayed at the same time
// and every NBG1 register doubles as the EXBG register. Table 2.2 lists
// them: N1ON (display), N1PRIN (priority), the W0/W1/SW window bits,
// N1CCEN/N1CCRT (color calculation), N1COEN/N1COSL (color offset),
// N1LCEN, N1SDEN, and so on.
//
// That is exactly why the picture is rendered here, into the NBG1 layer
// buffer, rather than blitted over the finished frame: everything
// downstream -- priority against the other scroll screens and the sprites,
// windows, color calculation, color offset -- is the NBG1 plumbing this
// renderer already implements, and it applies to the movie for free. The
// on-screen display of the BIOS Video CD player therefore lands above or
// below the picture according to the priorities it actually programs.
//
// Layer pixels encode their priority and color-calculation ratio in the
// alpha channel (see VDP2COLOR() in ygl.h and VDP2_SCREEN_SETUP() in
// common_glshader.c: bits 0-2 priority, bits 3-7 CC ratio), so the shader
// below writes the NBG1 values there.
//
// When the application never enables EXBG (EXTEN bit 0), there is nothing
// to composite into, and the picture is drawn into the back screen instead
// so that something is still visible -- experimental fallback, not
// hardware behaviour.

static GLuint mpegLayerTex = 0;
static int mpegLayerTexW = 0;
static int mpegLayerTexH = 0;
static u32 mpegLayerSerial = 0xFFFFFFFF;
static GLint mpegLayerPrg = -1;
static GLint mpegLayerLocSize = -1;
static GLint mpegLayerLocAlpha = -1;

static const char mpegLayer_v[] =
  SHADER_VERSION
  "layout (location = 0) in vec2 a_position;   \n"
  "void main()       \n"
  "{ \n"
  "  gl_Position = vec4(a_position.x, a_position.y, 0.0, 1.0); \n"
  "} \n";

// u_size.xy = size of the area being drawn, u_size.z = height of the
// target buffer (the area sits at its bottom, glViewport(0,0,w,h)).
// u_alpha is the encoded priority/CC-ratio byte, already divided by 255.
static const char mpegLayer_f[] =
  SHADER_VERSION
  "#ifdef GL_ES\n"
  "precision highp float; \n"
  "#endif\n"
  "layout(origin_upper_left) in vec4 gl_FragCoord; \n"
  "uniform sampler2D u_mpeg; \n"
  "uniform vec3 u_size; \n"
  "uniform float u_alpha; \n"
  "out vec4 fragColor; \n"
  "void main() \n"
  "{ \n"
  "  vec2 uv = vec2(gl_FragCoord.x / u_size.x, \n"
  "                 (gl_FragCoord.y - (u_size.z - u_size.y)) / u_size.y); \n"
  "  fragColor = vec4(texture(u_mpeg, uv).rgb, u_alpha); \n"
  "} \n";

static void VIDCSMpegLayerLog(GLuint obj, int isprogram)
{
  char log[1024];
  GLsizei len = 0;
  if (isprogram) glGetProgramInfoLog(obj, sizeof(log) - 1, &len, log);
  else glGetShaderInfoLog(obj, sizeof(log) - 1, &len, log);
  log[len > 0 ? len : 0] = '\0';
  YuiMsg("%s\n", log);
}

static int VIDCSMpegLayerProgram(void)
{
  const GLchar *vsrc[] = { mpegLayer_v, NULL };
  const GLchar *fsrc[] = { mpegLayer_f, NULL };
  GLuint vs, fs, prg;
  GLint ok = GL_FALSE;

  if (mpegLayerPrg != -1)
    return mpegLayerPrg != 0;

  mpegLayerPrg = 0; // 0 = failed, don't retry every frame

  vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, vsrc, NULL);
  glCompileShader(vs);
  glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
  if (ok == GL_FALSE) { YuiMsg("MPEG card: layer vertex shader failed to compile\n"); VIDCSMpegLayerLog(vs, 0); return 0; }

  fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, fsrc, NULL);
  glCompileShader(fs);
  glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
  if (ok == GL_FALSE) { YuiMsg("MPEG card: layer fragment shader failed to compile\n"); VIDCSMpegLayerLog(fs, 0); return 0; }

  prg = glCreateProgram();
  glAttachShader(prg, vs);
  glAttachShader(prg, fs);
  glLinkProgram(prg);
  glGetProgramiv(prg, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) { YuiMsg("MPEG card: layer shader failed to link\n"); VIDCSMpegLayerLog(prg, 1); return 0; }

  glUseProgram(prg);
  glUniform1i(glGetUniformLocation(prg, "u_mpeg"), 0);
  mpegLayerLocSize = glGetUniformLocation(prg, "u_size");
  mpegLayerLocAlpha = glGetUniformLocation(prg, "u_alpha");
  mpegLayerPrg = (GLint)prg;
  return 1;
}

// True when the application has routed the card into VDP2: external screen
// input enabled (EXTEN bit 0, EXBGEN) with a non-zero priority, since
// priority 0 means "not displayed" for every VDP2 screen (§11.1).
//
// Note it does NOT require N1ON (BGON bit 1). EXBGEN is itself the enable
// for the external screen: Table 2.2 of ST-058-R2, which lists every
// register that configures EXBG, names N1TPON at 180020H bit 9 but not
// N1ON at bit 1. The Video CD player indeed runs with BGON=0001, i.e. NBG1
// switched off -- which is consistent, since EXBG replaces NBG1 and the two
// cannot be displayed together (§1.3). Requiring that bit hid the picture
// and sent it to the back-screen fallback.
static int VIDCSMpegExbgEnabled(Vdp2 *varVdp2Regs)
{
  if (varVdp2Regs == NULL)
    return 0;

  return (varVdp2Regs->EXTEN & 0x1) &&
         (((varVdp2Regs->PRINA >> 8) & 0x7) != 0);
}

// Uploads the current picture and draws it over the bound buffer.
// `alphabyte` goes to the alpha channel verbatim (priority + CC ratio for
// a scroll layer, 0xFF for the opaque back-screen fallback).
static void VIDCSDrawMpegPicture(int width, int height, int bufheight, u32 alphabyte)
{
  static const float vertexPosition[] = {
    1.0f, -1.0f,
    -1.0f, -1.0f,
    1.0f, 1.0f,
    -1.0f, 1.0f };
  const u8 *rgba;
  int mw = 0, mh = 0;

  if ((rgba = MpegCardGetFrameRGBA(&mw, &mh)) == NULL || mw <= 0 || mh <= 0)
    return;
  if (!VIDCSMpegLayerProgram())
    return;

  if (mpegLayerTex == 0)
    glGenTextures(1, &mpegLayerTex);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mpegLayerTex);

  if (mpegLayerSerial != MpegCardGetFrameSerial() || mw != mpegLayerTexW || mh != mpegLayerTexH)
  {
    // This renderer streams VDP data through pixel-unpack buffers; with
    // one still bound, `rgba` would be read as an offset into it.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (mw != mpegLayerTexW || mh != mpegLayerTexH)
    {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mw, mh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      mpegLayerTexW = mw;
      mpegLayerTexH = mh;
    }
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mw, mh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    mpegLayerSerial = MpegCardGetFrameSerial();
  }

  glUseProgram((GLuint)mpegLayerPrg);
  glUniform3f(mpegLayerLocSize, (float)width, (float)height, (float)bufheight);
  glUniform1f(mpegLayerLocAlpha, (float)(alphabyte & 0xFF) / 255.0f);

  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_DEPTH_TEST);

  glBindBuffer(GL_ARRAY_BUFFER, _Ygl->vertexPosition_buf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertexPosition), vertexPosition, GL_STREAM_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glEnableVertexAttribArray(0);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glDisableVertexAttribArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

// Draws the picture as the EXBG (= NBG1) layer. Called with screen_fbo
// bound and DrawBuffers[NBG1] selected, exactly like a normal scroll
// screen, so the result flows through the usual priority/window/color
// pipeline. Returns 1 when something was drawn.
static int VIDCSDrawMpegExbg(Vdp2 *varVdp2Regs)
{
  u32 priority, ccr, alphabyte;

  if (!MpegCardIsActive() || !VIDCSMpegExbgEnabled(varVdp2Regs))
    return 0;

  priority = (varVdp2Regs->PRINA >> 8) & 0x7;

  // Same NBG1 color-calculation ratio the rest of this renderer uses
  // (CCRNA upper byte, inverted: 0 = opaque), encoded the way
  // VDP2COLOR() does it.
  ccr = (u32)(((~(varVdp2Regs->CCRNA >> 8) & 0x1F) * 255) / 31);
  alphabyte = (ccr & 0xF8) | priority;

  VIDCSDrawMpegPicture(_Ygl->rwidth, _Ygl->rheight, _Ygl->rheight, alphabyte);
  return 1;
}

// Fallback when the application never enables EXBG: paint the picture on
// the back screen so it is at least visible, under every other layer.
static void VIDCSDrawMpegBackScreen(Vdp2 *varVdp2Regs, int width, int height)
{

  if (!MpegCardIsActive() || VIDCSMpegExbgEnabled(varVdp2Regs))
    return;

  // Keep the back screen's own alpha: it carries its color-calculation
  // ratio (see COMMON_START in common_glshader.c).
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  VIDCSDrawMpegPicture(width, height, _Ygl->rheight, 0xFF);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

#define YGLDEBUG

extern int GlHeight;
extern int GlWidth;

extern int DrawVDP2Screen(Vdp2 *varVdp2Regs, int id);

extern void YglUpdateVdp2Reg();
extern SpriteMode setupBlend(Vdp2 *varVdp2Regs, int layer);
extern Vdp2 *VIDCSBlendRegsForLayer(Vdp2 *base, int layer);
extern int setupColorMode(Vdp2 *varVdp2Regs, int layer);
extern int setupShadow(Vdp2 *varVdp2Regs, int layer);
extern int setupBlur(Vdp2 *varVdp2Regs, int layer);
extern int YglDrawBackScreen();
extern int YglFillWithBackScreen();

//////////////////////////////////////////////////////////////////////////////
int VIDCSEraseWriteVdp1(int id) {

  float col[4] = {0.0};
  u16 color;
  int priority;
  u32 alpha = 0;
  int status = 0;
  if (_Ygl->vdp1_pbo[0] == 0) return 0;

  _Ygl->vdp1_stencil_mode = 0;

  color = Vdp1Regs->EWDR;

  int shift = ((Vdp1Regs->TVMR & 0x1) == 1)?4:3;
  int limits[4] = {0};
  limits[0] = ((Vdp1Regs->EWLR>>9)&0x3F)<<shift;
  limits[1] = ((Vdp1Regs->EWLR)&0x1FF); /* VDP1 §4.3: 9-bit Y covers both 256/512 modes */

  limits[2] = (((Vdp1Regs->EWRR>>9)&0x7F)<<shift) - 1;
  limits[3] = ((Vdp1Regs->EWRR)&0x1FF); /* VDP1 §4.3: 9-bit Y covers both 256/512 modes */

  //Prohibited value - Example Quake first screens
  if ((limits[2] == -1)||(limits[3] == 0)) return 0;

  if ((limits[0] >= limits[2]) || (limits[1] > limits[3])) {
    /* VDP1 Manual §4.3 p.49: "erase/write is performed for 1 dot
     * only" when X1 >= X3 or Y1 > Y3. Collapse the rect to a
     * 1×1 erase at (X1, Y1) rather than skipping entirely. */
    limits[2] = limits[0];
    limits[3] = limits[1];
  }


//Can be usefull for next steps to evaluate effective possible pixels which can be deleted during VBLANK
//see p49 of vdp1 doc. A raster is the number of maxLinecount
/*
  int nbPixels = (x3-x1+1)*(y3-y1+1) x 8;
  int nbRaster =
*/
  col[0] = (color & 0xFF) / 255.0f;
  col[1] = ((color >> 8) & 0xFF) / 255.0f;

  /* ST-013-R3 p.46 (EWDR) : en frame buffer 8 bits/pixel l'effacement se
   * fait 2 pixels a la fois, les bits 15~8 servant aux coordonnees X
   * PAIRES et les bits 7~0 aux X IMPAIRES. En 16 bits/pixel les 16 bits
   * forment une seule donnee : colOdd vaut alors col et le shader ne fait
   * aucune distinction. */
  float colOdd[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int   xdiv = 1;
  if ((Vdp1Regs->TVMR & 0x1) == 1) {
    col[0]    = ((color >> 8) & 0xFF) / 255.0f;  /* X pairs   : octet haut */
    col[1]    = 0.0f;
    colOdd[0] = (color & 0xFF) / 255.0f;         /* X impairs : octet bas  */
    colOdd[1] = 0.0f;
    xdiv = (_Ygl->vdp1width > 512) ? (_Ygl->vdp1width / 512) : 1;
  } else {
    colOdd[0] = col[0];
    colOdd[1] = col[1];
  }

  vdp1_clear(id, col, colOdd, xdiv, limits);

  //Get back to drawframe
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
  /* VDP1 Manual §4.3 p.49: erase rect is inclusive on both axes.
   * Return pixel count, not cycles — the caller knows the FB mode
   * and can divide by pixels-per-cycle (2 for 16bpp, 4 for 8bpp)
   * if it needs cycle budget. */
  return (limits[2] - limits[0] + 1) * (limits[3] - limits[1] + 1);
}

void VIDCSFinsihDraw(void) {
}

//////////////////////////////////////////////////////////////////////////////

void VIDCSRenderVDP1(void) {
  TRACE_RENDER("VIDCSRenderVDP1");
  FRAMELOG("VIDCSRenderVDP1: drawframe =%d %d\n", _Ygl->drawframe, yabsys.LineCount);
  // vdp1_compute();
}

void VIDCSFrameChangeVdp1(){
  u32 current_drawframe = 0;
  if (_Ygl->shallVdp1Erase[_Ygl->readframe] != 0) {
    FRAMELOG("FB %d is erased now\n", _Ygl->readframe);
    _Ygl->shallVdp1Erase[_Ygl->readframe] = 0;
    VIDCSEraseWriteVdp1(_Ygl->readframe);
    clearVDP1Framebuffer(_Ygl->readframe);
  }
  VIDCSRenderVDP1();
  current_drawframe = _Ygl->drawframe;
  _Ygl->drawframe = _Ygl->readframe;
  _Ygl->readframe = current_drawframe;
  _Ygl->vdp1fb_read_buf[_Ygl->drawframe] = NULL;

  FRAMELOG("VIDCSFrameChangeVdp1: swap drawframe =%d readframe = %d (%d)\n", _Ygl->drawframe, _Ygl->readframe, yabsys.LineCount);
}

extern int WinS[enBGMAX+1];
extern int WinS_mode[enBGMAX+1];

static int warning = 0;


GLuint GetCSVDP1fb(int id) {
  if (id == 0) return get_vdp1_tex(_Ygl->readframe);
  else return get_vdp1_mesh(_Ygl->readframe);
}

void finishCSRender() {
  for (int i=0; i<SPRITE; i++)
    YglReset(_Ygl->vdp2levels[i]);
  glViewport(_Ygl->originx, _Ygl->originy, GlWidth, GlHeight);
  glScissor(0, 0, _Ygl->width, _Ygl->height);
  glUseProgram(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  OSDDisplayMessages(NULL,0,0);

  _Ygl->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);

  if (VIDCore->startVdp1Render) VIDCore->startVdp1Render();
}

void VIDCSRender(Vdp2 *varVdp2Regs) {
   TRACE_RENDER("VIDCSRender");
   double scale = 1.0;
   GLuint cprg=0;
   GLuint srcTexture;
   GLuint VDP1fb[2];
   int nbPass = 0;
   unsigned int i,j;
   double w = 0;
   double h = 0;
   double x = 0;
   double y = 0;
   float col[4] = {0.0f,0.0f,0.0f,0.0f};
   float colopaque[4] = {0.0f,0.0f,0.0f,1.0f};
   int img[6] = {0};
   int lncl[7] = {0};
   int lncl_draw[7] = {0};
   int winS_draw = 0;
   int winS_mode_draw= 0;
   int win0_draw = 0;
   int win0_mode_draw = 0;
   int win1_draw = 0;
   int win1_mode_draw= 0;
   int win_op_draw = 0;
   int win_all_draw = 0;
   int drawScreen[enBGMAX];
   SpriteMode mode;
   GLenum DrawBuffers[8]= {GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_COLOR_ATTACHMENT2,GL_COLOR_ATTACHMENT3,GL_COLOR_ATTACHMENT4,GL_COLOR_ATTACHMENT5,GL_COLOR_ATTACHMENT6,GL_COLOR_ATTACHMENT7};
   RATIOMODE modeScreen = _Ygl->stretch;

   int width = (_Ygl->width*2.0)/(_Ygl->vdp2wdensity);
   int height = _Ygl->height;

   double dar = (double)GlWidth/(double)GlHeight;
   double par = 4.0/3.0;

   float Intw = (float)GlWidth/(float)width;
   float Inth = (float)GlHeight/(float)height;

    if (_Ygl->interlace != DOUBLE_INTERLACE) {
      height = height*2.0;
      Inth /= 2.0;
    }
    float Int  = 1.0;

   if (modeScreen == ORIGINAL_RATIO) {
     if (yabsys.CurSH2FreqType == CLKTYPE_26MHZ) {
       if (yabsys.IsPal)
        //4:3 corresponds to 720*576
        par = (((float)width/720.0) * 4.0) / (((float)height/576.0) * 3.0) ;
       else
        //4:3 corresponds to 720*480
        par = (((float)width/720.0) * 4.0) / (((float)height/480.0) * 3.0) ;
     } else {
       if (yabsys.IsPal)
        //4:3 corresponds to 768*576
        par = (((float)width/768.0) * 4.0) / (((float)height/576.0) * 3.0) ;
       else
        //4:3 corresponds to 768*480
        par = (((float)width/768.0) * 4.0) / (((float)height/480.0) * 3.0) ;
     }
   }

   if ((floor(Intw) == 0)&&((modeScreen == INTEGER_RATIO)||(modeScreen == INTEGER_RATIO_FULL))) {
     if (warning == 0) YuiMsg("Window width is too small - Do not use integer scaling or reduce scaling\n");
     warning = 1;
     modeScreen = ORIGINAL_RATIO;
     Intw = 1.0;
     width = _Ygl->width*2.0/_Ygl->vdp2wdensity;
   }
   if ((floor(Inth) == 0)&&((modeScreen == INTEGER_RATIO)||(modeScreen == INTEGER_RATIO_FULL))) {
     if (warning == 0) YuiMsg("Window height is too small - Do not use integer scaling or reduce scaling\n");
     warning = 1;
     modeScreen = ORIGINAL_RATIO;
     Inth = 1.0;
     width = _Ygl->width*2.0/_Ygl->vdp2wdensity;
   }
   if (modeScreen != INTEGER_RATIO)  Int = (Inth<Intw)?Inth:Intw;
   glDepthMask(GL_FALSE);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_BLEND);

   glBindVertexArray(_Ygl->vao);
   /* Guard against the window between Vdp1Reset() and the first VDP2
    * register write that defines the render resolution. Dividing by
    * zero below would produce +Inf and then UB on the int cast. */
   if (_Ygl->rwidth <= 0 || _Ygl->rheight <= 0) {
     finishCSRender();
     return;
   }
#ifndef __LIBRETRO__
   switch(modeScreen) {
     case STRETCH_RATIO:
       w = GlWidth;
       h = GlHeight;
       x = 0;
       y = 0;
       break;
    case ORIGINAL_RATIO:
       w = (dar>par)?(double)GlHeight*par:GlWidth;
       h = (dar>par)?(double)GlHeight:(double)GlWidth/par;
       x = (GlWidth-w)/2;
       y = (GlHeight-h)/2;
       break;
     case INTEGER_RATIO:
     case INTEGER_RATIO_FULL:
      Int = floor(Int);
       w = Int * width;
       h = Int * height;
       x = (GlWidth-w)/2;
       y = (GlHeight-h)/2;
       break;
     default:
        break;
    }
    scale = MAX(w / (double)_Ygl->rwidth,
                h / (double)_Ygl->rheight);
#else
  //Libretro is taking care to the resize
  w = width;
  h = height;
  x = y = 0;
#endif
   glViewport(0, 0, GlWidth, GlHeight);

   VIDCore->setupFrame();

    glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->original_fbo);
    glDrawBuffers(NB_RENDER_LAYER, &DrawBuffers[0]);
    //glClearBufferfv(GL_COLOR, 0, col);
#ifdef DEBUG_BLIT
    //glClearBufferfv(GL_COLOR, 1, col);
    //glClearBufferfv(GL_COLOR, 2, col);
    //glClearBufferfv(GL_COLOR, 3, col);
    //glClearBufferfv(GL_COLOR, 4, col);
#endif

   glDepthMask(GL_FALSE);
   glViewport(0, 0, _Ygl->width, _Ygl->height);
   glGetIntegerv( GL_VIEWPORT, _Ygl->m_viewport );
   glScissor(0, 0, _Ygl->width, _Ygl->height);
   glEnable(GL_SCISSOR_TEST);

   //glClearBufferfv(GL_COLOR, 0, colopaque);
   //glClearBufferfi(GL_DEPTH_STENCIL, 0, 0, 0);
   if ((varVdp2Regs->TVMD & 0x8000)==0) {
     /* ST-058-R2, TVMD: DISP=0 -> the picture is not displayed; the whole
      * display shows black (BDCLMD=0) or the back screen (BDCLMD=1).
      * Returning without touching the output left the last composed image
      * on screen instead. */
     float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
     glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
     glViewport(0, 0, GlWidth, GlHeight);
     glScissor(0, 0, GlWidth, GlHeight);
     glClearBufferfv(GL_COLOR, 0, (varVdp2Regs->TVMD & 0x100) ? _Ygl->last_back_color : black);
   }
   if (((varVdp2Regs->TVMD & 0x8000)==0) || (YglTM_vdp2 == NULL)) {
     finishCSRender();
     return;
   }
   glBindTexture(GL_TEXTURE_2D, YglTM_vdp2->textureID);
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

   YglUpdateVdp2Reg();
   if (_Ygl->needWinUpdate) {
     YglSetWindow(0);
     YglSetWindow(1);
     _Ygl->needWinUpdate = 0;
   }
   cprg = -1;

   if (_Ygl->ColorRamNeedSync != 0) {
     //Need to update the colorRamLine mapping
     glBindTexture(GL_TEXTURE_2D, _Ygl->cram_map_tex);
     glTexImage2D(GL_TEXTURE_2D,
          0,
          GL_RGBA,
          512, 1,
          0,
          GL_RGBA, GL_UNSIGNED_BYTE,
          &_Ygl->colorRamIndexFull[0]);
    _Ygl->ColorRamNeedSync = 0;
   }
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, YglTM_vdp2->textureID);

  int min = 8;
  int oldPrio = 0;

  int nbPrio = 0;
  int minPrio = -1;
  int allPrio = 0;


  for (int i = 0; i < SPRITE; i++) {
    if ((i == RBG0) || (i == RBG1)) {
      glViewport(0, 0, _Ygl->width, _Ygl->height);
      glScissor(0, 0, _Ygl->width, _Ygl->height);
      glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->rbg_compute_fbo);
      if ( i == RBG0)
        glDrawBuffers(1, &DrawBuffers[0]);
      else
        glDrawBuffers(1, &DrawBuffers[1]);
    } else {
      glViewport(0, 0, _Ygl->rwidth, _Ygl->rheight);
      glScissor(0, 0, _Ygl->rwidth, _Ygl->rheight);
      glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->screen_fbo);
      glDrawBuffers(1, &DrawBuffers[i]);
    }
    drawScreen[i] = DrawVDP2Screen(varVdp2Regs, i);
    // Video CD Card: when EXBG is enabled the external picture *is* the
    // NBG1 screen (VDP2 manual §1.3/§2.5), so it replaces whatever NBG1
    // drew. DrawVDP2Screen() still runs first, to consume and reset the
    // NBG1 command queue for this frame.
    if (i == NBG1 && VIDCSDrawMpegExbg(varVdp2Regs))
      drawScreen[i] = 1;
    if ((Vdp2External.disptoggle & (1<<i)) == 0) {
      drawScreen[i] = 0;
    }
  }

  const int vdp2screens[] = {RBG0, RBG1, NBG0, NBG1, NBG2, NBG3};

  int prioscreens[6] = {0};
  int modescreens[7] = {0};
  int useLineColorOffset[6] = {0};
  int isRGB[7] = {0};
  int isBlur[7] = {0};
  int isPerline[8] = {0};
  int isShadow[7] = {0};
  glDisable(GL_BLEND);
  int id = 0;

  /* VDP2 Manual §10 p.195 (LNCLEN register 1800E8H):
   *   bit 0 = N0LCEN (NBG0, shared with RBG1)
   *   bit 1 = N1LCEN (NBG1, shared with EXBG)
   *   bit 2 = N2LCEN (NBG2)
   *   bit 3 = N3LCEN (NBG3)
   *   bit 4 = R0LCEN (RBG0)
   *   bit 5 = SPLCEN (Sprite)                                     
   */
  lncl[0] = (varVdp2Regs->LNCLEN >> 0) & 0x1; /* NBG0 */
  lncl[1] = (varVdp2Regs->LNCLEN >> 1) & 0x1; /* NBG1 */
  lncl[2] = (varVdp2Regs->LNCLEN >> 2) & 0x1; /* NBG2 */
  lncl[3] = (varVdp2Regs->LNCLEN >> 3) & 0x1; /* NBG3 */
  lncl[4] = (varVdp2Regs->LNCLEN >> 4) & 0x1; /* RBG0 */
  lncl[5] = lncl[0];                          /* RBG1 shares N0LCEN with NBG0 */
  lncl[6] = (varVdp2Regs->LNCLEN >> 5) & 0x1; /* SPRITE */

  for (int j=0; j<6; j++) {
    if (drawScreen[vdp2screens[j]] != 0) {
      if ((vdp2screens[j] == RBG0) ||(vdp2screens[j] == RBG1)) {
        if (vdp2screens[j] == RBG0)
        prioscreens[id] = _Ygl->rbg_compute_fbotex[0];
        else
        prioscreens[id] = _Ygl->rbg_compute_fbotex[1];
      } else {
        prioscreens[id] = _Ygl->screen_fbotex[vdp2screens[j]];
      }
      if (vdp2screens[j] == RBG0) useLineColorOffset[id] = _Ygl->useLineColorOffset[0];
      if (vdp2screens[j] == RBG1) useLineColorOffset[id] = _Ygl->useLineColorOffset[1];
      /* Blend mode from the first line where this layer's colour
       * calculation is enabled, not from line 0 only; the per-line enable
       * is applied in the shader. See VIDCSBlendRegsForLayer() (vidcs.c). */
      modescreens[id] =  setupBlend(VIDCSBlendRegsForLayer(varVdp2Regs, vdp2screens[j]), vdp2screens[j]);
      isRGB[id] = setupColorMode(varVdp2Regs, vdp2screens[j]);
      isBlur[id] = setupBlur(varVdp2Regs, vdp2screens[j]);
      isPerline[id] = vdp2screens[j];
      isShadow[id] = setupShadow(varVdp2Regs, vdp2screens[j]);
      lncl_draw[id] = lncl[vdp2screens[j]];
      winS_draw |= (WinS[vdp2screens[j]]<<id);
      winS_mode_draw |= (WinS_mode[vdp2screens[j]]<<id);
      win0_draw |= (_Ygl->Win0[vdp2screens[j]]<<id);
      win0_mode_draw |= (_Ygl->Win0_mode[vdp2screens[j]]<<id);
      win1_draw |= (_Ygl->Win1[vdp2screens[j]]<<id);
      win1_mode_draw |= (_Ygl->Win1_mode[vdp2screens[j]]<<id);
      win_op_draw |= (_Ygl->Win_op[vdp2screens[j]]<<id);
      win_all_draw |= (_Ygl->WinAll[vdp2screens[j]]<<id);
      id++;
    }
  }
  isBlur[6] = setupBlur(varVdp2Regs, SPRITE);
  lncl_draw[6] = lncl[6];
  isPerline[6] = 6;
  isPerline[7] = 7;

  /* The WinS / Win0 / Win1 arrays are sized enBGMAX+1 (=8) so
   * slots SPRITE (6) and CCWIN (7) exist. Slot 7 represents the
   * Color Calculation Window defined in VDP2 Manual §11.6 — it
   * gates color-calc across all layers and is distinct from the
   * per-layer sprite window covered by slot SPRITE.
   * Reference: VDP2 User's Manual ST-058-R2-060194 §11.5-11.6 */
  enum { SLOT_CCWIN = enBGMAX };   /* slot 7 */
  for (int i = SPRITE; i <= SLOT_CCWIN; i++) {
    winS_draw      |= WinS[i]            << i;
    winS_mode_draw |= WinS_mode[i]       << i;
    win0_draw      |= _Ygl->Win0[i]      << i;
    win0_mode_draw |= _Ygl->Win0_mode[i] << i;
    win1_draw      |= _Ygl->Win1[i]      << i;
    win1_mode_draw |= _Ygl->Win1_mode[i] << i;
    win_op_draw    |= _Ygl->Win_op[i]    << i;
    win_all_draw   |= _Ygl->WinAll[i]    << i;
  }

  isShadow[6] = setupShadow(varVdp2Regs, SPRITE); //Use sprite index for background suuport

  glViewport(0, 0, _Ygl->width, _Ygl->height);
  glGetIntegerv( GL_VIEWPORT, _Ygl->m_viewport );
  glScissor(0, 0, _Ygl->width, _Ygl->height);

  modescreens[6] =  setupBlend(varVdp2Regs, 6);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->back_fbo);
  glDrawBuffers(1, &DrawBuffers[0]);
  // Toujours appeler YglDrawBackScreen() : en mode single color,
  // Vdp2DrawBackScreen() a déjà rempli back_tex avec la couleur uniforme
  // sur toutes les lignes. Sans cet appel, back_fbotex garde les données
  // résiduelles du frame précédent, visibles via YglFillWithBackScreen()
  // et le uniform s_back du shader de composition.
  YglDrawBackScreen();
  // Video CD Card fallback path only (see VIDCSDrawMpegBackScreen).
  VIDCSDrawMpegBackScreen(varVdp2Regs, _Ygl->width, _Ygl->height);

  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->original_fbo);

  glDrawBuffers(NB_RENDER_LAYER, &DrawBuffers[0]);
  glClearBufferfi(GL_DEPTH_STENCIL, 0, 0, 0);

  _Ygl->win_all_draw = win_all_draw;
  YglBlitTexture( prioscreens, modescreens, isRGB, isBlur, isPerline, isShadow, lncl_draw, GetCSVDP1fb, winS_draw, winS_mode_draw, win0_draw, win0_mode_draw, win1_draw, win1_mode_draw, win_op_draw, useLineColorOffset, varVdp2Regs);
  srcTexture = _Ygl->original_fbotex[0];

   int scali = (int)(scale);
   glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
#ifndef __LIBRETRO__
  /* Border colour mode of the composed frame (varVdp2Regs), not of the
   * live register which may already belong to the next frame. */
  if ((varVdp2Regs->TVMD & 0x100) != 0) {
    //Use last border color to clear the screen
    glViewport(0, 0, GlWidth, GlHeight);
    glScissor(0, 0, GlWidth, GlHeight);
    glClearBufferfv(GL_COLOR, 0, _Ygl->last_back_color);
    //draw back screen where other pixels are not drawn
    glViewport(0, y, GlWidth, h);
    /* See final blit below: only clip a pixel in integer-ratio
     * modes where the integer source step leaves a visible seam. */
    int border_clip = (modeScreen == INTEGER_RATIO
                    || modeScreen == INTEGER_RATIO_FULL) ? scali : 0;
    glScissor(0, y, GlWidth, h - border_clip);
    //Take care of border
    YglFillWithBackScreen();
  } else {
    glViewport(0, 0, GlWidth, GlHeight);
    glScissor(0, 0, GlWidth, GlHeight);
    float black[4] = {0.0};
    glClearBufferfv(GL_COLOR, 0, black);
  }
   glViewport(x, y, w, h);
   /* Only clip the last row/column in integer scaling modes where
    * the integer source step leaves a visible seam. In STRETCH /
    * ORIGINAL modes the filter covers the edge cleanly, and the
    * subtraction produces a persistent 1-pixel black border. */
   {
     int border_clip = (modeScreen == INTEGER_RATIO
                     || modeScreen == INTEGER_RATIO_FULL) ? scali : 0;
     glScissor(x, y, w - border_clip, h - border_clip);
   }
#endif
   YglBlitFramebuffer(srcTexture, _Ygl->width, _Ygl->height, w, h);

  finishCSRender();
  return;
}
