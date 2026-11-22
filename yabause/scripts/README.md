# Lua scripts

Open **Debug > Lua scripts**, choose a `.lua` file and press *Load / Reload*, or start Kronos with
`--lua script.lua`. A script is loaded (and stopped) at the next emulated frame, so the emulation
must be running. Everything runs on the emulation thread, between two frames.

## API

| Function | Description |
|---|---|
| `memory.readu8/readu16/readu32(addr)` | Read as seen by the master SH-2 (big endian). |
| `memory.reads8/reads16/reads32(addr)` | Same, signed. |
| `memory.readbytes(addr, len)` | A Lua string of `len` bytes (max 65536). |
| `memory.writeu8/writeu16/writeu32(addr, value)` | Write; the SH-2 code cache is notified. |
| `sh2.getregs(cpu)` | `cpu` 0 = master, 1 = slave. Table with `[0]`..`[15]` (R0-R15), `PC PR SR GBR VBR MACH MACL`. |
| `emu.framecount()` | Frame counter of the emulation. |
| `emu.on_frame(fn)` | Call `fn()` after every frame; returns an id. |
| `emu.remove_callback(id)` | Remove a callback. |
| `emu.log(...)` / `print(...)` | Print to the script console. |

Reading hardware registers (`0x25F...`, `0x5A...`) goes through the normal bus and can have side
effects, exactly as a CPU read would.

## Limits

* Sandboxed: no `io`, `os`, `package`, `debug`, `dofile` or `loadfile`.
* A script call that runs more than about 30 million VM instructions is stopped (infinite loop guard).
* A callback that raises an error is removed; the error and traceback go to the console.
* No input injection, no memory-write hooks and no pause from Lua yet.

## Example

See `example-watch.lua`.
