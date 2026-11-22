-- Logs the master SH-2 program counter and a Work RAM-H long every 60 frames.
local WATCH = 0x06000000

emu.on_frame(function()
  local f = emu.framecount()
  if f % 60 == 0 then
    local r = sh2.getregs(0)
    print(string.format("frame %d  PC=%08X  [%08X]=%08X", f, r.PC, WATCH, memory.readu32(WATCH)))
  end
end)
