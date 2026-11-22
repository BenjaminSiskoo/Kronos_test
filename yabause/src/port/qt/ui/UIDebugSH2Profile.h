/*  SH-2 cycle profiler window (Kronos). GPL v2+ */
#ifndef UIDEBUGSH2PROFILE_H
#define UIDEBUGSH2PROFILE_H

#include "../QtYabause.h"
#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTimer;
struct ProfileSymbolMap;

/* Non-modal window: unlike the other debug windows it must NOT pause the
   emulation, since the profiler only collects data while the game runs.
   It needs the "SH2 Debug" core (the Performance core has no per-instruction
   hook); the window says so when another core is selected. */
class UIDebugSH2Profile : public QDialog
{
	Q_OBJECT

public:
	explicit UIDebugSH2Profile( QWidget* parent = 0 );
	~UIDebugSH2Profile();

private slots:
	void onStartStop();
	void onReset();
	void onExport();
	void onLoadMap();
	void refresh();

private:
	void fillTable( QTableWidget* table, int cpu, bool functions, u64 total );
	void fillHotspots( int cpu, u64 total );
	void loadMapFile( const QString& path, bool quiet );
	QString symbolFor( u32 addr ) const;
	void fillLoops( int cpu, u64 total );
	void updateButtons();

	QComboBox* mCpu;
	QLabel* mStatus;
	QLabel* mMapStatus;
	ProfileSymbolMap* mSymbols;
	QPushButton* mStartStop;
	QTabWidget* mTabs;
	QTableWidget* mFunctions;
	QTableWidget* mBlocks;
	QTableWidget* mHotspots;
	QTableWidget* mLoops;
	QTableWidget* mAreas;
	QTimer* mTimer;
};

#endif // UIDEBUGSH2PROFILE_H
