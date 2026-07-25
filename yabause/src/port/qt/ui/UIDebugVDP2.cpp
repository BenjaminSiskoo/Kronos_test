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
#include <QApplication>

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
    // `viewer` est parenté sur `this` (voir le constructeur) : c'est le
    // mécanisme parent/enfant de Qt qui se charge de le détruire lors de la
    // destruction de la classe de base QDialog, pas l'ordre des membres
    // C++. Rien à faire explicitement ici, ce destructeur reste vide.
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

    if (!Vdp2Regs)
    {
        // Masquer tous les groupes et indiquer visuellement que VDP2 est inactif
        for (int i = 0; i < 8; i++) {
            DebugGrid->removeWidget(items[i].cb);
            items[i].cb->setVisible(false);
        }
        // Vider aussi la liste des couches du viewer : sans ça, le combo
        // cbScreen du UIDebugVDP2Viewer garde d'anciennes entrées
        // (NBG0, NBG1, ...) qui ne correspondent plus à rien de valide
        // une fois le VDP2 désinitialisé.
        viewer->clearItems();
        pbViewer->setEnabled(false);
        pbViewer->setToolTip(tr("VDP2 non initialisé"));
        setWindowTitle(tr("VDP2 Debug — inactif"));
        QtYabause::retranslateWidget(this);
        return;
    }

    // VDP2 actif : restaurer l'état des boutons
    pbViewer->setEnabled(true);
    pbViewer->setToolTip(tr("Ouvrir le visualiseur VDP2"));
    setWindowTitle(tr("VDP2 Debug"));

    int activeCount = 0;
    viewer->clearItems();

    for (int i = 0; i < 8; i++) {
        // 1. Retirer le widget du layout (no-op si pas encore ajouté)
        DebugGrid->removeWidget(items[i].cb);
        // 2. Masquer par défaut avant de recalculer
        items[i].cb->setVisible(false);

        // 3. Mise à jour des informations et test d'activation
        bool isVisible = updateInfoDisplay(items[i].debugStats, items[i].cb, items[i].pte);

        // 4. Si la couche est active, on la replace dans la grille
        if (isVisible) {
            DebugGrid->addWidget(items[i].cb, activeCount / 3, activeCount % 3);
            activeCount++;
            viewer->addItem(items[i].layerId);
        }
    }

    QtYabause::retranslateWidget(this);
}

bool UIDebugVDP2::updateInfoDisplay(void (*debugStats)(char *, int *),
                                    QGroupBox *cb, QPlainTextEdit *pte)
{
    char tempstr[4096];
    // Garantir la null-termination même si debugStats remplit le buffer en entier
    memset(tempstr, 0, sizeof(tempstr));
    int isScreenEnabled = 0;

    // Appel à la fonction de statistiques du noyau (vdp2debug.c)
    debugStats(tempstr, &isScreenEnabled);
    // Sécurité : forcer la null-termination du dernier octet
    tempstr[sizeof(tempstr)-1] = '\0';

    if (isScreenEnabled) {
        cb->setVisible(true);
        QString newText = QString::fromUtf8(tempstr);
        // Mise à jour conditionnelle pour éviter le scintillement
        if (pte->toPlainText() != newText) {
            pte->setPlainText(newText);
        }
        // Tooltip : afficher les 3 premières lignes comme résumé au survol
        QStringList lines = newText.split('\n', Qt::SkipEmptyParts);
        cb->setToolTip(lines.mid(0, 3).join('\n'));
        // Indicateur visuel : titre en gras quand la couche est active
        cb->setStyleSheet("QGroupBox { font-weight: bold; }");
    } else {
        cb->setVisible(false);
        cb->setToolTip(tr("Couche inactive"));
        cb->setStyleSheet("");
        // Vider le texte si la couche est désactivée pour ne pas afficher de données obsolètes
        if (!pte->toPlainText().isEmpty())
            pte->clear();
    }

    return (isScreenEnabled != 0);
}

void UIDebugVDP2::on_pbViewer_clicked()
{
    // show() non-bloquant : permet de continuer à utiliser le dialogue principal
    // (cliquer "Next Frame") pendant que le viewer est ouvert.
    // raise() + activateWindow() pour ramener la fenêtre au premier plan si déjà ouverte.
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
}

void UIDebugVDP2::on_pbNextButton_clicked()
{
    if (mLock != NULL) {
        // Désactiver le bouton pendant l'exécution pour éviter les double-clics
        pbNextButton->setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);

        mLock->step();
        updateScreenInfos();

        // Mettre à jour le viewer seulement s'il est visible (évite un travail inutile)
        if (viewer->isVisible())
            viewer->refresh();

        QApplication::restoreOverrideCursor();
        pbNextButton->setEnabled(true);
    }
}
