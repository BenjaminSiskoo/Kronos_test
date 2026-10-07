#ifndef __SCANLINE_INCLUDE_H__
#define __SCANLINE_INCLUDE_H__
/* The line parity is computed on the Saturn line grid (srcLines =
 * _Ygl->rheight, uniform of fblit_head), not on the texels of the displayed
 * texture. The texture height is rheight * vdp1ratio without upscale, but
 * 4x/6x/3x rheight (or the window height with FSR) after an upscale filter:
 * dividing by "scale" (vdp1ratio) then gave half-lines 4x too thin, landing
 * exactly on integer boundaries when the output is 2x the texture height,
 * so the bright/dim choice depended on float rounding (whole picture dimmed,
 * or one triangle of the quad dimmed and the other not).
 * Normal (non interlaced): 2 half-lines per Saturn line, one dimmed.
 * Interlaced: rheight already holds both fields, dim the other field.
 * GLSL ES has no implicit int -> float conversion and mod() only takes
 * floats: the parity uses an integer AND, so it also builds as 310 es. */
static const GLchar Yglprg_blit_scanline_f[] =
"    float alpha = cos(3.1415*lineNumber[1]/decim * (vTexCoord.y+1.0)/2.0);\n" //Analog ray of CRT looks like a square sine
"    alpha = 0.5 + (alpha*alpha)/2.0;\n"
"    fragColor = vec4(fragColor.xyz*alpha, 1.0);\n";

static const GLchar Yglprg_blit_scanline_is_f[] =
"    float alpha = 1.0;\n"
"    int coord = int(floor(srcLines*2.0*vTexCoord.y));\n"
"    if ((coord & 1) != field) alpha = 0.3;\n"
"    fragColor = vec4(fragColor.xyz*alpha, 1.0);\n";

static const GLchar Yglprg_blit_scanline_interlace_is_f[] =
"    float alpha = 1.0;\n"
"    int coord = int(floor(srcLines*vTexCoord.y));\n"
"    if ((coord & 1) != field) alpha = 0.3;\n"
"    fragColor = vec4(fragColor.xyz*alpha, 1.0);\n";

#endif
