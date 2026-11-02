# Kronos libretro

## How to build
From the root of the repository:

```
make -j4 -C yabause/src/libretro/ generate-files
make -j4 -C yabause/src/libretro/
```

`generate-files` builds the Musashi 68000 opcode tables (`m68kops.h`, `m68kop*.c`);
it is needed once, before the first build and after a `make clean`.

The library is built as `yabause/src/libretro/kronos_libretro.so`
(`kronos_libretro.dll` on Windows, `kronos_libretro.dylib` on macOS).

You'll need RetroArch (or another libretro frontend) to run this library.

### With Visual Studio (MSVC)
`CMakeLists.txt` in this folder builds the same core with CMake. In Visual
Studio 2022: *File > Open > Folder* on `yabause/src/libretro`, pick the
`x64 Release` (or `x64 Debug`) configuration, then *Build > Build All*. The
DLL is written to `yabause/src/libretro/out/build/<configuration>/kronos_libretro.dll`.
The Musashi tables are generated during the build, no `generate-files` step.

From a "x64 Native Tools Command Prompt":
```
cmake -S yabause/src/libretro -B build_libretro -G "Visual Studio 17 2022" -A x64
cmake --build build_libretro --config Release
```

To debug the core: *File > Open > Project/Solution*, select `retroarch.exe`,
then in the properties of that debug project set the arguments to
`-L <path>\kronos_libretro.dll "<path to the game>" --verbose`.

## Requirements
The core renders with OpenGL compute shaders: the frontend must provide an
OpenGL **4.3 core profile** context. In RetroArch, set the video driver to `glcore`
(or `gl`).

## How to run games
Command line looks like this :

`retroarch -L path/to/kronos_libretro.so path/to/game`

Supported images: `.cue` (BIN/CUE), `.iso`, `.mds`, `.ccd`, `.chd`, `.zip`
(a BIN/CUE set zipped, or a ST-V romset from MAME) and `.m3u` playlists for
multi-disc games.

## Where do you copy the bioses
Bioses go to the SYSTEM_DIRECTORY/kronos directory (SYSTEM_DIRECTORY itself is
also searched):
- Saturn: `saturn_bios.bin` (`sega_101.bin` and `mpr-17933.bin` are also
  accepted in SYSTEM_DIRECTORY).
- ST-V: `stvbios.zip` from MAME.
- Video CD Card: its ROM as `mpeg.bin` or `mpeg.zip` (raw dump or zip
  archive).

## Video CD
With the core option "Video CD Card" on `Auto` (default), the card is plugged
when the disc is a Video CD (INFO.VCD starting with `VIDEO_CD`, `SUPERVCD` or
`HQ-VCD`). `Enabled` plugs it for every disc. Video CDs are played by the
BIOS: they need the real Saturn BIOS (not "Force HLE BIOS") and the card ROM.
A Video CD inserted later from a `.m3u` playlist needs `Enabled`.

## Video filters
"Video filter" and "Upscale filter" offer the filters and upscalers of the
standalone version. Except for the deinterlacing filters, the picture is then
drawn at the largest integer multiple of its size that fits the output buffer
(1280x720 with the internal resolution "original", larger with 2X, 4X...),
so that the CRT mask, the scanlines or HQ4x have pixels to work on.

## Saves
Saturn backup memory goes to SAVE_DIRECTORY/kronos/saturn, ST-V NVRAM and EEPROM
to SAVE_DIRECTORY/kronos/stv. The core option "Share saves with beetle" uses
Beetle Saturn's file names instead.

## Currently known issues
- The core crashes when you switch from fullscreen to windowed on the fly.
- macOS: no OpenGL 4.3, the core cannot run there.
- Android (GLES build): the core asks for an OpenGL 4.3 core context, not
  tested on GLES.
- Peripherals: Saturn pad, 3D pad, wheel and mouse, multitap; ST-V games with
  the standard JAMMA controls (2 or 3 buttons, 6 buttons). The light gun, the
  keyboard, the Mahjong panel and the hopper/trackball cabinets of the
  standalone version are not mapped.
