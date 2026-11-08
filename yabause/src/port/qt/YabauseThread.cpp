/*  Copyright 2005 Guillaume Duhamel
	Copyright 2005-2006, 2013 Theo Berkau
	Copyright 2008 Filipe Azevedo <pasnox@gmail.com>

	This file is part of Yabause.

	Yabause is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Yabause is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Yabause; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "YabauseThread.h"
#include "Settings.h"
#include "VolatileSettings.h"
#include "ui/UIPortManager.h"
#include "ui/UIYabause.h"
#include "YabauseGL.h"

#include "peripheral.h"
#include "sci.h"
#include "commfdd.h"
#include "memory.h"
#include "yui.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QDebug>

YabauseThread::YabauseThread( QObject* o )
	: QObject( o )
{
	mPause = true;
	mInit = -1;
	memset(&mYabauseConf, 0, sizeof(mYabauseConf));
	showFPS = false;
	mIsCdIn = false;
	mIsCDDirty = false;
}

YabauseThread::~YabauseThread()
{
	deInitEmulation();
}

yabauseinit_struct* YabauseThread::yabauseConf()
{
	return &mYabauseConf;
}

void YabauseThread::initEmulation()
{
	if ( mInit < 0 ) {
		reloadSettings();
		mInit = YabauseInit( &mYabauseConf );
		SetOSDToggle(showFPS);
		if ( mInit >= 0 )
			reloadCommPort();
		if ( mInit >= 0 )
			// reloadSettings() (via reloadControllers()) already wired the
			// saved bindings once, but that happened *before* YabauseInit()
			// actually loaded/identified the cartridge - for ST-V, that
			// means before yabsys.stvInputType was known (STV/STV6B vs.
			// PATOCAR/MICROMBC), which several hopper-cabinet key handlers
			// (Power Button, Hopper Sensor Test, etc.) branch on. Re-run it
			// now that the game is identified, so bindings are correct from
			// the very first frame instead of only after the player opens
			// Settings once and it gets rewired as a side effect.
			reloadControllers();
		if ( mInit >= 0 )
			loadCommandLineBinary();
	}
}

/* --binary FILE[:ADDRESS] (Arguments.cpp): the program replaces the boot of
   the disc. MappedMemoryLoadExec() resets the machine without loading a
   disc (YabauseResetNoLoad), puts it in the state left by the BIOS
   (YabauseSpeedySetup), copies the program and sets the master SH-2 PC:
   CPE entry point, or ADDRESS for a raw binary. Called after YabauseInit()
   and after each YabauseReset(), so a reset restarts the program like it
   would restart a disc. A failure drops the program (no error at each
   reset) and the machine keeps the normal boot. */
void YabauseThread::loadCommandLineBinary()
{
	VolatileSettings* vs = QtYabause::volatileSettings();
	const QString file = vs->value( "autorun/binary" ).toString();

	if ( file.isEmpty() || mInit < 0 || MSH2 == NULL )
		return;

	const u32 address = vs->value( "autorun/binary/address", 0x06004000u ).toUInt();
	u32 entry = 0;
	const int ret = MappedMemoryLoadExec( MSH2, QFile::encodeName( file ).constData(), address, 1, &entry );

	if ( ret != 0 )
	{
		QString reason;
		switch ( ret )
		{
			case -2: reason = QtYabause::translate( "not enough memory" ); break;
			case -3: reason = QtYabause::translate( "invalid or truncated CPE file" ); break;
			case -4: reason = QtYabause::translate( "the CPE file has no entry point" ); break;
			default: reason = QtYabause::translate( "can't read the file" ); break;
		}
		dropCommandLineBinary();
		emit error( QtYabause::translate( "Can't load the program %1: %2." ).arg( file, reason ), false );
		return;
	}

	YuiMsg( "Program %s loaded, PC = %08X\n", QFile::encodeName( file ).constData(), (unsigned)entry );
}

/* Another disc or file was opened: the program given with --binary no
   longer replaces its boot. */
void YabauseThread::dropCommandLineBinary()
{
	VolatileSettings* vs = QtYabause::volatileSettings();
	vs->removeValue( "autorun/binary" );
	vs->removeValue( "autorun/binary/address" );
}

/* Communication connector (back of the console, SH-2 SCI, sci.c): the
   Floppy Disk Drive when it is enabled in Settings > Cart/Memory
   (CommPort/FloppyEnabled) or with --fdd. The floppy is fdd_part0.bin /
   fdd_part1.bin in CommPort/FloppyFolder (default: <data folder>/floppy).
   An old .ini without CommPort/FloppyEnabled keeps the previous rule: the
   drive is plugged as soon as a folder is set. */
void YabauseThread::reloadCommPort()
{
	VolatileSettings* vs = QtYabause::volatileSettings();
	QString folder = vs->value( "CommPort/FloppyFolder" ).toString();
	const bool enabled = vs->value( "CommPort/FloppyEnabled", !folder.isEmpty() ).toBool();
	if ( enabled && folder.isEmpty() )
		folder = getDataDirPath() + "/floppy";
	if ( enabled )
	{
		QDir().mkpath( folder );
		FddSetFolder( QFile::encodeName( QDir( folder ).absolutePath() ).constData() );
		FddSetWriteProtect( vs->value( "CommPort/FloppyWriteProtect", false ).toBool() );
		SCISetCommPortDevice( FddGetDevice() );
	}
	else if ( SCIGetCommPortDevice() == FddGetDevice() )
	{
		FddSetFolder( NULL );
		SCISetCommPortDevice( NULL );
	}
}

void YabauseThread::deInitEmulation()
{
	YabauseDeInit();
	mInit = -1;
	/* Le coeur est arrete : plus aucun pointeur vers les chaines remplacees. */
	freeRetiredStrings();
}

bool YabauseThread::pauseEmulation( bool pause, bool reset )
{
	if ( mInit < 0 )
	{
		emit error( QtYabause::translate( "Can't Pause kronos while not initialized." ), false );
		return false;
	}

	mPause = pause;

	if (mIsCDDirty && mIsCdIn && !pause && (mYabauseConf.cdpath!= NULL) && (strlen(mYabauseConf.cdpath)>0)) {
		CloseTray();
		mIsCDDirty = false;
	}

	if ( mPause ) {
		ScspMuteAudio(SCSP_MUTE_SYSTEM);
	}
	else {
    resetSyncVideo();
		ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
	}

	VolatileSettings * vs = QtYabause::volatileSettings();

	if (vs->value("autostart").toBool())
	{
		if (vs->value("autorun/load").toBool()) {
			YabLoadStateSlot( QFile::encodeName( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString() ).constData(), vs->value("autorun/load/slot").toInt() );
		}
	}

	emit this->pause( mPause );

	return true;
}

bool YabauseThread::resetEmulation()
{
	if (yabsys.isReloadingImage == 2) {
		mInit = -1;
		dropCommandLineBinary();
	}
	yabsys.isReloadingImage = 0;
	if ( mInit < 0 ) {
		initEmulation();
	}

	if ( mInit < 0 )
	{
		emit error( QtYabause::translate( "Can't initialize Kronos." ), false );
		return false;
	}
	YabauseReset();
	loadCommandLineBinary();

	emit reset();

	return true;
}

void YabauseThread::reloadControllers()
{
	PerPortReset();
	QtYabause::clearPadsBits();

	Settings* settings = QtYabause::settings();

	emit toggleEmulateMouse( false, false );

	/* SBom Joycard turbo speed, common to every Joycard. */
	PerJoycardSetTurboRate( settings->value( SETTINGS_JOYCARD_TURBO_RATE, JOYCARD_TURBO_RATE_DEFAULT ).toInt() );

	for ( uint port = 1; port < 3; port++ )
	{
		settings->beginGroup( QString( "Input/Port/%1/Id" ).arg( port ) );
		QStringList ids = settings->childGroups();
		settings->endGroup();

		/* 6Player multitap: --multitap (volatile) first, then the setting of
		   the port. Plugged in before the controllers, which then go to its
		   connectors (PerMultitapAdd()). */
		{
			const QVariant mtOverride = QtYabause::volatileSettings()->value( QString( SETTINGS_OVERRIDE_MULTITAP ).arg( port ) );
			const bool multitap = mtOverride.isValid()
				? mtOverride.toBool()
				: settings->value( QString( SETTINGS_PORT_MULTITAP ).arg( port ), false ).toBool();
			if ( multitap && PerMultitapAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 ) != 0 )
				QtYabause::appendLog( "Can't plug in the multitap" );
		}

		/* --port1 / --port2 (Arguments.cpp): "Input/Override/PortN" holds one
		   controller type per slot, e.g. "2,2,22". It replaces the controllers
		   saved in the settings for that port; the key bindings are still the
		   ones saved in the settings for the slot and the type. */
		const QVariant portOverride = QtYabause::volatileSettings()->value( QString( "Input/Override/Port%1" ).arg( port ) );
		QStringList overrideTypes;
		if ( portOverride.isValid() )
		{
			overrideTypes = portOverride.toString().split( ',' );
			ids.clear();
			for ( int slot = 1; slot <= overrideTypes.size(); slot++ )
				ids << QString::number( slot );
		}

		ids.sort();
		foreach ( const QString& id, ids )
		{
			uint type = portOverride.isValid()
				? overrideTypes.at( id.toInt() - 1 ).toUInt()
				: settings->value( QString( UIPortManager::mSettingsType ).arg( port ).arg( id ) ).toUInt();
			switch ( type )
			{
                                case PERCABINET:
                                {
                                  PerCab_struct* padbits = PerCabAdd( NULL );
                                  // Mahjong Panel games (kiwames/vmahjong/myfairld) store their
                                  // PERMAHJONG_* bindings under this same PERCABINET bucket
                                  // (see UIMahjongSetting) - connect the panel too and route
                                  // each key to whichever controller it actually belongs to.
                                  // (PERMAHJONG_FIRST, not PERMAHJONG_A, is the true lower
                                  // bound of that id range - see peripheral.h.)
                                  const bool isMahjong = ( yabsys.stvInputType == STVMP || yabsys.stvInputType == VMAHJONG || yabsys.stvInputType == MYFAIRLD );
                                  PerMahjongPanel_struct* mjbits = isMahjong ? PerMahjongAdd( NULL ) : NULL;
                                  settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
                                  QStringList padKeys = settings->childKeys();
				  settings->endGroup();
				  padKeys.sort();
                                  foreach ( const QString& padKey, padKeys )
				  {
				    const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( padKey ) ).toString();
				    void* controller = ( isMahjong && padKey.toUInt() >= PERMAHJONG_FIRST ) ? (void*)mjbits : (void*)padbits;
				    PerSetKey( key.toUInt(), padKey.toUInt(), controller );
				  }
                                  break;
                                }
				case PERPAD:
				{
					PerPad_struct* padbits = PerPadAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList padKeys = settings->childKeys();
					settings->endGroup();

					padKeys.sort();
					foreach ( const QString& padKey, padKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( padKey ) ).toString();

						PerSetKey( key.toUInt(), padKey.toUInt(), padbits );
					}
					break;
				}
				case PERPAD_TWINSTICK:
				{
					// Twin Stick (STN-50): a standard pad, its own bindings.
					PerPad_struct* padbits = PerPadAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );
					if ( !padbits )
					{
						QtYabause::appendLog( "Can't plug in the Twin Stick" );
						break;
					}

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList padKeys = settings->childKeys();
					settings->endGroup();

					foreach ( const QString& padKey, padKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( padKey ) ).toString();
						PerSetKey( key.toUInt(), padKey.toUInt(), padbits );
					}
					break;
				}
				case PERPAD_JOYCARD:
				{
					// SBom Joycard: a standard pad driven by the Joycard state.
					PerJoycard_struct* jc = PerJoycardAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );
					if ( !jc )
					{
						QtYabause::appendLog( "Can't plug in the SBom Joycard" );
						break;
					}

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList padKeys = settings->childKeys();
					settings->endGroup();

					foreach ( const QString& padKey, padKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( padKey ) ).toString();
						PerSetKey( key.toUInt(), padKey.toUInt(), jc );
					}
					break;
				}
				case PERWHEEL:
            {
               PerAnalog_struct* analogbits = PerWheelAdd(port == 1 ? &PORTDATA1 : &PORTDATA2);

               settings->beginGroup(QString("Input/Port/%1/Id/%2/Controller/%3/Key").arg(port).arg(id).arg(type));
               QStringList analogKeys = settings->childKeys();
               settings->endGroup();

               analogKeys.sort();
               foreach(const QString& analogKey, analogKeys)
               {
                  const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(analogKey)).toString();

                  PerSetKey(key.toUInt(), analogKey.toUInt(), analogbits);
               }
               break;
            }
            case PERMISSIONSTICK:
            {
               PerAnalog_struct* analogbits = PerMissionStickAdd(port == 1 ? &PORTDATA1 : &PORTDATA2);

               settings->beginGroup(QString("Input/Port/%1/Id/%2/Controller/%3/Key").arg(port).arg(id).arg(type));
               QStringList analogKeys = settings->childKeys();
               settings->endGroup();

               analogKeys.sort();
               foreach(const QString& analogKey, analogKeys)
               {
                  const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(analogKey)).toString();

                  PerSetKey(key.toUInt(), analogKey.toUInt(), analogbits);
               }
               break;
            }
            case PERTWINSTICKS:
            {
               PerAnalog_struct* analogbits = PerTwinSticksAdd(port == 1 ? &PORTDATA1 : &PORTDATA2);

               settings->beginGroup(QString("Input/Port/%1/Id/%2/Controller/%3/Key").arg(port).arg(id).arg(type));
               QStringList analogKeys = settings->childKeys();
               settings->endGroup();

               analogKeys.sort();
               foreach(const QString& analogKey, analogKeys)
               {
                  const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(analogKey)).toString();

                  PerSetKey(key.toUInt(), analogKey.toUInt(), analogbits);
               }
               break;
            }
				case PER3DPAD:
				{
					PerAnalog_struct* analogbits = Per3DPadAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList analogKeys = settings->childKeys();
					settings->endGroup();

					analogKeys.sort();
					foreach ( const QString& analogKey, analogKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( analogKey ) ).toString();

						PerSetKey( key.toUInt(), analogKey.toUInt(), analogbits );
					}
					break;
				}
				case PERGUN:
				{
					PerGun_struct* gunbits = PerGunAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );
					if ( !gunbits )
						QtYabause::appendLog( "Can't plug in the gun: it must be alone on its port, without multitap" );
					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList gunKeys = settings->childKeys();
					settings->endGroup();

					gunKeys.sort();
					foreach ( const QString& gunKey, gunKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( gunKey ) ).toString();

						PerSetKey( key.toUInt(), gunKey.toUInt(), gunbits );
					}
					emit toggleEmulateMouse( true, true );
					break;
				}
				case PERKEYBOARD:
				case PERKEYBOARD_NETLINK:
				{
					// No key bindings: the host keyboard is fed as a whole by
					// KeyboardQt (UIYabause key events), by key position. The
					// model only decides which keys exist (PERKB_MODEL_*).
					const int model = ( type == PERKEYBOARD_NETLINK ) ? PERKB_MODEL_NETLINK_US : PERKB_MODEL_SATURN_JP;
					if ( !PerKeyboardAddModel( port == 1 ? &PORTDATA1 : &PORTDATA2, model ) )
						QtYabause::appendLog( "Can't plug in the keyboard" );
					break;
				}
				case PERMOUSE:
				{
					PerMouse_struct* mousebits = PerMouseAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList mouseKeys = settings->childKeys();
					settings->endGroup();

					mouseKeys.sort();
					foreach ( const QString& mouseKey, mouseKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( mouseKey ) ).toString();

						PerSetKey( key.toUInt(), mouseKey.toUInt(), mousebits );
					}

					emit toggleEmulateMouse( true, false );
					break;
				}
				case 0:
					// Unconnected
					break;
				default:
					QtYabause::appendLog( "Invalid controller type" );
					break;
			}
		}
	}
}

static struct tm *localtime_qt(const QDateTime &input, struct tm *result)
{
	QDate date(input.date());
	result->tm_year = date.year() - 1900;
	result->tm_mon = date.month();
	result->tm_mday = date.day();
	result->tm_wday = date.dayOfWeek();
	result->tm_yday = date.dayOfYear();

	QTime time(input.time());
	result->tm_sec = time.second();
	result->tm_min = time.minute();
	result->tm_hour = time.hour();

	return result;
}

void YabauseThread::reloadClock()
{
	QString tmp;
	/* Volatile settings fall back to the saved ones, and let --clock-sync /
	   --base-time override them for this run. */
	VolatileSettings* s = QtYabause::volatileSettings();

	if (mYabauseConf.basetime == 0)
		tmp = "";
	else
		tmp = QDateTime::fromTime_t(mYabauseConf.basetime).toString();

	// Clock sync
	mYabauseConf.clocksync = (int)s->value( "General/ClockSync", mYabauseConf.clocksync ).toBool();
	tmp = s->value( "General/FixedBaseTime", tmp ).toString();
	if (!tmp.isEmpty() && mYabauseConf.clocksync)
	{
		QDateTime date = QDateTime::fromString(tmp, Qt::ISODate);
		mYabauseConf.basetime = (long)date.toTime_t();
	}
	else {
		mYabauseConf.basetime = 0;
	}
}

/* Chaines de mYabauseConf.
 *
 * reloadSettings() refaisait un strdup() de chaque chemin a chaque appel sans
 * jamais liberer l'ancien (resetYabauseConf() les effacait par memset) : une
 * fuite a chaque rechargement des reglages. On ne peut pas non plus les
 * liberer tout de suite : le coeur garde certains pointeurs tant qu'il tourne
 * (CartridgeArea->filename = cartpath, smpcfilename = smpcpath, eepromdir...).
 *
 * confString() reutilise la chaine precedente quand la valeur n'a pas change
 * (cas normal : plus aucune allocation) ; une chaine remplacee est mise de
 * cote et liberee seulement quand le coeur est arrete (deInitEmulation()). */
const char *YabauseThread::confString( const char *previous, const QByteArray &value )
{
	if ( previous && ( strcmp( previous, value.constData() ) == 0 ) )
		return previous;
	return strdup( value.constData() );
}

/* Chemin de fichier pour fopen() du coeur : encodage du systeme de fichiers
   (page de code ANSI sous Windows). toLatin1() remplacait par '?' tout
   caractere hors Latin-1 (dossier utilisateur, nom de jeu...), et le fichier
   n'etait plus trouve. */
QByteArray YabauseThread::pathBytes( const QVariant &value )
{
	return QFile::encodeName( value.toString() );
}

void YabauseThread::retireConfStrings( const yabauseinit_struct &previous )
{
	const char * const before[] = {
		previous.stvgame, previous.biospath, previous.cdpath, previous.buppath,
		previous.smpcpath, previous.mpegpath, previous.cartpath, previous.eepromdir,
		previous.modemip, previous.modemport };
	const char * const after[] = {
		mYabauseConf.stvgame, mYabauseConf.biospath, mYabauseConf.cdpath, mYabauseConf.buppath,
		mYabauseConf.smpcpath, mYabauseConf.mpegpath, mYabauseConf.cartpath, mYabauseConf.eepromdir,
		mYabauseConf.modemip, mYabauseConf.modemport };
	for ( unsigned i = 0; i < sizeof( before ) / sizeof( before[0] ); i++ )
	{
		bool used = false;
		if ( before[i] == NULL )
			continue;
		for ( unsigned j = 0; j < sizeof( after ) / sizeof( after[0] ); j++ )
			if ( after[j] == before[i] )
				used = true;
		if ( !used && !mRetiredStrings.contains( (char *)before[i] ) )
			mRetiredStrings.append( (char *)before[i] );
	}
}

void YabauseThread::freeRetiredStrings()
{
	foreach ( char *str, mRetiredStrings )
		free( str );
	mRetiredStrings.clear();
}

void YabauseThread::reloadSettings()
{
	//QMutexLocker l( &mMutex );
	// get settings pointer
	VolatileSettings* vs = QtYabause::volatileSettings();

	/* Chaines actuelles : reutilisees si leur valeur ne change pas, voir
	   confString(). */
	const yabauseinit_struct prev = mYabauseConf;

	// reset yabause conf
	resetYabauseConf();

	// read & apply settings
	mYabauseConf.m68kcoretype = vs->value("Advanced/68kCore", mYabauseConf.m68kcoretype).toInt();
	mYabauseConf.percoretype = vs->value( "Input/PerCore", mYabauseConf.percoretype ).toInt();
	mYabauseConf.sh2coretype = vs->value( "Advanced/SH2Interpreter", mYabauseConf.sh2coretype ).toInt();
	mYabauseConf.vidcoretype = vs->value( "Video/VideoCore", mYabauseConf.vidcoretype ).toInt();
	if (mYabauseConf.vidcoretype != VIDCORE_CS) {
		//Force vidcore to be CS. It is the only version supported now
		mYabauseConf.vidcoretype = VIDCORE_CS;
	}
	mYabauseConf.osdcoretype = vs->value( "Video/OSDCore", mYabauseConf.osdcoretype ).toInt();
	mYabauseConf.sndcoretype = vs->value( "Sound/SoundCore", mYabauseConf.sndcoretype ).toInt();
	mYabauseConf.cdcoretype = vs->value( "General/CdRom", mYabauseConf.cdcoretype ).toInt();
	mYabauseConf.carttype = vs->value( "Cartridge/Type", mYabauseConf.carttype ).toInt();
	if (mYabauseConf.carttype == CART_ROMSTV) mYabauseConf.cdcoretype = CDCORE_DUMMY;
	mYabauseConf.stvgame = confString( prev.stvgame, vs->value( "Cartridge/STVGame" ).toString().toLatin1() );
	/* Saturn console region (Settings > General > Console region). "Auto"
	   or empty: the first area symbol of the disc, as before. Otherwise the
	   letter of the area symbol (ST-040-R4 table 4.3), turned into the SMPC
	   region code. */
	mYabauseConf.regionid = 0;
	{
		const QString cr = vs->value( "General/ConsoleRegion", "Auto" ).toString();
		if ( !cr.isEmpty() && cr != "Auto" )
		{
			switch ( cr[0].toLatin1() )
			{
				case 'J': mYabauseConf.regionid = REGION_JAPAN; break;
				case 'T': mYabauseConf.regionid = REGION_ASIANTSC; break;
				case 'U': mYabauseConf.regionid = REGION_NORTHAMERICA; break;
				case 'B': mYabauseConf.regionid = REGION_CENTRALSOUTHAMERICANTSC; break;
				case 'K': mYabauseConf.regionid = REGION_KOREA; break;
				case 'A': mYabauseConf.regionid = REGION_ASIAPAL; break;
				case 'E': mYabauseConf.regionid = REGION_EUROPE; break;
				case 'L': mYabauseConf.regionid = REGION_CENTRALSOUTHAMERICAPAL; break;
				default : mYabauseConf.regionid = 0; break;
			}
		}
	}
	const QString r = vs->value( "STV/Region", "Auto" ).toString();
	if ( r.isEmpty() || r == "Auto" )
		mYabauseConf.stv_favorite_region = 1;
	else
	{
		switch ( r[0].toLatin1() )
		{
			case 'E': mYabauseConf.stv_favorite_region = 1; break;
			case 'U': mYabauseConf.stv_favorite_region = 2; break;
			case 'J': mYabauseConf.stv_favorite_region = 4; break;
			case 'T': mYabauseConf.stv_favorite_region = 8; break;
			default : mYabauseConf.stv_favorite_region = 1; break;
		}
	}
	const QString r1 = vs->value( "General/SystemLanguageID", mYabauseConf.languageid ).toString();
	if ( r1.isEmpty() || r1 == "" )
		mYabauseConf.languageid = 0;
	else
	{
		switch ( r1[0].toLatin1() )
		{
			case '0': mYabauseConf.languageid = 0; break;
			case '1': mYabauseConf.languageid = 1; break;
			case '2': mYabauseConf.languageid = 2; break;
			case '3': mYabauseConf.languageid = 3; break;
			case '4': mYabauseConf.languageid = 4; break;
			case '5': mYabauseConf.languageid = 5; break;
		}
	}
	if (vs->value("General/EnableEmulatedBios", false).toBool())
		mYabauseConf.biospath = confString( prev.biospath, QByteArray( "" ) );
	else
		mYabauseConf.biospath = confString( prev.biospath, pathBytes( vs->value( "General/Bios" ) ) );

	mYabauseConf.smpcpath = confString( prev.smpcpath, pathBytes( vs->value( "General/BiosSettings" ) ) );
	mYabauseConf.cdpath = confString( prev.cdpath, pathBytes( vs->value( "General/CdRomISO" ) ) );
	QtYabause::updateTitle();
	showFPS = vs->value( "General/ShowFPS", false ).toBool();
	mYabauseConf.vsyncon = vs->value("General/EnableVSync", true).toBool();
	mYabauseConf.usecache = vs->value("General/SH2Cache", false).toBool();
	{
		/* Meme valeur par defaut que la fenetre de reglages (UISettings.cpp,
		   "Memory/Path" = <donnees>/bkram.bin). Ici la valeur par defaut etait
		   mYabauseConf.buppath, NULL apres resetYabauseConf() : tant que les
		   reglages n'avaient jamais ete valides, le chemin etait vide, la RAM
		   de sauvegarde etait formatee a chaque lancement et jamais ecrite
		   (T123Save() ignore un nom vide) - toutes les sauvegardes perdues a
		   la fermeture. */
		QString bupPath = vs->value( "Memory/Path" ).toString();
		if ( bupPath.isEmpty() )
			bupPath = getDataDirPath().append( "/bkram.bin" );
		QDir().mkpath( QFileInfo( bupPath ).absolutePath() );
		/* QFile::encodeName : encodage du systeme de fichiers (toLatin1()
		   rendait '?' pour un caractere hors Latin-1 dans le chemin, par
		   exemple dans le nom d'utilisateur Windows). */
		mYabauseConf.buppath = confString( prev.buppath, QFile::encodeName( bupPath ) );
	}
	mYabauseConf.mpegpath = confString( prev.mpegpath, pathBytes( vs->value( "MpegROM/Path" ) ) );
	if (vs->value("Memory/ExtendMemory", true).toBool()) {
		mYabauseConf.extend_backup = 1;
	} else {
		mYabauseConf.extend_backup = 0;
	}
	mYabauseConf.cartpath = confString( prev.cartpath, pathBytes( vs->value( "Cartridge/Path" ) ) );
	mYabauseConf.eepromdir = confString( prev.eepromdir, QByteArray( mYabauseConf.cartpath ) );
	mYabauseConf.modemip = confString( prev.modemip, vs->value( "Cartridge/ModemIP" ).toString().toLatin1() );
	mYabauseConf.modemport = confString( prev.modemport, vs->value( "Cartridge/ModemPort" ).toString().toLatin1() );

	mYabauseConf.video_filter_type = vs->value("Video/filter_type", mYabauseConf.video_filter_type).toInt();
	mYabauseConf.video_upscale_type = vs->value("Video/upscale_type", mYabauseConf.video_upscale_type).toInt();
	mYabauseConf.polygon_generation_mode = vs->value("Video/polygon_generation_mode", mYabauseConf.polygon_generation_mode).toInt();
	mYabauseConf.resolution_mode = vs->value("Video/resolution_mode", mYabauseConf.resolution_mode).toInt();
	mYabauseConf.stretch = vs->value("Video/AspectRatio", mYabauseConf.stretch).toInt();
	mYabauseConf.wireframe_mode = vs->value("Video/Wireframe", mYabauseConf.wireframe_mode).toInt();
	mYabauseConf.meshmode = vs->value("Video/MeshMode", mYabauseConf.meshmode).toInt();
	mYabauseConf.bandingmode = vs->value("Video/BandingMode", mYabauseConf.bandingmode).toInt();

	// TODO : expose this in qt ui
	mYabauseConf.auto_cart = 1;

	emit requestSize( QSize( vs->value( "Video/WinWidth", 0 ).toInt(), vs->value( "Video/WinHeight", 0 ).toInt() ) );
	// emit requestFullscreen( vs->value( "Video/Fullscreen", false ).toBool() );
	emit requestVolumeChange( vs->value( "Sound/Volume", 100 ).toInt() );

	/* Chaines de l'ancienne configuration qui ne sont plus utilisees. */
	retireConfStrings( prev );

	reloadClock();
	reloadControllers();
}

bool YabauseThread::emulationRunning()
{
	//QMutexLocker l( &mMutex );
	return  !mPause;
}

bool YabauseThread::emulationPaused()
{
	//QMutexLocker l( &mMutex );
	return mPause;
}

void YabauseThread::OpenTray(){
	Cs2ForceOpenTray();
}

int YabauseThread::CloseTray(){
	VolatileSettings* vs = QtYabause::volatileSettings();
	mYabauseConf.cdcoretype = vs->value("General/CdRom", mYabauseConf.cdcoretype).toInt();
	{
		const char *previous = mYabauseConf.cdpath;
		mYabauseConf.cdpath = confString( previous, pathBytes( vs->value( "General/CdRomISO" ) ) );
		if ( previous && ( previous != mYabauseConf.cdpath ) && !mRetiredStrings.contains( (char *)previous ) )
			mRetiredStrings.append( (char *)previous );
	}

	return (Cs2ForceCloseTray(mYabauseConf.cdcoretype, mYabauseConf.cdpath)==0);
}

void YabauseThread::resetYabauseConf()
{
	// free structure
	memset( &mYabauseConf, 0, sizeof( yabauseinit_struct ) );
	// fill default structure
	/* Same default as the settings dialog (UISettings: default68kCore()).
	   M68KCORE_C68K is not in M68KCoreList[] of the Qt port (Dummy and
	   Musashi only), and M68KInit() silently falls back to the Dummy core
	   for an unknown id: without a saved "Advanced/68kCore" the sound CPU
	   never ran, and games that wait for their sound driver hung (Destruction
	   Derby 2 prototype: the master waits at 06004F0C for the driver's first
	   sound request). */
	mYabauseConf.m68kcoretype = QtYabause::default68kCore().id;
	mYabauseConf.percoretype = QtYabause::defaultPERCore().id;
	mYabauseConf.sh2coretype = SH2CORE_DEFAULT;
	mYabauseConf.vidcoretype = QtYabause::defaultVIDCore().id;
	mYabauseConf.sndcoretype = QtYabause::defaultSNDCore().id;
	mYabauseConf.cdcoretype = QtYabause::defaultCDCore().id;
	mYabauseConf.carttype = CART_NONE;
	mYabauseConf.regionid = 0;
	mYabauseConf.languageid = 0;
	mYabauseConf.biospath = 0;
	mYabauseConf.smpcpath = 0;
	mYabauseConf.cdpath = 0;
	mYabauseConf.buppath = 0;
	mYabauseConf.mpegpath = 0;
	mYabauseConf.cartpath = 0;
  mYabauseConf.stvgame = 0;
	mYabauseConf.skip_load = 0;
	int numThreads = QThread::idealThreadCount();
	mYabauseConf.usethreads = numThreads <= 1 ? 0 : 1;
	mYabauseConf.numthreads = numThreads < 0 ? 1 : numThreads;
	mYabauseConf.video_filter_type = 0;
	mYabauseConf.video_upscale_type = 0;
	mYabauseConf.polygon_generation_mode = 0;
        mYabauseConf.resolution_mode = 1;
        mYabauseConf.stretch = 0;
}
