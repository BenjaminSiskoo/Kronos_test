// Scanlines with a "mix / split" control.
//
//   SCAN_STRENGTH : how dark the gap between two lines gets (0 = no scanlines)
//   SCAN_SPLIT    : 0 = lines blend smoothly into each other (mixed),
//                   1 = thin dark gaps between clearly separate lines (split)
//   SCAN_BRIGHT   : brightness gain that makes up for the darker gaps
//
// One scanline = one line of the Saturn picture (srcLines), whatever the
// internal resolution or the window size.
#pragma filter linear
#pragma parameter SCAN_STRENGTH "Scanline strength" 0.40 0.0 1.0 0.05
#pragma parameter SCAN_SPLIT "Separation (0 = mixed, 1 = split)" 0.50 0.0 1.0 0.05
#pragma parameter SCAN_BRIGHT "Brightness compensation" 1.10 0.5 1.5 0.05

vec4 Filter(sampler2D tex, vec2 uv)
{
  vec4 c = texture(tex, uv);
  float pos = fract(uv.y * max(srcLines, 1.0));   // 0..1 across one Saturn line
  float edge = abs(pos - 0.5) * 2.0;              // 0 at the centre, 1 at the gap
  float shape = pow(edge, mix(1.0, 6.0, SCAN_SPLIT));
  float gain = (1.0 - SCAN_STRENGTH * shape) * SCAN_BRIGHT;
  return vec4(clamp(c.rgb * gain, 0.0, 1.0), c.a);
}
