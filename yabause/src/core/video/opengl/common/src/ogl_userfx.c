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

/*! \file ogl_userfx.c
    \brief External GLSL presets: one or several chained passes applied to the
           final picture. See shaders/README.md for the file formats.

    Design
    ------
    A preset is either
      - a bare ".glsl" file : one pass, defines  vec4 Filter(sampler2D, vec2)
      - a ".kfx" file       : a list of ".glsl" passes with, for each pass, the
                              sampling filter, the size of its output, an alias
                              and optional parameter defaults.
    Every pass is the same kind of shader as the built-in filters of
    YglBlitFramebuffer(): a Filter() function wrapped by a fixed vertex shader
    that draws a quad. Pass N reads the output of pass N-1 (pass 0 reads the
    composed Saturn frame); the last pass draws into the framebuffer that the
    caller bound. Earlier outputs and the original frame stay readable by later
    passes (Original, PassOutput0..6 and the aliases).

    Threading : same as the video core. The UI functions are called from the
    GUI thread, which is also the emulation thread in the Qt port; they only do
    file and parsing work and never call OpenGL. */

#ifndef __LIBRETRO__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "ygl.h"
#include "yui.h"
#include "ogl_userfx.h"

#ifdef _WIN32
#define UFX_STAT _stat
#define UFX_STRUCT_STAT struct _stat
#else
#define UFX_STAT stat
#define UFX_STRUCT_STAT struct stat
#endif

#define UFX_PATH_LEN 600
#define UFX_MAX_OVERRIDES 96
#define UFX_RELOAD_PERIOD 30   /* frames between two checks of the file dates */

enum { SCALE_SOURCE = 0, SCALE_VIEWPORT = 1, SCALE_ABSOLUTE = 2 };

/* ---------------------------------------------------------------- state --- */

typedef struct {
  char   file[UFX_PATH_LEN];
  char  *text;                 /* contents, '\0' terminated */
  long   mtime;
  int    linear;               /* sample the input with bilinear filtering */
  int    scaleType;
  float  sx, sy;               /* factor, or pixels when SCALE_ABSOLUTE */
  int    floatFb;              /* RGBA16F target */
  char   alias[32];
} UfxPass;

typedef struct {
  char    path[UFX_PATH_LEN];
  long    mtime;
  char    name[96];
  int     isPreset;            /* .kfx */
  int     npass;
  int     ok;                  /* every file was read */
  UfxPass pass[USERFX_MAX_PASSES];
  int     nparam;
  YglUserFxParam prm[USERFX_MAX_PARAMS];
  /* defaults given by the preset file ("param_NAME = value") */
  int     npdef;
  char    pdefName[USERFX_MAX_PARAMS][48];
  float   pdefValue[USERFX_MAX_PARAMS];
} UfxPreset;

typedef struct {
  char  name[48];
  float value;
} UfxOverride;

static UfxPreset  P;                         /* parsed preset */
static UfxOverride OV[UFX_MAX_OVERRIDES];    /* values chosen by the user */
static int        nOV = 0;
static char       status[640] = "no preset";
static int        dirty = 0;                 /* GL objects must be rebuilt */
static unsigned   frameCounter = 0;

enum {
  U_FWIDTH, U_FHEIGHT, U_LINENUMBER, U_DECIM, U_FIELD, U_SCALE, U_OUTSIZE,
  U_SRCLINES, U_SRCCOLS, U_ROTATED, U_FRAMECOUNT, U_INPUTSIZE, U_ORIGINALSIZE,
  U_PASSSIZE, U_COUNT
};
static const char *const uNames[U_COUNT] = {
  "fWidth", "fHeight", "lineNumber", "decim", "field", "scale", "outSize",
  "srcLines", "srcCols", "rotated", "FrameCount", "InputSize", "OriginalSize",
  "PassSize"
};

typedef struct {
  GLuint prg;
  GLuint tex;
  GLuint fbo;
  int    tw, th;
  int    texFloat;
  GLint  loc[U_COUNT];
  GLint  ploc[USERFX_MAX_PARAMS];
} UfxGl;

static UfxGl  G[USERFX_MAX_PASSES];
static int    glState = 0;                   /* 0 = to build, 1 = ready, 2 = failed */
static GLuint tcBuf[2] = { 0, 0 };

/* Names a parameter or an alias may not take */
static const char *const reserved[] = {
  "fWidth", "fHeight", "lineNumber", "decim", "field", "scale", "outSize",
  "srcLines", "srcCols", "rotated", "u_Src", "vTexCoord", "fragColor",
  "FrameCount", "InputSize", "OriginalSize", "PassSize", "SourceSize",
  "OutputSize", "Original", "Filter", "main", NULL
};

/* -------------------------------------------------------------- helpers --- */

static void setStatus(const char *fmt, const char *a, const char *b)
{
  snprintf(status, sizeof(status), fmt, a ? a : "", b ? b : "");
}

static int isIdentStart(int c) { return isalpha(c) || c == '_'; }
static int isIdentChar(int c)  { return isalnum(c) || c == '_'; }

static int validIdent(const char *s)
{
  int i;
  if (s == NULL || !isIdentStart((unsigned char)s[0])) return 0;
  for (i = 1; s[i] != 0; i++) if (!isIdentChar((unsigned char)s[i])) return 0;
  return 1;
}

static int isReserved(const char *s)
{
  int i;
  for (i = 0; reserved[i] != NULL; i++) if (strcmp(reserved[i], s) == 0) return 1;
  if (strncmp(s, "PassOutput", 10) == 0) return 1;
  return 0;
}

static int endsWithNoCase(const char *s, const char *suffix)
{
  size_t a = strlen(s), b = strlen(suffix), i;
  if (b > a) return 0;
  for (i = 0; i < b; i++)
    if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
  return 1;
}

/* Locale independent: the decimal separator is always '.' */
static int parseFloat(const char **pp, float *out)
{
  const char *p = *pp;
  double v = 0.0, scale = 1.0;
  int neg = 0, digits = 0;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
  while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; digits++; }
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') { scale *= 0.1; v += (*p - '0') * scale; p++; digits++; }
  }
  if (digits == 0) return 0;
  if (*p == 'e' || *p == 'E') {
    const char *q = p + 1;
    int eneg = 0, e = 0, ed = 0;
    if (*q == '+' || *q == '-') { eneg = (*q == '-'); q++; }
    while (*q >= '0' && *q <= '9') { if (e < 400) e = e * 10 + (*q - '0'); q++; ed++; }
    if (ed > 0) {
      while (e-- > 0) v = eneg ? v * 0.1 : v * 10.0;
      p = q;
    }
  }
  *out = (float)(neg ? -v : v);
  *pp = p;
  return 1;
}

static long fileTime(const char *path)
{
  UFX_STRUCT_STAT st;
  if (UFX_STAT(path, &st) != 0) return -1;
  return (long)st.st_mtime;
}

/* Whole file in a new buffer, or NULL (the reason goes to the status). */
static char *readFile(const char *path)
{
  FILE *f = fopen(path, "rb");
  char *buf;
  long size;
  size_t got;
  if (f == NULL) { setStatus("cannot open %s%s", path, ""); return NULL; }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0 || size > USERFX_MAX_FILE) {
    fclose(f);
    setStatus("%s is empty, unreadable or over 256 KB%s", path, "");
    return NULL;
  }
  buf = (char *)malloc((size_t)size + 1);
  if (buf == NULL) { fclose(f); return NULL; }
  got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[got] = 0;
  /* a NUL inside the text would cut the shader short */
  if (strlen(buf) != got) {
    free(buf);
    setStatus("%s contains a NUL byte%s", path, "");
    return NULL;
  }
  return buf;
}

static int isAbsolutePath(const char *p)
{
  if (p[0] == '/' || p[0] == '\\') return 1;
  if (isalpha((unsigned char)p[0]) && p[1] == ':') return 1;
  return 0;
}

static void joinPath(char *out, size_t cap, const char *presetPath, const char *file)
{
  const char *slash, *slash2;
  size_t n;
  if (isAbsolutePath(file)) { snprintf(out, cap, "%s", file); return; }
  slash  = strrchr(presetPath, '/');
  slash2 = strrchr(presetPath, '\\');
  if (slash2 != NULL && (slash == NULL || slash2 > slash)) slash = slash2;
  if (slash == NULL) { snprintf(out, cap, "%s", file); return; }
  n = (size_t)(slash - presetPath) + 1;
  if (n >= cap) n = cap - 1;
  memcpy(out, presetPath, n);
  snprintf(out + n, cap - n, "%s", file);
}

static void trim(char *s)
{
  size_t n = strlen(s), i = 0;
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
  while (s[i] == ' ' || s[i] == '\t') i++;
  if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

/* -------------------------------------------------------- parameters ----- */

static int findOverride(const char *name)
{
  int i;
  for (i = 0; i < nOV; i++) if (strcmp(OV[i].name, name) == 0) return i;
  return -1;
}

static void setOverride(const char *name, float v)
{
  int i = findOverride(name);
  if (i < 0) {
    if (nOV >= UFX_MAX_OVERRIDES) return;
    i = nOV++;
    snprintf(OV[i].name, sizeof(OV[i].name), "%s", name);
  }
  OV[i].value = v;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* default < preset file < user value */
static void resolveValues(void)
{
  int i, j;
  for (i = 0; i < P.nparam; i++) {
    YglUserFxParam *p = &P.prm[i];
    p->value = p->def;
    for (j = 0; j < P.npdef; j++)
      if (strcmp(P.pdefName[j], p->name) == 0) p->value = P.pdefValue[j];
    j = findOverride(p->name);
    if (j >= 0) p->value = OV[j].value;
    p->value = clampf(p->value, p->min, p->max);
  }
}

static int findParam(const char *name)
{
  int i;
  for (i = 0; i < P.nparam; i++) if (strcmp(P.prm[i].name, name) == 0) return i;
  return -1;
}

/* #pragma parameter NAME "Label" default min max [step] */
static void parseParamLine(const char *p, const char *file)
{
  YglUserFxParam prm;
  char name[48];
  size_t n = 0;
  float def, mn, mx, step = 0.0f;

  while (*p == ' ' || *p == '\t') p++;
  while (isIdentChar((unsigned char)*p) && n < sizeof(name) - 1) name[n++] = *p++;
  name[n] = 0;
  if (!validIdent(name) || isIdentChar((unsigned char)*p)) {
    YuiMsg("User shader %s: bad parameter name, line ignored\n", file);
    return;
  }
  while (*p == ' ' || *p == '\t') p++;
  memset(&prm, 0, sizeof(prm));
  if (*p == '"') {
    size_t k = 0;
    p++;
    while (*p != 0 && *p != '"' && *p != '\n' && k < sizeof(prm.label) - 1) prm.label[k++] = *p++;
    prm.label[k] = 0;
    if (*p == '"') p++;
  } else {
    snprintf(prm.label, sizeof(prm.label), "%s", name);
  }
  if (!parseFloat(&p, &def) || !parseFloat(&p, &mn) || !parseFloat(&p, &mx)) {
    YuiMsg("User shader %s: parameter %s needs default, min and max, line ignored\n", file, name);
    return;
  }
  if (!parseFloat(&p, &step) || step <= 0.0f) step = (mx - mn) / 100.0f;
  if (mx < mn) { float t = mn; mn = mx; mx = t; }
  if (isReserved(name)) {
    YuiMsg("User shader %s: parameter %s uses a reserved name, ignored\n", file, name);
    return;
  }
  if (findParam(name) >= 0) return;               /* first declaration wins */
  if (P.nparam >= USERFX_MAX_PARAMS) {
    YuiMsg("User shader %s: more than %d parameters, %s ignored\n", file, USERFX_MAX_PARAMS, name);
    return;
  }
  snprintf(prm.name, sizeof(prm.name), "%s", name);
  prm.def = def; prm.min = mn; prm.max = mx; prm.step = step; prm.value = def;
  P.prm[P.nparam++] = prm;
}

/* Scans the shader text for "#pragma parameter" and "#pragma filter linear". */
static void scanPragmas(UfxPass *ps, int fileHasFilterKey)
{
  const char *p = ps->text;
  while (*p != 0) {
    const char *s = p;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "#pragma", 7) == 0 && (s[7] == ' ' || s[7] == '\t')) {
      s += 7;
      while (*s == ' ' || *s == '\t') s++;
      if (strncmp(s, "parameter", 9) == 0 && (s[9] == ' ' || s[9] == '\t')) {
        parseParamLine(s + 9, ps->file);
      } else if (strncmp(s, "filter", 6) == 0 && (s[6] == ' ' || s[6] == '\t')) {
        s += 6;
        while (*s == ' ' || *s == '\t') s++;
        if (!fileHasFilterKey) ps->linear = (strncmp(s, "linear", 6) == 0);
      }
    }
    p = strchr(p, '\n');
    if (p == NULL) break;
    p++;
  }
}

/* ------------------------------------------------------------- loading --- */

static void freePasses(void)
{
  int i;
  for (i = 0; i < USERFX_MAX_PASSES; i++) {
    free(P.pass[i].text);
    P.pass[i].text = NULL;
  }
}

static void clearPreset(void)
{
  freePasses();
  memset(&P, 0, sizeof(P));
}

static int loadPassFile(UfxPass *ps)
{
  /* the date is kept even when the read fails, so that fixing the file is noticed */
  ps->mtime = fileTime(ps->file);
  ps->text = readFile(ps->file);
  if (ps->text == NULL) return -1;
  return 0;
}

/* Splits "scale_type2" into base "scale_type" and index 2 (-1 if no digits). */
static int splitKey(const char *key, char *base, size_t cap)
{
  size_t n = strlen(key), d = n;
  int idx;
  while (d > 0 && key[d - 1] >= '0' && key[d - 1] <= '9') d--;
  if (d == n) { snprintf(base, cap, "%s", key); return -1; }
  idx = atoi(key + d);
  if (d >= cap) d = cap - 1;
  memcpy(base, key, d);
  base[d] = 0;
  return idx;
}

static int parseKfx(const char *text)
{
  char *copy = (char *)malloc(strlen(text) + 1);
  char *line, *next;
  int declared = 0, i;
  if (copy == NULL) return -1;
  strcpy(copy, text);

  for (i = 0; i < USERFX_MAX_PASSES; i++) {
    P.pass[i].scaleType = SCALE_SOURCE;
    P.pass[i].sx = 1.0f; P.pass[i].sy = 1.0f;
  }
  /* mark "filter" keys given explicitly, they win over #pragma filter */
  {
    int explicitFilter[USERFX_MAX_PASSES] = { 0 };
    int shaderGiven[USERFX_MAX_PASSES] = { 0 };

    for (line = copy; line != NULL; line = next) {
      char *eq, key[64], base[64], val[UFX_PATH_LEN];
      int idx;
      next = strchr(line, '\n');
      if (next != NULL) { *next = 0; next++; }
      trim(line);
      if (line[0] == 0 || line[0] == '#' || (line[0] == '/' && line[1] == '/') || line[0] == ';') continue;
      eq = strchr(line, '=');
      if (eq == NULL) continue;
      *eq = 0;
      snprintf(key, sizeof(key), "%s", line);
      snprintf(val, sizeof(val), "%s", eq + 1);
      trim(key); trim(val);
      if (val[0] == '"') {                     /* "quoted value" */
        size_t n = strlen(val);
        if (n >= 2 && val[n - 1] == '"') { val[n - 1] = 0; memmove(val, val + 1, n - 1); }
      }

      if (strncmp(key, "param_", 6) == 0) {      /* case-sensitive name */
        const char *v = val;
        float f;
        if (validIdent(key + 6) && parseFloat(&v, &f) && P.npdef < USERFX_MAX_PARAMS) {
          snprintf(P.pdefName[P.npdef], sizeof(P.pdefName[0]), "%s", key + 6);
          P.pdefValue[P.npdef++] = f;
        }
        continue;
      }
      {
        char *c;
        for (c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
      }
      idx = splitKey(key, base, sizeof(base));
      if (idx < 0) {
        if (strcmp(key, "shaders") == 0) declared = atoi(val);
        else if (strcmp(key, "name") == 0) snprintf(P.name, sizeof(P.name), "%s", val);
        continue;
      }
      if (idx >= USERFX_MAX_PASSES) continue;
      if (strcmp(base, "shader") == 0) {
        joinPath(P.pass[idx].file, sizeof(P.pass[idx].file), P.path, val);
        shaderGiven[idx] = 1;
      } else if (strcmp(base, "filter") == 0 || strcmp(base, "filter_linear") == 0) {
        P.pass[idx].linear = (strcmp(val, "linear") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        explicitFilter[idx] = 1;
      } else if (strcmp(base, "scale_type") == 0) {
        if (strcmp(val, "viewport") == 0)      P.pass[idx].scaleType = SCALE_VIEWPORT;
        else if (strcmp(val, "absolute") == 0) P.pass[idx].scaleType = SCALE_ABSOLUTE;
        else                                   P.pass[idx].scaleType = SCALE_SOURCE;
      } else if (strcmp(base, "scale") == 0) {
        const char *v = val; float f;
        if (parseFloat(&v, &f)) { P.pass[idx].sx = f; P.pass[idx].sy = f; }
      } else if (strcmp(base, "scale_x") == 0) {
        const char *v = val; float f;
        if (parseFloat(&v, &f)) P.pass[idx].sx = f;
      } else if (strcmp(base, "scale_y") == 0) {
        const char *v = val; float f;
        if (parseFloat(&v, &f)) P.pass[idx].sy = f;
      } else if (strcmp(base, "alias") == 0) {
        if (validIdent(val) && !isReserved(val) && strlen(val) < sizeof(P.pass[idx].alias))
          snprintf(P.pass[idx].alias, sizeof(P.pass[idx].alias), "%s", val);
        else
          YuiMsg("User shader %s: alias \"%s\" is not usable, ignored\n", P.path, val);
      } else if (strcmp(base, "float_framebuffer") == 0 || strcmp(base, "float") == 0) {
        P.pass[idx].floatFb = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
      }
    }

    if (declared < 1 || declared > USERFX_MAX_PASSES) {
      free(copy);
      setStatus("%s: \"shaders\" must be between 1 and 8%s", P.path, "");
      return -1;
    }
    for (i = 0; i < declared; i++) {
      if (!shaderGiven[i]) {
        char num[16];
        free(copy);
        snprintf(num, sizeof(num), "%d", i);
        setStatus("%s: shader%s is missing", P.path, num);
        return -1;
      }
    }
    P.npass = declared;
    for (i = 0; i < declared; i++) P.pass[i].mtime = fileTime(P.pass[i].file);
    for (i = 0; i < declared; i++) {
      if (loadPassFile(&P.pass[i]) != 0) { free(copy); return -1; }
      if (!explicitFilter[i]) P.pass[i].linear = 0;
      scanPragmas(&P.pass[i], explicitFilter[i]);
    }
  }
  free(copy);
  return 0;
}

static int loadCurrent(void)
{
  char path[UFX_PATH_LEN];
  int rc;
  snprintf(path, sizeof(path), "%s", P.path);
  clearPreset();
  snprintf(P.path, sizeof(P.path), "%s", path);
  dirty = 1;
  if (path[0] == 0) { setStatus("no preset%s%s", "", ""); return 0; }

  P.mtime = fileTime(path);
  {
    const char *slash = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs != NULL && (slash == NULL || bs > slash)) slash = bs;
    snprintf(P.name, sizeof(P.name), "%s", slash != NULL ? slash + 1 : path);
  }

  if (endsWithNoCase(path, ".kfx")) {
    char *text = readFile(path);
    P.isPreset = 1;
    if (text == NULL) return -1;
    rc = parseKfx(text);
    free(text);
    if (rc != 0) return -1;
  } else {
    UfxPass *ps = &P.pass[0];
    P.isPreset = 0;
    snprintf(ps->file, sizeof(ps->file), "%s", path);
    ps->sx = 1.0f; ps->sy = 1.0f;
    ps->scaleType = SCALE_SOURCE;
    P.npass = 1;
    if (loadPassFile(ps) != 0) return -1;
    scanPragmas(ps, 0);
  }
  resolveValues();
  P.ok = 1;
  setStatus("%s%s", "ok", "");
  return 0;
}

/* -------------------------------------------------------------- public ---- */

int YglUserFxSetPreset(const char *path)
{
  char keep[UFX_PATH_LEN];
  snprintf(keep, sizeof(keep), "%s", path != NULL ? path : "");
  clearPreset();
  snprintf(P.path, sizeof(P.path), "%s", keep);
  return loadCurrent();
}

const char *YglUserFxGetPreset(void) { return P.path; }

int YglUserFxReload(void) { return loadCurrent(); }

const char *YglUserFxGetName(void) { return P.name; }
int YglUserFxGetPassCount(void) { return P.ok ? P.npass : 0; }
const char *YglUserFxGetStatus(void) { return status; }

int YglUserFxGetParamCount(void) { return P.nparam; }

int YglUserFxGetParam(int index, YglUserFxParam *out)
{
  if (index < 0 || index >= P.nparam || out == NULL) return -1;
  *out = P.prm[index];
  return 0;
}

int YglUserFxSetParamValue(const char *name, float value)
{
  int i;
  if (name == NULL) return -1;
  setOverride(name, value);
  i = findParam(name);
  if (i < 0) return -1;
  P.prm[i].value = clampf(value, P.prm[i].min, P.prm[i].max);
  return 0;
}

void YglUserFxSetParamsSpec(const char *spec)
{
  const char *p = spec;
  nOV = 0;
  while (p != NULL && *p != 0) {
    char name[48];
    size_t n = 0;
    float v;
    while (*p == ' ' || *p == ';') p++;
    while (isIdentChar((unsigned char)*p) && n < sizeof(name) - 1) name[n++] = *p++;
    name[n] = 0;
    if (*p == '=') {
      p++;
      if (n > 0 && parseFloat(&p, &v)) setOverride(name, v);
    }
    while (*p != 0 && *p != ';') p++;
  }
  resolveValues();
}

int YglUserFxGetParamsSpec(char *buf, int bufsize)
{
  int i, len = 0;
  if (buf == NULL || bufsize < 1) return 0;
  buf[0] = 0;
  for (i = 0; i < P.nparam; i++) {
    char item[96];
    char *c;
    int n;
    if (P.prm[i].value == P.prm[i].def) continue;
    n = snprintf(item, sizeof(item), "%s%s=%.6g", len > 0 ? ";" : "", P.prm[i].name, (double)P.prm[i].value);
    for (c = item; *c; c++) if (*c == ',') *c = '.';   /* never a decimal comma */
    if (n < 0 || len + n >= bufsize) break;
    memcpy(buf + len, item, (size_t)n + 1);
    len += n;
  }
  return len;
}

/* ------------------------------------------------------- GL: building ----- */

static void logShader(GLuint obj, int isProgram)
{
  GLint len = 0;
  char *log;
  if (isProgram) glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
  else           glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
  if (len <= 1) return;
  log = (char *)malloc((size_t)len);
  if (log == NULL) return;
  if (isProgram) glGetProgramInfoLog(obj, len, NULL, log);
  else           glGetShaderInfoLog(obj, len, NULL, log);
  YuiMsg("Shaderlog:\n%s\n", log);
  free(log);
}

static void releaseGl(void)
{
  int i;
  for (i = 0; i < USERFX_MAX_PASSES; i++) {
    if (G[i].prg != 0) glDeleteProgram(G[i].prg);
    if (G[i].fbo != 0) glDeleteFramebuffers(1, &G[i].fbo);
    if (G[i].tex != 0) glDeleteTextures(1, &G[i].tex);
    memset(&G[i], 0, sizeof(G[i]));
  }
  glState = 0;
}

/* #version / #extension are the program's business */
static void neutralizeDirectives(char *text)
{
  char *p = text;
  while (*p != 0) {
    char *s = p;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "#version", 8) == 0 || strncmp(s, "#extension", 10) == 0) { s[0] = '/'; s[1] = '/'; }
    p = strchr(p, '\n');
    if (p == NULL) break;
    p++;
  }
}

/* true if the shader already declares "uniform ... NAME" itself */
static int declaresUniform(const char *text, const char *name)
{
  size_t n = strlen(name);
  const char *p = text;
  while ((p = strstr(p, name)) != NULL) {
    int before = (p == text) ? 0 : (unsigned char)p[-1];
    int after = (unsigned char)p[n];
    if (!isIdentChar(before) && !isIdentChar(after)) {
      const char *ls = p;
      while (ls > text && ls[-1] != '\n') ls--;
      {
        const char *u = ls;
        while (u < p) {
          if (strncmp(u, "//", 2) == 0) break;
          if (strncmp(u, "uniform", 7) == 0 && !isIdentChar((unsigned char)u[7]) &&
              (u == ls || !isIdentChar((unsigned char)u[-1]))) return 1;
          u++;
        }
      }
    }
    p += n;
  }
  return 0;
}

static const char vertSrc[] =
  SHADER_VERSION
  "layout (location = 0) in vec2 aPosition;   \n"
  "layout (location = 1) in vec2 aTexCoord;   \n"
  "out  highp vec2 vTexCoord;     \n"
  " void main(void) \n"
  " { \n"
  " vTexCoord = aTexCoord;     \n"
  " gl_Position = vec4(aPosition.x, aPosition.y, 0.0, 1.0); \n"
  " } \n";

/* Same uniforms as the built-in filters (see fblit_head in ogl_shader.c). */
static const char fragHead[] =
  SHADER_VERSION
  "#ifdef GL_ES\n"
  "precision highp float;       \n"
  "#endif\n"
  "uniform float fWidth; \n"
  "uniform float fHeight; \n"
  "uniform vec2 lineNumber; \n"
  "uniform float decim; \n"
  "uniform int field; \n"
  "uniform int scale; \n"
  "uniform vec2 outSize; \n"
  "uniform float srcLines; \n"
  "uniform float srcCols; \n"
  "uniform int rotated; \n"
  "in highp vec2 vTexCoord;     \n"
  "uniform sampler2D u_Src;     \n"
  "out vec4 fragColor; \n";

static const char fragMain[] =
  "void main()   \n"
  "{   \n"
  "  fragColor = Filter( u_Src, vTexCoord ); \n"
  "} \n";

static char *makePrologue(int pass)
{
  size_t cap = 2048 + (size_t)P.nparam * 96 + (size_t)pass * 128;
  char *s = (char *)malloc(cap);
  char line[160];
  int i;
  if (s == NULL) return NULL;
  strcpy(s,
    "uniform int FrameCount;\n"
    "uniform vec4 InputSize;\n"
    "uniform vec4 OriginalSize;\n"
    "uniform vec4 PassSize;\n"
    "uniform sampler2D Original;\n"
    "#define SourceSize vec4(srcCols, srcLines, 1.0/srcCols, 1.0/srcLines)\n"
    "#define OutputSize vec4(outSize, 1.0/outSize.x, 1.0/outSize.y)\n");
  for (i = 0; i < pass; i++) {
    snprintf(line, sizeof(line), "uniform sampler2D PassOutput%d;\n", i);
    strcat(s, line);
    if (P.pass[i].alias[0] != 0) {
      snprintf(line, sizeof(line), "uniform sampler2D %s;\n", P.pass[i].alias);
      strcat(s, line);
    }
  }
  for (i = 0; i < P.nparam; i++) {
    if (declaresUniform(P.pass[pass].text, P.prm[i].name)) continue;
    snprintf(line, sizeof(line), "uniform float %s;\n", P.prm[i].name);
    strcat(s, line);
  }
  strcat(s, "#line 1\n");
  return s;
}

static GLuint compileStage(GLenum type, int n, const GLchar **src, const char *what, const char *file)
{
  GLuint sh = glCreateShader(type);
  GLint ok = GL_FALSE;
  glShaderSource(sh, n, src, NULL);
  glCompileShader(sh);
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (ok == GL_FALSE) {
    YuiMsg("User shader %s: %s compile error (line numbers are those of the file)\n", file, what);
    logShader(sh, 0);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static int buildPass(int i)
{
  UfxPass *ps = &P.pass[i];
  UfxGl *g = &G[i];
  char *prologue;
  const GLchar *vs[1];
  const GLchar *fs[4];
  GLuint v, f;
  GLint ok = GL_FALSE;
  int k;

  neutralizeDirectives(ps->text);
  prologue = makePrologue(i);
  if (prologue == NULL) return -1;

  vs[0] = vertSrc;
  fs[0] = fragHead; fs[1] = prologue; fs[2] = ps->text; fs[3] = fragMain;

  v = compileStage(GL_VERTEX_SHADER, 1, vs, "vertex shader", ps->file);
  if (v == 0) { free(prologue); return -1; }
  f = compileStage(GL_FRAGMENT_SHADER, 4, fs, "fragment shader", ps->file);
  free(prologue);
  if (f == 0) { glDeleteShader(v); setStatus("compile error in %s (see the log)%s", ps->file, ""); return -1; }

  g->prg = glCreateProgram();
  glAttachShader(g->prg, v);
  glAttachShader(g->prg, f);
  glLinkProgram(g->prg);
  glDeleteShader(v);
  glDeleteShader(f);
  glGetProgramiv(g->prg, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    YuiMsg("User shader %s: link error\n", ps->file);
    logShader(g->prg, 1);
    glDeleteProgram(g->prg);
    g->prg = 0;
    setStatus("link error in %s (see the log)%s", ps->file, "");
    return -1;
  }

  glUseProgram(g->prg);
  glUniform1i(glGetUniformLocation(g->prg, "u_Src"), 0);
  glUniform1i(glGetUniformLocation(g->prg, "Original"), 1);
  for (k = 0; k < i; k++) {
    char nm[24];
    snprintf(nm, sizeof(nm), "PassOutput%d", k);
    glUniform1i(glGetUniformLocation(g->prg, nm), 2 + k);
    if (P.pass[k].alias[0] != 0)
      glUniform1i(glGetUniformLocation(g->prg, P.pass[k].alias), 2 + k);
  }
  for (k = 0; k < U_COUNT; k++) g->loc[k] = glGetUniformLocation(g->prg, uNames[k]);
  for (k = 0; k < P.nparam; k++) g->ploc[k] = glGetUniformLocation(g->prg, P.prm[k].name);
  return 0;
}

static int buildAll(void)
{
  int i;
  for (i = 0; i < P.npass; i++) {
    if (buildPass(i) != 0) {
      for (; i >= 0; i--) {
        if (G[i].prg != 0) { glDeleteProgram(G[i].prg); G[i].prg = 0; }
      }
      return -1;
    }
  }
  setStatus("%s%s", "ok", "");
  return 0;
}

/* Output texture and framebuffer of an intermediate pass, (re)created when the
   size changes. */
static int ensureTarget(int i, int w, int h)
{
  UfxGl *g = &G[i];
  int wantFloat = P.pass[i].floatFb;
  int attempt;
  if (g->tex != 0 && g->tw == w && g->th == h && g->texFloat == wantFloat) return 0;
  if (g->tex == 0) glGenTextures(1, &g->tex);
  if (g->fbo == 0) glGenFramebuffers(1, &g->fbo);

  for (attempt = 0; attempt < 2; attempt++) {
    GLenum status_;
    glBindTexture(GL_TEXTURE_2D, g->tex);
    if (wantFloat)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    else
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex, 0);
    status_ = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status_ == GL_FRAMEBUFFER_COMPLETE) {
      g->tw = w; g->th = h; g->texFloat = wantFloat;
      return 0;
    }
    if (!wantFloat) break;
    YuiMsg("User shader: float framebuffer not supported, using 8 bits for pass %d\n", i);
    wantFloat = 0;
    P.pass[i].floatFb = 0;
  }
  YuiMsg("User shader: cannot create the %dx%d target of pass %d\n", w, h, i);
  return -1;
}

static void passOutputSize(const UfxPass *ps, int inW, int inH, float outW, float outH, int *w, int *h)
{
  float fw, fh;
  switch (ps->scaleType) {
    case SCALE_VIEWPORT: fw = outW * ps->sx; fh = outH * ps->sy; break;
    case SCALE_ABSOLUTE: fw = ps->sx;        fh = ps->sy;        break;
    default:             fw = (float)inW * ps->sx; fh = (float)inH * ps->sy; break;
  }
  *w = (int)(fw + 0.5f); *h = (int)(fh + 0.5f);
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
  if (*w > 8192) *w = 8192;
  if (*h > 8192) *h = 8192;
}

static void setFilter(GLuint tex, int linear)
{
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}

/* Reload the files that changed on disk (called every UFX_RELOAD_PERIOD frames). */
static void checkHotReload(void)
{
  int i, changed = 0;
  if (P.path[0] == 0) return;
  if (fileTime(P.path) != P.mtime) changed = 1;
  for (i = 0; i < P.npass && !changed; i++)
    if (fileTime(P.pass[i].file) != P.pass[i].mtime) changed = 1;
  if (changed) {
    YuiMsg("User shader: %s changed, reloading\n", P.path);
    loadCurrent();
  }
}

void YglUserFxDestroy(void)
{
  releaseGl();
  if (tcBuf[0] != 0 || tcBuf[1] != 0) { glDeleteBuffers(2, tcBuf); tcBuf[0] = tcBuf[1] = 0; }
}

int YglUserFxBlit(unsigned int srcTexture, float w, float h,
                  float outW, float outH, float nbLines,
                  int isRotated, int field, int scale,
                  float srcCols, float srcLines)
{
  static const float vertexPosition[] = {
    1.0f, -1.0f,
    -1.0f, -1.0f,
    1.0f, 1.0f,
    -1.0f, 1.0f };
  static const float textureCoord[16] = {
    1.0f, 0.0f,
    0.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 1.0f,
    0.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 0.0f,
    1.0f, 1.0f };
  GLint vp[4], sc[4];
  GLboolean scOn;
  GLuint inTex = srcTexture;
  int inW = (int)(w + 0.5f), inH = (int)(h + 0.5f);
  const int origW = inW, origH = inH;
  int i, k, rot = (isRotated != 0), decim;

  frameCounter++;
  if (P.path[0] == 0) return -1;
  if ((frameCounter % UFX_RELOAD_PERIOD) == 0) checkHotReload();
  if (!P.ok || P.npass == 0) return -1;

  if (dirty) { releaseGl(); dirty = 0; }
  if (glState == 0) glState = (buildAll() == 0) ? 1 : 2;
  if (glState != 1) return -1;

  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_SCISSOR_BOX, sc);
  scOn = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);

  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER, _Ygl->vertexPosition_buf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertexPosition), vertexPosition, GL_STREAM_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  if (tcBuf[0] == 0) glGenBuffers(2, tcBuf);

  decim = (int)((outH + nbLines) / nbLines);
  if (decim < 2) decim = 2;

  for (i = 0; i < P.npass; i++) {
    UfxPass *ps = &P.pass[i];
    UfxGl *g = &G[i];
    int last = (i == P.npass - 1);
    int tw = 0, th = 0;
    int passRot = last ? rot : 0;

    if (!last) {
      passOutputSize(ps, inW, inH, outW, outH, &tw, &th);
      if (ensureTarget(i, tw, th) != 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        glScissor(sc[0], sc[1], sc[2], sc[3]);
        if (scOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glState = 2;
        setStatus("cannot create an intermediate texture%s%s", "", "");
        return -1;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
      glDisable(GL_SCISSOR_TEST);
      glViewport(0, 0, tw, th);
    } else {
      glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
      glViewport(vp[0], vp[1], vp[2], vp[3]);
      glScissor(sc[0], sc[1], sc[2], sc[3]);
      if (scOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
      tw = (int)(outW + 0.5f); th = (int)(outH + 0.5f);
    }

    glUseProgram(g->prg);
    glBindBuffer(GL_ARRAY_BUFFER, tcBuf[passRot]);
    glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(float), &textureCoord[passRot * 8], GL_STREAM_DRAW);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);

    /* uniforms shared with the built-in filters */
    glUniform1f(g->loc[U_FWIDTH], (float)inW);
    glUniform1f(g->loc[U_FHEIGHT], (float)inH);
    glUniform2f(g->loc[U_LINENUMBER], nbLines, outH);
    glUniform1f(g->loc[U_DECIM], (float)decim);
    glUniform1i(g->loc[U_FIELD], field);
    glUniform1i(g->loc[U_SCALE], scale);
    glUniform2f(g->loc[U_OUTSIZE], outW, outH);
    glUniform1f(g->loc[U_SRCLINES], srcLines);
    glUniform1f(g->loc[U_SRCCOLS], srcCols);
    glUniform1i(g->loc[U_ROTATED], passRot);
    glUniform1i(g->loc[U_FRAMECOUNT], (int)frameCounter);
    glUniform4f(g->loc[U_INPUTSIZE], (float)inW, (float)inH, 1.0f / (float)inW, 1.0f / (float)inH);
    glUniform4f(g->loc[U_ORIGINALSIZE], (float)origW, (float)origH, 1.0f / (float)origW, 1.0f / (float)origH);
    glUniform4f(g->loc[U_PASSSIZE], (float)tw, (float)th, 1.0f / (float)tw, 1.0f / (float)th);
    for (k = 0; k < P.nparam; k++) glUniform1f(g->ploc[k], P.prm[k].value);

    /* textures: 0 = input, 1 = original frame, 2.. = outputs of earlier passes */
    glActiveTexture(GL_TEXTURE0);
    setFilter(inTex, ps->linear);
    glActiveTexture(GL_TEXTURE1);
    setFilter(srcTexture, ps->linear);
    for (k = 0; k < i; k++) {
      glActiveTexture(GL_TEXTURE0 + 2 + k);
      setFilter(G[k].tex, ps->linear);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (!last) { inTex = g->tex; inW = tw; inH = th; }
  }

  /* clean up like the built-in blit does */
  for (k = P.npass - 2; k >= 0; k--) {
    glActiveTexture(GL_TEXTURE0 + 2 + k);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, srcTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glBindFramebuffer(GL_FRAMEBUFFER, _Ygl->default_fbo);
  return 0;
}

#endif /* __LIBRETRO__ */
