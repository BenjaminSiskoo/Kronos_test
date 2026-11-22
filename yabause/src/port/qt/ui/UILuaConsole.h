/*  Lua script console (Kronos). GPL v2+ */
#ifndef UILUACONSOLE_H
#define UILUACONSOLE_H

#include "../QtYabause.h"
#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

/* Non-modal: scripts run while the game runs. */
class UILuaConsole : public QDialog
{
	Q_OBJECT

public:
	explicit UILuaConsole( QWidget* parent = 0 );

private slots:
	void onBrowse();
	void onLoad();
	void onStop();
	void onLog( const QString& line );
	void onStateChanged( bool loaded );

private:
	void updateStatus();

	QLineEdit* mPath;
	QPlainTextEdit* mLog;
	QLabel* mStatus;
	QPushButton* mLoad;
	QPushButton* mStop;
};

#endif // UILUACONSOLE_H
