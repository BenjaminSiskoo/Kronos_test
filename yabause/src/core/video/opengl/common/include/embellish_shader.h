#ifndef __EMBELLISH_SHADER_INCLUDE_H__
#define __EMBELLISH_SHADER_INCLUDE_H__

/*
 * Additional "embellishment" filters for the final blit
 * (YglBlitFramebuffer, ogl_shader.c).
 *
 * Each string only defines  vec4 Filter(sampler2D, vec2)  and is inserted
 * between fblit_head and fblit_img, like fblitbicubic_img. They rely on the
 * uniforms declared in fblit_head:
 *   fWidth, fHeight : size of the texture being displayed (texels)
 *   outSize         : size of the output viewport, in texture orientation
 *                     (x/y swapped when the screen is rotated)
 *   srcLines        : number of Saturn lines of the frame (CRT beam pitch)
 *   rotated         : 1 when the picture is displayed rotated by 90 degrees
 *
 *  - Lanczos3        : 6x6 windowed-sinc resampling with light anti-ringing
 *                      (clamped towards the 2x2 nearest texels). Sharper
 *                      than bicubic for non-integer scaling.
 *  - Sharp bilinear  : integer nearest prescale followed by bilinear
 *                      (same maths as libretro's "sharp-bilinear-simple"):
 *                      crisp pixels without the uneven pixel widths of
 *                      nearest at non-integer ratios. Needs GL_LINEAR.
 *  - FXAA            : Timothy Lottes' FXAA (console/"FXAA 2" variant),
 *                      edge anti-aliasing computed on the source texels,
 *                      then bilinear scaling. Best with an internal
 *                      resolution of 2x or more (polygon edges); on 1x
 *                      pixel art it softens sprites. Needs GL_LINEAR.
 *  - CRT             : gaussian scanline beams (width grows with
 *                      brightness) on the Saturn line grid, aperture
 *                      grille mask on output pixels, gamma 2.4 in / 2.2
 *                      out. Needs GL_LINEAR (horizontal interpolation).
 */

static const char fblitlanczos3_img[] =
  "#define LANCZOS_ANTI_RINGING 0.5\n"
  "float lanczos3(float x)\n"
  "{\n"
  "  x = abs(x);\n"
  "  if (x < 1.0e-5) return 1.0;\n"
  "  if (x >= 3.0) return 0.0;\n"
  "  float px = 3.14159265358979 * x;\n"
  "  return 3.0 * sin(px) * sin(px / 3.0) / (px * px);\n"
  "}\n"
  "vec4 Filter( sampler2D textureSampler, vec2 TexCoord )\n"
  "{\n"
  "  vec2 texSize = vec2(fWidth, fHeight);\n"
  "  vec2 pos = TexCoord * texSize - 0.5;\n"
  "  vec2 base = floor(pos);\n"
  "  vec2 fr = pos - base;\n"
  "  ivec2 ib = ivec2(base);\n"
  "  ivec2 im = ivec2(texSize) - ivec2(1);\n"
  "  float wx[6];\n"
  "  float wy[6];\n"
  "  for (int t = 0; t < 6; t++) {\n"
  "    wx[t] = lanczos3(float(t - 2) - fr.x);\n"
  "    wy[t] = lanczos3(float(t - 2) - fr.y);\n"
  "  }\n"
  "  vec3 sum = vec3(0.0);\n"
  "  float wsum = 0.0;\n"
  "  vec3 mn = vec3(1.0);\n"
  "  vec3 mx = vec3(0.0);\n"
  "  for (int j = 0; j < 6; j++) {\n"
  "    for (int i = 0; i < 6; i++) {\n"
  "      vec3 c = texelFetch(textureSampler, clamp(ib + ivec2(i - 2, j - 2), ivec2(0), im), 0).rgb;\n"
  "      float w = wx[i] * wy[j];\n"
  "      sum += c * w;\n"
  "      wsum += w;\n"
  "      if ((i == 2 || i == 3) && (j == 2 || j == 3)) {\n"
  "        mn = min(mn, c);\n"
  "        mx = max(mx, c);\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "  vec3 col = sum / wsum;\n"
  "  col = mix(col, clamp(col, mn, mx), LANCZOS_ANTI_RINGING);\n"
  "  return vec4(clamp(col, 0.0, 1.0), 1.0);\n"
  "}\n";

static const char fblitsharpbilinear_img[] =
  "vec4 Filter( sampler2D textureSampler, vec2 TexCoord )\n"
  "{\n"
  "  vec2 texSize = vec2(fWidth, fHeight);\n"
  "  vec2 prescale = max(floor(outSize / texSize), vec2(1.0));\n"
  "  vec2 texel = TexCoord * texSize;\n"
  "  vec2 texel_floored = floor(texel);\n"
  "  vec2 s = texel - texel_floored;\n"
  "  vec2 region_range = 0.5 - 0.5 / prescale;\n"
  "  vec2 center_dist = s - 0.5;\n"
  "  vec2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * prescale + 0.5;\n"
  "  return texture(textureSampler, (texel_floored + f) / texSize);\n"
  "}\n";

static const char fblitfxaa_img[] =
  "#define FXAA_SPAN_MAX   8.0\n"
  "#define FXAA_REDUCE_MUL (1.0 / 8.0)\n"
  "#define FXAA_REDUCE_MIN (1.0 / 128.0)\n"
  /* FXAA evaluated at a texel centre of the source */
  "vec3 FxaaAt( sampler2D textureSampler, vec2 uv, vec2 rcpFrame )\n"
  "{\n"
  "  vec3 luma = vec3(0.299, 0.587, 0.114);\n"
  "  vec3 rgbNW = texture(textureSampler, uv + vec2(-1.0, -1.0) * rcpFrame).rgb;\n"
  "  vec3 rgbNE = texture(textureSampler, uv + vec2( 1.0, -1.0) * rcpFrame).rgb;\n"
  "  vec3 rgbSW = texture(textureSampler, uv + vec2(-1.0,  1.0) * rcpFrame).rgb;\n"
  "  vec3 rgbSE = texture(textureSampler, uv + vec2( 1.0,  1.0) * rcpFrame).rgb;\n"
  "  vec3 rgbM  = texture(textureSampler, uv).rgb;\n"
  "  float lumaNW = dot(rgbNW, luma);\n"
  "  float lumaNE = dot(rgbNE, luma);\n"
  "  float lumaSW = dot(rgbSW, luma);\n"
  "  float lumaSE = dot(rgbSE, luma);\n"
  "  float lumaM  = dot(rgbM,  luma);\n"
  "  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));\n"
  "  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));\n"
  "  vec2 dir;\n"
  "  dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
  "  dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
  "  float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);\n"
  "  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
  "  dir = clamp(dir * rcpDirMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * rcpFrame;\n"
  "  vec3 rgbA = 0.5 * (texture(textureSampler, uv + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
  "                     texture(textureSampler, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
  "  vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(textureSampler, uv + dir * -0.5).rgb +\n"
  "                                   texture(textureSampler, uv + dir *  0.5).rgb);\n"
  "  float lumaB = dot(rgbB, luma);\n"
  "  if ((lumaB < lumaMin) || (lumaB > lumaMax)) return rgbA;\n"
  "  return rgbB;\n"
  "}\n"
  /* FXAA works on source pixels: evaluate it at the 4 surrounding texel
   * centres and interpolate, i.e. "FXAA pass at source resolution then
   * bilinear scaling", without an extra framebuffer. Evaluating it at an
   * arbitrary sub-texel position smears the edges when the output is
   * much larger than the source. */
  "vec4 Filter( sampler2D textureSampler, vec2 TexCoord )\n"
  "{\n"
  "  vec2 texSize = vec2(fWidth, fHeight);\n"
  "  vec2 rcpFrame = 1.0 / texSize;\n"
  "  vec2 pos = TexCoord * texSize - 0.5;\n"
  "  vec2 base = floor(pos);\n"
  "  vec2 fr = pos - base;\n"
  "  vec2 uv = (base + 0.5) * rcpFrame;\n"
  "  vec3 c00 = FxaaAt(textureSampler, uv, rcpFrame);\n"
  "  vec3 c10 = FxaaAt(textureSampler, uv + vec2(rcpFrame.x, 0.0), rcpFrame);\n"
  "  vec3 c01 = FxaaAt(textureSampler, uv + vec2(0.0, rcpFrame.y), rcpFrame);\n"
  "  vec3 c11 = FxaaAt(textureSampler, uv + rcpFrame, rcpFrame);\n"
  "  return vec4(mix(mix(c00, c10, fr.x), mix(c01, c11, fr.x), fr.y), 1.0);\n"
  "}\n";

static const char fblitcrt_img[] =
  "#define CRT_GAMMA_IN    2.4\n"
  "#define CRT_GAMMA_OUT   2.2\n"
  "#define CRT_BEAM_MIN    0.30\n"
  "#define CRT_BEAM_MAX    0.48\n"
  "#define CRT_MASK_DARK   0.70\n"
  "#define CRT_BRIGHTNESS  1.45\n"
  "vec3 crtLine(sampler2D s, float x, float row, float lines)\n"
  "{\n"
  "  float v = (clamp(row, 0.0, lines - 1.0) + 0.5) / lines;\n"
  "  return pow(texture(s, vec2(x, v)).rgb, vec3(CRT_GAMMA_IN));\n"
  "}\n"
  "float crtBeam(float dist, vec3 c)\n"
  "{\n"
  "  float w = mix(CRT_BEAM_MIN, CRT_BEAM_MAX, max(c.r, max(c.g, c.b)));\n"
  "  return exp(-0.5 * (dist * dist) / (w * w));\n"
  "}\n"
  "vec4 Filter( sampler2D textureSampler, vec2 TexCoord )\n"
  "{\n"
  "  float lines = max(srcLines, 1.0);\n"
  "  float pos = TexCoord.y * lines - 0.5;\n"
  "  float row = floor(pos);\n"
  "  float fy = pos - row;\n"
  "  vec3 c0 = crtLine(textureSampler, TexCoord.x, row, lines);\n"
  "  vec3 c1 = crtLine(textureSampler, TexCoord.x, row + 1.0, lines);\n"
  "  vec3 col = c0 * crtBeam(fy, c0) + c1 * crtBeam(1.0 - fy, c1);\n"
  "  float mpos = (rotated != 0) ? gl_FragCoord.y : gl_FragCoord.x;\n"
  "  int m = int(mod(floor(mpos), 3.0));\n"
  "  vec3 mask = vec3(CRT_MASK_DARK);\n"
  "  if (m == 0) mask.r = 1.0;\n"
  "  else if (m == 1) mask.g = 1.0;\n"
  "  else mask.b = 1.0;\n"
  "  col *= mask * CRT_BRIGHTNESS;\n"
  "  return vec4(pow(clamp(col, 0.0, 1.0), vec3(1.0 / CRT_GAMMA_OUT)), 1.0);\n"
  "}\n";

#endif
