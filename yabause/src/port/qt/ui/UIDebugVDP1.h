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
#ifndef UIDEBUGVDP1_H
#define UIDEBUGVDP1_H

#include "ui_UIDebugVDP1.h"
#include "../QtYabause.h"
#include "UIYabause.h"
#include <QImage>
#include <QByteArray>

class UIDebugVDP1 : public QDialog, public Ui::UIDebugVDP1
{
    Q_OBJECT
public:
    explicit UIDebugVDP1(QWidget* parent = 0, YabauseLocker* lock = 0);
    ~UIDebugVDP1();

protected:
    u32 *vdp1texture = NULL;
    u8  *vdp1RawTexture = NULL;
    int  vdp1RawNumBytes = 0;
    int  vdp1texturew = 1, vdp1textureh = 1;
    YabauseLocker* mLock;

    /* Copie de la VRAM VDP1 prise au moment ou la liste de commandes est
     * remplie. Tout ce que la fenetre affiche ensuite -- noms, detail,
     * texture, dump brut -- est relu dans cette copie et jamais dans la
     * VRAM vivante, qui continue d'etre reecrite par le jeu. Sans cela le
     * nom d'une ligne et son detail peuvent decrire deux commandes
     * differentes, et l'export peut contenir une liste et un dump brut qui
     * se contredisent. */
    QByteArray mVdp1RamSnapshot;
    void captureVdp1Ram();

    void fillCommandList();
    void updateVdp1Registers();
    void syncOnVdp1Entry(int cursel);
    void clearVdp1Display(); // <--- Correctement déclaré ici

protected slots:
    void on_pbSaveBitmap_clicked();
    void on_pbSaveRawSprite_clicked();
    void on_pbNextButton_clicked();
    void on_lwCommandRaw_itemSelectionChanged();
    void on_lwCommandList_itemSelectionChanged();
    void on_pbExportDebugInfo_clicked();
};

#endif // UIDEBUGVDP1_H
