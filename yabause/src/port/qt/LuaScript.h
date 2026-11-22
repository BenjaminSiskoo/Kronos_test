/*  Lua scripting for the Qt port (Kronos). GPL v2+

    All Lua state lives on the emulation thread: the UI only posts requests
    (load / unload), and LuaScript::frameHook() - called once per frame from
    YuiEndOfFrame() - carries them out and runs the frame callbacks. A script
    therefore never races the emulated CPUs.

    Lua API (see yabause/scripts/README.md):
      memory.readu8/16/32(addr)  memory.reads8/16/32(addr)  memory.readbytes(addr, len)
      memory.writeu8/16/32(addr, value)
      sh2.getregs(cpu)           cpu: 0 = master, 1 = slave
      emu.framecount()  emu.log(...)  emu.on_frame(fn)  emu.remove_callback(id)
      print(...)                 goes to the script console
*/
#ifndef LUASCRIPT_H
#define LUASCRIPT_H

#include <QObject>
#include <QString>

class LuaScriptHost : public QObject
{
	Q_OBJECT
public:
	static LuaScriptHost* instance();

signals:
	void logLine( const QString& line );   // may be emitted from the emulation thread
	void stateChanged( bool loaded );

private:
	LuaScriptHost() {}
};

namespace LuaScript
{
	/* UI thread: executed at the next emulated frame. */
	void requestLoad( const QString& path );
	void requestUnload();

	bool isLoaded();
	QString loadedPath();

	/* Emulation thread, once per frame. */
	void frameHook();
}

#endif // LUASCRIPT_H
