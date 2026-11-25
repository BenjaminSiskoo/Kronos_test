// Pass 2 of crt-glow.kfx: vertical blur (9 taps), InputSize.w = 1 / height.
#pragma parameter GLOW_RADIUS "Glow radius" 1.5 0.5 4.0 0.1

vec4 Filter(sampler2D tex, vec2 uv)
{
  float d = InputSize.w * GLOW_RADIUS;
  vec3 s = texture(tex, uv).rgb * 0.2270270;
  s += (texture(tex, uv + vec2(0.0, 1.3846154 * d)).rgb + texture(tex, uv - vec2(0.0, 1.3846154 * d)).rgb) * 0.3162162;
  s += (texture(tex, uv + vec2(0.0, 3.2307692 * d)).rgb + texture(tex, uv - vec2(0.0, 3.2307692 * d)).rgb) * 0.0702703;
  return vec4(s, 1.0);
}
