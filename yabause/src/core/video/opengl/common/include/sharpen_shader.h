#ifndef __SHARPEN_INCLUDE_H__
#define __SHARPEN_INCLUDE_H__
static const GLchar Yglprg_blit_sharpen_v[] =
"#version 310 es \n"
" layout (location = 0) in vec2 VertexCoord;\n"
" layout (location = 1) in vec2 TexCoord;\n"
"out vec2 vTexCoord;\n"
"\n"
"void main()\n"
"{\n"
"    gl_Position = vec4(VertexCoord, 0.0, 1.0);\n"
"    vTexCoord = TexCoord;\n"
"}\n";

static const GLchar Yglprg_blit_sharpen_f[] =
"#version 310 es \n"
"\n"
"#ifdef GL_ES\n"
"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
"precision highp float;\n"
"#else\n"
"precision mediump float;\n"
"#endif\n"
"#endif\n"
"\n"
"uniform vec2 TextureSize;\n"
"uniform vec2 DrawingSize;\n"
"uniform sampler2D Texture;\n"
"out vec4 fragColor;\n"
"in vec2 vTexCoord;\n"
"\n"
/* Contrast-adaptive sharpen: unlike HQ4x/xBRZ (built for flat-color
 * pixel art and prone to smearing Gouraud-shaded 3D scenes), this
 * works on any content. It samples the 4 direct neighbours, measures
 * local contrast, and pushes the centre pixel away from the
 * neighbours' average -- more where there is an edge, ~0 on flat
 * areas -- so gradients and flat fills are left untouched while
 * silhouettes and details come out crisp instead of bilinear-blurry. */
"#define SHARPNESS 0.65\n"
"#define FLAT_THRESHOLD 0.06\n"
"\n"
"void main()\n"
"{\n"
"  vec2 px = 1.0 / TextureSize;\n"
"\n"
"  vec3 c = texture(Texture, vTexCoord).rgb;\n"
"  vec3 n = texture(Texture, vTexCoord + vec2( 0.0, -px.y)).rgb;\n"
"  vec3 s = texture(Texture, vTexCoord + vec2( 0.0,  px.y)).rgb;\n"
"  vec3 w = texture(Texture, vTexCoord + vec2(-px.x,  0.0)).rgb;\n"
"  vec3 e = texture(Texture, vTexCoord + vec2( px.x,  0.0)).rgb;\n"
"\n"
"  vec3 avg = (n + s + w + e) * 0.25;\n"
"  float contrast = max(max(abs(c.r - avg.r), abs(c.g - avg.g)), abs(c.b - avg.b));\n"
"  float amount = SHARPNESS * smoothstep(0.0, FLAT_THRESHOLD, contrast);\n"
"\n"
"  vec3 result = c + amount * (c - avg);\n"
"  fragColor = vec4(clamp(result, 0.0, 1.0), 1.0);\n"
"}\n";

#endif
