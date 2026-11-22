/*  Lua scripting for the Qt port (Kronos). GPL v2+ */
#include "LuaScript.h"
#include "QtYabause.h"
#include "VolatileSettings.h"

#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QByteArray>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace {

/* Pending requests, written by the UI thread, consumed by frameHook(). */
QMutex gMutex;
bool gWantLoad = false;
bool gWantUnload = false;
QString gWantPath;
bool gLoadedFlag = false;      // copy of (L != NULL), readable from the UI
QString gLoadedPath;
bool gAutorunChecked = false;

/* Only touched on the emulation thread. */
lua_State* L = NULL;

const char* kCallbacksKey = "kronos_frame_callbacks";
const int kHookStep = 1000000;        // VM instructions between two checks
const int kMaxSteps = 30;             // ~30 M instructions per call, then the script is stopped
int gSteps = 0;

void log( const QString& s )
{
	emit LuaScriptHost::instance()->logLine( s );
}

void budgetHook( lua_State* st, lua_Debug* )
{
	if ( ++gSteps > kMaxSteps ) {
		gSteps = 0;
		luaL_error( st, "script stopped: it ran too long without returning (infinite loop?)" );
	}
}

int traceback( lua_State* st )
{
	const char* msg = lua_tostring( st, 1 );
	luaL_traceback( st, st, msg ? msg : "(error object is not a string)", 1 );
	return 1;
}

/* Runs the function on top of the stack (with nargs arguments below it). */
bool protectedCall( int nargs, int nres )
{
	const int base = lua_gettop( L ) - nargs;
	lua_pushcfunction( L, traceback );
	lua_insert( L, base );
	gSteps = 0;
	const int rc = lua_pcall( L, nargs, nres, base );
	lua_remove( L, base );
	if ( rc != LUA_OK ) {
		log( QString( "Lua error: %1" ).arg( QString::fromUtf8( lua_tostring( L, -1 ) ) ) );
		lua_pop( L, 1 );
		return false;
	}
	return true;
}

/* ---- API -------------------------------------------------------------- */

int apiPrint( lua_State* st )
{
	/* No C++ object may be alive here: luaL_tolstring() can raise a Lua error
	   (a faulty __tostring), which longjmps over this frame. */
	const int n = lua_gettop( st );
	luaL_Buffer b;
	luaL_buffinit( st, &b );
	for ( int i = 1; i <= n; i++ ) {
		if ( i > 1 ) luaL_addchar( &b, '\t' );
		luaL_tolstring( st, i, NULL );
		luaL_addvalue( &b );
	}
	luaL_pushresult( &b );
	size_t len = 0;
	const char* s = lua_tolstring( st, -1, &len );
	log( QString::fromUtf8( s, (int)len ) );
	return 0;
}

SH2_struct* masterCpu( lua_State* st )
{
	if ( MSH2 == NULL )
		luaL_error( st, "the emulation is not running" );
	return MSH2;
}

u32 checkAddr( lua_State* st, int idx )
{
	return (u32)luaL_checkinteger( st, idx );
}

int memReadU8( lua_State* st )  { lua_pushinteger( st, SH2MappedMemoryReadByte( masterCpu( st ), checkAddr( st, 1 ) ) ); return 1; }
int memReadU16( lua_State* st ) { lua_pushinteger( st, SH2MappedMemoryReadWord( masterCpu( st ), checkAddr( st, 1 ) ) ); return 1; }
int memReadU32( lua_State* st ) { lua_pushinteger( st, (lua_Integer)SH2MappedMemoryReadLong( masterCpu( st ), checkAddr( st, 1 ) ) ); return 1; }
int memReadS8( lua_State* st )  { lua_pushinteger( st, (s8)SH2MappedMemoryReadByte( masterCpu( st ), checkAddr( st, 1 ) ) ); return 1; }
int memReadS16( lua_State* st ) { lua_pushinteger( st, (s16)SH2MappedMemoryReadWord( masterCpu( st ), checkAddr( st, 1 ) ) ); return 1; }
int memReadS32( lua_State* st ) { lua_pushinteger( st, (s32)SH2MappedMemoryReadLong( masterCpu( st ), checkAddr( st, 1 ) ) ); return 1; }

int memReadBytes( lua_State* st )
{
	SH2_struct* cpu = masterCpu( st );
	const u32 addr = checkAddr( st, 1 );
	const lua_Integer len = luaL_checkinteger( st, 2 );
	if ( len < 0 || len > 0x10000 )
		return luaL_error( st, "memory.readbytes: length must be between 0 and 65536" );
	QByteArray buf( (int)len, 0 );
	for ( int i = 0; i < (int)len; i++ )
		buf[i] = (char)SH2MappedMemoryReadByte( cpu, addr + (u32)i );
	lua_pushlstring( st, buf.constData(), (size_t)buf.size() );
	return 1;
}

int memWriteU8( lua_State* st )
{
	SH2_struct* cpu = masterCpu( st );
	const u32 addr = checkAddr( st, 1 );
	SH2MappedMemoryWriteByte( cpu, addr, (u8)luaL_checkinteger( st, 2 ) );
	SH2WriteNotify( cpu, addr, 1 );
	return 0;
}
int memWriteU16( lua_State* st )
{
	SH2_struct* cpu = masterCpu( st );
	const u32 addr = checkAddr( st, 1 );
	SH2MappedMemoryWriteWord( cpu, addr, (u16)luaL_checkinteger( st, 2 ) );
	SH2WriteNotify( cpu, addr, 2 );
	return 0;
}
int memWriteU32( lua_State* st )
{
	SH2_struct* cpu = masterCpu( st );
	const u32 addr = checkAddr( st, 1 );
	SH2MappedMemoryWriteLong( cpu, addr, (u32)luaL_checkinteger( st, 2 ) );
	SH2WriteNotify( cpu, addr, 4 );
	return 0;
}

int sh2GetRegs( lua_State* st )
{
	const lua_Integer which = luaL_optinteger( st, 1, 0 );
	SH2_struct* cpu = ( which == 1 ) ? SSH2 : MSH2;
	if ( cpu == NULL )
		return luaL_error( st, "sh2.getregs: the emulation is not running" );
	sh2regs_struct r;
	SH2GetRegisters( cpu, &r );
	lua_createtable( st, 16, 8 );
	for ( int i = 0; i < 16; i++ ) {
		lua_pushinteger( st, (lua_Integer)r.R[i] );
		lua_rawseti( st, -2, i );              // regs[0] .. regs[15]
	}
	lua_pushinteger( st, r.PC );   lua_setfield( st, -2, "PC" );
	lua_pushinteger( st, r.PR );   lua_setfield( st, -2, "PR" );
	lua_pushinteger( st, r.SR.all ); lua_setfield( st, -2, "SR" );
	lua_pushinteger( st, r.GBR );  lua_setfield( st, -2, "GBR" );
	lua_pushinteger( st, r.VBR );  lua_setfield( st, -2, "VBR" );
	lua_pushinteger( st, r.MACH ); lua_setfield( st, -2, "MACH" );
	lua_pushinteger( st, r.MACL ); lua_setfield( st, -2, "MACL" );
	return 1;
}

int emuFrameCount( lua_State* st )
{
	lua_pushinteger( st, (lua_Integer)yabsys.frame_count );
	return 1;
}

int emuOnFrame( lua_State* st )
{
	luaL_checktype( st, 1, LUA_TFUNCTION );
	lua_getfield( st, LUA_REGISTRYINDEX, kCallbacksKey );
	const lua_Integer id = (lua_Integer)lua_rawlen( st, -1 ) + 1;
	lua_pushvalue( st, 1 );
	lua_rawseti( st, -2, id );
	lua_pushinteger( st, id );
	return 1;
}

int emuRemoveCallback( lua_State* st )
{
	const lua_Integer id = luaL_checkinteger( st, 1 );
	lua_getfield( st, LUA_REGISTRYINDEX, kCallbacksKey );
	lua_pushnil( st );
	lua_rawseti( st, -2, id );
	return 0;
}

void registerFunctions( const char* table, const luaL_Reg* funcs )
{
	lua_newtable( L );
	luaL_setfuncs( L, funcs, 0 );
	lua_setglobal( L, table );
}

void closeState()
{
	if ( L != NULL ) {
		lua_close( L );
		L = NULL;
	}
}

bool openAndRun( const QString& path )
{
	QFile f( path );
	if ( !f.open( QIODevice::ReadOnly ) ) {
		log( QString( "Lua: cannot open %1" ).arg( path ) );
		return false;
	}
	const QByteArray code = f.readAll();
	f.close();

	L = luaL_newstate();
	if ( L == NULL ) {
		log( "Lua: out of memory" );
		return false;
	}

	/* Sandbox: no io, os, package or debug library, no file loaders. */
	static const luaL_Reg libs[] = {
		{ LUA_GNAME, luaopen_base }, { LUA_TABLIBNAME, luaopen_table },
		{ LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math },
		{ LUA_UTF8LIBNAME, luaopen_utf8 }, { LUA_COLIBNAME, luaopen_coroutine },
		{ NULL, NULL }
	};
	for ( const luaL_Reg* l = libs; l->func; l++ ) {
		luaL_requiref( L, l->name, l->func, 1 );
		lua_pop( L, 1 );
	}
	lua_pushnil( L ); lua_setglobal( L, "dofile" );
	lua_pushnil( L ); lua_setglobal( L, "loadfile" );
	lua_pushcfunction( L, apiPrint ); lua_setglobal( L, "print" );

	lua_newtable( L );
	lua_setfield( L, LUA_REGISTRYINDEX, kCallbacksKey );

	static const luaL_Reg memoryFuncs[] = {
		{ "readu8", memReadU8 }, { "readu16", memReadU16 }, { "readu32", memReadU32 },
		{ "reads8", memReadS8 }, { "reads16", memReadS16 }, { "reads32", memReadS32 },
		{ "readbytes", memReadBytes },
		{ "writeu8", memWriteU8 }, { "writeu16", memWriteU16 }, { "writeu32", memWriteU32 },
		{ NULL, NULL }
	};
	static const luaL_Reg sh2Funcs[] = { { "getregs", sh2GetRegs }, { NULL, NULL } };
	static const luaL_Reg emuFuncs[] = {
		{ "framecount", emuFrameCount }, { "log", apiPrint },
		{ "on_frame", emuOnFrame }, { "remove_callback", emuRemoveCallback },
		{ NULL, NULL }
	};
	registerFunctions( "memory", memoryFuncs );
	registerFunctions( "sh2", sh2Funcs );
	registerFunctions( "emu", emuFuncs );

	lua_sethook( L, budgetHook, LUA_MASKCOUNT, kHookStep );

	const QByteArray chunkName = "@" + QFile::encodeName( path );
	if ( luaL_loadbuffer( L, code.constData(), (size_t)code.size(), chunkName.constData() ) != LUA_OK ) {
		log( QString( "Lua error: %1" ).arg( QString::fromUtf8( lua_tostring( L, -1 ) ) ) );
		closeState();
		return false;
	}
	if ( !protectedCall( 0, 0 ) ) {
		closeState();
		return false;
	}
	return true;
}

void runFrameCallbacks()
{
	lua_getfield( L, LUA_REGISTRYINDEX, kCallbacksKey );
	const int tab = lua_gettop( L );
	const lua_Integer n = (lua_Integer)lua_rawlen( L, tab );
	for ( lua_Integer i = 1; i <= n; i++ ) {
		lua_rawgeti( L, tab, i );
		if ( lua_isnil( L, -1 ) ) { lua_pop( L, 1 ); continue; }
		if ( !protectedCall( 0, 0 ) ) {
			/* A callback that raised an error is dropped, not retried every frame. */
			lua_pushnil( L );
			lua_rawseti( L, tab, i );
		}
	}
	lua_settop( L, tab - 1 );
}

} // namespace

LuaScriptHost* LuaScriptHost::instance()
{
	static LuaScriptHost* host = new LuaScriptHost;
	return host;
}

namespace LuaScript {

void requestLoad( const QString& path )
{
	QMutexLocker lock( &gMutex );
	gWantLoad = true;
	gWantUnload = true;      // replaces the script already running
	gWantPath = path;
}

void requestUnload()
{
	QMutexLocker lock( &gMutex );
	gWantUnload = true;
	gWantLoad = false;
}

bool isLoaded()
{
	QMutexLocker lock( &gMutex );
	return gLoadedFlag;
}

QString loadedPath()
{
	QMutexLocker lock( &gMutex );
	return gLoadedPath;
}

void frameHook()
{
	if ( !gAutorunChecked ) {
		gAutorunChecked = true;
		VolatileSettings* vs = QtYabause::volatileSettings( false );
		if ( vs != NULL ) {
			const QString p = vs->value( "lua/script" ).toString();
			if ( !p.isEmpty() )
				requestLoad( p );
		}
	}

	bool doUnload, doLoad;
	QString path;
	{
		QMutexLocker lock( &gMutex );
		doUnload = gWantUnload;
		doLoad = gWantLoad;
		path = gWantPath;
		gWantUnload = gWantLoad = false;
	}

	if ( doUnload && L != NULL ) {
		closeState();
		{ QMutexLocker lock( &gMutex ); gLoadedFlag = false; gLoadedPath.clear(); }
		log( "Lua: script stopped" );
		emit LuaScriptHost::instance()->stateChanged( false );
	}
	if ( doLoad ) {
		const bool ok = openAndRun( path );
		{ QMutexLocker lock( &gMutex ); gLoadedFlag = ok; gLoadedPath = ok ? path : QString(); }
		if ( ok )
			log( QString( "Lua: %1 loaded" ).arg( path ) );
		emit LuaScriptHost::instance()->stateChanged( ok );
	}

	if ( L != NULL )
		runFrameCallbacks();
}

} // namespace LuaScript
