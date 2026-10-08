#ifndef __CRT_CGWG_SHADER_INCLUDE_H__
#define __CRT_CGWG_SHADER_INCLUDE_H__

/*
    cgwg's CRT shader

    Copyright (C) 2010-2011 cgwg, Themaister

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    (cgwg gave their consent to have their code distributed under the GPL in
    this message:

        http://board.byuu.org/viewtopic.php?p=26075#p26075

        "Feel free to distribute my shaders under the GPL. After all, the
        barrel distortion code was taken from the Curvature shader, which is
        under the GPL."
    )
*/

/*
 * GLSL port of libretro common-shaders crt/crt-cgwg-fast.cgp
 * (1 pass, crt/shaders/crt-cgwg-fast.cg, filter_linear0 = false, viewport
 * scale) for the final blit (YglBlitFramebuffer, AA_CRT_CGWG_FAST).
 *
 * Only defines  vec4 Filter(sampler2D, vec2) , inserted between fblit_head
 * and fblit_img like the other embellishment filters. Uses the fblit_head
 * uniforms:
 *   srcCols, srcLines : Saturn resolution (rwidth, rheight). Used as the
 *                       Cg texture_size so the beams follow the Saturn
 *                       lines whatever the internal resolution (video_size
 *                       == texture_size, the whole texture is the picture).
 *   outSize           : output size in texture orientation (Cg output_size);
 *                       the mask therefore follows the tube axis when the
 *                       picture is rotated, as on a vertical monitor.
 * Point sampling (texture is set to GL_NEAREST by YglBlitFramebuffer).
 * Cg mul(float4 coeffs, float4x3 rows) = sum(coeffs[k] * row k).
 */
static const char fblitcrtcgwg_img[] =
  "#define CRTCGWG_GAMMA 2.7\n"
  "#define CRTCGWG_PI 3.141592653589\n"
  "vec4 Filter( sampler2D s0, vec2 texCoord )\n"
  "{\n"
  "  vec2 texture_size = vec2(max(srcCols, 1.0), max(srcLines, 1.0));\n"
  "  vec2 delta = 1.0 / texture_size;\n"
  "  float dx = delta.x;\n"
  "  float dy = delta.y;\n"
  "  vec2 c01 = texCoord + vec2(-dx, 0.0);\n"
  "  vec2 c11 = texCoord;\n"
  "  vec2 c21 = texCoord + vec2(dx, 0.0);\n"
  "  vec2 c31 = texCoord + vec2(2.0 * dx, 0.0);\n"
  "  vec2 c02 = texCoord + vec2(-dx, dy);\n"
  "  vec2 c12 = texCoord + vec2(0.0, dy);\n"
  "  vec2 c22 = texCoord + vec2(dx, dy);\n"
  "  vec2 c32 = texCoord + vec2(2.0 * dx, dy);\n"
  "  float mod_factor = c11.x * outSize.x;\n"
  "  vec2 uv_ratio = fract(c11 * texture_size);\n"
  "\n"
  "  vec4 coeffs = vec4(1.0 + uv_ratio.x, uv_ratio.x, 1.0 - uv_ratio.x, 2.0 - uv_ratio.x) + 0.005;\n"
  "  coeffs = sin(CRTCGWG_PI * coeffs) * sin(0.5 * CRTCGWG_PI * coeffs) / (coeffs * coeffs);\n"
  "  coeffs = coeffs / dot(coeffs, vec4(1.0));\n"
  "\n"
  "  vec3 weights  = vec3(3.33 * uv_ratio.y);\n"
  "  vec3 weights2 = vec3(uv_ratio.y * -3.33 + 3.33);\n"
  "\n"
  "  vec3 col  = clamp(coeffs.x * texture(s0, c01).rgb + coeffs.y * texture(s0, c11).rgb +\n"
  "                    coeffs.z * texture(s0, c21).rgb + coeffs.w * texture(s0, c31).rgb, 0.0, 1.0);\n"
  "  vec3 col2 = clamp(coeffs.x * texture(s0, c02).rgb + coeffs.y * texture(s0, c12).rgb +\n"
  "                    coeffs.z * texture(s0, c22).rgb + coeffs.w * texture(s0, c32).rgb, 0.0, 1.0);\n"
  "\n"
  "  vec3 wid  = 2.0 * pow(col,  vec3(4.0)) + 2.0;\n"
  "  vec3 wid2 = 2.0 * pow(col2, vec3(4.0)) + 2.0;\n"
  "\n"
  "  col  = pow(col,  vec3(CRTCGWG_GAMMA));\n"
  "  col2 = pow(col2, vec3(CRTCGWG_GAMMA));\n"
  "\n"
  "  vec3 sqrt1 = inversesqrt(0.5 * wid);\n"
  "  vec3 sqrt2 = inversesqrt(0.5 * wid2);\n"
  "  vec3 pow_mul1 = weights * sqrt1;\n"
  "  vec3 pow_mul2 = weights2 * sqrt2;\n"
  "  vec3 div1 = 0.1320 * wid + 0.392;\n"
  "  vec3 div2 = 0.1320 * wid2 + 0.392;\n"
  "  vec3 pow1 = -pow(pow_mul1, wid);\n"
  "  vec3 pow2 = -pow(pow_mul2, wid2);\n"
  "  weights  = exp(pow1) / div1;\n"
  "  weights2 = exp(pow2) / div2;\n"
  "\n"
  "  vec3 multi = col * weights + col2 * weights2;\n"
  "  vec3 mcol = mix(vec3(1.0, 0.7, 1.0), vec3(0.7, 1.0, 0.7), floor(mod(mod_factor, 2.0)));\n"
  "  return vec4(pow(mcol * multi, vec3(0.454545)), 1.0);\n"
  "}\n";

#endif
