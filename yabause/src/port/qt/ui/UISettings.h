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
#ifndef UISETTINGS_H
#define UISETTINGS_H

#include "ui_UISettings.h"
#include "../QtYabause.h"
#include "UIYabause.h"

QStringList getCdDriveList();

class UISettings : public QDialog, public Ui::UISettings
{
	Q_OBJECT

public:
	UISettings(QList <translation_struct> *translations, QWidget* parent = 0 );
	int exec() override;
	void setCurrentOpenedTab(int idx);

protected:
	QList <translation_struct> trans;
	QList <QAction*> actionsList;

	void requestFile( const QString& caption, QLineEdit* edit, const QString& filters = QString(), QString proposedPath = QString());
	void requestNewFile( const QString& caption, QLineEdit* edit, const QString& filters = QString(), QString proposedPath = QString());
	void requestFolder( const QString& caption, QLineEdit* edit, QString proposedPath = QString());
	void requestSTVFolder(const QString & caption, QLineEdit * edit, QString proposedPath = QString());
	void setupCdDrives();
	void setupCameras( const QString& selected );
	void setupSideBar();
	void refreshSideBar();
	void updateGunMouseLabel( int value );
	QList<QAction*> parentActions() const;
	void loadCores();
	void loadTranslations();
	void populateShortcutsView();
	void applyShortcuts();
	void loadSettings();
	void saveSettings();

protected slots:
	void on_leBios_textChanged(const QString & text);
	void on_cbFloppyEnabled_toggled( bool checked );
	void on_tbCameraRefresh_clicked();
	void on_leBiosSettings_textChanged(const QString & text);
	void tbBrowse_clicked();
	void on_cbInput_currentIndexChanged( int id );
	void on_cbCdRom_currentIndexChanged( int id );
	void on_cbClockSync_stateChanged( int state );
	void on_cbCartridge_currentIndexChanged( int id );
	void on_cbAutostart_toggled(bool enable);
	void on_sGunMouseSensitivity_valueChanged( int value );
	void accept();
	void reject() override;
	void changeResolution(int id);
  void changeFilterMode(int id);
  void changeUpscaleMode(int id);
  void changeAspectRatio(int id);
	void changeWireframe(int id);
	void changeMeshMode(int id);
	void changeBandingMode(int id);
	void updateSwapModeWidgets();
private:
	QString getCartridgePathSettingsKey(int cartridgeType = -1) const;
	void updateVolatileSettings() const;
	void refreshUserShaderList( const QString& select );
	void rebuildUserShaderParams();
	void applyUserShaderPreset();
	void updateUserShaderStatus();
	void loadUserShaderSettings( const Settings* s );
	void saveUserShaderSettings( Settings* s ) const;
	bool mUserShaderBusy = false;

	int selectedCartridgeType = 0;
};

#endif // UISETTINGS_H
