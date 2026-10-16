#ifndef __CELLSHADING_SHADER_INCLUDE_H__
#define __CELLSHADING_SHADER_INCLUDE_H__

/*
 * Cell shading (toon) filter for the final blit
 * (YglBlitFramebuffer, ogl_shader.c, AA_CELL_SHADING).
 *
 * Only defines  vec4 Filter(sampler2D, vec2) , inserted between fblit_head
 * and fblit_img like the other embellishment filters. Uses the fblit_head
 * uniforms:
 *   fWidth, fHeight : size of the texture being displayed (texels)
 *   srcCols, srcLines : Saturn resolution (rwidth, rheight). Used to express
 *                       the outline width in Saturn pixels whatever the
 *                       internal resolution / upscaling factor.
 *
 * Two stages, both computed on the final picture (no depth buffer is
 * available at this point, so this is a pure image-space filter):
 *
 *  1. Posterisation: the brightness (max of r,g,b) is quantised to
 *     CEL_BANDS levels while the hue/saturation are kept (the colour is
 *     rescaled, not converted). The darkest band is lifted to
 *     CEL_MIN_LEVEL so shadows do not turn black. Slightly anti-aliased step
 *     (fwidth) so the band borders do not shimmer. A small saturation
 *     boost gives the flat "ink and paint" look.
 *  2. Outlines: a Sobel operator on the RGB channels (so a hue change with
 *     the same luminance is also detected) darkens the pixels lying on
 *     strong edges (gradient normalised to 0..1 per channel). The line is
 *     the local colour multiplied by CEL_OUTLINE_DARKEN, not pure black,
 *     and blended with CEL_OUTLINE_STRENGTH. The sampling step is
 *     CEL_OUTLINE_WIDTH Saturn pixels (never below one texel).
 *     CEL_EDGE_LOW / CEL_EDGE_HIGH are the smoothstep bounds: raise them if
 *     the Saturn dithering / mesh / pixel-art patterns produce noisy lines.
 *
 * Needs GL_LINEAR (taps are taken between texels at non integer scaling).
 */
static const char fblitcellshading_img[] =
  "#define CEL_BANDS          6.0\n"
  "#define CEL_MIN_LEVEL      0.12\n"
  "#define CEL_SATURATION     1.10\n"
  "#define CEL_OUTLINE_WIDTH  0.75\n"
  "#define CEL_EDGE_LOW       0.30\n"
  "#define CEL_EDGE_HIGH      0.80\n"
  "#define CEL_OUTLINE_STRENGTH 0.55\n"
  "#define CEL_OUTLINE_DARKEN 0.25\n"
  "vec3 celTap( sampler2D s, vec2 uv )\n"
  "{\n"
  "  return texture(s, uv).rgb;\n"
  "}\n"
  "vec4 Filter( sampler2D textureSampler, vec2 TexCoord )\n"
  "{\n"
  "  vec2 texSize = vec2(fWidth, fHeight);\n"
  /* outline step: CEL_OUTLINE_WIDTH Saturn pixels, at least one texel */
  "  vec2 ratio = max(texSize / vec2(max(srcCols, 1.0), max(srcLines, 1.0)), vec2(1.0));\n"
  "  vec2 o = max(ratio * CEL_OUTLINE_WIDTH, vec2(1.0)) / texSize;\n"
  "  vec3 c = celTap(textureSampler, TexCoord);\n"
  /* --- 1. posterisation, hue preserved --- */
  "  float v = max(c.r, max(c.g, c.b));\n"
  "  float x = v * CEL_BANDS;\n"
  "  float w = clamp(fwidth(x), 0.0, 0.5);\n"
  "  float q = (floor(x) + smoothstep(0.5 - w, 0.5 + w, fract(x))) / CEL_BANDS;\n"
  "  q = mix(CEL_MIN_LEVEL, 1.0, q);\n"
  "  vec3 col = c * (q / max(v, 1.0e-4));\n"
  "  float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
  "  col = clamp(mix(vec3(l), col, CEL_SATURATION), 0.0, 1.0);\n"
  /* --- 2. Sobel outlines on the original picture --- */
  "  vec3 tl = celTap(textureSampler, TexCoord + vec2(-o.x, -o.y));\n"
  "  vec3 tc = celTap(textureSampler, TexCoord + vec2( 0.0, -o.y));\n"
  "  vec3 tr = celTap(textureSampler, TexCoord + vec2( o.x, -o.y));\n"
  "  vec3 ml = celTap(textureSampler, TexCoord + vec2(-o.x,  0.0));\n"
  "  vec3 mr = celTap(textureSampler, TexCoord + vec2( o.x,  0.0));\n"
  "  vec3 bl = celTap(textureSampler, TexCoord + vec2(-o.x,  o.y));\n"
  "  vec3 bc = celTap(textureSampler, TexCoord + vec2( 0.0,  o.y));\n"
  "  vec3 br = celTap(textureSampler, TexCoord + vec2( o.x,  o.y));\n"
  "  vec3 gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);\n"
  "  vec3 gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);\n"
  "  float edge = sqrt(dot(gx, gx) + dot(gy, gy)) * 0.25;\n"
  "  float line = smoothstep(CEL_EDGE_LOW, CEL_EDGE_HIGH, edge) * CEL_OUTLINE_STRENGTH;\n"
  "  col = mix(col, col * CEL_OUTLINE_DARKEN, line);\n"
  "  return vec4(col, 1.0);\n"
  "}\n";

#endif
