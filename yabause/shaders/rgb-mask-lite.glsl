// Light RGB phosphor mask with a soft vignette. Meant to be used with a
// window or a screen well above the Saturn resolution.
//
//   MASK_STRENGTH : visibility of the red/green/blue columns
//   VIGNETTE      : darkening of the corners
#pragma filter linear
#pragma parameter MASK_STRENGTH "RGB mask strength" 0.25 0.0 1.0 0.05
#pragma parameter VIGNETTE "Vignette" 0.20 0.0 1.0 0.05

vec4 Filter(sampler2D tex, vec2 uv)
{
  vec4 c = texture(tex, uv);
  // output pixel column in texture orientation (OutputSize.x pixels wide)
  float col = floor(uv.x * OutputSize.x);
  float k = mod(col, 3.0);
  vec3 mask = vec3(1.0 - MASK_STRENGTH);
  if (k < 1.0) mask.r = 1.0; else if (k < 2.0) mask.g = 1.0; else mask.b = 1.0;
  mask *= 1.0 + MASK_STRENGTH * 0.5;               // keep the average level
  vec2 d = uv - 0.5;
  float vig = 1.0 - VIGNETTE * dot(d, d) * 2.0;
  return vec4(clamp(c.rgb * mask * vig, 0.0, 1.0), c.a);
}
