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

#include "UIDebugVDP2.h"
#include "UIDebugVDP2Viewer.h"
#include "CommonDialogs.h"

// Structure de mappage pour lier les fonctions du noyau aux widgets UI
typedef struct {
    void (*debugStats)(char *, int *);
    QGroupBox    *cb;
    QPlainTextEdit *pte;
    int layerId;
} debugItem_s;

UIDebugVDP2::UIDebugVDP2(QWidget* p, YabauseLocker *lock)
    : QDialog(p), mLock(lock)
{
    setupUi(this);
    viewer = new UIDebugVDP2Viewer(this);
    updateScreenInfos();
}

UIDebugVDP2::~UIDebugVDP2()
{
    // Le viewer est un enfant de ce dialogue, Qt le gérera, 
    // mais on s'assure qu'aucune mise à jour ne se produise après destruction.
}

void UIDebugVDP2::updateScreenInfos()
{
    // Liste exhaustive de toutes les couches gérées par le VDP2
    debugItem_s items[8] = {
        {Vdp2DebugStatsNBG0,    NBG0Debug,    pteNBG0Info,    0},
        {Vdp2DebugStatsNBG1,    NBG1Debug,    pteNBG1Info,    1},
        {Vdp2DebugStatsNBG2,    NBG2Debug,    pteNBG2Info,    2},
        {Vdp2DebugStatsNBG3,    NBG3Debug,    pteNBG3Info,    3},
        {Vdp2DebugStatsRBG0,    RBG0Debug,    pteRBG0Info,    4},
        {Vdp2DebugStatsRBG1,    RBG1Debug,    pteRBG1Info,    5},
        {Vdp2DebugStatsSprite,  SPRITEDebug,  pteSPRITEInfo,  6},
        {Vdp2DebugStatsGeneral, GeneralDebug, pteGeneralInfo, 7}
    };

    if (Vdp2Regs)
    {
        int activeCount = 0;
        viewer->clearItems();

        for (int i = 0; i < 8; i++) {
            // 1. On retire le widget du layout avant de recalculer sa position
            DebugGrid->removeWidget(items[i].cb);

            // 2. Mise à jour des chaînes de caractères provenant du Core
            bool isVisible = updateInfoDisplay(items[i].debugStats, items[i].cb, items[i].pte);

            // 3. Si la couche est active (enabled), on l'ajoute à la grille dynamiquement
            if (isVisible) {
                // Placement automatique dans une grille de 3 colonnes
                DebugGrid->addWidget(items[i].cb, activeCount / 3, activeCount % 3);
                activeCount++;
                viewer->addItem(items[i].layerId);
            }
        }
    }

    QtYabause::retranslateWidget(this);
}

bool UIDebugVDP2::updateInfoDisplay(void (*debugStats)(char *, int *),
                                    QGroupBox *cb, QPlainTextEdit *pte)
{
    char tempstr[4096] = {0}; // Buffer étendu pour éviter les troncatures
    int isScreenEnabled = 0;

    // Appel à la fonction de statistiques du noyau (vdp2debug.c)
    debugStats(tempstr, &isScreenEnabled);

    if (isScreenEnabled) {
        cb->setVisible(true);
        
        // Optimisation : On ne met à jour le texte que s'il a changé pour éviter le scintillement
        QString newText = QString::fromUtf8(tempstr);
        if (pte->toPlainText() != newText) {
            pte->setPlainText(newText);
        }
    } else {
        cb->setVisible(false);
    }
    
    return (isScreenEnabled != 0);
}

void UIDebugVDP2::on_pbViewer_clicked()
{
    // Utilisation de show() au lieu d'exec() si vous voulez que le viewer 
    // soit non-bloquant, ou exec() pour un dialogue modal.
    viewer->exec();
}

void UIDebugVDP2::on_pbNextButton_clicked()
{
    if (mLock != NULL) {
        mLock->step();
        updateScreenInfos();
        viewer->refresh();
    }
}
