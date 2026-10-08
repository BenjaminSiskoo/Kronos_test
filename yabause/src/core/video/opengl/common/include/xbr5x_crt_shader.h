#ifndef __XBR5X_CRT_SHADER_INCLUDE_H__
#define __XBR5X_CRT_SHADER_INCLUDE_H__

/*
   Hyllian's 5xBR v3.7c + CRT-caligari (squared) Shader

   Copyright (C) 2011/2012 Hyllian/Jararaca - sergiogdb@gmail.com

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

/*
 * GLSL port of libretro common-shaders xbr/shaders/legacy/5xbr-v3.7c-plus-crt.cg
 * for the upscale stage (upscale_shader.c, UP_5XBR_CRT).
 *
 * - The Cg vertex shader only precomputed the 21 neighbour coordinates;
 *   here they are computed in the fragment shader on the Saturn pixel grid
 *   (DrawingSize = rwidth x rheight), sampled at the pixel centres of the
 *   composed texture (internal resolution 1x/2x/4x) and clamped to the
 *   picture, like the Scale2x/3x ports.
 * - Cg bool4 operators (&&, ||, !, !=) are component-wise; GLSL has no
 *   component-wise logic on bvec, so the rules are evaluated on 0.0/1.0
 *   masks: AND = a*b, OR = max(a,b), NOT = 1-a.
 * - The output is drawn at the output (viewport) size so the CRT-caligari
 *   vertical spots are rendered at full resolution, as with the libretro
 *   preset (scale_type viewport).
 * - Cg mul(float4x3(P,Q,R,S), v) = vec4(dot(P,v), dot(Q,v), dot(R,v), dot(S,v)).
 *
 * Needs SHADER_VERSION (ygl.h) and Yglprg_upscale_pass_v (upscale_ext_shader.h).
 */
static const GLchar Yglprg_upscale_5xbr_crt_f[] =
SHADER_VERSION
"#ifdef GL_ES\n"
"precision highp float;\n"
"precision highp int;\n"
"#endif\n"
"uniform sampler2D Texture;\n"
"uniform vec2 DrawingSize;\n"
"in highp vec2 vTexCoord;\n"
"out vec4 fragColor;\n"
"\n"
"#define coef 2.0\n"
"#define InputGamma 2.4\n"
"#define OutputGamma 2.2\n"
"#define GAMMA_IN(color)  pow(color, vec3(InputGamma))\n"
"#define GAMMA_OUT(color) pow(color, vec3(1.0 / OutputGamma))\n"
/* 0.5 = the spot stays inside the original pixel
 * 1.0 = the spot bleeds up to the center of next pixel */
"#define SPOT_HEIGHT 0.5\n"
/* Used to counteract the desaturation effect of weighting. */
"#define COLOR_BOOST 1.45\n"
"\n"
"const vec3 yw = 48.0 * vec3(0.299, 0.587, 0.114);\n"
"\n"
"vec3 S(vec2 p)\n"
"{\n"
"  p = clamp(p, vec2(0.0), DrawingSize - vec2(1.0));\n"
"  return texture(Texture, (p + vec2(0.5)) / DrawingSize).rgb;\n"
"}\n"
"vec4 Y4(vec3 P, vec3 Q, vec3 R, vec3 T)\n"
"{\n"
"  return vec4(dot(P, yw), dot(Q, yw), dot(R, yw), dot(T, yw));\n"
"}\n"
"vec4 df(vec4 A, vec4 B) { return abs(A - B); }\n"
/* masks: 1.0 = true */
"vec4 eq(vec4 A, vec4 B)  { return vec4(lessThan(df(A, B), vec4(15.0))); }\n"
"vec4 neq(vec4 A, vec4 B) { return vec4(notEqual(A, B)); }\n"
"vec4 gt(vec4 A, vec4 B)  { return vec4(greaterThan(A, B)); }\n"
"vec4 NOT(vec4 a) { return vec4(1.0) - a; }\n"
"vec4 OR(vec4 a, vec4 b) { return max(a, b); }\n"
"vec4 weighted_distance(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h)\n"
"{\n"
"  return (df(a,b) + df(a,c) + df(d,e) + df(d,f) + 4.0*df(g,h));\n"
"}\n"
"float WEIGHT(float w)\n"
"{\n"
"  w = min(w, 1.0);\n"
"  w = 1.0 - w * w;\n"
"  return w * w;\n"
"}\n"
"\n"
"void main()\n"
"{\n"
"  vec2 pos = vTexCoord * DrawingSize;\n"
"  vec2 tc = floor(pos);\n"
"  vec2 fp = pos - tc;\n"
/*    A1 B1 C1
 * A0  A  B  C C4
 * D0  D  E  F F4
 * G0  G  H  I I4
 *    G5 H5 I5      (B = y-1) */
"  vec3 A1 = S(tc + vec2(-1.0, -2.0));\n"
"  vec3 B1 = S(tc + vec2( 0.0, -2.0));\n"
"  vec3 C1 = S(tc + vec2( 1.0, -2.0));\n"
"  vec3 A  = S(tc + vec2(-1.0, -1.0));\n"
"  vec3 B  = S(tc + vec2( 0.0, -1.0));\n"
"  vec3 C  = S(tc + vec2( 1.0, -1.0));\n"
"  vec3 D  = S(tc + vec2(-1.0,  0.0));\n"
"  vec3 E  = S(tc);\n"
"  vec3 F  = S(tc + vec2( 1.0,  0.0));\n"
"  vec3 G  = S(tc + vec2(-1.0,  1.0));\n"
"  vec3 H  = S(tc + vec2( 0.0,  1.0));\n"
"  vec3 I  = S(tc + vec2( 1.0,  1.0));\n"
"  vec3 G5 = S(tc + vec2(-1.0,  2.0));\n"
"  vec3 H5 = S(tc + vec2( 0.0,  2.0));\n"
"  vec3 I5 = S(tc + vec2( 1.0,  2.0));\n"
"  vec3 A0 = S(tc + vec2(-2.0, -1.0));\n"
"  vec3 D0 = S(tc + vec2(-2.0,  0.0));\n"
"  vec3 G0 = S(tc + vec2(-2.0,  1.0));\n"
"  vec3 C4 = S(tc + vec2( 2.0, -1.0));\n"
"  vec3 F4 = S(tc + vec2( 2.0,  0.0));\n"
"  vec3 I4 = S(tc + vec2( 2.0,  1.0));\n"
"\n"
"  vec4 b = Y4(B, D, H, F);\n"
"  vec4 c = Y4(C, A, G, I);\n"
"  vec4 e = Y4(E, E, E, E);\n"
"  vec4 d = b.yzwx;\n"
"  vec4 f = b.wxyz;\n"
"  vec4 g = c.zwxy;\n"
"  vec4 h = b.zwxy;\n"
"  vec4 i = c.wxyz;\n"
"  vec4 i4 = Y4(I4, C1, A0, G5);\n"
"  vec4 i5 = Y4(I5, C4, A1, G0);\n"
"  vec4 h5 = Y4(H5, F4, B1, D0);\n"
"  vec4 f4 = h5.yzwx;\n"
"\n"
"  vec4 Ao = vec4( 1.0, -1.0, -1.0, 1.0 );\n"
"  vec4 Bo = vec4( 1.0,  1.0, -1.0,-1.0 );\n"
"  vec4 Co = vec4( 1.5,  0.5, -0.5, 0.5 );\n"
"  vec4 Ax = vec4( 1.0, -1.0, -1.0, 1.0 );\n"
"  vec4 Bx = vec4( 0.5,  2.0, -0.5,-2.0 );\n"
"  vec4 Cx = vec4( 1.0,  1.0, -0.5, 0.0 );\n"
"  vec4 Ay = vec4( 1.0, -1.0, -1.0, 1.0 );\n"
"  vec4 By = vec4( 2.0,  0.5, -2.0,-0.5 );\n"
"  vec4 Cy = vec4( 2.0,  0.0, -1.0, 0.5 );\n"
"\n"
/* These inequations define the line below which interpolation occurs. */
"  vec4 fx      = gt(Ao*fp.y + Bo*fp.x, Co);\n"
"  vec4 fx_left = gt(Ax*fp.y + Bx*fp.x, Cx);\n"
"  vec4 fx_up   = gt(Ay*fp.y + By*fp.x, Cy);\n"
"\n"
/* (e!=f) && (e!=h) && ( !eq(f,b) && !eq(f,c) || !eq(h,d) && !eq(h,g) ||
 *  eq(e,i) && (!eq(f,f4) && !eq(f,i4) || !eq(h,h5) && !eq(h,i5)) ||
 *  eq(e,g) || eq(e,c) ) */
"  vec4 r1 = NOT(eq(f,b)) * NOT(eq(f,c));\n"
"  vec4 r2 = NOT(eq(h,d)) * NOT(eq(h,g));\n"
"  vec4 r3 = eq(e,i) * OR(NOT(eq(f,f4)) * NOT(eq(f,i4)), NOT(eq(h,h5)) * NOT(eq(h,i5)));\n"
"  vec4 interp_restriction_lv1 = neq(e,f) * neq(e,h) *\n"
"         OR(OR(OR(r1, r2), OR(r3, eq(e,g))), eq(e,c));\n"
"  vec4 interp_restriction_lv2_left = neq(e,g) * neq(d,g);\n"
"  vec4 interp_restriction_lv2_up   = neq(e,c) * neq(b,c);\n"
"\n"
"  vec4 edr      = vec4(lessThan(weighted_distance(e, c, g, i, h5, f4, h, f),\n"
"                                weighted_distance(h, d, i5, f, i4, b, e, i))) * interp_restriction_lv1;\n"
"  vec4 edr_left = vec4(lessThanEqual(coef*df(f,g), df(h,c))) * interp_restriction_lv2_left;\n"
"  vec4 edr_up   = vec4(greaterThanEqual(df(f,g), coef*df(h,c))) * interp_restriction_lv2_up;\n"
"\n"
"  vec4 nc = edr * OR(OR(fx, edr_left * fx_left), edr_up * fx_up);\n"
"  vec4 px = vec4(lessThanEqual(df(e,f), df(e,h)));\n"
"\n"
"  vec3 res = E;\n"
"  if      (nc.x > 0.5) res = (px.x > 0.5) ? F : H;\n"
"  else if (nc.y > 0.5) res = (px.y > 0.5) ? B : F;\n"
"  else if (nc.z > 0.5) res = (px.z > 0.5) ? D : B;\n"
"  else if (nc.w > 0.5) res = (px.w > 0.5) ? H : D;\n"
"\n"
/* CRT-caligari - only vertical blend */
"  vec3 color = GAMMA_IN(res);\n"
"  float ddy = fp.y - 0.5;\n"
"  float v_weight_00 = WEIGHT(abs(ddy) / SPOT_HEIGHT);\n"
"  color *= vec3(v_weight_00);\n"
/* get closest vertical neighbour to blend */
"  vec3 coords10;\n"
"  if (ddy > 0.0) {\n"
"    coords10 = H;\n"
"    ddy = 1.0 - ddy;\n"
"  } else {\n"
"    coords10 = B;\n"
"    ddy = 1.0 + ddy;\n"
"  }\n"
"  vec3 colorNB = GAMMA_IN(coords10);\n"
"  float v_weight_10 = WEIGHT(ddy / SPOT_HEIGHT);\n"
"  color += colorNB * vec3(v_weight_10);\n"
"  color *= vec3(COLOR_BOOST);\n"
"  fragColor = vec4(clamp(GAMMA_OUT(color), 0.0, 1.0), 1.0);\n"
"}\n";

#endif
