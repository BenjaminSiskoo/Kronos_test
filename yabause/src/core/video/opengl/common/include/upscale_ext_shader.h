#ifndef __UPSCALE_EXT_SHADER_INCLUDE_H__
#define __UPSCALE_EXT_SHADER_INCLUDE_H__

/*
 * Additional upscale filters for the final output stage
 * (YglUpscaleFramebuffer, upscale_shader.c).
 *
 *  - Scale2x / Scale3x (AdvMAME2x/3x, Andrea Mazzoleni's EPX derivative,
 *    algorithm described at scale2x.sourceforge.net). Scale4x is Scale2x
 *    applied twice (two passes), exactly like AdvMAME4x.
 *    Edge-directed, no blending: pixel art keeps its palette (no new
 *    colours are invented), staircases on diagonals are smoothed.
 *
 *  - AMD FidelityFX Super Resolution 1.0, port of ffx_fsr1.h
 *    (Copyright (c) 2021 Advanced Micro Devices, Inc. - MIT license).
 *    Pass 1 = EASU (Edge Adaptive Spatial Upsampling), resamples the
 *    composed frame to the output (window) resolution.
 *    Pass 2 = RCAS (Robust Contrast Adaptive Sharpening), at output
 *    resolution, with the optional noise removal (FSR_RCAS_DENOISE)
 *    enabled because Saturn games use dithering / mesh patterns a lot.
 *    The 12-tap EASU kernel is read with texelFetch instead of
 *    textureGather so it compiles on GLSL 3.30 core as well as 3.10 es;
 *    the maths are the FP32 path of the reference (the approximated
 *    reciprocals are replaced by exact ones with a zero guard).
 *
 * All shaders use SHADER_VERSION (ygl.h), so ygl.h must be included
 * before this header.
 *
 * Uniforms (set by upscale_shader.c):
 *   Texture     : source texture, unit 0
 *   TextureSize : source texture size in texels
 *   DrawingSize : Scale2x/3x -> sampling grid of the source (logical
 *                 pixels); EASU -> output size in pixels
 *   Sharpness   : RCAS only, linear value = exp2(-stops)
 */

static const GLchar Yglprg_upscale_pass_v[] =
SHADER_VERSION
"layout (location = 0) in vec2 VertexCoord;\n"
"layout (location = 1) in vec2 TexCoord;\n"
"out highp vec2 vTexCoord;\n"
"void main()\n"
"{\n"
"  gl_Position = vec4(VertexCoord, 0.0, 1.0);\n"
"  vTexCoord = TexCoord;\n"
"}\n";

/* ------------------------------------------------------------------ */
/* Scale2x / Scale3x                                                   */
/* ------------------------------------------------------------------ */

#define UPSCALE_EXT_SCALEX_HEAD \
"#ifdef GL_ES\n" \
"precision highp float;\n" \
"precision highp int;\n" \
"#endif\n" \
"uniform sampler2D Texture;\n" \
"uniform vec2 DrawingSize;\n" \
"in highp vec2 vTexCoord;\n" \
"out vec4 fragColor;\n" \
"vec3 S(vec2 p)\n" \
"{\n" \
"  p = clamp(p, vec2(0.0), DrawingSize - vec2(1.0));\n" \
"  return texture(Texture, (p + vec2(0.5)) / DrawingSize).rgb;\n" \
"}\n" \
"bool eq(vec3 a, vec3 b)\n" \
"{\n" \
"  return all(lessThan(abs(a - b), vec3(0.5 / 255.0)));\n" \
"}\n"

/* "top" = neighbour B = y-1 in texture space. The algorithm is symmetric
 * under a vertical flip, so the GL bottom-left origin does not matter. */
static const GLchar Yglprg_upscale_scale2x_f[] =
SHADER_VERSION
UPSCALE_EXT_SCALEX_HEAD
"void main()\n"
"{\n"
"  vec2 pos = vTexCoord * DrawingSize;\n"
"  vec2 p = floor(pos);\n"
"  vec2 sub = pos - p;\n"
"  vec3 E = S(p);\n"
"  vec3 B = S(p + vec2( 0.0, -1.0));\n"
"  vec3 D = S(p + vec2(-1.0,  0.0));\n"
"  vec3 F = S(p + vec2( 1.0,  0.0));\n"
"  vec3 H = S(p + vec2( 0.0,  1.0));\n"
"  vec3 r = E;\n"
"  if (!eq(B, H) && !eq(D, F)) {\n"
"    bool left = sub.x < 0.5;\n"
"    bool top  = sub.y < 0.5;\n"
"    if (top) {\n"
"      if (left) r = eq(D, B) ? D : E;\n"
"      else      r = eq(B, F) ? F : E;\n"
"    } else {\n"
"      if (left) r = eq(D, H) ? D : E;\n"
"      else      r = eq(H, F) ? F : E;\n"
"    }\n"
"  }\n"
"  fragColor = vec4(r, 1.0);\n"
"}\n";

static const GLchar Yglprg_upscale_scale3x_f[] =
SHADER_VERSION
UPSCALE_EXT_SCALEX_HEAD
"void main()\n"
"{\n"
"  vec2 pos = vTexCoord * DrawingSize;\n"
"  vec2 p = floor(pos);\n"
"  ivec2 sp = clamp(ivec2((pos - p) * 3.0), ivec2(0), ivec2(2));\n"
"  int idx = sp.y * 3 + sp.x;\n"
"  vec3 A = S(p + vec2(-1.0, -1.0));\n"
"  vec3 B = S(p + vec2( 0.0, -1.0));\n"
"  vec3 C = S(p + vec2( 1.0, -1.0));\n"
"  vec3 D = S(p + vec2(-1.0,  0.0));\n"
"  vec3 E = S(p);\n"
"  vec3 F = S(p + vec2( 1.0,  0.0));\n"
"  vec3 G = S(p + vec2(-1.0,  1.0));\n"
"  vec3 H = S(p + vec2( 0.0,  1.0));\n"
"  vec3 I = S(p + vec2( 1.0,  1.0));\n"
"  vec3 r = E;\n"
"  if (!eq(B, H) && !eq(D, F)) {\n"
"    bool db = eq(D, B);\n"
"    bool bf = eq(B, F);\n"
"    bool dh = eq(D, H);\n"
"    bool hf = eq(H, F);\n"
"    if (idx == 0)      r = db ? D : E;\n"
"    else if (idx == 1) r = ((db && !eq(E, C)) || (bf && !eq(E, A))) ? B : E;\n"
"    else if (idx == 2) r = bf ? F : E;\n"
"    else if (idx == 3) r = ((db && !eq(E, G)) || (dh && !eq(E, A))) ? D : E;\n"
"    else if (idx == 5) r = ((bf && !eq(E, I)) || (hf && !eq(E, C))) ? F : E;\n"
"    else if (idx == 6) r = dh ? D : E;\n"
"    else if (idx == 7) r = ((dh && !eq(E, I)) || (hf && !eq(E, G))) ? H : E;\n"
"    else if (idx == 8) r = hf ? F : E;\n"
"  }\n"
"  fragColor = vec4(r, 1.0);\n"
"}\n";

/* ------------------------------------------------------------------ */
/* AMD FSR 1.0 - EASU                                                  */
/* ------------------------------------------------------------------ */
static const GLchar Yglprg_upscale_fsr_easu_f[] =
SHADER_VERSION
"#ifdef GL_ES\n"
"precision highp float;\n"
"precision highp int;\n"
"#endif\n"
"uniform sampler2D Texture;\n"
"uniform vec2 TextureSize;\n"   /* input size  */
"uniform vec2 DrawingSize;\n"   /* output size */
"out vec4 fragColor;\n"
"\n"
"vec3 FsrLoad(ivec2 p)\n"
"{\n"
"  return texelFetch(Texture, clamp(p, ivec2(0), ivec2(TextureSize) - ivec2(1)), 0).rgb;\n"
"}\n"
/* Luma times 2 (ffx_fsr1.h: B*0.5 + (R*0.5 + G)) */
"float FsrLuma(vec3 c)\n"
"{\n"
"  return c.b * 0.5 + (c.r * 0.5 + c.g);\n"
"}\n"
"\n"
"void FsrEasuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,\n"
"                float lob, float clp, vec3 c)\n"
"{\n"
"  vec2 v;\n"
"  v.x = (off.x * ( dir.x)) + (off.y * dir.y);\n"
"  v.y = (off.x * (-dir.y)) + (off.y * dir.x);\n"
"  v *= len;\n"
"  float d2 = v.x * v.x + v.y * v.y;\n"
"  d2 = min(d2, clp);\n"
"  float wB = (2.0 / 5.0) * d2 - 1.0;\n"
"  float wA = lob * d2 - 1.0;\n"
"  wB *= wB;\n"
"  wA *= wA;\n"
"  wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);\n"
"  float w = wB * wA;\n"
"  aC += c * w;\n"
"  aW += w;\n"
"}\n"
"\n"
"void FsrEasuSet(inout vec2 dir, inout float len, float w,\n"
"                float lA, float lB, float lC, float lD, float lE)\n"
"{\n"
"  float dc = lD - lC;\n"
"  float cb = lC - lB;\n"
"  float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0 / 65536.0);\n"
"  float dirX = lD - lB;\n"
"  dir.x += dirX * w;\n"
"  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);\n"
"  lenX *= lenX;\n"
"  len += lenX * w;\n"
"  float ec = lE - lC;\n"
"  float ca = lC - lA;\n"
"  float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0 / 65536.0);\n"
"  float dirY = lE - lA;\n"
"  dir.y += dirY * w;\n"
"  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);\n"
"  lenY *= lenY;\n"
"  len += lenY * w;\n"
"}\n"
"\n"
"void main()\n"
"{\n"
/* FsrEasuCon: con0 = {in/out, 0.5*in/out - 0.5} */
"  vec2 ratio = TextureSize / DrawingSize;\n"
"  vec2 pp = floor(gl_FragCoord.xy) * ratio + (0.5 * ratio - 0.5);\n"
"  vec2 fp = floor(pp);\n"
"  pp -= fp;\n"
"  ivec2 f0 = ivec2(fp);\n"
/*    b c
 *  e f g h
 *  i j k l
 *    n o     */
"  vec3 b = FsrLoad(f0 + ivec2( 0, -1));\n"
"  vec3 c = FsrLoad(f0 + ivec2( 1, -1));\n"
"  vec3 e = FsrLoad(f0 + ivec2(-1,  0));\n"
"  vec3 f = FsrLoad(f0);\n"
"  vec3 g = FsrLoad(f0 + ivec2( 1,  0));\n"
"  vec3 h = FsrLoad(f0 + ivec2( 2,  0));\n"
"  vec3 i = FsrLoad(f0 + ivec2(-1,  1));\n"
"  vec3 j = FsrLoad(f0 + ivec2( 0,  1));\n"
"  vec3 k = FsrLoad(f0 + ivec2( 1,  1));\n"
"  vec3 l = FsrLoad(f0 + ivec2( 2,  1));\n"
"  vec3 n = FsrLoad(f0 + ivec2( 0,  2));\n"
"  vec3 o = FsrLoad(f0 + ivec2( 1,  2));\n"
"  float bL = FsrLuma(b);\n"
"  float cL = FsrLuma(c);\n"
"  float eL = FsrLuma(e);\n"
"  float fL = FsrLuma(f);\n"
"  float gL = FsrLuma(g);\n"
"  float hL = FsrLuma(h);\n"
"  float iL = FsrLuma(i);\n"
"  float jL = FsrLuma(j);\n"
"  float kL = FsrLuma(k);\n"
"  float lL = FsrLuma(l);\n"
"  float nL = FsrLuma(n);\n"
"  float oL = FsrLuma(o);\n"
/* Direction and length, bilinearly accumulated over the 4 centre taps */
"  vec2 dir = vec2(0.0);\n"
"  float len = 0.0;\n"
"  FsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
"  FsrEasuSet(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);\n"
"  FsrEasuSet(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);\n"
"  FsrEasuSet(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);\n"
/* Normalize, with cleanup close to zero */
"  vec2 dir2 = dir * dir;\n"
"  float dirR = dir2.x + dir2.y;\n"
"  bool zro = dirR < (1.0 / 32768.0);\n"
"  dirR = zro ? 1.0 : inversesqrt(max(dirR, 1.0 / 32768.0));\n"
"  dir.x = zro ? 1.0 : dir.x;\n"
"  dir *= dirR;\n"
/* {0 to 2} -> {0 to 1}, shaped with a square */
"  len = len * 0.5;\n"
"  len *= len;\n"
/* Stretch kernel {1.0 vert|horz, sqrt(2.0) on diagonal} */
"  float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));\n"
"  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
"  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
"  float clp = 1.0 / lob;\n"
/* Deringing limits = min/max of the 4 nearest */
"  vec3 min4 = min(min(f, g), min(j, k));\n"
"  vec3 max4 = max(max(f, g), max(j, k));\n"
"  vec3 aC = vec3(0.0);\n"
"  float aW = 0.0;\n"
"  FsrEasuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);\n"
"  FsrEasuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);\n"
"  FsrEasuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);\n"
"  FsrEasuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);\n"
"  FsrEasuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);\n"
"  FsrEasuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);\n"
"  FsrEasuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);\n"
"  FsrEasuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);\n"
"  FsrEasuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);\n"
"  FsrEasuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);\n"
"  FsrEasuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);\n"
"  FsrEasuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);\n"
"  vec3 pix = min(max4, max(min4, aC / aW));\n"
"  fragColor = vec4(pix, 1.0);\n"
"}\n";

/* ------------------------------------------------------------------ */
/* AMD FSR 1.0 - RCAS (input and output have the same size)            */
/* ------------------------------------------------------------------ */
static const GLchar Yglprg_upscale_fsr_rcas_f[] =
SHADER_VERSION
"#ifdef GL_ES\n"
"precision highp float;\n"
"precision highp int;\n"
"#endif\n"
"uniform sampler2D Texture;\n"
"uniform vec2 TextureSize;\n"
"uniform float Sharpness;\n"
"out vec4 fragColor;\n"
"#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))\n"
"vec3 RcasLoad(ivec2 p)\n"
"{\n"
"  return texelFetch(Texture, clamp(p, ivec2(0), ivec2(TextureSize) - ivec2(1)), 0).rgb;\n"
"}\n"
"void main()\n"
"{\n"
/*    b
 *  d e f
 *    h     */
"  ivec2 sp = ivec2(gl_FragCoord.xy);\n"
"  vec3 b = RcasLoad(sp + ivec2( 0, -1));\n"
"  vec3 d = RcasLoad(sp + ivec2(-1,  0));\n"
"  vec3 e = RcasLoad(sp);\n"
"  vec3 f = RcasLoad(sp + ivec2( 1,  0));\n"
"  vec3 h = RcasLoad(sp + ivec2( 0,  1));\n"
"  float bL = b.b * 0.5 + (b.r * 0.5 + b.g);\n"
"  float dL = d.b * 0.5 + (d.r * 0.5 + d.g);\n"
"  float eL = e.b * 0.5 + (e.r * 0.5 + e.g);\n"
"  float fL = f.b * 0.5 + (f.r * 0.5 + f.g);\n"
"  float hL = h.b * 0.5 + (h.r * 0.5 + h.g);\n"
/* Noise detection (FSR_RCAS_DENOISE) */
"  float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;\n"
"  float lmax = max(max(max(bL, dL), max(eL, fL)), hL);\n"
"  float lmin = min(min(min(bL, dL), min(eL, fL)), hL);\n"
"  nz = clamp(abs(nz) / max(lmax - lmin, 1.0 / 65536.0), 0.0, 1.0);\n"
"  nz = -0.5 * nz + 1.0;\n"
/* Min and max of the ring */
"  vec3 mn4 = min(min(b, d), min(f, h));\n"
"  vec3 mx4 = max(max(b, d), max(f, h));\n"
"  vec2 peakC = vec2(1.0, -1.0 * 4.0);\n"
/* Limiters */
"  vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
"  vec3 hitMax = (peakC.x - mx4) / min(4.0 * mn4 + peakC.y, vec3(-1.0 / 65536.0));\n"
"  vec3 lobeRGB = max(-hitMin, hitMax);\n"
"  float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * Sharpness;\n"
"  lobe *= nz;\n"
/* Resolve */
"  float rcpL = 1.0 / (4.0 * lobe + 1.0);\n"
"  vec3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;\n"
"  fragColor = vec4(clamp(pix, 0.0, 1.0), 1.0);\n"
"}\n";

#endif
