# Kronos shader presets

Presets for the video filter **User shader (preset)** (Settings → Video →
*User shader*). A preset is either

* a **`.glsl` file**: one pass, or
* a **`.kfx` file**: a list of `.glsl` passes chained one after the other.

The passes are fragment shaders of the same kind as the built-in filters of
Kronos. The Qt version of Kronos only: the libretro core shows the plain
picture.

## Where to put the files

Copy them into one of these folders, then press *Reload* in the settings:

* `shaders/` inside the Kronos data folder (the folder that holds `bkram.bin`
  and `screenshots/`)
* `shaders/` next to the Kronos executable

The list shows every `.kfx` file, in the folder or in a sub folder, and the
`.glsl` files that sit directly in the folder (a `.glsl` in a sub folder is
taken for the pass of a `.kfx`). *Browse...* picks a file from anywhere else.

A file that is modified while it is in use is reloaded automatically (checked
every half second), so a shader can be edited with the game running. If a file
cannot be read or compiled, the plain picture is shown and the reason is
written to the log; fix the file and it is picked up again. After an edit that
adds or removes `#pragma parameter` lines, press *Reload* to refresh the
parameter list of the settings window.

## `.glsl` pass

Each pass must define

```glsl
vec4 Filter(sampler2D tex, vec2 uv)
```

which returns the colour of the output pixel at texture coordinate `uv`
(the function that the built-in filters of Kronos define). `tex` is the input
of the pass: the Saturn picture for the first pass, the output of the previous
pass for the others.

Do **not** write `#version` or `#extension` lines: Kronos adds the version and
turns those two directives into comments if they are present.

### Uniforms and helpers

| Name | Type | Meaning |
|---|---|---|
| `srcCols`, `srcLines` | float | Saturn picture size (before upscaling) |
| `outSize` | vec2 | final output size in texture orientation |
| `SourceSize` | vec4 | `(srcCols, srcLines, 1/srcCols, 1/srcLines)` |
| `OutputSize` | vec4 | `(outSize, 1/outSize.x, 1/outSize.y)` |
| `InputSize` | vec4 | size of the input texture of this pass, `(w, h, 1/w, 1/h)` |
| `PassSize` | vec4 | size of the output of this pass, same layout |
| `OriginalSize` | vec4 | size of the picture before the first pass, same layout |
| `Original` | sampler2D | the picture before the first pass (every pass can read it) |
| `PassOutput0`..`PassOutput6` | sampler2D | output of an earlier pass (only the passes before this one) |
| *alias* | sampler2D | the same, by the `alias` name given in the `.kfx` |
| `FrameCount` | int | frames since the shader was loaded |
| `fWidth`, `fHeight` | float | size of the input texture of this pass |
| `field` | int | current field (interlaced output) |
| `scale` | int | VDP1 resolution factor |
| `rotated` | int | 1 when the picture is rotated (last pass only) |
| `u_Src` | sampler2D | the input (the `tex` argument of `Filter`) |

A parameter may not take the name of one of these.

### Directives (one per line)

```glsl
#pragma filter linear
```
Samples the input with bilinear filtering (default: point sampling). In a
`.kfx` the `filterN` key wins over this line.

```glsl
#pragma parameter NAME "Label shown in the settings" default min max step
```
Adds an adjustable value. Kronos declares `uniform float NAME;` for you (if the
file declares it itself, that is accepted too) and shows a spin box in the
settings. The values are saved with the other settings. When several passes
declare the same name, the first declaration counts and the value is shared.
At most 32 parameters per preset.

## `.kfx` preset

A text file of `key = value` lines. Lines starting with `#`, `;` or `//` are
comments. File names are relative to the folder of the `.kfx`.

```ini
name = CRT glow
shaders = 4                  # number of passes, 1 to 8

shader0 = glow-bright.glsl   # first pass: reads the Saturn picture
filter0 = linear             # linear | nearest: how this pass samples its inputs
scale_type0 = viewport       # size of its output: source | viewport | absolute
scale0 = 0.5                 # factor (scale_x0 / scale_y0 for each axis)
alias0 = Bright              # optional name of its output, for the later passes
float0 = true                # optional: 16-bit float output (RGBA16F)

shader3 = glow-mix.glsl      # last pass: draws on the screen, no size to give
filter3 = linear

param_GLOW_AMOUNT = 0.6      # default of a parameter (replaces the #pragma one)
```

| Key | Default | Meaning |
|---|---|---|
| `name` | file name | name shown in the settings |
| `shaders` | | number of passes (1 to 8) |
| `shaderN` | | the `.glsl` file of pass N (N from 0) |
| `filterN` | `nearest` (or `#pragma filter`) | sampling of the inputs of pass N |
| `scale_typeN` | `source` | `source`: factor of the input size; `viewport`: factor of the output size; `absolute`: size in pixels |
| `scaleN`, `scale_xN`, `scale_yN` | `1.0` | factor, or pixels with `absolute` |
| `aliasN` | | name of the output of pass N, usable as a sampler in the later passes |
| `floatN` | `false` | 16-bit float output |
| `param_NAME` | | default value of the parameter NAME |

The last pass always draws at the size of the screen; its `scale` keys are
ignored. A value chosen in the settings wins over `param_NAME`.

## Limits

* 8 passes, 32 parameters per preset, 256 KB per file.
* No previous frames and no look-up textures.
* An output of a pass is an 8-bit texture unless `floatN = true` (if the
  graphics card refuses float targets, 8 bits are used).
* File names are read with the program's 8-bit file name encoding, so keep the
  path free of unusual characters.

## Included presets

* `scanlines-mix-split.glsl` : scanlines, from smoothly mixed to clearly split.
* `color-tweak.glsl` : brightness, contrast, saturation, gamma.
* `rgb-mask-lite.glsl` : light RGB mask with a vignette.
* `sharp-scanlines.kfx` : two passes, reuses the two files above (colour
  adjustment on a 4x enlarged picture, then the scanlines).
* `crt-glow/crt-glow.kfx` : four passes: highlights, horizontal blur, vertical
  blur, then the picture with scanlines plus the glow.
