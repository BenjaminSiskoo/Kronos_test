// Pass 3 (last) of crt-glow.kfx: the original picture with scanlines, plus
// the blurred highlights (this pass's input) added on top.
// "Original" is the picture before the first pass.
#pragma parameter GLOW_AMOUNT "Glow amount" 0.60 0.0 2.0 0.05
#pragma parameter SCAN_STRENGTH "Scanline strength" 0.35 0.0 1.0 0.05

vec4 Filter(sampler2D tex, vec2 uv)
{
  vec3 base = texture(Original, uv).rgb;
  vec3 glow = texture(tex, uv).rgb;
  float pos = fract(uv.y * max(srcLines, 1.0));
  float edge = abs(pos - 0.5) * 2.0;
  float scan = 1.0 - SCAN_STRENGTH * pow(edge, 3.0);
  return vec4(clamp(base * scan + glow * GLOW_AMOUNT, 0.0, 1.0), 1.0);
}
