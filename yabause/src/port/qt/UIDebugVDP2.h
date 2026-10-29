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

#ifndef UIDEBUGVDP2_H
#define UIDEBUGVDP2_H

#include "ui_UIDebugVDP2.h"
#include "UIDebugVDP2Viewer.h"
#include "../QtYabause.h"
#include "UIYabause.h"

#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;
class QButtonGroup;

// ============================================================
//  Etat d'une couche tel qu'affiche par Vdp2LayerStrip.
//  Calcule par UIDebugVDP2::computeLanes() depuis les registres, avec les
//  memes regles que le renderer (isEnabled(), vidcs.c ; ST-058-R2 sec.4.1).
// ============================================================
struct Vdp2LaneInfo
{
    enum State { Off = 0, Shown, Masked, Prio0, UserHidden };

    QString name;
    State   state = Off;
    int     priority = 0;       // priorite de base (0-7)
    int     prioMin = 0;        // sprite : plus petite des 8 priorites
    bool    togglable = false;  // case d'affichage disponible
    bool    userVisible = true; // bit disptoggle correspondant
    QStringList badges;         // CC, OFS, W, Z, LC, SH...
    QString why;                // texte d'info-bulle
};

// ============================================================
//  Vdp2LayerStrip -- une barre par couche, hauteur = priorite, couleur =
//  etat (meme principe que SlotMeterWidget de UIDebugSCSPChan). Clic sur la
//  barre : selection. Clic sur la case sous le nom : afficher/masquer.
//  Pas de Q_OBJECT : les rappels passent par std::function.
// ============================================================
class Vdp2LayerStrip : public QWidget
{
public:
    enum { LaneCount = 8 };

    explicit Vdp2LayerStrip(QWidget *parent = 0);

    void setLanes(const Vdp2LaneInfo *lanes);
    void setSelected(int lane);
    int  selected() const { return sel_; }

    static QColor colorForState(int state, const QPalette &pal);

    std::function<void(int)> onSelect;
    std::function<void(int)> onToggle;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Geo { QRectF area; double laneW; double toggleTop; double toggleH; };
    Geo  geometry() const;
    int  laneAt(const QPointF &p, bool *onToggle) const;

    Vdp2LaneInfo lanes_[LaneCount];
    int sel_ = 0;
};

// ============================================================
//  Vdp2VramCycleWidget -- creneaux T0-T7 des quatre banques VRAM
//  (CYCA0/CYCA1/CYCB0/CYCB1, ST-058-R2 Table 3.5), avec le partage des
//  banques (RAMCTL VRAMD/VRBMD) et les banques reservees a RBG0 (RDBS),
//  dont les creneaux sont ignores (ST-058-R2 p.149).
// ============================================================
class Vdp2VramCycleWidget : public QWidget
{
public:
    explicit Vdp2VramCycleWidget(QWidget *parent = 0);
    void refreshFromRegs();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    static QString commandName(int cmd);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    QRectF cellRect(int bank, int slot) const;
    u8   cmd_[4][8];
    bool valid_ = false;
    bool split_[2] = { false, false };   // VRAM-A, VRAM-B partitionnees
    int  rdbs_[4] = { 0, 0, 0, 0 };      // usage RBG0 de chaque banque
    bool rbg0On_ = false;
    bool hires_ = false;                 // 640/704 : seuls T0-T3 comptent
};

// ============================================================
//  UIDebugVDP2
// ============================================================
class UIDebugVDP2 : public QDialog, public Ui::UIDebugVDP2
{
    Q_OBJECT
public:
    explicit UIDebugVDP2(QWidget* parent = 0, YabauseLocker *lock = 0);
    ~UIDebugVDP2();

protected:
    // `viewer` est parente sur `this` : Qt le detruit avec la fenetre.
    UIDebugVDP2Viewer *viewer;

protected slots:
    void onViewerClicked();
    void onGfxEditorClicked();
    void onNextFrame();
    void onLiveToggled(bool live);
    void onLiveTick();
    void onExport();
    void onShowAll();
    void onTabClicked(int id);

private:
    void buildUi();
    void applyStyle();
    void updateAll();
    void updateHeader();
    void computeLanes(Vdp2LaneInfo *lanes) const;
    void updateViewerLayers();
    void showDetail(int tab);
    QString statsText(int tab, bool *enabled) const;
    QString fullReport() const;
    void setLive(bool live);
    YabauseThread *emuThread() const;

    // const : le pointeur n'est jamais reassigne apres le constructeur
    YabauseLocker* const mLock;

    Vdp2LayerStrip      *strip = nullptr;
    Vdp2VramCycleWidget *vram = nullptr;
    QTreeWidget  *tree = nullptr;
    QButtonGroup *tabs = nullptr;
    QLabel *lChipRes = nullptr, *lChipCram = nullptr, *lChipState = nullptr;
    QLabel *lDetailTitle = nullptr, *lStripHint = nullptr, *lVramInfo = nullptr, *lInactive = nullptr;
    QWidget *body = nullptr;
    QPushButton *pbViewer = nullptr, *pbNextButton = nullptr, *pbLive = nullptr;
    QPushButton *pbGfxEditor = nullptr;
    QPushButton *pbExport = nullptr, *pbShowAll = nullptr;
    QTimer *liveTimer = nullptr;

    int  mTab = 0;          // 0-5 couches, 6 sprite, 7 general
    QString mShownText;     // texte deja affiche dans l'arbre (evite les reconstructions)
    int  mShownTab = -1;
    bool mLive = false;
    int  mTick = 0;
    int  mViewerMask = -1;  // couches deja listees dans le viewer
};

#endif // UIDEBUGVDP2_H
