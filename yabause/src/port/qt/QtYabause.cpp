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
#include "QtYabause.h"
#include "ui/UIYabause.h"
#include "Settings.h"
#include "VolatileSettings.h"

#include <QApplication>
#include <QLabel>
#include <QGroupBox>
#include <QTreeWidget>
#include <QTableWidget>
#include <QComboBox>
#include <QTabWidget>
#include <QToolBox>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QAbstractButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QPointer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QCoreApplication>
#include <QTranslator>
#include <QLibraryInfo>

#ifndef ARCH_IS_WINDOWS
#include <sys/time.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>

// cores

#ifdef Q_OS_WIN
extern CDInterface SPTICD;
#endif

extern "C" {
M68K_struct * M68KCoreList[] = {
&M68KDummy,
&M68KMusashi,
NULL
};

SH2Interface_struct *SH2CoreList[] = {
&SH2KronosInterpreter,
&SH2KronosDebugInterpreter,
NULL
};

PerInterface_struct *PERCoreList[] = {
&PERDummy,
&PERQT,
#ifdef HAVE_LIBSDL
&PERSDLJoy,
#endif
#ifdef __APPLE__
&PERMacJoy,
#endif
#ifdef ARCH_IS_LINUX
&PERLinuxJoy,
#endif
#if defined HAVE_DIRECTINPUT
&PERDIRECTX,
#endif
NULL
};

CDInterface *CDCoreList[] = {
&DummyCD,
&ISOCD,
#ifndef UNKNOWN_ARCH
&ArchCD,
#endif
NULL
};

SoundInterface_struct *SNDCoreList[] = {
&SNDDummy,
#ifdef HAVE_LIBSDL
&SNDSDL,
#endif
#ifdef HAVE_LIBAL
&SNDAL,
#endif
#ifdef ARCH_IS_MACOSX
&SNDMac,
#endif
#if defined HAVE_DIRECTSOUND
&SNDDIRECTX,
#endif
NULL
};

VideoInterface_struct *VIDCoreList[] = {
#ifdef HAVE_LIBGL
&VIDCS,
#endif
#ifdef USE_SOFT_RENDER
&VIDSoft,
#endif
NULL
};

#ifdef YAB_PORT_OSD
#include "nanovg_osdcore.h"
OSD_struct *OSDCoreList[] = {
#ifdef HAVE_LIBGL
&OSDNnovg,
#endif
&OSDDummy,
NULL
};
#endif

}

// main window
QPointer<UIYabause> mUIYabause = 0;
// settings object
QPointer<Settings> mSettings = 0;
QPointer<VolatileSettings> mVolatileSettings = 0;
// ports padbits
QMap<uint, PerPad_struct*> mPort1PadsBits;
QMap<uint, PerPad_struct*> mPort2PadsBits;
QMap<uint, PerCab_struct*> mPortIOBits;
QMap<uint, PerGun_struct*> mPort1GunBits;
QMap<uint, PerGun_struct*> mPort2GunBits;
QMap<uint, PerMouse_struct*> mPort1MouseBits;
QMap<uint, PerMouse_struct*> mPort2MouseBits;
QMap<uint, PerAnalog_struct*> mPort1AnalogBits;
QMap<uint, PerAnalog_struct*> mPort2AnalogBits;

/* On Windows YuiMsg goes to OutputDebugString and to the in-app log dock, so
   there is no stdout to redirect and no practical way to capture a diagnostic
   run. Setting KRONOS_LOG_FILE to a path mirrors every message to that file,
   flushed on each line so a crash or a hang still leaves a usable log. */
static FILE *kronosLogFile = NULL;
static int   kronosLogTried = 0;

static void kronosLogWrite(const char *s)
{
   if (!kronosLogTried)
   {
      const char *path = getenv("KRONOS_LOG_FILE");
      kronosLogTried = 1;
      if (path && *path)
         kronosLogFile = fopen(path, "w");
   }
   if (kronosLogFile)
   {
      fputs(s, kronosLogFile);
      fflush(kronosLogFile);
   }
}

extern "C"
{

#ifdef WIN32
#ifndef vasprintf
		int vasprintf(char **strp, const char *fmt, va_list ap) {
			/* Was hardcoded to 512 bytes, so anything longer came out cut in
			   half: a register dump barely fits and a hang report does not.
			   Size the buffer from the format instead. */
			va_list ap2;
			va_copy(ap2, ap);
			int len = _vscprintf(fmt, ap2);
			va_end(ap2);
			if (len < 0) return -1;
			char *str = (char*)malloc((size_t)len + 1);
			if (str == NULL) return -1;
			int r = vsnprintf_s(str, (size_t)len + 1, _TRUNCATE, fmt, ap); /* "secure" version of vsprintf */
			if (r == -1) return free(str), -1;
			*strp = str;
			return r;
		}
#endif
        void YuiMsg(const char *format, ...) {
          int r;
	  char *str=NULL;
          va_list arglist;
          va_start( arglist, format );
          r = vasprintf(&str, format, arglist);
	  va_end( arglist );
	  if (r > 0) {
            //QtYabause::mainWindow()->appendLog( str );
		  /* The wide buffer used to be a fixed wchar_t[512] fed with
		     strlen(str) + 1, which only stayed inside its bounds because
		     vasprintf truncated at 512. Size it from the string. */
		  size_t wlen = strlen(str) + 1;
		  wchar_t *wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
		  if (wtext != NULL) {
		    mbstowcs(wtext, str, wlen);//Plus null
		    ::OutputDebugString(wtext);
		    free(wtext);
		  }
			kronosLogWrite( str );
			if (mUIYabause) mUIYabause->appendLog( str );
	      free(str);
	  }
       }
#else
	void YuiMsg(const char *format, ...) {
		char dest[512];
		va_list argptr;
		va_start(argptr, format);
		vsnprintf(dest, 512, format, argptr);
		va_end(argptr);
		printf("%s", dest);
		fflush(stdout);
		kronosLogWrite( dest );
		if (mUIYabause) mUIYabause->appendLog( dest );
}

#endif

       void YuiErrorMsg(const char *error_text)
       {
				 emit mUIYabause->mYabauseThread->error( error_text, false );
         YuiMsg("Error: %s\n", error_text);
       }

  void YuiEndOfFrame()
	{

	}
	void YuiSwapBuffers()
	{
          QtYabause::mainWindow()->swapBuffers();
        }

	int YuiGetFB()
	{
          return 0;
        }

#if defined(HAVE_DIRECTINPUT) || defined(HAVE_DIRECTSOUND)
	HWND DXGetWindow()
	{
		return (HWND)mUIYabause->winId();
	}
#endif
}

void QtYabause::appendLog( const char* str )
{
#ifdef _WIN32
  wchar_t wtext[512];
  mbstowcs(wtext, str, strlen(str) + 1);//Plus null
  LPWSTR ptr = wtext;
	::OutputDebugString(ptr);
#else
  printf("%s\n", str);
#endif
	mUIYabause->appendLog(str);
}


UIYabause* QtYabause::mainWindow( bool create )
{
        qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "1");
	if ( !mUIYabause && create )
		mUIYabause = new UIYabause;
	return mUIYabause;
}

void QtYabause::updateTitle() {
VolatileSettings* vs = QtYabause::volatileSettings();
QString name;
if (vs->value( "Cartridge/Type", 0 ).toInt() == CART_ROMSTV) { //STV ROM
	name = QString("STV: %1").arg(vs->value("Cartridge/STVGameName").toString());
} else {
	QString filename = vs->value("General/CdRomISO").toString();
	if (filename != NULL) {
		QString core = QString("CDROM: %1");
		if (vs->value( "General/CdRom", CDCORE_ISO ).toInt() == CDCORE_ISO) {
			core = QString("ISO: %1");
		}
		QFileInfo fi(filename);
		if (fi.exists()) {
			name = core.arg(fi.fileName());
		}
	}
}

	QString title( "Kronos v%1" );
	title=title.arg( VERSION );
	title.append(" ").append(name);

	mainWindow()->setWindowTitle(title);
}

Settings* QtYabause::settings( bool create )
{
	if ( !mSettings && create )
		mSettings = new Settings();
	return mSettings;
}

VolatileSettings* QtYabause::volatileSettings( bool create )
{
	if ( !mVolatileSettings && create )
		mVolatileSettings = new VolatileSettings();
	return mVolatileSettings;
}

// ------------------------------------------------------------------------
//  Translations (mini18n .yts files)
// ------------------------------------------------------------------------

/* Folder holding the .yts files. On Windows YTSDIR is the relative path
   "trans": it used to be resolved against the current working directory, so
   the translations were not found when Kronos was started from a shortcut or
   a launcher with another working directory. A relative YTSDIR is now
   resolved against the folder of the executable. */
QString QtYabause::translationDirectory()
{
	const QString dir = QString::fromUtf8( YTSDIR );
	if ( QDir::isAbsolutePath( dir ) )
		return QDir::cleanPath( dir );
	return QDir::cleanPath( QDir( QCoreApplication::applicationDirPath() ).filePath( dir ) );
}

QList <translation_struct> QtYabause::getTranslationList()
{
	QList <translation_struct> translations;
#ifdef HAVE_LIBMINI18N
	const QDir transDir( translationDirectory(), "*.yts", QDir::Name, QDir::Files );

	foreach ( const QString& file, transDir.entryList() )
	{
		// skip the files written by the "log untranslated strings" option
		if ( file.endsWith( "_log.yts" ) )
			continue;

		translation_struct trans;
		trans.file = transDir.filePath( file );

		/* Reduce the file name to a locale code. Installed files are named
		   "fr.yts", "pt_BR.yts"..., but a build tree or a manual copy uses the
		   source names "kronos_fr.yts" (and older packages "yabause_fr.yts").
		   QString::remove() works in place: the previous code tested the
		   prefix on the already modified name and only knew "yabause_", so
		   "kronos_pt_BR" became an invalid locale and was listed as "C". */
		QString localeStr = QFileInfo( file ).completeBaseName();
		if ( localeStr.startsWith( "kronos_" ) )
			localeStr.remove( 0, 7 );
		else if ( localeStr.startsWith( "yabause_" ) )
			localeStr.remove( 0, 8 );
		if ( localeStr == "default" )
			continue; // template with empty values, not a translation

		const QLocale locale( localeStr );
		if ( locale.language() == QLocale::C )
		{
			trans.name = localeStr;
		}
		else
		{
			trans.name = locale.nativeLanguageName();
			if ( trans.name.isEmpty() )
				trans.name = QLocale::languageToString( locale.language() );
			// pt / pt_BR, zh_CN / zh_TW: show the country so both can be told apart
			if ( localeStr.contains( '_' ) )
			{
				QString country = locale.nativeCountryName();
				if ( country.isEmpty() )
					country = QLocale::countryToString( locale.country() );
				trans.name += QString( " (%1)" ).arg( country );
			}
			if ( !trans.name.isEmpty() )
				trans.name = trans.name.left( 1 ).toUpper() + trans.name.mid( 1 );
		}
		translations.append( trans );
	}
#endif
	return translations;
}

#ifdef HAVE_LIBMINI18N
static bool loadTranslationFile( const QString& path )
{
	if ( path.isEmpty() || !QFileInfo( path ).isFile() )
		return false;
	const QByteArray encoded = QFile::encodeName( path );
	return mini18n_set_locale( encoded.constData() ) == 0;
}

// "kronos_pt_BR.yts" / "pt_BR.yts" -> "pt_BR"
static QString localeOfTranslationFile( const QString& path )
{
	QString name = QFileInfo( path ).completeBaseName();
	if ( name.startsWith( "kronos_" ) )
		name.remove( 0, 7 );
	else if ( name.startsWith( "yabause_" ) )
		name.remove( 0, 8 );
	return name;
}

/* The .yts files only cover Kronos' own strings. The texts drawn by Qt itself
   (QMessageBox / QDialogButtonBox buttons "Yes", "No", "Cancel", "Abort",
   "Ignore", the Qt file and input dialogs...) need Qt's own catalog, which was
   never loaded: those buttons stayed in English in every language. Load
   qtbase_<lang>.qm from the Qt installation or from a "translations" folder
   next to the executable (where windeployqt puts it). An empty locale
   removes it (English). */
static void setQtTranslator( const QString& locale )
{
	static QTranslator* qtTranslator = NULL;
	if ( qtTranslator )
	{
		QCoreApplication::removeTranslator( qtTranslator );
		delete qtTranslator;
		qtTranslator = NULL;
	}
	if ( locale.isEmpty() || locale.startsWith( "en" ) )
		return;

	const QStringList dirs = QStringList()
		<< QDir( QCoreApplication::applicationDirPath() ).filePath( "translations" )
		<< QLibraryInfo::location( QLibraryInfo::TranslationsPath );
	QStringList names;
	names << "qtbase_" + locale << "qtbase_" + locale.section( '_', 0, 0 )
		<< "qt_" + locale << "qt_" + locale.section( '_', 0, 0 );

	QTranslator* t = new QTranslator;
	foreach ( const QString& name, names )
		foreach ( const QString& dir, dirs )
			if ( t->load( name, dir ) )
			{
				QCoreApplication::installTranslator( t );
				qtTranslator = t;
				return;
			}
	delete t;
}
#endif

int QtYabause::setTranslationFile()
{
#ifdef HAVE_LIBMINI18N
	const QString s = settings()->value( "General/Translation" ).toString();
	if ( s == "#" )
	{
		setQtTranslator( QString() );
		return 0; // magic value for "no translation" (aka english)
	}

	bool loaded = false;
	QString locale;
	if ( !s.isEmpty() )
	{
		/* The setting stores the full path chosen in the settings dialog. If
		   Kronos was moved or reinstalled elsewhere, look for the same file
		   name in the current translation folder instead of silently falling
		   back to the system language. */
		loaded = loadTranslationFile( s ) ||
			loadTranslationFile( QDir( translationDirectory() ).filePath( QFileInfo( s ).fileName() ) );
		if ( loaded )
			locale = localeOfTranslationFile( s );
	}

	if ( !loaded )
	{
		/* "Use System Locale": take the language from Qt rather than from
		   mini18n_set_domain(). mini18n only reads $LANG on Unix (LC_ALL and
		   LC_MESSAGES are ignored) and builds wrong codes on Windows from the
		   3-letter abbreviations ("JPN" -> "jp", "CHS" -> "ch"), so Japanese
		   and Chinese were never picked up there. */
		const QDir dir( translationDirectory() );
		QStringList candidates;
		foreach ( const QString& ui, QLocale::system().uiLanguages() )
		{
			const QString name = QLocale( ui ).name(); // "pt_BR", "zh_TW", "fr_FR"...
			candidates << name << name.section( '_', 0, 0 );
		}
		candidates << QLocale::system().name() << QLocale::system().name().section( '_', 0, 0 );
		candidates.removeDuplicates();
		foreach ( const QString& c, candidates )
		{
			if ( c.isEmpty() || c == "C" || c.startsWith( "en" ) )
				continue;
			if ( loadTranslationFile( dir.filePath( c + ".yts" ) ) ||
				loadTranslationFile( dir.filePath( "kronos_" + c + ".yts" ) ) )
			{
				loaded = true;
				locale = c;
				break;
			}
		}
		// last resort: mini18n's own lookup
		if ( !loaded )
		{
			const QByteArray encoded = QFile::encodeName( translationDirectory() );
			loaded = mini18n_set_domain( encoded.constData() ) == 0;
			if ( loaded )
				locale = QLocale::system().name();
		}
	}

	setQtTranslator( loaded ? locale : QString() );

	if ( loaded )
	{
		QtYabause::retranslateApplication();
		if ( logTranslation() != 0 )
			qWarning( "Can't log translation !" );
		return 0;
	}
#endif
	return -1;
}

int QtYabause::logTranslation()
{
#ifdef HAVE_LIBMINI18N
	if (settings()->value( "General/LogUntranslated", false ).toBool())
	{
		const QString s = settings()->value( "General/Translation" ).toString();
		if ( s.isEmpty() || s == "#" )
			return 0;
		const QString logPath = QString( s ).replace( ".yts", "_log.yts" );
		const QByteArray encoded = QFile::encodeName( logPath );
		return mini18n_set_log( encoded.constData() );
	}
#endif
	return 0;
}

void QtYabause::closeTranslation()
{
#ifdef HAVE_LIBMINI18N
	mini18n_close();
#endif
}

QString QtYabause::translate( const QString& string )
{
#ifdef HAVE_LIBMINI18N
	if ( string.isEmpty() )
		return string;
	const QByteArray key = string.toUtf8();
	return QString::fromUtf8( _( key.constData() ) );
#else
	return string;
#endif
}

#ifdef HAVE_LIBMINI18N
namespace {

/* Every translated text is remembered on its object as two dynamic
   properties: the source (English) string and the translation that was
   applied. This is what makes the retranslation work:
   - calling it twice is harmless (the second pass translates the same source
     again instead of looking up an already translated text);
   - switching language at run time works: the source is translated again
     with the new file, whereas the previous code looked up the French text
     in the German file, found nothing and left the dialog in French;
   - a text replaced by code since the last pass (it no longer matches the
     stored translation) is taken as the new source.
   'slot' tells apart several texts of one object (text, toolTip, item 3...). */
QString retranslated( QObject* o, const QString& slot, const QString& current )
{
	if ( current.isEmpty() )
		return current;
	const QByteArray srcName = QByteArray( "_yts_src_" ) + slot.toLatin1();
	const QByteArray trName = QByteArray( "_yts_tr_" ) + slot.toLatin1();
	const QVariant lastSrc = o->property( srcName.constData() );
	const QVariant lastTr = o->property( trName.constData() );
	QString source = current;
	if ( lastSrc.isValid() && lastTr.isValid() && lastTr.toString() == current )
		source = lastSrc.toString();
	const QString t = QtYabause::translate( source );
	// only touch the properties when they change (each setProperty() sends a
	// QDynamicPropertyChangeEvent, and some debug windows retranslate often)
	if ( !lastSrc.isValid() || lastSrc.toString() != source )
		o->setProperty( srcName.constData(), source );
	if ( !lastTr.isValid() || lastTr.toString() != t )
		o->setProperty( trName.constData(), t );
	return t;
}

// Text Qt derives from an action's text when no tooltip/icon text is set.
QString strippedActionText( QString s )
{
	s.remove( QString::fromLatin1( "..." ) );
	s.remove( QChar( 0x2026 ) );
	QString out;
	for ( int i = 0; i < s.size(); i++ )
	{
		if ( s.at( i ) == QLatin1Char( '&' ) )
		{
			if ( i + 1 < s.size() && s.at( i + 1 ) == QLatin1Char( '&' ) )
				out += s.at( ++i );
			continue;
		}
		out += s.at( i );
	}
	return out.trimmed();
}

#define YTS_SET( obj, slot, getter, setter ) \
	do { \
		const QString cur_ = (getter); \
		const QString new_ = retranslated( (obj), (slot), cur_ ); \
		if ( new_ != cur_ ) \
			setter; \
	} while ( 0 )

void retranslateAction( QAction* a )
{
	if ( !a || a->isSeparator() )
		return;
	// a submenu's action carries the menu title, handled with the QMenu itself
	if ( a->menu() )
		return;

	YTS_SET( a, "text", a->text(), a->setText( new_ ) );
	YTS_SET( a, "statusTip", a->statusTip(), a->setStatusTip( new_ ) );
	YTS_SET( a, "whatsThis", a->whatsThis(), a->setWhatsThis( new_ ) );
	// Leave implicit values alone: Qt derives them from the text, setting them
	// would freeze them to the current language.
	if ( a->toolTip() != strippedActionText( a->text() ) )
		YTS_SET( a, "toolTip", a->toolTip(), a->setToolTip( new_ ) );
	if ( a->iconText() != strippedActionText( a->text() ) )
		YTS_SET( a, "iconText", a->iconText(), a->setIconText( new_ ) );
}

void retranslateWidgetProperties( QWidget* w )
{
	YTS_SET( w, "accessibleDescription", w->accessibleDescription(), w->setAccessibleDescription( new_ ) );
	YTS_SET( w, "accessibleName", w->accessibleName(), w->setAccessibleName( new_ ) );
	YTS_SET( w, "statusTip", w->statusTip(), w->setStatusTip( new_ ) );
	YTS_SET( w, "toolTip", w->toolTip(), w->setToolTip( new_ ) );
	YTS_SET( w, "whatsThis", w->whatsThis(), w->setWhatsThis( new_ ) );
	YTS_SET( w, "windowIconText", w->windowIconText(), w->setWindowIconText( new_ ) );
	YTS_SET( w, "windowTitle", w->windowTitle(), w->setWindowTitle( new_ ) );
}

void retranslateTreeHeader( QTreeWidget* tw )
{
	QTreeWidgetItem* h = tw->headerItem();
	if ( !h )
		return;
	for ( int i = 0; i < h->columnCount(); i++ )
	{
		const QString n = QString::number( i );
		YTS_SET( tw, "hdrText" + n, h->text( i ), h->setText( i, new_ ) );
		YTS_SET( tw, "hdrToolTip" + n, h->toolTip( i ), h->setToolTip( i, new_ ) );
		YTS_SET( tw, "hdrStatusTip" + n, h->statusTip( i ), h->setStatusTip( i, new_ ) );
		YTS_SET( tw, "hdrWhatsThis" + n, h->whatsThis( i ), h->setWhatsThis( i, new_ ) );
	}
}

void retranslateTableHeaders( QTableWidget* tw )
{
	for ( int i = 0; i < tw->columnCount(); i++ )
	{
		QTableWidgetItem* it = tw->horizontalHeaderItem( i );
		if ( it )
		{
			const QString n = QString::number( i );
			YTS_SET( tw, "hHdrText" + n, it->text(), it->setText( new_ ) );
			YTS_SET( tw, "hHdrToolTip" + n, it->toolTip(), it->setToolTip( new_ ) );
		}
	}
	for ( int i = 0; i < tw->rowCount(); i++ )
	{
		QTableWidgetItem* it = tw->verticalHeaderItem( i );
		if ( it )
			YTS_SET( tw, "vHdrText" + QString::number( i ), it->text(), it->setText( new_ ) );
	}
}

void retranslateWidgetTree( QWidget* widget, bool recursive )
{
	if ( !widget )
		return;

	retranslateWidgetProperties( widget );

	/* qobject_cast instead of comparing metaObject()->className(): the old
	   test missed every subclass (UIPortManager is a QGroupBox,
	   UIShortcutManager a QTreeWidget...), whose texts stayed in English. */
	if ( QLabel* l = qobject_cast<QLabel*>( widget ) )
		YTS_SET( l, "text", l->text(), l->setText( new_ ) );
	else if ( QAbstractButton* ab = qobject_cast<QAbstractButton*>( widget ) )
		YTS_SET( ab, "text", ab->text(), ab->setText( new_ ) );
	else if ( QGroupBox* gb = qobject_cast<QGroupBox*>( widget ) )
		YTS_SET( gb, "title", gb->title(), gb->setTitle( new_ ) );
	else if ( QMenu* m = qobject_cast<QMenu*>( widget ) )
		YTS_SET( m, "title", m->title(), m->setTitle( new_ ) );
	else if ( QLineEdit* le = qobject_cast<QLineEdit*>( widget ) )
		YTS_SET( le, "placeholderText", le->placeholderText(), le->setPlaceholderText( new_ ) );
	else if ( QPlainTextEdit* pte = qobject_cast<QPlainTextEdit*>( widget ) )
		YTS_SET( pte, "placeholderText", pte->placeholderText(), pte->setPlaceholderText( new_ ) );
	else if ( QTextEdit* te = qobject_cast<QTextEdit*>( widget ) )
		YTS_SET( te, "placeholderText", te->placeholderText(), te->setPlaceholderText( new_ ) );
	else if ( QSpinBox* sb = qobject_cast<QSpinBox*>( widget ) )
	{
		YTS_SET( sb, "prefix", sb->prefix(), sb->setPrefix( new_ ) );
		YTS_SET( sb, "suffix", sb->suffix(), sb->setSuffix( new_ ) );
		YTS_SET( sb, "specialValueText", sb->specialValueText(), sb->setSpecialValueText( new_ ) );
	}
	else if ( QDoubleSpinBox* dsb = qobject_cast<QDoubleSpinBox*>( widget ) )
	{
		YTS_SET( dsb, "prefix", dsb->prefix(), dsb->setPrefix( new_ ) );
		YTS_SET( dsb, "suffix", dsb->suffix(), dsb->setSuffix( new_ ) );
		YTS_SET( dsb, "specialValueText", dsb->specialValueText(), dsb->setSpecialValueText( new_ ) );
	}
	else if ( QComboBox* cb = qobject_cast<QComboBox*>( widget ) )
	{
		/* Items written in a .ui file are not covered by the widget
		   properties. Editable combos are skipped (their items are user
		   input), and signals are blocked so that renaming the current item
		   does not look like a selection change. */
		if ( !cb->isEditable() )
		{
			const QSignalBlocker blocker( cb );
			for ( int i = 0; i < cb->count(); i++ )
				YTS_SET( cb, "item" + QString::number( i ), cb->itemText( i ), cb->setItemText( i, new_ ) );
		}
	}
	else if ( QTabWidget* tw = qobject_cast<QTabWidget*>( widget ) )
	{
		for ( int i = 0; i < tw->count(); i++ )
		{
			const QString n = QString::number( i );
			YTS_SET( tw, "tabText" + n, tw->tabText( i ), tw->setTabText( i, new_ ) );
			YTS_SET( tw, "tabToolTip" + n, tw->tabToolTip( i ), tw->setTabToolTip( i, new_ ) );
		}
	}
	else if ( QToolBox* tb = qobject_cast<QToolBox*>( widget ) )
	{
		for ( int i = 0; i < tb->count(); i++ )
			YTS_SET( tb, "itemText" + QString::number( i ), tb->itemText( i ), tb->setItemText( i, new_ ) );
	}
	else if ( QTreeWidget* trw = qobject_cast<QTreeWidget*>( widget ) )
		retranslateTreeHeader( trw );
	else if ( QTableWidget* tbw = qobject_cast<QTableWidget*>( widget ) )
		retranslateTableHeaders( tbw );

	/* Actions: those shown by this widget (menus, toolbars, tool buttons) and
	   those it owns without showing them (actions declared in a .ui file but
	   only reachable through a shortcut, such as Save0..Save9 / Load0..Load9,
	   which are listed in the shortcut settings). The old code only handled
	   the actions of QMenu / QMenuBar. */
	foreach ( QAction* a, widget->actions() )
	{
		retranslateAction( a );
		// a menu reachable only through an action (no widget parent)
		if ( recursive && a->menu() && !a->menu()->parentWidget() )
			retranslateWidgetTree( a->menu(), true );
	}
	foreach ( QAction* a, widget->findChildren<QAction*>( QString(), Qt::FindDirectChildrenOnly ) )
		retranslateAction( a );

	if ( !recursive )
		return;

	/* Direct children only: the old code walked findChildren() (all the
	   descendants) and then recursed into each of them, so a widget N levels
	   deep was translated N times. */
	foreach ( QWidget* w, widget->findChildren<QWidget*>( QString(), Qt::FindDirectChildrenOnly ) )
		retranslateWidgetTree( w, true );
}

#undef YTS_SET

} // namespace
#endif

QString QtYabause::sourceText( const QAction* action )
{
#ifdef HAVE_LIBMINI18N
	if ( !action )
		return QString();
	/* English text of an action even when the UI is translated. Used as the
	   key of the "Shortcuts/" settings, which used to be the displayed text:
	   shortcuts saved from a translated UI were stored under the translated
	   name and not found again at the next start (shortcuts are loaded before
	   the UI is translated). */
	const QVariant src = action->property( "_yts_src_text" );
	const QVariant tr = action->property( "_yts_tr_text" );
	if ( src.isValid() && tr.isValid() && tr.toString() == action->text() )
		return src.toString();
	return action->text();
#else
	return action ? action->text() : QString();
#endif
}

void QtYabause::retranslateWidgetOnly( QWidget* widget )
{
#ifdef HAVE_LIBMINI18N
	if ( widget )
		retranslateWidgetProperties( widget );
#endif
}

void QtYabause::retranslateWidget( QWidget* widget )
{
#ifdef HAVE_LIBMINI18N
	retranslateWidgetTree( widget, true );
#endif
}

void QtYabause::retranslateApplication()
{
#ifdef HAVE_LIBMINI18N
	// every widget is a descendant of a top-level widget
	foreach ( QWidget* widget, QApplication::topLevelWidgets() )
		retranslateWidget( widget );
#endif
}

const char* QtYabause::getCurrentCdSerial()
{ return cdip ? cdip->itemnum : 0; }

M68K_struct* QtYabause::getM68KCore( int id )
{
	for ( int i = 0; M68KCoreList[i] != NULL; i++ )
		if ( M68KCoreList[i]->id == id )
			return M68KCoreList[i];
	return 0;
}

SH2Interface_struct* QtYabause::getSH2Core( int id )
{
	for ( int i = 0; SH2CoreList[i] != NULL; i++ )
		if ( SH2CoreList[i]->id == id )
			return SH2CoreList[i];
	return 0;
}

PerInterface_struct* QtYabause::getPERCore( int id )
{
	for ( int i = 0; PERCoreList[i] != NULL; i++ )
		if ( PERCoreList[i]->id == id )
			return PERCoreList[i];
	return 0;
}

CDInterface* QtYabause::getCDCore( int id )
{
	for ( int i = 0; CDCoreList[i] != NULL; i++ )
		if ( CDCoreList[i]->id == id )
			return CDCoreList[i];
	return 0;
}

SoundInterface_struct* QtYabause::getSNDCore( int id )
{
	for ( int i = 0; SNDCoreList[i] != NULL; i++ )
		if ( SNDCoreList[i]->id == id )
			return SNDCoreList[i];
	return 0;
}

VideoInterface_struct* QtYabause::getVDICore( int id )
{
	for ( int i = 0; VIDCoreList[i] != NULL; i++ )
		if ( VIDCoreList[i]->id == id )
			return VIDCoreList[i];
	return 0;
}

CDInterface QtYabause::defaultCDCore()
{
#ifndef UNKNOWN_ARCH
	return ArchCD;
#else
	return DummyCD;
#endif
}

SoundInterface_struct QtYabause::defaultSNDCore()
{
#ifdef HAVE_LIBSDL
	return SNDSDL;
#else
	return SNDDummy;
#endif
}

VideoInterface_struct QtYabause::defaultVIDCore()
{
#ifdef HAVE_LIBGL
  return VIDCS;
#else
	return VIDSoft;
#endif
}

OSD_struct QtYabause::defaultOSDCore()
{
#ifdef HAVE_LIBGL
        return OSDNnovg;
#else
	return OSDDummy;
#endif
}

PerInterface_struct QtYabause::defaultPERCore()
{
#if defined HAVE_LIBSDL
	return PERSDLJoy;
#elif defined HAVE_DIRECTINPUT
		return PERDIRECTX;
#else
	return PERQT;
#endif
}

M68K_struct QtYabause::default68kCore()
{
   return M68KMusashi;
}

SH2Interface_struct QtYabause::defaultSH2Core()
{
   return SH2KronosInterpreter;
}

QMap<uint, PerPad_struct*>* QtYabause::portPadsBits( uint portNumber )
{
	switch ( portNumber )
	{
		case 1:
			return &mPort1PadsBits;
			break;
		case 2:
			return &mPort2PadsBits;
			break;
		default:
			return 0;
			break;
	}
}

void QtYabause::clearPadsBits()
{
	mPort1PadsBits.clear();
	mPort2PadsBits.clear();
}

QMap<uint, PerCab_struct*>* QtYabause::portIOBits( ) {
  return &mPortIOBits;
}

void QtYabause::clearIOBits()
{
   mPortIOBits.clear();
}

QMap<uint, PerAnalog_struct*>* QtYabause::portAnalogBits( uint portNumber )
{
   switch ( portNumber )
   {
   case 1:
      return &mPort1AnalogBits;
      break;
   case 2:
      return &mPort2AnalogBits;
      break;
   default:
      return 0;
      break;
   }
}

void QtYabause::clear3DAnalogBits()
{
   mPort1AnalogBits.clear();
   mPort2AnalogBits.clear();
}

QMap<uint, PerGun_struct*>* QtYabause::portGunBits( uint portNumber )
{
	switch ( portNumber )
	{
	case 1:
		return &mPort1GunBits;
		break;
	case 2:
		return &mPort2GunBits;
		break;
	default:
		return 0;
		break;
	}
}

void QtYabause::clearGunBits()
{
	mPort1GunBits.clear();
	mPort2GunBits.clear();
}

QMap<uint, PerMouse_struct*>* QtYabause::portMouseBits( uint portNumber )
{
   switch ( portNumber )
   {
   case 1:
      return &mPort1MouseBits;
      break;
   case 2:
      return &mPort2MouseBits;
      break;
   default:
      return 0;
      break;
   }
}

void QtYabause::clearMouseBits()
{
   mPort1MouseBits.clear();
   mPort2MouseBits.clear();
}

QString QtYabause::DefaultPaths::Screenshots() { return getDataDirPath() + "/screenshots"; }

QString QtYabause::DefaultPaths::Cartridge() { return getDataDirPath(); }
