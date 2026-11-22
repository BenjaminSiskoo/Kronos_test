/*  SH-2 cycle profiler window (Kronos). GPL v2+ */
#include "UIDebugSH2Profile.h"
#include "../CommonDialogs.h"
#include "Settings.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <QVector>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#ifndef SH2CORE_KRONOS_DEBUG_INTERPRETER
#define SH2CORE_KRONOS_DEBUG_INTERPRETER 9 /* see sh2int_kronos.h */
#endif

/* Symbols of a GNU ld link map (-Wl,-Map=file.map): lines such as
       .text.init_GFS
                    0x0000000006003cc4       0x68 ../src/gfs_wrap.o
                    0x0000000006003cc4                init_GFS
   give the input sections (address, size, object) and the symbols. An address
   is named after the nearest symbol at or below it inside the same input
   section, as "name+0x1c". */
struct ProfileSymbolMap {
	struct Sym { quint32 addr; QString name; };
	struct Range { quint32 start; quint32 end; QString object; };
	QVector<Sym> syms;
	QVector<Range> ranges;
	QString path;

	bool load( const QString& file, QString* err )
	{
		QFile f( file );
		if ( !f.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
			if ( err ) *err = f.errorString();
			return false;
		}
		syms.clear();
		ranges.clear();
		static const QRegularExpression reSym( "^\\s+0x([0-9a-fA-F]+)\\s+(?!0x)(\\S.*?)\\s*$" );
		static const QRegularExpression reSecOne( "^\\s(\\.\\S+)\\s+0x([0-9a-fA-F]+)\\s+0x([0-9a-fA-F]+)\\s+(\\S.*?)\\s*$" );
		static const QRegularExpression reSecName( "^\\s(\\.\\S+)\\s*$" );
		static const QRegularExpression reSecBody( "^\\s+0x([0-9a-fA-F]+)\\s+0x([0-9a-fA-F]+)\\s+(\\S.*?)\\s*$" );
		bool pending = false;
		while ( !f.atEnd() ) {
			const QString line = QString::fromUtf8( f.readLine() );
			QRegularExpressionMatch m;
			if ( pending ) {
				pending = false;
				m = reSecBody.match( line );
				if ( m.hasMatch() ) {
					addRange( m.captured( 1 ), m.captured( 2 ), m.captured( 3 ) );
					continue;
				}
			}
			m = reSecOne.match( line );
			if ( m.hasMatch() ) {
				addRange( m.captured( 2 ), m.captured( 3 ), m.captured( 4 ) );
				continue;
			}
			if ( reSecName.match( line ).hasMatch() ) {
				pending = true;
				continue;
			}
			m = reSym.match( line );
			if ( m.hasMatch() ) {
				const QString name = m.captured( 2 );
				if ( name.startsWith( '.' ) || name.startsWith( '=' ) || name.startsWith( "PROVIDE" ) || name.contains( " = " ) )
					continue;
				bool ok = false;
				const quint64 a = m.captured( 1 ).toULongLong( &ok, 16 );
				if ( ok && a != 0 ) {
					Sym s; s.addr = (quint32)a; s.name = name;
					syms.append( s );
				}
			}
		}
		std::stable_sort( syms.begin(), syms.end(), []( const Sym& a, const Sym& b ) { return a.addr < b.addr; } );
		std::sort( ranges.begin(), ranges.end(), []( const Range& a, const Range& b ) { return a.start < b.start; } );
		path = file;
		return !syms.isEmpty();
	}

	void addRange( const QString& a, const QString& sz, const QString& obj )
	{
		bool ok1 = false, ok2 = false;
		const quint64 start = a.toULongLong( &ok1, 16 );
		const quint64 size = sz.toULongLong( &ok2, 16 );
		if ( !ok1 || !ok2 || start == 0 || size == 0 )
			return;
		Range r; r.start = (quint32)start; r.end = (quint32)( start + size ); r.object = QFileInfo( obj ).fileName();
		ranges.append( r );
	}

	QString lookup( quint32 addr ) const
	{
		if ( syms.isEmpty() )
			return QString();
		quint32 floor = 0;
		QString object;
		if ( !ranges.isEmpty() ) {
			/* input section containing the address */
			int lo = 0, hi = ranges.size();
			while ( lo < hi ) { const int m = ( lo + hi ) / 2; if ( ranges[m].start <= addr ) lo = m + 1; else hi = m; }
			if ( lo == 0 || addr >= ranges[lo - 1].end )
				return QString();
			floor = ranges[lo - 1].start;
			object = ranges[lo - 1].object;
		}
		/* last symbol at or below the address, not below the section start */
		int lo = 0, hi = syms.size();
		while ( lo < hi ) { const int m = ( lo + hi ) / 2; if ( syms[m].addr <= addr ) lo = m + 1; else hi = m; }
		if ( lo == 0 || syms[lo - 1].addr < floor )
			return object.isEmpty() ? QString() : QString( "[%1]" ).arg( object );
		const quint32 symAddr = syms[lo - 1].addr;
		QStringList names;
		for ( int i = lo - 1; i >= 0 && syms[i].addr == symAddr && names.size() < 3; i-- )
			names.prepend( syms[i].name );
		QString out = names.join( " / " );
		if ( addr != symAddr )
			out += QString( "+0x%1" ).arg( addr - symAddr, 0, 16 );
		return out;
	}
};

namespace {
const int kMaxRows = 500;

QTableWidgetItem* numItem( const QString& text, qulonglong sortKey )
{
	QTableWidgetItem* it = new QTableWidgetItem( text );
	it->setData( Qt::UserRole, sortKey );
	it->setTextAlignment( Qt::AlignRight | Qt::AlignVCenter );
	return it;
}

void setupTable( QTableWidget* t, const QStringList& heads )
{
	t->setColumnCount( heads.size() );
	t->setHorizontalHeaderLabels( heads );
	t->setEditTriggers( QAbstractItemView::NoEditTriggers );
	t->setSelectionBehavior( QAbstractItemView::SelectRows );
	t->verticalHeader()->hide();
	t->horizontalHeader()->setStretchLastSection( true );
}
}

UIDebugSH2Profile::UIDebugSH2Profile( QWidget* parent )
	: QDialog( parent ), mSymbols( new ProfileSymbolMap )
{
	setWindowTitle( QtYabause::translate( "SH-2 cycle profiler" ) );
	setAttribute( Qt::WA_DeleteOnClose, true );
	setModal( false );
	resize( 640, 520 );

	QVBoxLayout* root = new QVBoxLayout( this );

	QHBoxLayout* top = new QHBoxLayout;
	mCpu = new QComboBox;
	mCpu->addItem( QtYabause::translate( "Master SH-2" ) );
	mCpu->addItem( QtYabause::translate( "Slave SH-2" ) );
	mStartStop = new QPushButton;
	QPushButton* reset = new QPushButton( QtYabause::translate( "Reset" ) );
	QPushButton* exp = new QPushButton( QtYabause::translate( "Export CSV..." ) );
	QPushButton* loadMap = new QPushButton( QtYabause::translate( "Load .map..." ) );
	top->addWidget( mCpu );
	top->addWidget( mStartStop );
	top->addWidget( reset );
	top->addWidget( exp );
	top->addWidget( loadMap );
	top->addStretch( 1 );
	root->addLayout( top );

	mStatus = new QLabel;
	mStatus->setWordWrap( true );
	root->addWidget( mStatus );
	mMapStatus = new QLabel( QtYabause::translate( "No link map loaded: functions are shown by address only." ) );
	mMapStatus->setWordWrap( true );
	root->addWidget( mMapStatus );

	mTabs = new QTabWidget;
	mFunctions = new QTableWidget;
	setupTable( mFunctions, QStringList()
		<< QtYabause::translate( "Function" ) << QtYabause::translate( "Symbol" )
		<< QtYabause::translate( "Self cycles" ) << QtYabause::translate( "Self %" ) << QtYabause::translate( "Total cycles" )
		<< QtYabause::translate( "Calls" ) );
	mBlocks = new QTableWidget;
	setupTable( mBlocks, QStringList()
		<< QtYabause::translate( "256-byte block" ) << QtYabause::translate( "Cycles" )
		<< QtYabause::translate( "%" ) );
	mHotspots = new QTableWidget;
	setupTable( mHotspots, QStringList()
		<< QtYabause::translate( "Instruction address" ) << QtYabause::translate( "Symbol" )
		<< QtYabause::translate( "Cycles" ) << QtYabause::translate( "%" )
		<< QtYabause::translate( "Executions" ) );
	mLoops = new QTableWidget;
	setupTable( mLoops, QStringList()
		<< QtYabause::translate( "Loop start" ) << QtYabause::translate( "Loop end" )
		<< QtYabause::translate( "Symbol (start)" ) << QtYabause::translate( "Back jumps taken" ) << QtYabause::translate( "Cycles in range" )
		<< QtYabause::translate( "%" ) << QtYabause::translate( "Cycles / back jump" ) );
	mAreas = new QTableWidget;
	setupTable( mAreas, QStringList()
		<< QtYabause::translate( "Memory area" ) << QtYabause::translate( "Cycles" )
		<< QtYabause::translate( "%" ) );
	mTabs->addTab( mFunctions, QtYabause::translate( "Functions" ) );
	mTabs->addTab( mBlocks, QtYabause::translate( "Blocks" ) );
	mTabs->addTab( mHotspots, QtYabause::translate( "Hotspots" ) );
	mTabs->addTab( mLoops, QtYabause::translate( "Loops" ) );
	mTabs->addTab( mAreas, QtYabause::translate( "Areas" ) );
	root->addWidget( mTabs, 1 );

	connect( mStartStop, SIGNAL( clicked() ), this, SLOT( onStartStop() ) );
	connect( reset, SIGNAL( clicked() ), this, SLOT( onReset() ) );
	connect( exp, SIGNAL( clicked() ), this, SLOT( onExport() ) );
	connect( loadMap, SIGNAL( clicked() ), this, SLOT( onLoadMap() ) );
	connect( mCpu, SIGNAL( currentIndexChanged( int ) ), this, SLOT( refresh() ) );

	mTimer = new QTimer( this );
	connect( mTimer, SIGNAL( timeout() ), this, SLOT( refresh() ) );
	mTimer->start( 1000 );

	/* Reload the link map used last time. */
	Settings* s = QtYabause::settings();
	if ( s != NULL ) {
		const QString last = s->value( "Profiler/MapFile" ).toString();
		if ( !last.isEmpty() && QFileInfo( last ).exists() )
			loadMapFile( last, true );
	}

	updateButtons();
	refresh();
}

UIDebugSH2Profile::~UIDebugSH2Profile()
{
	delete mSymbols;
	/* The hook costs a test per instruction: leave it off once the window is
	   gone, so closing it brings the Debug core back to its usual speed. */
	SH2ProfileSetActive( 0 );
}

QString UIDebugSH2Profile::symbolFor( u32 addr ) const
{
	return mSymbols->lookup( addr );
}

void UIDebugSH2Profile::loadMapFile( const QString& path, bool quiet )
{
	QString err;
	if ( !mSymbols->load( path, &err ) ) {
		mSymbols->syms.clear();
		mSymbols->ranges.clear();
		mSymbols->path.clear();
		mMapStatus->setText( err.isEmpty()
			? QtYabause::translate( "No symbol found in %1 (expected a GNU ld map: -Wl,-Map=file.map)." ).arg( path )
			: QtYabause::translate( "Unable to read %1: %2" ).arg( path ).arg( err ) );
		if ( !quiet )
			CommonDialogs::error( mMapStatus->text() );
		return;
	}
	mMapStatus->setText( QtYabause::translate( "Link map: %1 (%2 symbols, %3 input sections)" )
		.arg( QFileInfo( path ).fileName() ).arg( mSymbols->syms.size() ).arg( mSymbols->ranges.size() ) );
	Settings* s = QtYabause::settings();
	if ( s != NULL )
		s->setValue( "Profiler/MapFile", path );
}

void UIDebugSH2Profile::onLoadMap()
{
	const QString f = CommonDialogs::getOpenFileName( ".", QtYabause::translate( "Choose the link map" ),
		QtYabause::translate( "Link maps (*.map *.txt);;All files (*)" ) );
	if ( f.isEmpty() )
		return;
	loadMapFile( f, false );
	refresh();
}

void UIDebugSH2Profile::updateButtons()
{
	mStartStop->setText( SH2ProfileActive ? QtYabause::translate( "Stop" )
	                                      : QtYabause::translate( "Start" ) );
}

void UIDebugSH2Profile::onStartStop()
{
	SH2ProfileSetActive( SH2ProfileActive ? 0 : 1 );
	updateButtons();
	refresh();
}

void UIDebugSH2Profile::onReset()
{
	SH2ProfileReset();
	refresh();
}

void UIDebugSH2Profile::fillTable( QTableWidget* table, int cpu, bool functions, u64 total )
{
	static sh2prof_entry rows[kMaxRows];
	const int n = functions ? SH2ProfileGetFunctions( cpu, rows, kMaxRows )
	                        : SH2ProfileGetBlocks( cpu, rows, kMaxRows );
	table->setRowCount( n );
	for ( int i = 0; i < n; i++ ) {
		const double pct = total ? 100.0 * (double)rows[i].excl / (double)total : 0.0;
		QString addr = functions && rows[i].addr == 0 && rows[i].calls == 0
			? QtYabause::translate( "(top level)" )
			: QString( "%1" ).arg( rows[i].addr, 8, 16, QChar( '0' ) ).toUpper();
		int col = 0;
		table->setItem( i, col++, new QTableWidgetItem( addr ) );
		if ( functions )
			table->setItem( i, col++, new QTableWidgetItem( rows[i].addr ? symbolFor( rows[i].addr ) : QString() ) );
		table->setItem( i, col++, numItem( QString::number( rows[i].excl ), rows[i].excl ) );
		table->setItem( i, col++, numItem( QString::number( pct, 'f', 2 ), (qulonglong)( pct * 100 ) ) );
		if ( functions ) {
			table->setItem( i, col++, numItem( QString::number( rows[i].incl ), rows[i].incl ) );
			table->setItem( i, col++, numItem( QString::number( rows[i].calls ), rows[i].calls ) );
		}
	}
}

static QString hex8( u32 v )
{
	return QString( "%1" ).arg( v, 8, 16, QChar( '0' ) ).toUpper();
}

void UIDebugSH2Profile::fillHotspots( int cpu, u64 total )
{
	static sh2prof_entry rows[kMaxRows];
	const int n = SH2ProfileGetHotspots( cpu, rows, kMaxRows );
	mHotspots->setRowCount( n );
	for ( int i = 0; i < n; i++ ) {
		const double pct = total ? 100.0 * (double)rows[i].excl / (double)total : 0.0;
		mHotspots->setItem( i, 0, new QTableWidgetItem( hex8( rows[i].addr ) ) );
		mHotspots->setItem( i, 1, new QTableWidgetItem( symbolFor( rows[i].addr ) ) );
		mHotspots->setItem( i, 2, numItem( QString::number( rows[i].excl ), rows[i].excl ) );
		mHotspots->setItem( i, 3, numItem( QString::number( pct, 'f', 2 ), (qulonglong)( pct * 100 ) ) );
		mHotspots->setItem( i, 4, numItem( QString::number( rows[i].calls ), rows[i].calls ) );
	}
}

void UIDebugSH2Profile::fillLoops( int cpu, u64 total )
{
	/* A loop is the range [start, end] closed by a backward BRA/BT/BF/BT/S/BF/S.
	   "Back jumps taken" is the number of passes minus one. The delay slot of a
	   branch runs inside the branch, so its cycles are counted at the branch. */
	static sh2prof_loop rows[2048];
	const int n = SH2ProfileGetLoops( cpu, rows, 2048 );
	const int shown = n < kMaxRows ? n : kMaxRows;
	mLoops->setRowCount( shown );
	for ( int i = 0; i < shown; i++ ) {
		const double pct = total ? 100.0 * (double)rows[i].cycles / (double)total : 0.0;
		const double per = rows[i].iterations ? (double)rows[i].cycles / (double)rows[i].iterations : 0.0;
		mLoops->setItem( i, 0, new QTableWidgetItem( hex8( rows[i].head ) ) );
		mLoops->setItem( i, 1, new QTableWidgetItem( hex8( rows[i].tail ) ) );
		mLoops->setItem( i, 2, new QTableWidgetItem( symbolFor( rows[i].head ) ) );
		mLoops->setItem( i, 3, numItem( QString::number( rows[i].iterations ), rows[i].iterations ) );
		mLoops->setItem( i, 4, numItem( QString::number( rows[i].cycles ), rows[i].cycles ) );
		mLoops->setItem( i, 5, numItem( QString::number( pct, 'f', 2 ), (qulonglong)( pct * 100 ) ) );
		mLoops->setItem( i, 6, numItem( QString::number( per, 'f', 1 ), (qulonglong)( per * 10 ) ) );
	}
}

void UIDebugSH2Profile::refresh()
{
	const int cpu = mCpu->currentIndex();
	const u64 total = SH2ProfileTotalCycles( cpu );

	QString st;
	if ( SH2Core == NULL || SH2Core->id != SH2CORE_KRONOS_DEBUG_INTERPRETER )
		st = QtYabause::translate( "Select the \"SH2 Debug\" core in Settings > Advanced, then restart the emulation: the other cores have no per-instruction hook." );
	else if ( SH2ProfileActive )
		st = QtYabause::translate( "Profiling. Cycles counted: %1 (the Debug core runs slower than the Performance core)." ).arg( total );
	else
		st = QtYabause::translate( "Stopped. Cycles counted: %1." ).arg( total );
	mStatus->setText( st );
	updateButtons();

	fillTable( mFunctions, cpu, true, total );
	fillTable( mBlocks, cpu, false, total );
	fillHotspots( cpu, total );
	fillLoops( cpu, total );

	static const char* areaNames[5] = { "BIOS ROM", "Low WRAM", "High WRAM", "Cartridge", "Other" };
	mAreas->setRowCount( 5 );
	for ( int a = 0; a < 5; a++ ) {
		const u64 c = SH2ProfileAreaCycles( cpu, a );
		const double pct = total ? 100.0 * (double)c / (double)total : 0.0;
		mAreas->setItem( a, 0, new QTableWidgetItem( QtYabause::translate( areaNames[a] ) ) );
		mAreas->setItem( a, 1, numItem( QString::number( c ), c ) );
		mAreas->setItem( a, 2, numItem( QString::number( pct, 'f', 2 ), (qulonglong)( pct * 100 ) ) );
	}
}

void UIDebugSH2Profile::onExport()
{
	/* Exports the tab that is shown, cell by cell. */
	QTableWidget* t = qobject_cast<QTableWidget*>( mTabs->currentWidget() );
	if ( t == NULL )
		return;
	const QString path = CommonDialogs::getSaveFileName( ".", QtYabause::translate( "Export the profile" ),
		QtYabause::translate( "CSV files (*.csv)" ) );
	if ( path.isEmpty() )
		return;
	QFile f( path );
	if ( !f.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
		CommonDialogs::error( QtYabause::translate( "Unable to save file" ) );
		return;
	}
	QTextStream out( &f );
	QStringList heads;
	for ( int col = 0; col < t->columnCount(); col++ )
		heads << t->horizontalHeaderItem( col )->text();
	out << heads.join( ',' ) << '\n';
	for ( int row = 0; row < t->rowCount(); row++ ) {
		QStringList cells;
		for ( int col = 0; col < t->columnCount(); col++ ) {
			QTableWidgetItem* it = t->item( row, col );
			cells << ( it ? it->text() : QString() );
		}
		out << cells.join( ',' ) << '\n';
	}
}
