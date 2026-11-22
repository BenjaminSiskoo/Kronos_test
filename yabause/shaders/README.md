# Kronos user shaders

Presets for the video filter **User shader (file)** (Settings → Video).

## Where to put the files

Copy the `.glsl` files into one of these folders, then reopen the settings
(or press *Reload*):

* `shaders/` inside the Kronos data folder (the folder that holds `bkram.bin`
  and `screenshots/`)
* `shaders/` next to the Kronos executable

Any other file can be picked with *Browse...*. A file that is modified while
it is in use is reloaded automatically, so a shader can be edited with the
game running. If a file cannot be read or compiled, the plain picture is shown
and the reason is written to the log; fix the file and it is picked up again.

## Format

One fragment shader per file. It must define

```glsl
vec4 Filter(sampler2D tex, vec2 uv)
```

which returns the colour of the output pixel at texture coordinate `uv`
(the same function that the built-in filters of Kronos define).

Do **not** write `#version` or `#extension` lines: Kronos adds the version and
turns those two directives into comments if they are present.

### Uniforms and helpers

| Name | Type | Meaning |
|---|---|---|
| `srcCols`, `srcLines` | float | Saturn picture size (before upscaling) |
| `outSize` | vec2 | output size in texture orientation |
| `SourceSize` | vec4 | `(srcCols, srcLines, 1/srcCols, 1/srcLines)` |
| `OutputSize` | vec4 | `(outSize, 1/outSize.x, 1/outSize.y)` |
| `FrameCount` | int | frames since the shader was loaded |
| `fWidth`, `fHeight` | float | size of the source texture |
| `field` | int | current field (interlaced output) |
| `scale` | int | VDP1 resolution factor |
| `rotated` | int | 1 when the picture is rotated |
| `u_Src` | sampler2D | the picture (the `tex` argument of `Filter`) |

Do not give a parameter the name of one of these uniforms.

### Directives (one per line)

```glsl
#pragma filter linear
```
Samples the picture with bilinear filtering. Without it, point sampling.

```glsl
#pragma parameter NAME "Label shown in the settings" default min max step
```
Adds an adjustable value. Kronos declares `uniform float NAME;` for you (do not
declare it in the file) and shows a spin box in the settings. The values are
saved with the other settings. At most 24 parameters per file.

### Limits

* One pass per file (no chained passes, no previous frames).
* Files over 256 KB are refused.
* Files are read with the program's 8-bit file name encoding, so keep the path
  free of unusual characters.

## Included presets

* `scanlines-mix-split.glsl` : scanlines, from smoothly mixed to clearly split.
* `color-tweak.glsl` : brightness, contrast, saturation, gamma.
* `rgb-mask-lite.glsl` : light RGB mask with a vignette.
