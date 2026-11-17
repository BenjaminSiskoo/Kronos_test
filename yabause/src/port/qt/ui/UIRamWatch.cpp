/*  UIRamWatch -- surveillance d'adresses memoire (outil de TAS), voir
    UIRamWatch.h.

    This file is part of Yabause (GPL v2 or later). */

#include "UIRamWatch.h"
#include "UIYabause.h"
#include "../CommonDialogs.h"
#include "../QtYabause.h"
#include "../Settings.h"
#include "../VolatileSettings.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

extern "C" {
#include "cs2.h"
#include "memory.h"
#include "movie.h"
}

namespace {

enum Column { ColAddress = 0, ColType, ColValue, ColHex, ColLabel, ColCount };

// nombre de rafraichissements pendant lesquels une valeur qui a change reste
// en evidence
const int kHighlightTicks = 12;

const char* const kTypeNames[UIRamWatch::TypeCount] = {
	"u8", "s8", "u16", "s16", "u32", "s32", "fixed 16.16"
};

QTableWidgetItem* readOnlyItem( const QString& text )
{
	QTableWidgetItem* item = new QTableWidgetItem( text );
	item->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled );
	return item;
}

} // namespace

QString UIRamWatch::typeName( int type )
{
	return ( type >= 0 && type < TypeCount ) ? QString::fromLatin1( kTypeNames[type] ) : QString( "?" );
}

int UIRamWatch::typeSize( int type )
{
	switch ( type )
	{
		case U8: case S8: return 1;
		case U16: case S16: return 2;
		default: return 4;
	}
}

UIRamWatch::UIRamWatch( UIYabause* parent )
	: QDialog( parent ), mMain( parent )
{
	setWindowTitle( QtYabause::translate( "RAM watch" ) );
	setWindowFlags( windowFlags() & ~Qt::WindowContextHelpButtonHint );
	setModal( false );
	resize( 520, 360 );

	mTable = new QTableWidget( 0, ColCount, this );
	mTable->setHorizontalHeaderLabels( QStringList()
		<< QtYabause::translate( "Address" )
		<< QtYabause::translate( "Type" )
		<< QtYabause::translate( "Value" )
		<< QtYabause::translate( "Hex" )
		<< QtYabause::translate( "Label" ) );
	mTable->setSelectionBehavior( QAbstractItemView::SelectRows );
	mTable->setSelectionMode( QAbstractItemView::ExtendedSelection );
	mTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
	mTable->verticalHeader()->setVisible( false );
	mTable->verticalHeader()->setDefaultSectionSize( mTable->fontMetrics().height() + 6 );
	mTable->horizontalHeader()->setStretchLastSection( true );
	{
		QFont f = mTable->font();
		f.setStyleHint( QFont::Monospace );
		f.setFamily( "monospace" );
		mTable->setFont( f );
	}

	QPushButton* add = new QPushButton( QtYabause::translate( "Add..." ), this );
	mEdit = new QPushButton( QtYabause::translate( "Edit..." ), this );
	mRemove = new QPushButton( QtYabause::translate( "Remove" ), this );
	mUp = new QPushButton( QtYabause::translate( "Up" ), this );
	mDown = new QPushButton( QtYabause::translate( "Down" ), this );
	QPushButton* load = new QPushButton( QtYabause::translate( "Load..." ), this );
	QPushButton* save = new QPushButton( QtYabause::translate( "Save..." ), this );
	// Entree dans la fenetre ne doit pas "cliquer" un bouton
	foreach ( QPushButton* b, QList<QPushButton*>() << add << mEdit << mRemove << mUp << mDown << load << save )
		b->setAutoDefault( false );

	QVBoxLayout* buttons = new QVBoxLayout;
	buttons->addWidget( add );
	buttons->addWidget( mEdit );
	buttons->addWidget( mRemove );
	buttons->addWidget( mUp );
	buttons->addWidget( mDown );
	buttons->addStretch( 1 );
	buttons->addWidget( load );
	buttons->addWidget( save );

	QHBoxLayout* top = new QHBoxLayout;
	top->addWidget( mTable, 1 );
	top->addLayout( buttons );

	mStatus = new QLabel( this );

	QVBoxLayout* all = new QVBoxLayout( this );
	all->addLayout( top, 1 );
	all->addWidget( mStatus );

	connect( add, SIGNAL( clicked() ), this, SLOT( addEntry() ) );
	connect( mEdit, SIGNAL( clicked() ), this, SLOT( editEntry() ) );
	connect( mRemove, SIGNAL( clicked() ), this, SLOT( removeEntries() ) );
	connect( mUp, SIGNAL( clicked() ), this, SLOT( moveUp() ) );
	connect( mDown, SIGNAL( clicked() ), this, SLOT( moveDown() ) );
	connect( load, SIGNAL( clicked() ), this, SLOT( loadFile() ) );
	connect( save, SIGNAL( clicked() ), this, SLOT( saveFile() ) );
	connect( mTable, SIGNAL( itemSelectionChanged() ), this, SLOT( selectionChanged() ) );
	connect( mTable, SIGNAL( cellDoubleClicked( int, int ) ), this, SLOT( editEntry() ) );
	// apres chaque trame de l'avance image par image, et a l'arrivee d'un
	// retour en arriere
	connect( mMain, SIGNAL( tasFrameStepped() ), this, SLOT( refresh() ) );
	connect( mMain, SIGNAL( tasSeekFinished() ), this, SLOT( refresh() ) );

	mGame = currentGame();
	loadSettings();
	rebuildTable();
	selectionChanged();

	mTimer = new QTimer( this );
	connect( mTimer, SIGNAL( timeout() ), this, SLOT( refresh() ) );
	mTimer->start( 33 );
}

void UIRamWatch::closeEvent( QCloseEvent* e )
{
	saveSettings();
	QDialog::closeEvent( e );
}

QString UIRamWatch::currentGame() const
{
	if ( mMain->mYabauseThread->init() < 0 || cdip == NULL )
		return QString();
	return QString::fromLatin1( cdip->itemnum ).trimmed();
}

//////////////////////////////////////////////////////////////////////////////
// Lecture et affichage
//////////////////////////////////////////////////////////////////////////////

bool UIRamWatch::readValue( const Entry& entry, quint32* raw ) const
{
	// pas de table memoire avant le demarrage de l'emulation
	if ( mMain->mYabauseThread->init() < 0 )
		return false;
	switch ( typeSize( entry.type ) )
	{
		case 1:
			*raw = DMAMappedMemoryReadByte( entry.address );
			break;
		case 2:
			// octet par octet : pas de contrainte d'alignement
			*raw = ( (quint32)DMAMappedMemoryReadByte( entry.address ) << 8 )
				| (quint32)DMAMappedMemoryReadByte( entry.address + 1 );
			break;
		default:
			*raw = ( (quint32)DMAMappedMemoryReadByte( entry.address ) << 24 )
				| ( (quint32)DMAMappedMemoryReadByte( entry.address + 1 ) << 16 )
				| ( (quint32)DMAMappedMemoryReadByte( entry.address + 2 ) << 8 )
				| (quint32)DMAMappedMemoryReadByte( entry.address + 3 );
			break;
	}
	return true;
}

QString UIRamWatch::formatValue( const Entry& entry, quint32 raw ) const
{
	switch ( entry.type )
	{
		case U8: case U16: case U32:
			return QString::number( raw );
		case S8:
			return QString::number( (int)(qint8)raw );
		case S16:
			return QString::number( (int)(qint16)raw );
		case S32:
			return QString::number( (qint32)raw );
		case Fixed16_16:
			return QString::number( (double)(qint32)raw / 65536.0, 'f', 4 );
		default:
			return QString();
	}
}

QString UIRamWatch::formatHex( const Entry& entry, quint32 raw ) const
{
	return QString( "%1" ).arg( raw, typeSize( entry.type ) * 2, 16, QChar( '0' ) ).toUpper();
}

void UIRamWatch::rebuildTable()
{
	mTable->setRowCount( mEntries.size() );
	for ( int row = 0; row < mEntries.size(); row++ )
	{
		for ( int col = 0; col < ColCount; col++ )
			if ( !mTable->item( row, col ) )
				mTable->setItem( row, col, readOnlyItem( QString() ) );
		mEntries[row].valid = false;
		mEntries[row].highlight = 0;
		updateRow( row, true );
	}
	mTable->resizeColumnToContents( ColAddress );
	mTable->resizeColumnToContents( ColType );
	mTable->setColumnWidth( ColValue, 110 );
	mTable->setColumnWidth( ColHex, 80 );
}

void UIRamWatch::updateRow( int row, bool force )
{
	Entry& e = mEntries[row];
	quint32 raw = 0;
	const bool ok = readValue( e, &raw );

	if ( force )
	{
		mTable->item( row, ColAddress )->setText( QString( "%1" ).arg( e.address, 8, 16, QChar( '0' ) ).toUpper() );
		mTable->item( row, ColType )->setText( typeName( e.type ) );
		mTable->item( row, ColLabel )->setText( e.label );
	}

	const bool changed = ok && e.valid && raw != e.last;
	if ( force || changed || ok != e.valid )
	{
		mTable->item( row, ColValue )->setText( ok ? formatValue( e, raw ) : QString( "-" ) );
		mTable->item( row, ColHex )->setText( ok ? formatHex( e, raw ) : QString( "-" ) );
	}

	const int before = e.highlight;
	if ( changed )
		e.highlight = kHighlightTicks;
	else if ( e.highlight > 0 )
		e.highlight--;
	if ( force || ( before > 0 ) != ( e.highlight > 0 ) )
	{
		const QBrush brush = ( e.highlight > 0 ) ? QBrush( QColor( 200, 0, 0 ) ) : mTable->palette().brush( QPalette::Text );
		mTable->item( row, ColValue )->setForeground( brush );
		mTable->item( row, ColHex )->setForeground( brush );
	}
	e.last = raw;
	e.valid = ok;
}

void UIRamWatch::refresh()
{
	// un autre jeu a ete charge : sa liste
	const QString game = currentGame();
	if ( game != mGame )
	{
		saveSettings();
		mGame = game;
		loadSettings();
		rebuildTable();
		selectionChanged();
	}
	if ( !isVisible() )
		return;
	for ( int row = 0; row < mEntries.size(); row++ )
		updateRow( row, false );

	QString status;
	if ( mMain->mYabauseThread->init() < 0 )
		status = QtYabause::translate( "The emulation is not started." );
	else
		status = QtYabause::translate( "Frame %1 - lag %2" ).arg( MovieGetFrameCounter() ).arg( MovieGetLagCount() );
	if ( status != mStatus->text() )
		mStatus->setText( status );
}

void UIRamWatch::selectionChanged()
{
	const int count = mTable->selectionModel()->selectedRows().size();
	mEdit->setEnabled( count == 1 );
	mRemove->setEnabled( count > 0 );
	mUp->setEnabled( count == 1 );
	mDown->setEnabled( count == 1 );
}

//////////////////////////////////////////////////////////////////////////////
// Edition de la liste
//////////////////////////////////////////////////////////////////////////////

bool UIRamWatch::editDialog( Entry& entry, const QString& title )
{
	QDialog dlg( this );
	dlg.setWindowTitle( title );
	dlg.setWindowFlags( dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint );

	QLineEdit* address = new QLineEdit( QString( "%1" ).arg( entry.address, 8, 16, QChar( '0' ) ).toUpper(), &dlg );
	address->setMaxLength( 8 );
	QComboBox* type = new QComboBox( &dlg );
	for ( int i = 0; i < TypeCount; i++ )
		type->addItem( typeName( i ) );
	type->setCurrentIndex( entry.type );
	QLineEdit* label = new QLineEdit( entry.label, &dlg );

	QFormLayout* form = new QFormLayout;
	form->addRow( QtYabause::translate( "Address (hex):" ), address );
	form->addRow( QtYabause::translate( "Type:" ), type );
	form->addRow( QtYabause::translate( "Label:" ), label );

	QDialogButtonBox* box = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg );
	connect( box, SIGNAL( accepted() ), &dlg, SLOT( accept() ) );
	connect( box, SIGNAL( rejected() ), &dlg, SLOT( reject() ) );

	QVBoxLayout* all = new QVBoxLayout( &dlg );
	all->addLayout( form );
	all->addWidget( box );

	for ( ;; )
	{
		if ( dlg.exec() != QDialog::Accepted )
			return false;
		bool ok = false;
		const quint32 value = address->text().trimmed().toUInt( &ok, 16 );
		if ( !ok )
		{
			CommonDialogs::error( QtYabause::translate( "The address must be a hexadecimal number (for example 060FFC00)." ) );
			continue;
		}
		entry.address = value;
		entry.type = type->currentIndex();
		// le fichier .wch a une entree par ligne
		entry.label = label->text().simplified();
		entry.valid = false;
		entry.highlight = 0;
		return true;
	}
}

void UIRamWatch::addEntry()
{
	Entry e;
	// Work RAM haute, la ou sont les variables de la plupart des jeux
	e.address = 0x06000000;
	e.type = U16;
	e.last = 0;
	e.valid = false;
	e.highlight = 0;
	const QList<QModelIndex> rows = mTable->selectionModel()->selectedRows();
	if ( rows.size() == 1 )
	{
		// a la suite de l'entree selectionnee
		const Entry& sel = mEntries[rows[0].row()];
		e.address = sel.address + typeSize( sel.type );
		e.type = sel.type;
	}
	if ( !editDialog( e, QtYabause::translate( "Add a watch" ) ) )
		return;
	mEntries.append( e );
	rebuildTable();
	mTable->selectRow( mEntries.size() - 1 );
	saveSettings();
}

void UIRamWatch::editEntry()
{
	const QList<QModelIndex> rows = mTable->selectionModel()->selectedRows();
	if ( rows.size() != 1 )
		return;
	const int row = rows[0].row();
	Entry e = mEntries[row];
	if ( !editDialog( e, QtYabause::translate( "Edit a watch" ) ) )
		return;
	mEntries[row] = e;
	updateRow( row, true );
	saveSettings();
}

void UIRamWatch::removeEntries()
{
	QList<int> rows;
	foreach ( const QModelIndex& index, mTable->selectionModel()->selectedRows() )
		rows.append( index.row() );
	std::sort( rows.begin(), rows.end() );
	for ( int i = rows.size() - 1; i >= 0; i-- )
		mEntries.removeAt( rows[i] );
	mTable->clearSelection();
	rebuildTable();
	selectionChanged();
	saveSettings();
}

void UIRamWatch::moveSelected( int delta )
{
	const QList<QModelIndex> rows = mTable->selectionModel()->selectedRows();
	if ( rows.size() != 1 )
		return;
	const int row = rows[0].row();
	const int to = row + delta;
	if ( to < 0 || to >= mEntries.size() )
		return;
	std::swap( mEntries[row], mEntries[to] );
	rebuildTable();
	mTable->selectRow( to );
	saveSettings();
}

void UIRamWatch::moveUp()
{
	moveSelected( -1 );
}

void UIRamWatch::moveDown()
{
	moveSelected( 1 );
}

//////////////////////////////////////////////////////////////////////////////
// Fichiers et reglages
//////////////////////////////////////////////////////////////////////////////

QString UIRamWatch::entryToLine( const Entry& entry )
{
	// "fixed 16.16" contient une espace : le type est ecrit par son numero
	return QString( "%1" ).arg( entry.address, 8, 16, QChar( '0' ) ).toUpper()
		+ " " + QString::number( entry.type )
		+ " " + entry.label;
}

bool UIRamWatch::lineToEntry( const QString& line, Entry& entry )
{
	const QString text = line.trimmed();
	if ( text.isEmpty() || text.startsWith( '#' ) )
		return false;
	const QString address = text.section( ' ', 0, 0 );
	const QString type = text.section( ' ', 1, 1 );
	bool ok1 = false, ok2 = false;
	entry.address = address.toUInt( &ok1, 16 );
	entry.type = type.toInt( &ok2 );
	if ( !ok1 || !ok2 || entry.type < 0 || entry.type >= TypeCount )
		return false;
	entry.label = text.section( ' ', 2 );
	entry.last = 0;
	entry.valid = false;
	entry.highlight = 0;
	return true;
}

void UIRamWatch::loadSettings()
{
	mEntries.clear();
	if ( mGame.isEmpty() )
		return;
	const QStringList lines = QtYabause::settings()->value( QString( "RamWatch/%1" ).arg( mGame ) ).toStringList();
	foreach ( const QString& line, lines )
	{
		Entry e;
		if ( lineToEntry( line, e ) )
			mEntries.append( e );
	}
}

void UIRamWatch::saveSettings()
{
	if ( mGame.isEmpty() )
		return;
	QStringList lines;
	foreach ( const Entry& e, mEntries )
		lines.append( entryToLine( e ) );
	Settings* s = QtYabause::settings();
	const QString key = QString( "RamWatch/%1" ).arg( mGame );
	if ( lines.isEmpty() )
		s->remove( key );
	else
		s->setValue( key, lines );
}

void UIRamWatch::loadFile()
{
	const QString fn = CommonDialogs::getOpenFileName( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString(),
		QtYabause::translate( "Load a watch list" ), QtYabause::translate( "Kronos watch list (*.wch)" ) );
	if ( fn.isEmpty() )
		return;
	QFile file( fn );
	if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
	{
		CommonDialogs::error( QtYabause::translate( "Couldn't open the watch list." ) );
		return;
	}
	QList<Entry> entries;
	QTextStream in( &file );
	in.setCodec( "UTF-8" );
	while ( !in.atEnd() )
	{
		Entry e;
		if ( lineToEntry( in.readLine(), e ) )
			entries.append( e );
	}
	mEntries = entries;
	mTable->clearSelection();
	rebuildTable();
	selectionChanged();
	saveSettings();
}

void UIRamWatch::saveFile()
{
	QString fn = CommonDialogs::getSaveFileName( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString(),
		QtYabause::translate( "Save the watch list" ), QtYabause::translate( "Kronos watch list (*.wch)" ) );
	if ( fn.isEmpty() )
		return;
	if ( QFileInfo( fn ).suffix().isEmpty() )
		fn += ".wch";
	QFile file( fn );
	if ( !file.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
	{
		CommonDialogs::error( QtYabause::translate( "Couldn't write the watch list." ) );
		return;
	}
	QTextStream out( &file );
	out.setCodec( "UTF-8" );
	out << "# Kronos RAM watch: address(hex) type(0=u8 1=s8 2=u16 3=s16 4=u32 5=s32 6=fixed16.16) label\n";
	foreach ( const Entry& e, mEntries )
		out << entryToLine( e ) << "\n";
}
