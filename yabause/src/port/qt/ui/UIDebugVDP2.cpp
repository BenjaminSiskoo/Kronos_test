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
#include <QClipboard>
#include <QTimer>
#include <QLabel>
#include <QProgressBar>
#include <QStatusBar>

// ─────────────────────────────────────────────────────────────────────────────
//  Structure de mappage : fonctions noyau → widgets UI
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    void (*debugStats)(char *, int *);
    QGroupBox      *cb;
    QPlainTextEdit *pte;
    int             layerId;
    const char     *layerName;   // [NEW] nom lisible pour la copie et les tooltips
} debugItem_s;

// ─────────────────────────────────────────────────────────────────────────────
//  Constructeur
// ─────────────────────────────────────────────────────────────────────────────
UIDebugVDP2::UIDebugVDP2(QWidget* p, YabauseLocker *lock)
    : QDialog(p)
    , mLock(lock)
    , mAutoTimer(new QTimer(this))
    , mAutoRunning(false)
    , mFrameCount(0)
{
    setupUi(this);
    viewer = new UIDebugVDP2Viewer(this);

    // ── [NEW] Barre de progression sur pbNextButton ───────────────────────
    // On place un QProgressBar dans la même ligne que le bouton Next Frame.
    // Le widget pbProgress doit exister dans le .ui, sinon créez-le ici :
    //   pbProgress->setMaximum(0); // mode indéterminé (spinning)
    //   pbProgress->setMaximum(1); // mode discret (0 = caché, 1 = plein)
    pbProgress->setRange(0, 1);
    pbProgress->setValue(1);
    pbProgress->setVisible(false);
    pbProgress->setMaximumHeight(4);       // fine barre sous le bouton
    pbProgress->setTextVisible(false);

    // ── [NEW] Compteur de frame dans la titlebar ──────────────────────────
    updateWindowTitle();

    // ── [NEW] Timer auto (intervalle par défaut : 1 frame ~ 16 ms NTSC) ──
    mAutoTimer->setInterval(16);
    connect(mAutoTimer, &QTimer::timeout, this, &UIDebugVDP2::autoStep);

    // ── [NEW] Tooltip des boutons fixes ──────────────────────────────────
    pbNextButton->setToolTip(tr("Avancer d'une frame (pas-à-pas)"));
    pbViewer->setToolTip(tr("Ouvrir le visualiseur VDP2 (non-bloquant)"));
    pbAutoButton->setToolTip(tr("Lancer/arrêter l'avance automatique frame par frame"));
    pbCopyAll->setToolTip(tr("Copier toutes les statistiques des couches actives dans le presse-papier"));

    updateScreenInfos();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Destructeur
// ─────────────────────────────────────────────────────────────────────────────
UIDebugVDP2::~UIDebugVDP2()
{
    // Stopper le timer avant la destruction pour éviter tout slot appelé sur
    // des objets déjà détruits.
    mAutoTimer->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  [NEW] Met à jour le titre de la fenêtre avec le numéro de frame
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::updateWindowTitle()
{
    if (!Vdp2Regs)
        setWindowTitle(tr("VDP2 Debug — inactif"));
    else
        setWindowTitle(tr("VDP2 Debug — Frame #%1").arg(mFrameCount, 4, 10, QChar('0')));
}

// ─────────────────────────────────────────────────────────────────────────────
//  [NEW] Met à jour la barre de statut permanente (signal, résolution, H/V)
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::updateStatusBar()
{
    if (!Vdp2Regs) {
        lStatusBar->setText(tr("VDP2 non initialisé"));
        lStatusBar->setStyleSheet("color: palette(mid);");
        lStatusDot->setStyleSheet(
            "background: #888; border-radius: 4px; min-width:8px; max-width:8px;"
            "min-height:8px; max-height:8px;");
        return;
    }

    // Signal PAL / NTSC — bit 0 de TVSTAT
    const bool isPal  = (Vdp2Regs->TVSTAT & 0x1) != 0;

    // Résolution — TVMD bits 1-0 = HRES, bits 5-4 = VRES
    static const char *hres[] = {"320","352","640","704"};
    static const char *vres[] = {"224","240","256","448/480"};
    const int hIdx = Vdp2Regs->TVMD & 0x3;
    const int vIdx = (Vdp2Regs->TVMD >> 4) & 0x3;

    // Mode entrelacement — TVMD bits 7-6
    static const char *lsmd[] = {"Non-entrelacé","(rsvd)","Simple","Double"};
    const int lIdx = (Vdp2Regs->TVMD >> 6) & 0x3;

    // Affichage actif — TVMD bit 15
    const bool disp = (Vdp2Regs->TVMD >> 15) & 1;

    // Compteurs H / V (lecture directe des registres)
    const int hcnt = Vdp2Regs->HCNT;
    const int vcnt = Vdp2Regs->VCNT;

    lStatusBar->setText(
        tr("%1  %2×%3  %4  H:%5 V:%6%7")
            .arg(isPal ? "PAL" : "NTSC")
            .arg(hres[hIdx]).arg(vres[vIdx])
            .arg(lsmd[lIdx])
            .arg(hcnt, 3).arg(vcnt, 3)
            .arg(disp ? "" : tr("  [DISP=OFF]"))
    );
    lStatusBar->setStyleSheet("color: palette(windowText);");

    // Dot de statut : vert si DISP=ON, gris sinon
    lStatusDot->setStyleSheet(
        disp
        ? "background: #5cb85c; border-radius: 4px;"
          " min-width:8px; max-width:8px; min-height:8px; max-height:8px;"
        : "background: #888; border-radius: 4px;"
          " min-width:8px; max-width:8px; min-height:8px; max-height:8px;"
    );
}

// ─────────────────────────────────────────────────────────────────────────────
//  [NEW] Message contextuel dans le footer
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::setFooterMessage(const QString &msg, bool isError)
{
    lFooter->setText(msg);
    lFooter->setStyleSheet(isError ? "color: #c0392b;" : "color: palette(mid);");
}

// ─────────────────────────────────────────────────────────────────────────────
//  updateScreenInfos  — reconstruit la grille des couches actives
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::updateScreenInfos()
{
    debugItem_s items[8] = {
        {Vdp2DebugStatsNBG0,    NBG0Debug,    pteNBG0Info,    0, "NBG0"},
        {Vdp2DebugStatsNBG1,    NBG1Debug,    pteNBG1Info,    1, "NBG1"},
        {Vdp2DebugStatsNBG2,    NBG2Debug,    pteNBG2Info,    2, "NBG2"},
        {Vdp2DebugStatsNBG3,    NBG3Debug,    pteNBG3Info,    3, "NBG3"},
        {Vdp2DebugStatsRBG0,    RBG0Debug,    pteRBG0Info,    4, "RBG0"},
        {Vdp2DebugStatsRBG1,    RBG1Debug,    pteRBG1Info,    5, "RBG1"},
        {Vdp2DebugStatsSprite,  SPRITEDebug,  pteSPRITEInfo,  6, "SPRITE"},
        {Vdp2DebugStatsGeneral, GeneralDebug, pteGeneralInfo, 7, "GENERAL"}
    };

    // ── VDP2 inactif ──────────────────────────────────────────────────────
    if (!Vdp2Regs)
    {
        for (int i = 0; i < 8; i++) {
            DebugGrid->removeWidget(items[i].cb);
            items[i].cb->setVisible(false);
        }
        pbViewer->setEnabled(false);
        pbAutoButton->setEnabled(false);
        pbCopyAll->setEnabled(false);
        pbNextButton->setEnabled(mLock != nullptr);
        updateStatusBar();
        updateWindowTitle();
        setFooterMessage(tr("VDP2 non initialisé — lancez l'émulation"), true);
        QtYabause::retranslateWidget(this);
        return;
    }

    // ── VDP2 actif ────────────────────────────────────────────────────────
    pbViewer->setEnabled(true);
    pbAutoButton->setEnabled(true);
    pbCopyAll->setEnabled(true);
    pbNextButton->setEnabled(true);

    int activeCount = 0;
    viewer->clearItems();

    for (int i = 0; i < 8; i++) {
        DebugGrid->removeWidget(items[i].cb);
        items[i].cb->setVisible(false);

        bool isVisible = updateInfoDisplay(items[i].debugStats, items[i].cb, items[i].pte);

        if (isVisible) {
            DebugGrid->addWidget(items[i].cb, activeCount / 3, activeCount % 3);
            activeCount++;
            viewer->addItem(items[i].layerId);
        }
    }

    // ── [NEW] Compteur de couches actives dans le label dédié ────────────
    lLayerCount->setText(tr("%1 / 8 couches actives").arg(activeCount));

    updateStatusBar();
    updateWindowTitle();

    if (!mAutoRunning)
        setFooterMessage(tr("Frame #%1 — %2 couche(s) active(s)")
                         .arg(mFrameCount, 4, 10, QChar('0'))
                         .arg(activeCount));

    QtYabause::retranslateWidget(this);
}

// ─────────────────────────────────────────────────────────────────────────────
//  updateInfoDisplay  — met à jour un GroupBox / PlainTextEdit pour une couche
// ─────────────────────────────────────────────────────────────────────────────
bool UIDebugVDP2::updateInfoDisplay(void (*debugStats)(char *, int *),
                                    QGroupBox *cb, QPlainTextEdit *pte)
{
    char tempstr[4096];
    memset(tempstr, 0, sizeof(tempstr));
    int isScreenEnabled = 0;

    debugStats(tempstr, &isScreenEnabled);
    tempstr[sizeof(tempstr) - 1] = '\0';

    if (isScreenEnabled) {
        cb->setVisible(true);
        QString newText = QString::fromUtf8(tempstr);

        // Mise à jour conditionnelle pour éviter le scintillement
        if (pte->toPlainText() != newText)
            pte->setPlainText(newText);

        // [NEW] Tooltip : résumé des 3 premières lignes au survol
        QStringList lines = newText.split('\n', Qt::SkipEmptyParts);
        cb->setToolTip(lines.mid(0, 3).join('\n'));

        // [NEW] Indicateur visuel : titre en gras, bordure gauche colorée
        cb->setStyleSheet(
            "QGroupBox { font-weight: bold;"
            " border-left: 3px solid #2980b9;"
            " padding-left: 4px; }"
        );
    } else {
        cb->setVisible(false);
        cb->setToolTip(tr("Couche inactive"));
        cb->setStyleSheet("");

        if (!pte->toPlainText().isEmpty())
            pte->clear();
    }

    return (isScreenEnabled != 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slot : bouton "Viewer"
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::on_pbViewer_clicked()
{
    // show() non-bloquant + raise() si déjà ouvert
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
    setFooterMessage(tr("Visualiseur ouvert"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slot : bouton "Next Frame"
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::on_pbNextButton_clicked()
{
    if (!mLock)
        return;

    // [NEW] Feedback visuel immédiat : barre de progression + curseur d'attente
    pbNextButton->setEnabled(false);
    pbProgress->setVisible(true);
    pbProgress->setValue(0);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    mLock->step();
    mFrameCount++;
    updateScreenInfos();

    if (viewer->isVisible())
        viewer->refresh();

    // Restauration
    pbProgress->setValue(1);
    QApplication::restoreOverrideCursor();
    pbNextButton->setEnabled(true);

    // Cacher la barre après un court délai pour que l'utilisateur la voie
    QTimer::singleShot(300, this, [this]{ pbProgress->setVisible(false); });
}

// ─────────────────────────────────────────────────────────────────────────────
//  [NEW] Slot : bouton "Auto" — démarre / arrête l'avance automatique
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::on_pbAutoButton_clicked()
{
    if (!mAutoRunning) {
        // Démarrage
        mAutoRunning = true;
        mAutoTimer->start();
        pbAutoButton->setText(tr("⏹ Stop"));
        pbAutoButton->setToolTip(tr("Arrêter l'avance automatique"));
        pbNextButton->setEnabled(false);   // éviter conflits pendant l'auto
        setFooterMessage(tr("Avance automatique en cours... (frame ~16 ms)"));
    } else {
        // Arrêt
        mAutoRunning = false;
        mAutoTimer->stop();
        pbAutoButton->setText(tr("▶ Auto"));
        pbAutoButton->setToolTip(tr("Lancer l'avance automatique frame par frame"));
        pbNextButton->setEnabled(true);
        setFooterMessage(tr("Avance automatique arrêtée — frame #%1")
                         .arg(mFrameCount, 4, 10, QChar('0')));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  [NEW] Slot interne du QTimer : avance une frame en mode auto
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::autoStep()
{
    if (!mLock || !Vdp2Regs) {
        // Sécurité : arrêt automatique si VDP2 disparaît
        on_pbAutoButton_clicked();
        return;
    }

    mLock->step();
    mFrameCount++;
    updateScreenInfos();

    if (viewer->isVisible())
        viewer->refresh();

    setFooterMessage(tr("Auto — frame #%1").arg(mFrameCount, 4, 10, QChar('0')));
}

// ─────────────────────────────────────────────────────────────────────────────
//  [NEW] Slot : bouton "Copier tout" — copie les stats de toutes les couches
// ─────────────────────────────────────────────────────────────────────────────
void UIDebugVDP2::on_pbCopyAll_clicked()
{
    if (!Vdp2Regs) {
        setFooterMessage(tr("Rien à copier : VDP2 inactif"), true);
        return;
    }

    // On parcourt les 8 PlainTextEdit et on concatène le contenu non vide
    static const struct { QPlainTextEdit *UIDebugVDP2::*pte; const char *name; } fields[] = {
        { &UIDebugVDP2::pteNBG0Info,    "NBG0"    },
        { &UIDebugVDP2::pteNBG1Info,    "NBG1"    },
        { &UIDebugVDP2::pteNBG2Info,    "NBG2"    },
        { &UIDebugVDP2::pteNBG3Info,    "NBG3"    },
        { &UIDebugVDP2::pteRBG0Info,    "RBG0"    },
        { &UIDebugVDP2::pteRBG1Info,    "RBG1"    },
        { &UIDebugVDP2::pteSPRITEInfo,  "SPRITE"  },
        { &UIDebugVDP2::pteGeneralInfo, "GENERAL" },
    };

    QString result;
    result += tr("=== VDP2 Debug — Frame #%1 ===\n\n")
                  .arg(mFrameCount, 4, 10, QChar('0'));

    for (const auto &f : fields) {
        const QString txt = (this->*f.pte)->toPlainText();
        if (!txt.isEmpty()) {
            result += QString("--- %1 ---\n").arg(f.name);
            result += txt;
            result += "\n\n";
        }
    }

    QApplication::clipboard()->setText(result);
    setFooterMessage(tr("Stats copiées dans le presse-papier ✓"));

    // [NEW] Feedback visuel temporaire sur le bouton
    const QString orig = pbCopyAll->text();
    pbCopyAll->setText(tr("✓ Copié !"));
    pbCopyAll->setEnabled(false);
    QTimer::singleShot(1200, this, [this, orig]{
        pbCopyAll->setText(orig);
        pbCopyAll->setEnabled(true);
    });
}
