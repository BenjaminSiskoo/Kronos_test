/*  Kronos - command line options of the Qt port.

    Every option lives in one table (see buildOptions()). --help and the
    Markdown reference (--help-markdown, see doc/COMMAND_LINE.md) are
    generated from that table, so they can't drift from what is parsed.

    Options only write "volatile" settings (VolatileSettings): they override
    the saved configuration for this run and are never written back to the
    .ini file. A value can be given as --option=value or --option value. */

#include "Arguments.h"

#include "VolatileSettings.h"
#include "QtYabause.h"
#include "stv.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegExp>
#include <QString>
#include <QStringList>

extern "C" {
extern M68K_struct* M68KCoreList[];
extern SH2Interface_struct* SH2CoreList[];
extern PerInterface_struct* PERCoreList[];
extern SoundInterface_struct* SNDCoreList[];
}

/* Fallbacks: these ids live in headers that not every build includes. */
#ifndef SH2CORE_KRONOS_INTERPRETER
#define SH2CORE_KRONOS_INTERPRETER 8
#endif
#ifndef SH2CORE_KRONOS_DEBUG_INTERPRETER
#define SH2CORE_KRONOS_DEBUG_INTERPRETER 9
#endif

namespace Arguments
{
namespace
{

typedef std::function<bool(const QString& value, QString& error)> Handler;

enum Kind
{
	Flag,   // --name
	Value,  // --name=value  or  --name value
	Bool    // --name  or  --name=0|1   (no value means "on")
};

/* Priorities: options run sorted by priority, then in command line order. */
enum Priority
{
	PrioInfo = 0,    // --help, --version: run first and exit
	PrioNormal = 1,
	PrioLate = 2     // needs the other options already applied (--stv-list)
};

struct Choice
{
	const char* name;   // NULL ends the list
	int value;
	bool hidden;        // alias: accepted, but not shown in --help
};

struct Option
{
	const char* shortname;  // "-b" or NULL
	const char* longname;   // "--bios"; NULL for a section heading
	const char* alias;      // hidden second long name or NULL
	Kind kind;
	QString parameter;      // placeholder shown in --help
	QString description;    // empty: option not shown in --help
	int priority;
	Handler handler;
};

VolatileSettings* vs()
{
	return QtYabause::volatileSettings();
}

QString absPath(const QString& path)
{
	return QFileInfo(path).absoluteFilePath();
}

bool parseBool(const QString& text, bool& out)
{
	const QString t = text.trimmed().toLower();
	if (t.isEmpty() || t == "1" || t == "on" || t == "true" || t == "yes") { out = true; return true; }
	if (t == "0" || t == "off" || t == "false" || t == "no") { out = false; return true; }
	return false;
}

QString choiceNames(const Choice* choices)
{
	QStringList names;
	for (const Choice* c = choices; c->name; ++c)
		if (!c->hidden)
			names << c->name;
	return names.join(", ");
}

bool findChoice(const Choice* choices, const QString& text, bool allowNumeric, int& out, QString& error)
{
	const QString t = text.trimmed().toLower();
	for (const Choice* c = choices; c->name; ++c)
		if (t == QString(c->name)) { out = c->value; return true; }
	if (allowNumeric)
	{
		bool ok = false;
		const int n = t.toInt(&ok, 0);
		if (ok)
			for (const Choice* c = choices; c->name; ++c)
				if (c->value == n) { out = n; return true; }
	}
	error = QString("unknown value '%1' (expected: %2)").arg(text, choiceNames(choices));
	return false;
}

template <typename T>
bool coreAvailable(T* const* list, int id)
{
	for (; *list; ++list)
		if ((*list)->id == id)
			return true;
	return false;
}

/* ---------- handler factories ---------- */

Handler setConst(const char* key, const QVariant& value)
{
	const QString k(key);
	return [k, value](const QString&, QString&) { vs()->setValue(k, value); return true; };
}

Handler setBoolean(const char* key)
{
	const QString k(key);
	return [k](const QString& v, QString& err)
	{
		bool b = false;
		if (!parseBool(v, b)) { err = "expected 0, 1, on, off, true, false, yes or no"; return false; }
		vs()->setValue(k, b);
		return true;
	};
}

Handler setInteger(const char* key, int min, int max)
{
	const QString k(key);
	return [k, min, max](const QString& v, QString& err)
	{
		bool ok = false;
		const int n = v.trimmed().toInt(&ok, 0);
		if (!ok || n < min || n > max) { err = QString("expected an integer between %1 and %2").arg(min).arg(max); return false; }
		vs()->setValue(k, n);
		return true;
	};
}

/* Path of a file (or folder) that must exist. */
Handler setPath(const char* key, bool mustExist, bool folder)
{
	const QString k(key);
	return [k, mustExist, folder](const QString& v, QString& err)
	{
		const QFileInfo fi(v);
		if (mustExist && !fi.exists()) { err = QString("'%1' not found").arg(v); return false; }
		if (mustExist && folder && !fi.isDir()) { err = QString("'%1' is not a folder").arg(v); return false; }
		if (mustExist && !folder && fi.isDir()) { err = QString("'%1' is a folder, a file is expected").arg(v); return false; }
		vs()->setValue(k, absPath(v));
		return true;
	};
}

Handler setChoice(const char* key, const Choice* choices, bool allowNumeric = true)
{
	const QString k(key);
	return [k, choices, allowNumeric](const QString& v, QString& err)
	{
		int n = 0;
		if (!findChoice(choices, v, allowNumeric, n, err)) return false;
		vs()->setValue(k, n);
		return true;
	};
}

/* ---------- value tables ---------- */

const Choice languages[] = {
	{ "english", 0, false }, { "deutsch", 1, false }, { "french", 2, false },
	{ "spanish", 3, false }, { "italian", 4, false }, { "japanese", 5, false },
	{ "german", 1, true }, { "francais", 2, true }, { "espanol", 3, true }, { "italiano", 4, true },
	{ NULL, 0, false } };

const Choice cartridges[] = {
	{ "none", CART_NONE, false },
	{ "par", CART_PAR, false },
	{ "bup4", CART_BACKUPRAM4MBIT, false },
	{ "bup8", CART_BACKUPRAM8MBIT, false },
	{ "bup16", CART_BACKUPRAM16MBIT, false },
	{ "bup32", CART_BACKUPRAM32MBIT, false },
	{ "dram8", CART_DRAM8MBIT, false },
	{ "dram32", CART_DRAM32MBIT, false },
	{ "dram128", CART_DRAM128MBIT, false },
	{ "rom16", CART_ROM16MBIT, false },
	{ "netlink", CART_NETLINK, false },
	{ "modem", CART_JAPMODEM, false },
	{ "stv", CART_ROMSTV, false },
	{ "dev", CART_DEV, false },
	{ "vcd", CART_MPEGCARD, false },
	{ "mpeg", CART_MPEGCARD, true },
	{ "mpegcard", CART_MPEGCARD, true },
	{ "japmodem", CART_JAPMODEM, true },
	{ NULL, 0, false } };

const Choice controllers[] = {
	{ "none", 0, false },
	{ "pad", PERPAD, false },
	{ "3dpad", PER3DPAD, false },
	{ "mission", PERMISSIONSTICK, false },
	{ "twin", PERTWINSTICKS, false },
	{ "gun", PERGUN, false },
	{ "keyboard", PERKEYBOARD, false },
	{ "mouse", PERMOUSE, false },
	{ "wheel", PERWHEEL, false },
	{ "cabinet", PERCABINET, false },
	{ "digital", PERPAD, true }, { "3d", PER3DPAD, true },
	{ "missionstick", PERMISSIONSTICK, true }, { "doublemission", PERTWINSTICKS, true },
	{ "twinsticks", PERTWINSTICKS, true }, { "lightgun", PERGUN, true },
	{ "racing", PERWHEEL, true }, { "stv", PERCABINET, true },
	{ "hopper", PERCABINET, true }, { "mahjong", PERCABINET, true },
	{ NULL, 0, false } };

const Choice perCores[] = {
	{ "dummy", PERCORE_DUMMY, false },
	{ "qt", PERCORE_QT, false },
	{ "sdl", PERCORE_SDLJOY, false },
	{ "linuxjoy", PERCORE_LINUXJOY, false },
	{ "macjoy", PERCORE_MACJOY, false },
#ifdef PERCORE_DIRECTX
	{ "directinput", PERCORE_DIRECTX, false },
#endif
	{ NULL, 0, false } };

const Choice soundCores[] = {
	{ "dummy", SNDCORE_DUMMY, false },
	{ "sdl", SNDCORE_SDL, false },
	{ "openal", SNDCORE_AL, false },
	{ "wav", SNDCORE_WAV, false },
#ifdef SNDCORE_DIRECTX
	{ "directsound", SNDCORE_DIRECTX, false },
#endif
#ifdef SNDCORE_MAC
	{ "coreaudio", SNDCORE_MAC, false },
#endif
	{ NULL, 0, false } };

const Choice sh2Cores[] = {
	{ "interpreter", SH2CORE_KRONOS_INTERPRETER, false },
	{ "debug", SH2CORE_KRONOS_DEBUG_INTERPRETER, false },
	{ NULL, 0, false } };

const Choice m68kCores[] = {
	{ "dummy", M68KCORE_DUMMY, false },
	{ "c68k", M68KCORE_C68K, false },
	{ "q68", M68KCORE_Q68, false },
	{ "musashi", M68KCORE_MUSASHI, false },
	{ NULL, 0, false } };

const Choice aspectRatios[] = {
	{ "original", 0, false }, { "stretch", 1, false },
	{ "pixel-perfect-fullscreen", 2, false }, { "pixel-perfect", 3, false },
	{ NULL, 0, false } };

const Choice resolutions[] = {
	{ "1x", 1, false }, { "2x", 8, false }, { "3x", 128, false },
	{ "4x", 32, false }, { "6x", 256, false }, { "native", 64, false },
	{ "original", 1, true },
	{ NULL, 0, false } };

const Choice filters[] = {
	{ "none", 0, false }, { "bilinear", 1, false }, { "bicubic", 2, false },
	{ "deinterlace-adaptive", 3, false }, { "deinterlace-debug", 4, false }, { "deinterlace-bob", 5, false },
	{ "scanline", 6, false }, { "lanczos3", 7, false }, { "sharp-bilinear", 8, false },
	{ "fxaa", 9, false }, { "crt", 10, false }, { "toon", 12, false },
	{ NULL, 0, false } };

const Choice upscalers[] = {
	{ "none", 0, false }, { "hq4x", 1, false }, { "4xbrz", 2, false }, { "6xbrz", 3, false },
	{ "sharpen", 4, false }, { "fsr", 5, false }, { "scale3x", 6, false }, { "scale4x", 7, false },
	{ NULL, 0, false } };

const Choice wireframes[] = {
	{ "off", 0, false }, { "sprite-colors", 1, false }, { "white", 2, false },
	{ "on", 1, true },
	{ NULL, 0, false } };

const Choice originalImproved[] = {
	{ "original", 0, false }, { "improved", 1, false },
	{ NULL, 0, false } };

const Choice stvRegions[] = {
	{ "auto", 0, false }, { "europe", 1, false }, { "usa", 2, false },
	{ "japan", 3, false }, { "asia", 4, false },
	{ "e", 1, true }, { "eu", 1, true }, { "pal", 1, true },
	{ "u", 2, true }, { "us", 2, true },
	{ "j", 3, true }, { "jp", 3, true },
	{ "t", 4, true },
	{ NULL, 0, false } };

/* ---------- option specific handlers ---------- */

bool doHelp(const QString&, QString&);
bool doHelpMarkdown(const QString&, QString&);

bool doVersion(const QString&, QString&)
{
	std::cout << VERSION << std::endl;
	exit(0);
}

bool doAutostart(const QString&, QString&)
{
	vs()->setValue("autorun", true);
	return true;
}

bool doAutoload(const QString& v, QString& err)
{
	bool ok = false;
	const int slot = v.trimmed().toInt(&ok);
	if (!ok || slot < 0 || slot > 9) { err = "expected a save state slot between 0 and 9"; return false; }
	vs()->setValue("autorun", true);
	/* YabauseThread only loads the state when "autostart" is set. */
	vs()->setValue("autostart", true);
	vs()->setValue("autorun/load", true);
	vs()->setValue("autorun/load/slot", slot);
	return true;
}

bool doBinary(const QString&, QString&)
{
	/* Historical behaviour: the file and the address were never used.
	   Loading a raw binary is not implemented in the Qt port yet. */
	vs()->setValue("autostart", true);
	return true;
}

bool doBios(const QString& v, QString& err)
{
	if (!setPath("General/Bios", true, false)(v, err))
		return false;
	vs()->setValue("General/EnableEmulatedBios", false);
	return true;
}

bool doIso(const QString& v, QString& err)
{
	const QFileInfo fi(v);
	if (!fi.exists()) { err = QString("'%1' not found").arg(v); return false; }
	vs()->setValue("General/CdRom", CDCORE_ISO);
	vs()->setValue("General/CdRomISO", absPath(v));
	return true;
}

bool doCdrom(const QString& v, QString&)
{
	vs()->setValue("General/CdRom", CDCORE_ARCH);
	vs()->setValue("General/CdRomISO", v);
	return true;
}

bool doNoDisc(const QString&, QString&)
{
	vs()->setValue("General/CdRom", CDCORE_DUMMY);
	return true;
}

bool doModemIp(const QString& v, QString& err)
{
	const QString n("(?:[0-1]?[0-9]?[0-9]|2[0-4][0-9]|25[0-5])");
	if (!QRegExp("^" + n + "\\." + n + "\\." + n + "\\." + n + "$").exactMatch(v.trimmed()))
	{
		err = "expected an IPv4 address such as 127.0.0.1";
		return false;
	}
	vs()->setValue("Cartridge/ModemIP", v.trimmed());
	return true;
}

bool doModemPort(const QString& v, QString& err)
{
	bool ok = false;
	const int p = v.trimmed().toInt(&ok);
	if (!ok || p < 1 || p > 65535) { err = "expected a port between 1 and 65535"; return false; }
	vs()->setValue("Cartridge/ModemPort", QString::number(p));
	return true;
}

/* --stv=<romset>  or  --stv=<folder>/<romset>.zip */
bool doStv(const QString& v, QString& err)
{
	QString romset = v.trimmed();
	if (romset.isEmpty()) { err = "expected a romset name or a .zip file"; return false; }

	if (romset.endsWith(".zip", Qt::CaseInsensitive))
	{
		const QFileInfo fi(romset);
		if (!fi.isFile()) { err = QString("'%1' not found").arg(v); return false; }
		vs()->setValue("Cartridge/Path", fi.absolutePath());
		romset = fi.completeBaseName();
	}
	else if (romset.contains('/') || romset.contains('\\'))
	{
		err = "expected a romset name (e.g. bakubaku) or a .zip file; use --stv-path for the folder";
		return false;
	}

	vs()->setValue("Cartridge/Type", CART_ROMSTV);
	vs()->setValue("Cartridge/STVGame", romset);
	vs()->setValue("Cartridge/STVGameName", romset);
	return true;
}

bool doStvRegion(const QString& v, QString& err)
{
	static const char* const letters[] = { "Auto", "E", "U", "J", "T" };
	int n = 0;
	if (!findChoice(stvRegions, v, false, n, err)) return false;
	vs()->setValue("STV/Region", QString(letters[n]));
	return true;
}

bool doStvList(const QString&, QString& err)
{
	const QString path = vs()->value("Cartridge/Path").toString();
	if (path.isEmpty() || !QFileInfo(path).isDir())
	{
		err = "no ROM folder: give one with --stv-path=<DIR> (or set it in Settings > Cartridge)";
		return false;
	}
	const int count = STVGetRomList(QFile::encodeName(path).constData(), 1);
	std::vector<std::pair<QString, QString> > games;
	for (int i = 0; i < count; i++)
	{
		const char* set = getSTVRomset(i);
		const char* name = getSTVGameName(i);
		games.push_back(std::make_pair(QString(set ? set : ""), QString(name ? name : "")));
	}
	std::sort(games.begin(), games.end());
	for (size_t i = 0; i < games.size(); i++)
		std::cout << games[i].first.toLocal8Bit().constData() << "\t"
		          << games[i].second.toLocal8Bit().constData() << std::endl;
	std::cerr << count << " ST-V game(s) found in " << path.toLocal8Bit().constData() << std::endl;
	exit(0);
}

/* --port1 / --port2 : one controller type per slot, comma separated.
   YabauseThread::reloadControllers() reads "Input/Override/PortN" and then
   ignores the controllers saved in the settings for that port. */
bool doPort(int port, const QString& v, QString& err)
{
	const QStringList tokens = v.split(',');
	if (tokens.size() > 6) { err = "at most 6 controllers per port (multitap)"; return false; }
	QStringList types;
	for (int i = 0; i < tokens.size(); i++)
	{
		int type = 0;
		if (!findChoice(controllers, tokens[i], true, type, err)) return false;
		types << QString::number(type);
	}
	vs()->setValue(QString("Input/Override/Port%1").arg(port), types.join(","));
	return true;
}

template <typename T>
Handler setCore(const char* key, const Choice* choices, T* const* list, const char* what)
{
	const QString k(key);
	const QString w(what);
	return [k, choices, list, w](const QString& v, QString& err)
	{
		int n = 0;
		if (!findChoice(choices, v, true, n, err)) return false;
		if (!coreAvailable(list, n)) { err = QString("this %1 core is not available in this build").arg(w); return false; }
		vs()->setValue(k, n);
		return true;
	};
}

bool doNoSound(const QString&, QString&)
{
	vs()->setValue("Sound/SoundCore", SNDCORE_DUMMY);
	return true;
}

bool doWindowSize(const QString& v, QString& err)
{
	QRegExp re("^(\\d+)[xX](\\d+)$");
	if (!re.exactMatch(v.trimmed()) || re.cap(1).toInt() < 1 || re.cap(2).toInt() < 1)
	{
		err = "expected WIDTHxHEIGHT, e.g. 1280x720";
		return false;
	}
	vs()->setValue("Video/WinWidth", re.cap(1).toInt());
	vs()->setValue("Video/WinHeight", re.cap(2).toInt());
	return true;
}

/* Only used when "Clock sync" is on, so giving a date turns it on. */
bool doBaseTime(const QString& v, QString& err)
{
	const QDateTime dt = QDateTime::fromString(v.trimmed(), Qt::ISODate);
	if (!dt.isValid()) { err = "expected an ISO 8601 date, e.g. 1996-01-01T12:00:00"; return false; }
	vs()->setValue("General/FixedBaseTime", dt.toString(Qt::ISODate));
	vs()->setValue("General/ClockSync", true);
	return true;
}

bool doLogFile(const QString& v, QString&)
{
	qputenv("KRONOS_LOG_FILE", QFile::encodeName(absPath(v)));
	return true;
}

/* ---------- the option table ---------- */

Option heading(const char* title)
{
	Option o = { NULL, NULL, NULL, Flag, QString(), QString(title), PrioNormal, Handler() };
	return o;
}

Option flag(const char* sh, const char* lo, const char* desc, int prio, Handler h, const char* alias = NULL)
{
	Option o = { sh, lo, alias, Flag, QString(), QString(desc), prio, h };
	return o;
}

Option value(const char* sh, const char* lo, const char* param, const char* desc, Handler h, const char* alias = NULL, int prio = PrioNormal)
{
	Option o = { sh, lo, alias, Value, QString(param), QString(desc), prio, h };
	return o;
}

Option boolean(const char* lo, const char* desc, Handler h)
{
	Option o = { NULL, lo, NULL, Bool, QString("0|1"), QString(desc), PrioNormal, h };
	return o;
}

Option choice(const char* sh, const char* lo, const char* param, const char* desc, const char* key,
              const Choice* choices, bool allowNumeric = true)
{
	const QString full = QString(desc) + " Values: " + choiceNames(choices) + ".";
	Option o = { sh, lo, NULL, Value, QString(param), full, PrioNormal, setChoice(key, choices, allowNumeric) };
	return o;
}

const std::vector<Option>& buildOptions()
{
	static std::vector<Option> o;
	if (!o.empty())
		return o;

	o.push_back(heading("Information"));
	o.push_back(flag("-h", "--help", "Show this help and exit.", PrioInfo, doHelp));
	o.push_back(flag("-v", "--version", "Show the version and exit.", PrioInfo, doVersion));
	o.push_back(flag(NULL, "--help-markdown", "", PrioInfo, doHelpMarkdown));

	o.push_back(heading("Start-up"));
	o.push_back(flag("-a", "--autostart", "Start the emulation automatically.", PrioNormal, doAutostart));
	o.push_back(value(NULL, "--autoload", "<SLOT>", "Start the emulation and load save state slot 0-9.", doAutoload));
	o.push_back(flag("-f", "--fullscreen", "Start in full screen.", PrioNormal, setConst("Video/Fullscreen", true)));
	o.push_back(value(NULL, "--binary", "<FILE>[:ADDRESS]", "Load a raw binary (not implemented yet in the Qt port).", doBinary));
	o.push_back(value(NULL, "--log-file", "<FILE>", "Also write the emulator log to a file (same as the KRONOS_LOG_FILE variable).", doLogFile));

	o.push_back(heading("BIOS and disc"));
	o.push_back(value("-b", "--bios", "<FILE>", "Use this Saturn BIOS file (turns the emulated BIOS off).", doBios));
	o.push_back(flag("-nb", "--no-bios", "Use the emulated BIOS.", PrioNormal, setConst("General/EnableEmulatedBios", true)));
	o.push_back(value(NULL, "--bios-settings", "<FILE>", "File that stores the BIOS (SMPC) settings.", setPath("General/BiosSettings", false, false)));
	o.push_back(value("-i", "--iso", "<FILE>", "Disc image to load (.cue, .iso, .chd, .zip ...).", doIso));
	o.push_back(value("-c", "--cdrom", "<DEVICE>", "Use a physical CD-ROM drive (e.g. D: or /dev/sr0).", doCdrom));
	o.push_back(flag(NULL, "--no-disc", "Start without a disc (BIOS CD player screen).", PrioNormal, doNoDisc));
	o.push_back(choice("-l", "--language", "<LANG>", "Saturn system language.", "General/SystemLanguageID", languages, true));

	o.push_back(heading("Cartridge, backup memory and save states"));
	{
		Option c = choice(NULL, "--cartridge", "<TYPE>", "Cartridge to plug in the cartridge port. 'stv' needs --stv-path and --stv; 'vcd' is the Video CD Card.", "Cartridge/Type", cartridges);
		o.push_back(c);
	}
	o.push_back(value(NULL, "--cartridge-path", "<FILE|DIR>", "File used by the cartridge (backup RAM, PAR, ROM) or the ST-V ROM folder.", setPath("Cartridge/Path", false, false)));
	o.push_back(value(NULL, "--modem-ip", "<IP>", "IP address of the Netlink / modem cartridge.", doModemIp));
	o.push_back(value(NULL, "--modem-port", "<PORT>", "TCP port of the Netlink / modem cartridge.", doModemPort));
	o.push_back(value(NULL, "--backup-ram", "<FILE>", "Internal backup memory file.", setPath("Memory/Path", false, false)));
	o.push_back(boolean("--extended-backup", "Use the extended internal backup memory.", setBoolean("Memory/ExtendMemory")));
	o.push_back(value(NULL, "--save-states", "<DIR>", "Folder of the save states.", setPath("General/SaveStates", false, true)));

	o.push_back(heading("ST-V (arcade)"));
	o.push_back(value(NULL, "--stv", "<ROMSET|FILE.zip>", "Run an ST-V game: a romset name (bakubaku) or the .zip itself. Selects the ST-V cartridge.", doStv));
	o.push_back(value(NULL, "--stv-path", "<DIR>", "Folder with stvbios.zip and the game .zip files (same as --cartridge-path).", setPath("Cartridge/Path", true, true)));
	{
		Option r = value(NULL, "--stv-region", "<REGION>", "", doStvRegion);
		r.description = QString("Region of the ST-V game. Values: %1.").arg(choiceNames(stvRegions));
		o.push_back(r);
	}
	o.push_back(flag(NULL, "--stv-list", "List the games found in the ST-V folder (romset and name), then exit.", PrioLate, doStvList));

	o.push_back(heading("Video CD Card (MPEG)"));
	o.push_back(value(NULL, "--mpeg-rom", "<FILE>", "MPEG ROM of the Video CD Card (.bin, .rom or .zip). Enough to enable the card; play a Video CD with --iso.", setPath("MpegROM/Path", true, false), "--vcd-rom"));

	o.push_back(heading("Controllers"));
	{
		Option p1 = value(NULL, "--port1", "<TYPE[,TYPE...]>", "", [](const QString& v, QString& e) { return doPort(1, v, e); });
		p1.description = QString("Controllers on port 1, one per slot (up to 6 with a multitap). "
		                         "Key bindings come from Settings > Input. Values: %1.").arg(choiceNames(controllers));
		o.push_back(p1);
		Option p2 = value(NULL, "--port2", "<TYPE[,TYPE...]>", "", [](const QString& v, QString& e) { return doPort(2, v, e); });
		p2.description = QString("Controllers on port 2, same syntax as --port1.");
		o.push_back(p2);
	}
	o.push_back(choice(NULL, "--per-core", "<CORE>", "Input backend.", "Input/PerCore", perCores));
	{
		Option& last = o.back();
		last.handler = setCore("Input/PerCore", perCores, PERCoreList, "input");
	}

	o.push_back(heading("Audio"));
	o.push_back(flag("-ns", "--no-sound", "Turn the sound off.", PrioNormal, doNoSound));
	o.push_back(value(NULL, "--volume", "<0-100>", "Sound volume.", setInteger("Sound/Volume", 0, 100)));
	{
		Option s = choice(NULL, "--sound-core", "<CORE>", "Sound output backend.", "Sound/SoundCore", soundCores);
		s.handler = setCore("Sound/SoundCore", soundCores, SNDCoreList, "sound");
		o.push_back(s);
	}

	o.push_back(heading("Video"));
	o.push_back(value(NULL, "--window-size", "<WxH>", "Size of the window, e.g. 1280x720.", doWindowSize));
	o.push_back(choice(NULL, "--aspect-ratio", "<MODE>", "Aspect ratio.", "Video/AspectRatio", aspectRatios));
	o.push_back(choice(NULL, "--resolution", "<MODE>", "Internal rendering resolution.", "Video/resolution_mode", resolutions, false));
	o.push_back(choice(NULL, "--filter", "<FILTER>", "Post-processing filter.", "Video/filter_type", filters));
	o.push_back(choice(NULL, "--upscale", "<MODE>", "Upscaling filter.", "Video/upscale_type", upscalers));
	o.push_back(choice(NULL, "--wireframe", "<MODE>", "Wireframe rendering (debug).", "Video/Wireframe", wireframes));
	o.push_back(choice(NULL, "--mesh", "<MODE>", "Mesh transparency mode.", "Video/MeshMode", originalImproved));
	o.push_back(choice(NULL, "--banding", "<MODE>", "Colour banding mode.", "Video/BandingMode", originalImproved));
	o.push_back(boolean("--vsync", "Vertical synchronization.", setBoolean("General/EnableVSync")));
	o.push_back(boolean("--show-fps", "Show the frame rate.", setBoolean("General/ShowFPS")));
	o.push_back(boolean("--autoframeskip", "Legacy option kept for compatibility, no effect in the current core (see --vsync).", setBoolean("General/EnableFrameSkipLimiter")));

	o.push_back(heading("Emulation"));
	{
		Option s = choice(NULL, "--sh2-core", "<CORE>", "SH-2 CPU core.", "Advanced/SH2Interpreter", sh2Cores);
		s.handler = setCore("Advanced/SH2Interpreter", sh2Cores, SH2CoreList, "SH-2");
		o.push_back(s);
		Option m = choice(NULL, "--m68k-core", "<CORE>", "68000 (sound CPU) core.", "Advanced/68kCore", m68kCores);
		m.handler = setCore("Advanced/68kCore", m68kCores, M68KCoreList, "68000");
		o.push_back(m);
	}
	o.push_back(boolean("--sh2-cache", "Emulate the SH-2 cache.", setBoolean("General/SH2Cache")));
	o.push_back(boolean("--clock-sync", "Synchronize the emulated clock to a fixed base time.", setBoolean("General/ClockSync")));
	o.push_back(value(NULL, "--base-time", "<DATE>", "Fixed base time, ISO 8601 (1996-01-01T12:00:00). Turns --clock-sync on.", doBaseTime));

	return o;
}

/* ---------- help ---------- */

QString wrap(const QString& text, int width, const QString& indent)
{
	QString out;
	QString line;
	foreach (const QString& word, text.split(' '))
	{
		if (!line.isEmpty() && line.length() + 1 + word.length() > width)
		{
			out += indent + line + "\n";
			line.clear();
		}
		line += (line.isEmpty() ? "" : " ") + word;
	}
	return out + indent + line + "\n";
}

QString syntax(const Option& o)
{
	QString s = o.longname;
	if (o.kind == Value) s += "=" + o.parameter;
	else if (o.kind == Bool) s += "[=" + o.parameter + "]";
	return s;
}

bool doHelp(const QString&, QString&)
{
	std::cout << "Kronos " << VERSION << " - Sega Saturn and ST-V emulator\n\n"
	          << "Usage: kronos [options] [disc_image]\n\n"
	          << "  disc_image   A file that is not an option is loaded as a disc image and\n"
	          << "               the emulation starts (same as -a -i <file>).\n\n"
	          << "A value is given as --option=value or --option value. Options with a list\n"
	          << "of values also accept the number stored in the settings file. On/off\n"
	          << "options accept 0, 1, on, off, true, false, yes, no; alone they mean on.\n"
	          << "Command line options override the saved settings for this run only.\n";

	const std::vector<Option>& options = buildOptions();
	for (size_t i = 0; i < options.size(); i++)
	{
		const Option& o = options[i];
		if (!o.longname)
		{
			std::cout << "\n" << o.description.toLocal8Bit().constData() << ":\n";
			continue;
		}
		if (o.description.isEmpty())
			continue;
		QString head = "  ";
		head += o.shortname ? QString(o.shortname) + ", " : QString("    ");
		head += syntax(o);
		std::cout << head.toLocal8Bit().constData() << "\n"
		          << wrap(o.description, 70, "        ").toLocal8Bit().constData();
	}
	std::cout << std::endl;
	exit(0);
}

bool doHelpMarkdown(const QString&, QString&)
{
	const std::vector<Option>& options = buildOptions();
	for (size_t i = 0; i < options.size(); i++)
	{
		const Option& o = options[i];
		if (!o.longname)
		{
			std::cout << "\n## " << o.description.toLocal8Bit().constData()
			          << "\n\n| Option | Description |\n|---|---|\n";
			continue;
		}
		if (o.description.isEmpty())
			continue;
		QString name = "`";
		name += o.shortname ? QString(o.shortname) + "`, `" : QString();
		name += syntax(o) + "`";
		std::cout << "| " << name.toLocal8Bit().constData() << " | "
		          << o.description.toLocal8Bit().constData() << " |\n";
	}
	exit(0);
}

struct Pending
{
	QString name;
	QString value;
	int priority;
	Handler handler;
};

void fail(const QString& name, const QString& value, const QString& error)
{
	std::cerr << "kronos: invalid " << name.toLocal8Bit().constData();
	if (!value.isNull())
		std::cerr << "='" << value.toLocal8Bit().constData() << "'";
	std::cerr << ": " << error.toLocal8Bit().constData() << "\nTry 'kronos --help'." << std::endl;
	exit(2);
}

} // namespace

void parse()
{
	const std::vector<Option>& options = buildOptions();
	const QStringList args = QCoreApplication::arguments();
	std::vector<Pending> pending;

	for (int i = 1; i < args.size(); i++)
	{
		const QString& arg = args[i];
		const Option* match = NULL;
		QString value;
		bool hasValue = false;

		if (arg.startsWith("-") && arg.size() > 1)
		{
			for (size_t k = 0; k < options.size() && !match; k++)
			{
				const Option& o = options[k];
				if (!o.longname)
					continue;
				if (o.shortname && arg == o.shortname)
					match = &o;
				for (int n = 0; n < 2 && !match; n++)
				{
					const char* name = n == 0 ? o.longname : o.alias;
					if (!name)
						continue;
					if (arg == name)
						match = &o;
					else if (arg.startsWith(QString(name) + "="))
					{
						match = &o;
						value = arg.mid((int)strlen(name) + 1);
						hasValue = true;
					}
				}
			}

			if (!match)
			{
				std::cerr << "kronos: ignoring unknown option '" << arg.toLocal8Bit().constData() << "'" << std::endl;
				continue;
			}
			if (match->kind == Value && !hasValue)
			{
				if (i + 1 >= args.size())
					fail(match->longname, QString(), "a value is required");
				value = args[++i];
			}
			if (match->kind == Flag && hasValue)
				fail(match->longname, QString(), "this option takes no value");

			Pending p = { match->longname, hasValue || match->kind == Value ? value : QString(), match->priority, match->handler };
			pending.push_back(p);
			continue;
		}

		/* Not an option: a disc image to load and start. */
		if (QFile::exists(arg))
		{
			Pending a = { "--autostart", QString(), PrioNormal, doAutostart };
			Pending d = { "disc image", arg, PrioNormal, doIso };
			pending.push_back(a);
			pending.push_back(d);
		}
		else
			std::cerr << "kronos: ignoring '" << arg.toLocal8Bit().constData() << "' (not an option, not an existing file)" << std::endl;
	}

	std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.priority < b.priority; });

	for (size_t i = 0; i < pending.size(); i++)
	{
		QString error;
		if (!pending[i].handler(pending[i].value, error))
			fail(pending[i].name, pending[i].value, error);
	}
}

}
