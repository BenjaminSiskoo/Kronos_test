/*  Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

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
#ifndef UIDEBUGVDP2VIEWER_H
#define UIDEBUGVDP2VIEWER_H

#include "ui_UIDebugVDP2Viewer.h"
#include "../QtYabause.h"

class UIDebugVDP2Viewer : public QDialog, public Ui::UIDebugVDP2Viewer
{
    Q_OBJECT
public:
    UIDebugVDP2Viewer(QWidget *parent = 0);
    ~UIDebugVDP2Viewer();
    void addItem(int i);
    void clearItems();
    int  exec() override;
    void showEvent(QShowEvent *) override;
    void refresh();   // called by UIDebugVDP2 on Next Frame

private:
    void displayCurrentScreen();
    void updateVdp2Registers();
    void updateStats();
    void updateColorRam();
    void updateVramHex();
    // Met à jour les données de l'onglet actuellement affiché dans
    // tabWidget (Registers/Debug/Color RAM/VRAM Hex). L'onglet Screen
    // Viewer (index 0) n'est pas géré ici : son rendu est piloté par les
    // signaux de cbScreen et par displayCurrentScreen() dans refresh().
    // Utilisé par refresh(), showEvent() et on_tabWidget_currentChanged()
    // pour éviter la duplication et les incohérences entre ces trois points
    // d'entrée.
    void refreshActiveTab();

protected:
	void wheelEvent(QWheelEvent *event) override;
    pixel_t *vdp2texture = NULL;
    int width = 0, height = 0;

private:
    // Mémorisation de la sélection courante de cbScreen entre un
    // clearItems() et les addItem() qui suivent, pour la restaurer si la
    // couche précédemment affichée est toujours active (cf. clearItems() /
    // addItem() dans le .cpp). Sans ça, updateScreenInfos() (appelé à
    // chaque "Next Frame") faisait revenir la sélection sur la première
    // couche active à chaque frame.
    bool mRestoreScreenId = false;
    int  mScreenIdToRestore = 0;

protected slots:
    void on_cbScreen_currentIndexChanged(int index);
    void on_pbSaveAsBitmap_clicked();
    void on_cbOpaque_toggled(bool enable);
    void on_tabWidget_currentChanged(int index);
    void on_cbVramBank_currentIndexChanged(int index);
    void on_pbVramGo_clicked();
    void on_cbCramHex_toggled(bool checked);
    void on_pbExportDebugInfo_clicked();
};

#endif // UIDEBUGVDP2VIEWER_H
