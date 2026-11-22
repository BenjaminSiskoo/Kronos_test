// Colour adjustment: brightness, contrast, saturation and gamma.
#pragma parameter BRIGHTNESS "Brightness" 0.00 -0.50 0.50 0.01
#pragma parameter CONTRAST "Contrast" 1.00 0.50 1.50 0.01
#pragma parameter SATURATION "Saturation" 1.00 0.00 2.00 0.05
#pragma parameter GAMMA "Gamma" 1.00 0.50 2.00 0.05

vec4 Filter(sampler2D tex, vec2 uv)
{
  vec4 c = texture(tex, uv);
  vec3 col = c.rgb;
  float luma = dot(col, vec3(0.299, 0.587, 0.114));
  col = mix(vec3(luma), col, SATURATION);
  col = (col - 0.5) * CONTRAST + 0.5 + BRIGHTNESS;
  col = pow(clamp(col, 0.0, 1.0), vec3(1.0 / GAMMA));
  return vec4(col, c.a);
}
