/*  SH-2 cycle profiler window (Kronos). GPL v2+ */
#include "UIDebugSH2Profile.h"
#include "../CommonDialogs.h"

#include <QComboBox>
#include <QFile>
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
	: QDialog( parent )
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
	top->addWidget( mCpu );
	top->addWidget( mStartStop );
	top->addWidget( reset );
	top->addWidget( exp );
	top->addStretch( 1 );
	root->addLayout( top );

	mStatus = new QLabel;
	mStatus->setWordWrap( true );
	root->addWidget( mStatus );

	mTabs = new QTabWidget;
	mFunctions = new QTableWidget;
	setupTable( mFunctions, QStringList()
		<< QtYabause::translate( "Function" ) << QtYabause::translate( "Self cycles" )
		<< QtYabause::translate( "Self %" ) << QtYabause::translate( "Total cycles" )
		<< QtYabause::translate( "Calls" ) );
	mBlocks = new QTableWidget;
	setupTable( mBlocks, QStringList()
		<< QtYabause::translate( "256-byte block" ) << QtYabause::translate( "Cycles" )
		<< QtYabause::translate( "%" ) );
	mAreas = new QTableWidget;
	setupTable( mAreas, QStringList()
		<< QtYabause::translate( "Memory area" ) << QtYabause::translate( "Cycles" )
		<< QtYabause::translate( "%" ) );
	mTabs->addTab( mFunctions, QtYabause::translate( "Functions" ) );
	mTabs->addTab( mBlocks, QtYabause::translate( "Blocks" ) );
	mTabs->addTab( mAreas, QtYabause::translate( "Areas" ) );
	root->addWidget( mTabs, 1 );

	connect( mStartStop, SIGNAL( clicked() ), this, SLOT( onStartStop() ) );
	connect( reset, SIGNAL( clicked() ), this, SLOT( onReset() ) );
	connect( exp, SIGNAL( clicked() ), this, SLOT( onExport() ) );
	connect( mCpu, SIGNAL( currentIndexChanged( int ) ), this, SLOT( refresh() ) );

	mTimer = new QTimer( this );
	connect( mTimer, SIGNAL( timeout() ), this, SLOT( refresh() ) );
	mTimer->start( 1000 );

	updateButtons();
	refresh();
}

UIDebugSH2Profile::~UIDebugSH2Profile()
{
	/* The hook costs a test per instruction: leave it off once the window is
	   gone, so closing it brings the Debug core back to its usual speed. */
	SH2ProfileSetActive( 0 );
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
	table->setSortingEnabled( false );
	table->setRowCount( n );
	for ( int i = 0; i < n; i++ ) {
		const double pct = total ? 100.0 * (double)rows[i].excl / (double)total : 0.0;
		QString addr = functions && rows[i].addr == 0 && rows[i].calls == 0
			? QtYabause::translate( "(top level)" )
			: QString( "%1" ).arg( rows[i].addr, 8, 16, QChar( '0' ) ).toUpper();
		table->setItem( i, 0, new QTableWidgetItem( addr ) );
		table->setItem( i, 1, numItem( QString::number( rows[i].excl ), rows[i].excl ) );
		if ( functions ) {
			table->setItem( i, 2, numItem( QString::number( pct, 'f', 2 ), (qulonglong)( pct * 100 ) ) );
			table->setItem( i, 3, numItem( QString::number( rows[i].incl ), rows[i].incl ) );
			table->setItem( i, 4, numItem( QString::number( rows[i].calls ), rows[i].calls ) );
		} else {
			table->setItem( i, 2, numItem( QString::number( pct, 'f', 2 ), (qulonglong)( pct * 100 ) ) );
		}
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
	const QString path = CommonDialogs::getSaveFileName( ".", QtYabause::translate( "Export the profile" ),
		QtYabause::translate( "CSV files (*.csv)" ) );
	if ( path.isEmpty() )
		return;
	QFile f( path );
	if ( !f.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
		CommonDialogs::error( QtYabause::translate( "Unable to save file" ) );
		return;
	}
	const int cpu = mCpu->currentIndex();
	static sh2prof_entry rows[4096];
	const int n = SH2ProfileGetFunctions( cpu, rows, 4096 );
	QTextStream out( &f );
	out << "address,self_cycles,total_cycles,calls\n";
	for ( int i = 0; i < n; i++ )
		out << QString( "%1" ).arg( rows[i].addr, 8, 16, QChar( '0' ) ).toUpper() << ','
		    << rows[i].excl << ',' << rows[i].incl << ',' << rows[i].calls << '\n';
}
