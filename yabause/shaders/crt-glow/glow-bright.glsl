// Pass 0 of crt-glow.kfx: keeps only the bright parts of the picture.
#pragma parameter GLOW_THRESHOLD "Glow threshold" 0.55 0.0 0.95 0.05

vec4 Filter(sampler2D tex, vec2 uv)
{
  vec3 c = texture(tex, uv).rgb;
  float l = max(c.r, max(c.g, c.b));
  float k = smoothstep(GLOW_THRESHOLD, 1.0, l);
  return vec4(c * k, 1.0);
}
