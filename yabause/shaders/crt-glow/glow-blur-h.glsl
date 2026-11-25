// Pass 1 of crt-glow.kfx: horizontal blur (9 taps). InputSize.z is 1 / width
// of the input texture of this pass.
#pragma parameter GLOW_RADIUS "Glow radius" 1.5 0.5 4.0 0.1

vec4 Filter(sampler2D tex, vec2 uv)
{
  float d = InputSize.z * GLOW_RADIUS;
  vec3 s = texture(tex, uv).rgb * 0.2270270;
  s += (texture(tex, uv + vec2(1.3846154 * d, 0.0)).rgb + texture(tex, uv - vec2(1.3846154 * d, 0.0)).rgb) * 0.3162162;
  s += (texture(tex, uv + vec2(3.2307692 * d, 0.0)).rgb + texture(tex, uv - vec2(3.2307692 * d, 0.0)).rgb) * 0.0702703;
  return vec4(s, 1.0);
}
