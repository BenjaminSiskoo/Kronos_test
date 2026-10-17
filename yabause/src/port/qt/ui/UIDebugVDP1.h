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
#include <QBrush>
#include <vector>

class QGraphicsView;
class QGraphicsRectItem;
class QLabel;

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
    // Dimensions de la texture décodée. Le buffer brut a les siennes :
    // Vdp1DebugRawTextureAtAddr() ne borne pas une largeur/hauteur nulle à 1
    // comme le fait Vdp1DebugTextureAtAddr(), et partager les mêmes variables
    // faisait construire la QImage avec les dimensions du buffer BRUT.
    int  vdp1texturew = 1, vdp1textureh = 1;
    int  vdp1RawW = 0, vdp1RawH = 0;
    YabauseLocker* mLock;
    // Copie de l'image de texture actuellement affichée (déjà convertie
    // rgbSwapped), pour que l'inspecteur de pixel (survol de gvTexture)
    // lise exactement ce qui est visible sans reconvertir le buffer brut.
    QImage currentTextureImage;

    // Adresse VRAM de chaque entrée de la liste, relevée en un seul parcours
    // de la table (Vdp1DebugGetCommandList). Le nom, le détail, la texture et
    // le buffer brut d'une ligne portent ainsi sur la même commande.
    std::vector<u32> mCmdAddrs;

    // Paramètres de la commande sélectionnée, utilisés par l'inspecteur de
    // pixel pour retrouver le code couleur brut sous le curseur.
    bool mCurIsSprite = false;
    int  mCurColorMode = 0;   // CMDPMOD bits 5-3
    bool mCurEndCode = false; // ECD = 0 -> end codes actifs
    bool mCurSPD = false;     // SPD = 1 -> code 0 opaque

    // Damier dessiné en coordonnées ÉCRAN (filtre Paint du viewport), donc
    // insensible au zoom.
    QBrush mCheckerBrush;

    // Fenêtre « Aperçu de la trame » : toutes les commandes assemblées à
    // leurs coordonnées (Vdp1DebugRenderFrame), la commande sélectionnée
    // encadrée. Non modale, créée au premier clic, rafraîchie avec la liste.
    QDialog *mFrameDlg = nullptr;
    QGraphicsView *mFrameView = nullptr;
    QLabel *mFrameInfo = nullptr;
    QGraphicsRectItem *mFrameSel = nullptr;
    QImage mFrameImage;
    std::vector<int> mFrameBoxes; // 4 entiers par commande : x0 y0 x1 y1

    void fillCommandList();
    void updateVdp1Registers();
    void syncOnVdp1Entry(int cursel);
    void clearVdp1Display();
    void fitTexture();
    bool rawCodeAt(int x, int y, u32 *code) const;
    void refreshFramePreview();
    void updateFrameSelection();
    void fitFrame();
    int  commandAtFramePoint(int x, int y) const;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

protected slots:
    void on_pbSaveBitmap_clicked();
    void on_pbSaveRawSprite_clicked();
    void on_pbNextButton_clicked();
    void on_pbResetZoom_clicked();
    void on_lwCommandRaw_itemSelectionChanged();
    void on_lwCommandList_itemSelectionChanged();
    void on_pbExportDebugInfo_clicked();
    void on_pbFramePreview_clicked();
};

#endif // UIDEBUGVDP1_H
