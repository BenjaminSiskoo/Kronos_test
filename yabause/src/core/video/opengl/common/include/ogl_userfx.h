/*  Copyright 2026 Kronos fork contributors

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

/*! \file ogl_userfx.h
    \brief External GLSL presets (one or several chained passes) for the
           final picture. Not built for libretro.

    Everything except YglUserFxBlit() and YglUserFxDestroy() is pure CPU work
    (files and parsing): it can be called from the UI without an OpenGL
    context. The GL objects are created and released inside YglUserFxBlit().
*/

#ifndef OGL_USERFX_H
#define OGL_USERFX_H

#ifdef __cplusplus
extern "C" {
#endif

#define USERFX_MAX_PASSES 8
#define USERFX_MAX_PARAMS 32
#define USERFX_MAX_FILE   (256 * 1024)

typedef struct {
  char  name[48];     /* GLSL identifier, also the key in the saved settings */
  char  label[96];
  float def;
  float min;
  float max;
  float step;
  float value;        /* current value (default, preset override or user value) */
} YglUserFxParam;

/* Selects the preset: a ".kfx" preset file (several passes) or a bare ".glsl"
   file (one pass). "" or NULL disables the filter. The files are read and
   parsed at once. Returns 0 when everything could be read, -1 otherwise (the
   reason is in YglUserFxGetStatus() and the filter then shows the plain
   picture). The shaders themselves are compiled at the first frame. */
int  YglUserFxSetPreset(const char *path);
const char *YglUserFxGetPreset(void);

/* Reads the preset and its shader files again (e.g. after an edit). */
int  YglUserFxReload(void);

/* Display name (the "name" key of a .kfx, otherwise the file name) and number
   of passes of the loaded preset. */
const char *YglUserFxGetName(void);
int  YglUserFxGetPassCount(void);

/* Adjustable parameters collected from the "#pragma parameter" lines. */
int  YglUserFxGetParamCount(void);
int  YglUserFxGetParam(int index, YglUserFxParam *out);
int  YglUserFxSetParamValue(const char *name, float value);

/* Saved form of the user's values: "NAME=value;NAME=value" (only the values
   that differ from the default). Setting a spec can be done before or after
   the preset is selected. */
void YglUserFxSetParamsSpec(const char *spec);
int  YglUserFxGetParamsSpec(char *buf, int bufsize);

/* Last message (error or "ok"), for the settings window. Never NULL. */
const char *YglUserFxGetStatus(void);

/* Draws the picture through the preset into the framebuffer that is bound
   (default_fbo) with the viewport/scissor set by the caller. Returns 0 when
   the picture was drawn, -1 when the caller must draw the plain picture.
     srcTexture : composed frame            w, h       : its size in pixels
     outW, outH : output size, texture orientation
     nbLines    : "lineNumber.x" of the built-in filters
     srcCols/srcLines : Saturn picture size (before upscaling)           */
int  YglUserFxBlit(unsigned int srcTexture, float w, float h,
                   float outW, float outH, float nbLines,
                   int isRotated, int field, int scale,
                   float srcCols, float srcLines);

/* Releases the GL objects (renderer shutdown or lost context). The parsed
   preset is kept. */
void YglUserFxDestroy(void);

#ifdef __cplusplus
}
#endif

#endif
