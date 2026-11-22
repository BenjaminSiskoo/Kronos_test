/*  Lua script console (Kronos). GPL v2+ */
#include "UILuaConsole.h"
#include "../CommonDialogs.h"
#include "../LuaScript.h"
#include "Settings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

UILuaConsole::UILuaConsole( QWidget* parent )
	: QDialog( parent )
{
	setWindowTitle( QtYabause::translate( "Lua script console" ) );
	setAttribute( Qt::WA_DeleteOnClose, true );
	setModal( false );
	resize( 640, 420 );

	QVBoxLayout* root = new QVBoxLayout( this );

	QHBoxLayout* row = new QHBoxLayout;
	mPath = new QLineEdit;
	mPath->setPlaceholderText( QtYabause::translate( "Lua script (.lua)" ) );
	QPushButton* browse = new QPushButton( QtYabause::translate( "Browse..." ) );
	mLoad = new QPushButton( QtYabause::translate( "Load / Reload" ) );
	mStop = new QPushButton( QtYabause::translate( "Stop" ) );
	row->addWidget( mPath, 1 );
	row->addWidget( browse );
	row->addWidget( mLoad );
	row->addWidget( mStop );
	root->addLayout( row );

	mStatus = new QLabel;
	mStatus->setWordWrap( true );
	root->addWidget( mStatus );

	mLog = new QPlainTextEdit;
	mLog->setReadOnly( true );
	mLog->setMaximumBlockCount( 5000 );
	root->addWidget( mLog, 1 );

	QPushButton* clear = new QPushButton( QtYabause::translate( "Clear" ) );
	QHBoxLayout* bottom = new QHBoxLayout;
	bottom->addStretch( 1 );
	bottom->addWidget( clear );
	root->addLayout( bottom );

	Settings* s = QtYabause::settings();
	if ( s != NULL )
		mPath->setText( s->value( "Lua/LastScript" ).toString() );
	if ( LuaScript::isLoaded() )
		mPath->setText( LuaScript::loadedPath() );

	connect( browse, SIGNAL( clicked() ), this, SLOT( onBrowse() ) );
	connect( mLoad, SIGNAL( clicked() ), this, SLOT( onLoad() ) );
	connect( mStop, SIGNAL( clicked() ), this, SLOT( onStop() ) );
	connect( clear, SIGNAL( clicked() ), mLog, SLOT( clear() ) );
	connect( LuaScriptHost::instance(), SIGNAL( logLine( QString ) ), this, SLOT( onLog( QString ) ), Qt::QueuedConnection );
	connect( LuaScriptHost::instance(), SIGNAL( stateChanged( bool ) ), this, SLOT( onStateChanged( bool ) ), Qt::QueuedConnection );

	updateStatus();
}

void UILuaConsole::updateStatus()
{
	if ( LuaScript::isLoaded() )
		mStatus->setText( QtYabause::translate( "Script running: %1" ).arg( LuaScript::loadedPath() ) );
	else
		mStatus->setText( QtYabause::translate( "No script running. A script is loaded or stopped at the next emulated frame, so the emulation must be running (not paused)." ) );
	mStop->setEnabled( LuaScript::isLoaded() );
}

void UILuaConsole::onBrowse()
{
	const QString f = CommonDialogs::getOpenFileName( mPath->text(), QtYabause::translate( "Choose a Lua script" ),
		QtYabause::translate( "Lua scripts (*.lua);;All files (*)" ) );
	if ( !f.isEmpty() )
		mPath->setText( f );
}

void UILuaConsole::onLoad()
{
	const QString p = mPath->text().trimmed();
	if ( p.isEmpty() )
		return;
	Settings* s = QtYabause::settings();
	if ( s != NULL )
		s->setValue( "Lua/LastScript", p );
	mLog->appendPlainText( QtYabause::translate( "Loading %1 at the next frame..." ).arg( p ) );
	LuaScript::requestLoad( p );
}

void UILuaConsole::onStop()
{
	LuaScript::requestUnload();
}

void UILuaConsole::onLog( const QString& line )
{
	mLog->appendPlainText( line );
}

void UILuaConsole::onStateChanged( bool )
{
	updateStatus();
}
