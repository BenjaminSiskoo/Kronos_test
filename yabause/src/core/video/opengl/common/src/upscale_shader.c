#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "ygl.h"
#include "hq4x_shader.h"
#include "hq4x_lut.h"
#include "6xbrz_shader.h"
#include "4xbrz_shader.h"
#include "sharpen_shader.h"
#include "upscale_ext_shader.h"

/*
 * Upscale stage of the final output (called by YglBlitFramebuffer).
 *
 * Filters are made of one or two passes. Each pass is a program built on
 * first use and kept until YglUpscaleDestroy() (called by
 * Ygl_prog_Destroy). A program that fails to build is marked as failed
 * and is not retried every frame; the caller then shows the composed
 * frame without upscale instead of aborting.
 *
 *  UP_HQ4X, UP_4XBRZ, UP_6XBRZ : 1 pass, Saturn resolution x4 / x6
 *  UP_SHARPEN                  : 1 pass, composed texture resolution
 *  UP_SCALE3X                  : 1 pass, Saturn resolution x3
 *  UP_SCALE4X                  : Scale2x -> Scale2x, Saturn resolution x4
 *  UP_FSR                      : EASU -> RCAS, output (viewport) resolution
 */

/* RCAS strength in stops (0 = maximum sharpness, each +1 halves it).
 * 0.2 is AMD's usual default. */
#define YGL_FSR_RCAS_SHARPNESS_STOPS 0.2f

enum {
  UPP_HQ4X = 0,
  UPP_4XBRZ,
  UPP_6XBRZ,
  UPP_SHARPEN,
  UPP_SCALE2X,
  UPP_SCALE3X,
  UPP_FSR_EASU,
  UPP_FSR_RCAS,
  UPP_MAX
};

typedef struct {
  GLint prg;      /* 0: not built yet, -1: build failed, >0: program */
  GLint u_dsize;  /* DrawingSize */
  GLint u_tsize;  /* TextureSize */
  GLint u_sharp;  /* Sharpness (RCAS) */
} UpscaleProgram;

static UpscaleProgram up_progs[UPP_MAX];

static const char * const up_prog_name[UPP_MAX] = {
  "HQ4x", "4xBRZ", "6xBRZ", "Sharpen", "Scale2x", "Scale3x", "FSR EASU", "FSR RCAS"
};

static const GLchar * up_vertex_src(int id) {
  switch (id) {
    case UPP_HQ4X:    return Yglprg_blit_hq4x_v;
    case UPP_4XBRZ:   return Yglprg_blit_4xbrz_v;
    case UPP_6XBRZ:   return Yglprg_blit_6xbrz_v;
    case UPP_SHARPEN: return Yglprg_blit_sharpen_v;
    default:          return Yglprg_upscale_pass_v;
  }
}

static const GLchar * up_fragment_src(int id) {
  switch (id) {
    case UPP_HQ4X:     return Yglprg_blit_hq4x_f;
    case UPP_4XBRZ:    return Yglprg_blit_4xbrz_f;
    case UPP_6XBRZ:    return Yglprg_blit_6xbrz_f;
    case UPP_SHARPEN:  return Yglprg_blit_sharpen_f;
    case UPP_SCALE2X:  return Yglprg_upscale_scale2x_f;
    case UPP_SCALE3X:  return Yglprg_upscale_scale3x_f;
    case UPP_FSR_EASU: return Yglprg_upscale_fsr_easu_f;
    case UPP_FSR_RCAS: return Yglprg_upscale_fsr_rcas_f;
    default:           return NULL;
  }
}

static GLuint up_lut_tex = 0;
static GLuint upscale_vbo = 0;

/* Intermediate target of the 2-pass filters */
static GLuint up_tmp_fbo = 0;
static GLuint up_tmp_tex = 0;
static int up_tmp_w = 0;
static int up_tmp_h = 0;

static void Ygl_printShaderError( GLuint shader )
{
  GLsizei bufSize;

  glGetShaderiv(shader, GL_INFO_LOG_LENGTH , &bufSize);

  if (bufSize > 1) {
    GLchar *infoLog;

    infoLog = (GLchar *)malloc(bufSize);
    if (infoLog != NULL) {
      GLsizei length;
      glGetShaderInfoLog(shader, bufSize, &length, infoLog);
      printf("Shaderlog:\n%s\n", infoLog);
      free(infoLog);
    }
  }
}

static void Ygl_printProgError( GLuint prog )
{
  GLint maxLength = 0;

  glGetProgramiv(prog, GL_INFO_LOG_LENGTH , &maxLength);

  if (maxLength > 1) {
    GLchar *infoLog;

    infoLog = (GLchar *)malloc(maxLength);
    if (infoLog != NULL) {
      GLsizei length;
      glGetProgramInfoLog(prog, maxLength, &length, infoLog);
      printf("Proglog:\n%s\n", infoLog);
      free(infoLog);
    }
  }
}

static GLuint up_compile(GLenum type, const GLchar * src, int id)
{
  GLint compiled = GL_FALSE;
  GLuint shader = glCreateShader(type);
  if (shader == 0) return 0;
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_FALSE) {
    printf("Upscale %s: compile error in %s shader.\n", up_prog_name[id],
           (type == GL_VERTEX_SHADER) ? "vertex" : "fragment");
    Ygl_printShaderError(shader);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

/* Build (once) and bind the program of a pass. Returns 0 on success. */
static int up_use_program(int id)
{
  UpscaleProgram * p = &up_progs[id];
  GLuint vshader, fshader, prg;
  GLint linked = GL_FALSE;

  if (p->prg > 0) {
    glUseProgram(p->prg);
    return 0;
  }
  if (p->prg < 0) return -1;

  p->prg = -1; /* until proven otherwise */

  vshader = up_compile(GL_VERTEX_SHADER, up_vertex_src(id), id);
  if (vshader == 0) return -1;
  fshader = up_compile(GL_FRAGMENT_SHADER, up_fragment_src(id), id);
  if (fshader == 0) {
    glDeleteShader(vshader);
    return -1;
  }

  prg = glCreateProgram();
  if (prg == 0) {
    glDeleteShader(vshader);
    glDeleteShader(fshader);
    return -1;
  }
  glAttachShader(prg, vshader);
  glAttachShader(prg, fshader);
  glLinkProgram(prg);
  glGetProgramiv(prg, GL_LINK_STATUS, &linked);
  glDetachShader(prg, vshader);
  glDetachShader(prg, fshader);
  glDeleteShader(vshader);
  glDeleteShader(fshader);
  if (linked == GL_FALSE) {
    printf("Upscale %s: link error.\n", up_prog_name[id]);
    Ygl_printProgError(prg);
    glDeleteProgram(prg);
    return -1;
  }

  p->prg = (GLint)prg;
  glUseProgram(prg);
  glUniform1i(glGetUniformLocation(prg, "Texture"), 0);
  if (id == UPP_HQ4X)
    glUniform1i(glGetUniformLocation(prg, "LUT"), 1);
  p->u_dsize = glGetUniformLocation(prg, "DrawingSize");
  p->u_tsize = glGetUniformLocation(prg, "TextureSize");
  p->u_sharp = glGetUniformLocation(prg, "Sharpness");
  return 0;
}

static int up_tmp_buffer(int w, int h)
{
  if (up_tmp_fbo == 0) {
    glGenTextures(1, &up_tmp_tex);
    glBindTexture(GL_TEXTURE_2D, up_tmp_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &up_tmp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, up_tmp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, up_tmp_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      printf("Upscale: intermediate framebuffer %dx%d incomplete\n", w, h);
      glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
      glDeleteFramebuffers(1, &up_tmp_fbo);
      glDeleteTextures(1, &up_tmp_tex);
      up_tmp_fbo = 0;
      up_tmp_tex = 0;
      up_tmp_w = up_tmp_h = 0;
      return -1;
    }
    up_tmp_w = w;
    up_tmp_h = h;
  } else if ((up_tmp_w != w) || (up_tmp_h != h)) {
    glBindTexture(GL_TEXTURE_2D, up_tmp_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    up_tmp_w = w;
    up_tmp_h = h;
  }
  return 0;
}

/* One full-screen pass: program id must be bound and its uniforms set. */
static void up_draw(int id, u32 tex, u32 fbo, int outw, int outh)
{
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, outw, outh);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (id == UPP_HQ4X) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, up_lut_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void up_set_sizes(int id, float dw, float dh, float tw, float th)
{
  UpscaleProgram * p = &up_progs[id];
  if (p->u_dsize >= 0) glUniform2f(p->u_dsize, dw, dh);
  if (p->u_tsize >= 0) glUniform2f(p->u_tsize, tw, th);
}

/* Size of the upscale output for a mode.
 *   w, h       : Saturn resolution (rwidth, rheight)
 *   texw, texh : composed texture size (internal resolution)
 *   dispw/h    : output viewport size, in texture orientation */
void YglUpscaleGetOutputSize(int mode, int w, int h, int texw, int texh, int dispw, int disph, int *outw, int *outh)
{
  switch (mode) {
    case UP_HQ4X:
    case UP_4XBRZ:
    case UP_SCALE4X:
      *outw = 4 * w;
      *outh = 4 * h;
      break;
    case UP_6XBRZ:
      *outw = 6 * w;
      *outh = 6 * h;
      break;
    case UP_SCALE3X:
      *outw = 3 * w;
      *outh = 3 * h;
      break;
    case UP_SHARPEN:
      /* Sharpen works on the composed texels: keep the internal
       * resolution instead of reducing it to the Saturn one. */
      *outw = texw;
      *outh = texh;
      break;
    case UP_FSR:
      *outw = dispw;
      *outh = disph;
      break;
    default:
      *outw = w;
      *outh = h;
      break;
  }
}

int YglUpscaleFramebuffer(u32 srcTexture, u32 targetFbo, float w, float h, float texw, float texh, int outw, int outh) {

  float const vertexPosition[] = {
    1.0, -1.0f,
    -1.0, -1.0f,
    1.0, 1.0f,
    -1.0, 1.0f,
    1.0f, 0.0f,
    0.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 1.0f
  };
  int ret = 0;
  int mode = _Ygl->upmode;

  if ((outw <= 0) || (outh <= 0) || (w <= 0.0f) || (h <= 0.0f)) return -1;

  /* Check the programs of the mode before touching any GL state */
  switch (mode) {
    case UP_HQ4X:    ret = up_use_program(UPP_HQ4X); break;
    case UP_4XBRZ:   ret = up_use_program(UPP_4XBRZ); break;
    case UP_6XBRZ:   ret = up_use_program(UPP_6XBRZ); break;
    case UP_SHARPEN: ret = up_use_program(UPP_SHARPEN); break;
    case UP_SCALE3X: ret = up_use_program(UPP_SCALE3X); break;
    case UP_SCALE4X: ret = up_use_program(UPP_SCALE2X); break;
    case UP_FSR:
      ret = up_use_program(UPP_FSR_RCAS);
      if (ret == 0) ret = up_use_program(UPP_FSR_EASU);
      break;
    default:
      ret = -1;
      break;
  }
  if (ret != 0) return -1;

  if ((mode == UP_HQ4X) && (up_lut_tex == 0)) {
    glGenTextures(1, &up_lut_tex);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, up_lut_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hq4x_LUT.width, hq4x_LUT.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, hq4x_LUT.pixel_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  if ((mode == UP_SCALE4X) || (mode == UP_FSR)) {
    int tw = (mode == UP_SCALE4X) ? (int)(2.0f * w) : outw;
    int th = (mode == UP_SCALE4X) ? (int)(2.0f * h) : outh;
    if (up_tmp_buffer(tw, th) != 0) return -1;
  }

  if (upscale_vbo == 0) {
    glGenBuffers(1, &upscale_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, upscale_vbo);
    glBufferData(GL_ARRAY_BUFFER, 16*sizeof(float), vertexPosition, GL_STATIC_DRAW);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);

  glBindBuffer(GL_ARRAY_BUFFER, upscale_vbo);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void*)(8*sizeof(float)));

  switch (mode) {
    case UP_HQ4X:
    case UP_4XBRZ:
    case UP_6XBRZ:
    case UP_SHARPEN:
    case UP_SCALE3X:
    {
      int id = (mode == UP_HQ4X) ? UPP_HQ4X :
               (mode == UP_4XBRZ) ? UPP_4XBRZ :
               (mode == UP_6XBRZ) ? UPP_6XBRZ :
               (mode == UP_SHARPEN) ? UPP_SHARPEN : UPP_SCALE3X;
      /* Already bound by the check above */
      up_set_sizes(id, w, h, texw, texh);
      up_draw(id, srcTexture, targetFbo, outw, outh);
      break;
    }
    case UP_SCALE4X:
      /* Scale2x on the Saturn pixel grid -> 2x, then Scale2x again -> 4x */
      up_set_sizes(UPP_SCALE2X, w, h, texw, texh);
      up_draw(UPP_SCALE2X, srcTexture, up_tmp_fbo, up_tmp_w, up_tmp_h);
      up_set_sizes(UPP_SCALE2X, (float)up_tmp_w, (float)up_tmp_h, (float)up_tmp_w, (float)up_tmp_h);
      up_draw(UPP_SCALE2X, up_tmp_tex, targetFbo, outw, outh);
      break;
    case UP_FSR:
      /* EASU: composed texture (internal resolution) -> output size */
      up_set_sizes(UPP_FSR_EASU, (float)outw, (float)outh, texw, texh);
      up_draw(UPP_FSR_EASU, srcTexture, up_tmp_fbo, outw, outh);
      /* RCAS at output size */
      up_use_program(UPP_FSR_RCAS);
      up_set_sizes(UPP_FSR_RCAS, (float)outw, (float)outh, (float)outw, (float)outh);
      if (up_progs[UPP_FSR_RCAS].u_sharp >= 0)
        glUniform1f(up_progs[UPP_FSR_RCAS].u_sharp, powf(2.0f, -YGL_FSR_RCAS_SHARPNESS_STOPS));
      up_draw(UPP_FSR_RCAS, up_tmp_tex, targetFbo, outw, outh);
      break;
  }

  // Clean up
  glActiveTexture(GL_TEXTURE0);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);

  return 0;
}

void YglUpscaleDestroy(void)
{
  int i;
  for (i = 0; i < UPP_MAX; i++) {
    if (up_progs[i].prg > 0) glDeleteProgram(up_progs[i].prg);
    up_progs[i].prg = 0;
    up_progs[i].u_dsize = -1;
    up_progs[i].u_tsize = -1;
    up_progs[i].u_sharp = -1;
  }
  if (up_lut_tex != 0) glDeleteTextures(1, &up_lut_tex);
  up_lut_tex = 0;
  if (upscale_vbo != 0) glDeleteBuffers(1, &upscale_vbo);
  upscale_vbo = 0;
  if (up_tmp_fbo != 0) glDeleteFramebuffers(1, &up_tmp_fbo);
  if (up_tmp_tex != 0) glDeleteTextures(1, &up_tmp_tex);
  up_tmp_fbo = 0;
  up_tmp_tex = 0;
  up_tmp_w = 0;
  up_tmp_h = 0;
}
