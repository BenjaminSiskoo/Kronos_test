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

/* ============================================================================
 *  Fenetre "Debug VDP2", sur le modele de UIDebugSCSPChan :
 *    - en-tete : titre + pastilles (resolution, mode CRAM, Live / En pause) ;
 *    - carte "Couches" : une barre par ecran (hauteur = priorite, couleur =
 *      etat), cases d'affichage (disptoggle), badges CC/OFS/W/Z/LC/SH ;
 *    - carte "Detail" : texte de Vdp2DebugStatsXXX() regroupe par theme dans
 *      un arbre, alertes en tete ;
 *    - carte "VRAM" : creneaux T0-T7 des quatre banques ;
 *    - pied : Viewer, export texte, image suivante, Live / Pause, fermer.
 *
 *  Sources : ST-058-R2 (VDP2 User's Manual) pour les registres ; regles
 *  d'affichage reprises de isEnabled() (vidcs.c) pour rester coherent avec
 *  ce que le renderer dessine vraiment.
 * ========================================================================== */

#include "UIDebugVDP2.h"
#include "UIDebugVDP2Viewer.h"
#include "CommonDialogs.h"
#include "../YabauseThread.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstring>

extern "C" {
#include "vdp1.h"
#include "vdp2.h"
#include "vdp2debug.h"
}

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QColor mixColors(const QColor &a, const QColor &b, double t)
{
    return QColor((int)(a.red() * t + b.red() * (1.0 - t)),
                  (int)(a.green() * t + b.green() * (1.0 - t)),
                  (int)(a.blue() * t + b.blue() * (1.0 - t)));
}

// Texte secondaire qui suit le theme (clair ou sombre)
QColor dimColor(const QWidget *w)
{
    return mixColors(w->palette().color(QPalette::WindowText), w->palette().color(QPalette::Window), 0.6);
}

QFont monoFont()
{
    QFont f(QString::fromLatin1("Courier New"));
    f.setStyleHint(QFont::Monospace);
    return f;
}

const char *kLaneNames[Vdp2LayerStrip::LaneCount] = {
    "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG1", "SPR", "BACK"
};

const char *kTabNames[8] = { "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG1", "Sprite", "General" };

typedef void (*StatsFn)(char *, int *);
const StatsFn kStats[8] = {
    Vdp2DebugStatsNBG0, Vdp2DebugStatsNBG1, Vdp2DebugStatsNBG2, Vdp2DebugStatsNBG3,
    Vdp2DebugStatsRBG0, Vdp2DebugStatsRBG1, Vdp2DebugStatsSprite, Vdp2DebugStatsGeneral
};

// Priorite du champ 3 bits bas / haut d'un registre PRIxx
inline int prioLo(u16 r) { return r & 7; }
inline int prioHi(u16 r) { return (r >> 8) & 7; }

} // namespace

// ============================================================
//  Vdp2LayerStrip
// ============================================================

Vdp2LayerStrip::Vdp2LayerStrip(QWidget *p) : QWidget(p)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    for (int i = 0; i < LaneCount; i++) lanes_[i].name = QString::fromLatin1(kLaneNames[i]);
}

QSize Vdp2LayerStrip::sizeHint() const { return QSize(760, 250); }
QSize Vdp2LayerStrip::minimumSizeHint() const { return QSize(480, 190); }

void Vdp2LayerStrip::setLanes(const Vdp2LaneInfo *lanes)
{
    for (int i = 0; i < LaneCount; i++) lanes_[i] = lanes[i];
    update();
}

void Vdp2LayerStrip::setSelected(int lane)
{
    if (lane < 0 || lane >= LaneCount) return;
    sel_ = lane;
    update();
}

// Couleurs d'etat : memes teintes que la legende de SlotMeterWidget.
QColor Vdp2LayerStrip::colorForState(int state, const QPalette &pal)
{
    switch (state)
    {
        case Vdp2LaneInfo::Shown:      return QColor(70, 167, 88);    // affiche
        case Vdp2LaneInfo::Masked:     return QColor(229, 72, 77);    // masque par une regle
        case Vdp2LaneInfo::UserHidden: return QColor(222, 160, 40);   // masque par l'utilisateur
        case Vdp2LaneInfo::Prio0:      return QColor(62, 99, 221);    // priorite 0
        default:                       return pal.color(QPalette::Mid);
    }
}

Vdp2LayerStrip::Geo Vdp2LayerStrip::geometry() const
{
    const QFontMetrics fm(font());
    const int margin = 8, axisW = fm.horizontalAdvance(QLatin1String("7")) + 10;
    const int legendH = fm.height() + 10;
    const int nameH = fm.height() + 4;
    const int badgeH = fm.height() + 2;
    const int toggleH = fm.height() + 6;
    Geo g;
    g.area = QRectF(margin + axisW, margin,
                    width() - 2 * margin - axisW,
                    height() - 2 * margin - legendH - nameH - badgeH - toggleH);
    g.laneW = g.area.width() / LaneCount;
    g.toggleTop = g.area.bottom() + nameH + badgeH;
    g.toggleH = toggleH;
    return g;
}

int Vdp2LayerStrip::laneAt(const QPointF &p, bool *onToggle) const
{
    const Geo g = geometry();
    if (p.x() < g.area.left() || p.x() >= g.area.right()) return -1;
    const int lane = (int)((p.x() - g.area.left()) / g.laneW);
    if (lane < 0 || lane >= LaneCount) return -1;
    *onToggle = (p.y() >= g.toggleTop && p.y() < g.toggleTop + g.toggleH);
    if (p.y() < g.area.top() || p.y() >= g.toggleTop + g.toggleH) return -1;
    return lane;
}

void Vdp2LayerStrip::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    bool onToggle = false;
    const int lane = laneAt(e->pos(), &onToggle);
    if (lane < 0) return;
    if (onToggle && lanes_[lane].togglable)
    {
        if (this->onToggle) this->onToggle(lane);
    }
    else
    {
        setSelected(lane);
        if (onSelect) onSelect(lane);
    }
}

bool Vdp2LayerStrip::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip)
    {
        QHelpEvent *he = static_cast<QHelpEvent *>(e);
        bool onToggle = false;
        const int lane = laneAt(he->pos(), &onToggle);
        if (lane >= 0)
        {
            QString tip = lanes_[lane].name + QLatin1String("\n") + lanes_[lane].why;
            if (onToggle && lanes_[lane].togglable)
                tip = tr_("Click to show or hide this layer in the emulator output (takes effect on the next frame).");
            QToolTip::showText(he->globalPos(), tip, this);
        }
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

void Vdp2LayerStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QFontMetrics fm(font());
    const Geo g = geometry();
    if (g.area.width() < 64 || g.area.height() < 24) return;

    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = dimColor(this);
    const double stepH = g.area.height() / 7.0;

    // Graduations 0-7
    for (int q = 0; q <= 7; q++)
    {
        const double y = g.area.bottom() - q * stepH;
        painter.setPen(QPen(mixColors(text, pal.color(QPalette::Base), q == 0 ? 0.3 : 0.1), 1));
        painter.drawLine(QPointF(g.area.left(), y), QPointF(g.area.right(), y));
        painter.setPen(dim);
        painter.drawText(QRectF(0, y - fm.height() / 2.0, g.area.left() - 6, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, QString::number(q));
    }

    const double barW = qMax(8.0, g.laneW * 0.55);
    for (int i = 0; i < LaneCount; i++)
    {
        const Vdp2LaneInfo &L = lanes_[i];
        const double x0 = g.area.left() + i * g.laneW;
        const double x = x0 + (g.laneW - barW) / 2.0;

        if (i == sel_)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(mixColors(pal.color(QPalette::Highlight), pal.color(QPalette::Base), 0.18));
            painter.drawRoundedRect(QRectF(x0 + 2, g.area.top() - 4, g.laneW - 4,
                                           g.toggleTop + g.toggleH - g.area.top() + 2), 6, 6);
        }

        // Piste
        painter.setPen(Qt::NoPen);
        painter.setBrush(pal.color(QPalette::AlternateBase));
        painter.drawRoundedRect(QRectF(x, g.area.top(), barW, g.area.height()), 3, 3);

        // Barre : priorite. Le sprite a 8 registres : barre jusqu'au max,
        // partie basse (jusqu'au min) plus claire.
        const QColor c = colorForState(L.state, pal);
        const double h = (L.state == Vdp2LaneInfo::Off) ? 2.0 : qMax(3.0, L.priority * stepH);
        painter.setBrush(c);
        painter.drawRoundedRect(QRectF(x, g.area.bottom() - h, barW, h), 3, 3);
        if (L.prioMin > 0 && L.prioMin < L.priority && L.state != Vdp2LaneInfo::Off)
        {
            painter.setBrush(mixColors(c, pal.color(QPalette::Base), 0.55));
            painter.drawRoundedRect(QRectF(x, g.area.bottom() - L.prioMin * stepH, barW, L.prioMin * stepH), 3, 3);
        }

        // Nom
        QFont bold = font(); bold.setBold(i == sel_);
        painter.setFont(bold);
        painter.setPen(L.state == Vdp2LaneInfo::Off ? dim : text);
        painter.drawText(QRectF(x0, g.area.bottom() + 3, g.laneW, fm.height() + 2),
                         Qt::AlignHCenter | Qt::AlignTop, L.name);
        painter.setFont(font());

        // Badges
        QFont smallFont = font(); smallFont.setPointSizeF(qMax(6.0, font().pointSizeF() * 0.8));
        painter.setFont(smallFont);
        painter.setPen(dim);
        painter.drawText(QRectF(x0, g.area.bottom() + fm.height() + 5, g.laneW, fm.height()),
                         Qt::AlignHCenter | Qt::AlignTop, L.badges.join(QLatin1Char(' ')));
        painter.setFont(font());

        // Case d'affichage
        if (L.togglable)
        {
            const double s = qMin(g.toggleH - 6, 14.0);
            const QRectF box(x0 + (g.laneW - s) / 2.0, g.toggleTop + (g.toggleH - s) / 2.0, s, s);
            painter.setPen(QPen(pal.color(QPalette::Mid), 1));
            painter.setBrush(L.userVisible ? pal.color(QPalette::Highlight) : pal.color(QPalette::Base));
            painter.drawRoundedRect(box, 3, 3);
            if (L.userVisible)
            {
                painter.setPen(QPen(pal.color(QPalette::HighlightedText), 2));
                painter.drawLine(QPointF(box.left() + s * 0.22, box.top() + s * 0.55),
                                 QPointF(box.left() + s * 0.42, box.top() + s * 0.75));
                painter.drawLine(QPointF(box.left() + s * 0.42, box.top() + s * 0.75),
                                 QPointF(box.left() + s * 0.80, box.top() + s * 0.28));
            }
        }
    }

    // Legende
    struct { const char *text; int state; } legend[] = {
        { "Displayed", Vdp2LaneInfo::Shown }, { "Hidden by a rule", Vdp2LaneInfo::Masked },
        { "Priority 0", Vdp2LaneInfo::Prio0 }, { "Hidden by you", Vdp2LaneInfo::UserHidden },
        { "BGON off", Vdp2LaneInfo::Off }
    };
    double lx = g.area.left();
    const double ly = height() - 8 - fm.height() - 2;
    for (size_t k = 0; k < sizeof(legend) / sizeof(legend[0]); k++)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colorForState(legend[k].state, pal));
        painter.drawRoundedRect(QRectF(lx, ly + 3, 10, 10), 2, 2);
        const QString t = tr_(legend[k].text);
        const int tw = fm.horizontalAdvance(t);
        painter.setPen(text);
        painter.drawText(QRectF(lx + 14, ly, tw + 4, fm.height() + 2), Qt::AlignLeft | Qt::AlignVCenter, t);
        lx += 14 + tw + 22;
    }
}

// ============================================================
//  Vdp2VramCycleWidget
// ============================================================

Vdp2VramCycleWidget::Vdp2VramCycleWidget(QWidget *p) : QWidget(p)
{
    memset(cmd_, 0xF, sizeof(cmd_));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);
}

QSize Vdp2VramCycleWidget::sizeHint() const { return QSize(520, 4 * 30 + 30); }
QSize Vdp2VramCycleWidget::minimumSizeHint() const { return QSize(360, 4 * 22 + 26); }

// ST-058-R2 Table 3.5 (Access command)
QString Vdp2VramCycleWidget::commandName(int cmd)
{
    switch (cmd & 0xF)
    {
        case 0x0: return QLatin1String("N0 PN");  case 0x1: return QLatin1String("N1 PN");
        case 0x2: return QLatin1String("N2 PN");  case 0x3: return QLatin1String("N3 PN");
        case 0x4: return QLatin1String("N0 CP");  case 0x5: return QLatin1String("N1 CP");
        case 0x6: return QLatin1String("N2 CP");  case 0x7: return QLatin1String("N3 CP");
        case 0xC: return QLatin1String("N0 VCS"); case 0xD: return QLatin1String("N1 VCS");
        case 0xE: return QLatin1String("CPU");    case 0xF: return QString();
        default:  return QLatin1String("PROHIB");
    }
}

void Vdp2VramCycleWidget::refreshFromRegs()
{
    valid_ = (Vdp2Regs != NULL);
    if (!valid_) { update(); return; }
    const Vdp2 *r = Vdp2Regs;
    // CYCxnL = T0-T3 (nibble haut = T0), CYCxnU = T4-T7
    const u16 lo[4] = { r->CYCA0L, r->CYCA1L, r->CYCB0L, r->CYCB1L };
    const u16 hi[4] = { r->CYCA0U, r->CYCA1U, r->CYCB0U, r->CYCB1U };
    for (int b = 0; b < 4; b++)
        for (int t = 0; t < 4; t++)
        {
            cmd_[b][t]     = (lo[b] >> (12 - 4 * t)) & 0xF;
            cmd_[b][t + 4] = (hi[b] >> (12 - 4 * t)) & 0xF;
        }
    split_[0] = (r->RAMCTL >> 8) & 1;   // VRAMD
    split_[1] = (r->RAMCTL >> 9) & 1;   // VRBMD
    for (int b = 0; b < 4; b++) rdbs_[b] = (r->RAMCTL >> (2 * b)) & 3;
    rbg0On_ = (r->BGON & 0x10) != 0;
    hires_ = (r->TVMD & 0x2) != 0;      // HRESO 2/3/6/7 : 640/704
    update();
}

QRectF Vdp2VramCycleWidget::cellRect(int bank, int slot) const
{
    const QFontMetrics fm(font());
    const double labelW = fm.horizontalAdvance(QLatin1String("A1 ")) + 10;
    const double headH = fm.height() + 6;
    const double rowH = (height() - headH - 2) / 4.0;
    const double colW = (width() - labelW - 2) / 8.0;
    return QRectF(labelW + slot * colW + 1, headH + bank * rowH + 1, colW - 2, rowH - 2);
}

bool Vdp2VramCycleWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip && valid_)
    {
        QHelpEvent *he = static_cast<QHelpEvent *>(e);
        static const char *bankName[4] = { "VRAM-A0", "VRAM-A1", "VRAM-B0", "VRAM-B1" };
        static const char *rdbsName[4] = { "", "coefficient table", "pattern name table", "character / bitmap data" };
        for (int b = 0; b < 4; b++)
            for (int t = 0; t < 8; t++)
                if (cellRect(b, t).contains(he->pos()))
                {
                    QString tip = QString::fromLatin1("%1  T%2 : 0x%3  %4").arg(QLatin1String(bankName[b])).arg(t)
                                  .arg(cmd_[b][t], 1, 16).arg(commandName(cmd_[b][t]).isEmpty()
                                       ? tr_("no access") : commandName(cmd_[b][t]));
                    const int phys = (b & 1) && !split_[b >> 1] ? -1 : b;
                    if (phys < 0) tip += QLatin1Char('\n') + tr_("Bank not partitioned: this register is not used.");
                    else if (rbg0On_ && rdbs_[b] != 0)
                        tip += QLatin1Char('\n') + tr_("Reserved for RBG0 (%1): cycle pattern ignored (ST-058-R2 p.149).")
                                                   .arg(tr_(rdbsName[rdbs_[b]]));
                    if (hires_ && t >= 4) tip += QLatin1Char('\n') + tr_("High resolution: only T0-T3 are used.");
                    QToolTip::showText(he->globalPos(), tip, this);
                    return true;
                }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

void Vdp2VramCycleWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QFontMetrics fm(font());
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = dimColor(this);
    if (!valid_) { painter.setPen(dim); painter.drawText(rect(), Qt::AlignCenter, tr_("VDP2 not initialised.")); return; }

    // Une teinte par ecran : PN = clair, CP = plein, VCS = hachure
    const QColor layerCol[4] = { QColor(62, 99, 221), QColor(70, 167, 88), QColor(142, 78, 198), QColor(18, 165, 148) };
    static const char *bankName[4] = { "A0", "A1", "B0", "B1" };
    static const char *rdbsShort[4] = { "", "RBG0 coef.", "RBG0 PN", "RBG0 char." };

    for (int t = 0; t < 8; t++)
    {
        const QRectF c = cellRect(0, t);
        painter.setPen((hires_ && t >= 4) ? dim : text);
        painter.drawText(QRectF(c.left(), 0, c.width(), fm.height() + 4), Qt::AlignCenter, QString::fromLatin1("T%1").arg(t));
    }

    QFont smallFont = font(); smallFont.setPointSizeF(qMax(6.0, font().pointSizeF() * 0.85));
    for (int b = 0; b < 4; b++)
    {
        const QRectF first = cellRect(b, 0);
        painter.setFont(font());
        painter.setPen(text);
        painter.drawText(QRectF(0, first.top(), first.left() - 4, first.height()), Qt::AlignRight | Qt::AlignVCenter,
                         QLatin1String(bankName[b]));

        const bool unused = (b & 1) && !split_[b >> 1];
        const bool rbg = !unused && rbg0On_ && rdbs_[b] != 0;
        if (unused || rbg)
        {
            const QRectF row(first.left(), first.top(), cellRect(b, 7).right() - first.left(), first.height());
            painter.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DashLine));
            painter.setBrush(rbg ? mixColors(QColor(222, 160, 40), pal.color(QPalette::Base), 0.25) : Qt::NoBrush);
            painter.drawRoundedRect(row, 4, 4);
            painter.setPen(dim);
            const QString msg = unused ? tr_("not partitioned (%1 covers the whole bank)").arg(QLatin1String(bankName[b - 1]))
                                       : tr_("reserved for %1 - cycle pattern ignored").arg(QLatin1String(rdbsShort[rdbs_[b]]));
            painter.drawText(row, Qt::AlignCenter, fm.elidedText(msg, Qt::ElideRight, (int)row.width() - 8));
            continue;
        }

        painter.setFont(smallFont);
        for (int t = 0; t < 8; t++)
        {
            const QRectF c = cellRect(b, t);
            const int cmd = cmd_[b][t];
            const bool ignored = hires_ && t >= 4;
            QColor fill = pal.color(QPalette::AlternateBase);
            Qt::BrushStyle style = Qt::SolidPattern;
            if (cmd <= 7)      fill = (cmd < 4) ? mixColors(layerCol[cmd & 3], pal.color(QPalette::Base), 0.45) : layerCol[cmd & 3];
            else if (cmd == 0xC || cmd == 0xD) { fill = layerCol[cmd & 1]; style = Qt::BDiagPattern; }
            else if (cmd == 0xE) fill = mixColors(text, pal.color(QPalette::Base), 0.35);
            else if (cmd != 0xF) fill = QColor(229, 72, 77);
            if (ignored) fill = mixColors(fill, pal.color(QPalette::Base), 0.3);

            painter.setPen(QPen(pal.color(QPalette::Mid), 1));
            painter.setBrush(pal.color(QPalette::Base));
            painter.drawRoundedRect(c, 3, 3);
            painter.setBrush(QBrush(fill, style));
            painter.drawRoundedRect(c, 3, 3);
            QString n = commandName(cmd);
            // Nom complet si la case est assez large, sinon forme courte
            // (PN0, CP1, VC0, CPU, !!) : la carte se lit aussi a 360 px.
            if (!n.isEmpty() && QFontMetrics(smallFont).horizontalAdvance(n) > c.width() - 4)
            {
                if (cmd <= 3)       n = QString::fromLatin1("PN%1").arg(cmd);
                else if (cmd <= 7)  n = QString::fromLatin1("CP%1").arg(cmd - 4);
                else if (cmd == 0xC || cmd == 0xD) n = QString::fromLatin1("VC%1").arg(cmd - 0xC);
                else if (cmd != 0xE) n = QLatin1String("!!");
            }
            if (!n.isEmpty())
            {
                painter.setPen((cmd >= 4 && cmd <= 7) || cmd == 0x8 || cmd == 0x9 || cmd == 0xA || cmd == 0xB
                               ? QColor(Qt::white) : text);
                painter.drawText(c, Qt::AlignCenter, n);
            }
        }
    }
    painter.setFont(font());
}

// ============================================================
//  UIDebugVDP2
// ============================================================

UIDebugVDP2::UIDebugVDP2(QWidget* p, YabauseLocker *lock)
    : QDialog(p), mLock(lock)
{
    setupUi(this);
    viewer = new UIDebugVDP2Viewer(this);
    buildUi();
    applyStyle();
    updateAll();
    QtYabause::retranslateWidget(this);
}

UIDebugVDP2::~UIDebugVDP2()
{
    // Si Live tournait, on remet l'emulation en pause : le YabauseLocker du
    // menu (UIYabause::on_aViewDebugVDP2_triggered) relancera ensuite
    // l'emulation seulement si elle tournait a l'ouverture de la fenetre.
    if (mLive) setLive(false);
}

YabauseThread *UIDebugVDP2::emuThread() const
{
    UIYabause *w = QtYabause::mainWindow(false);
    return w ? w->mYabauseThread : NULL;
}

void UIDebugVDP2::buildUi()
{
    QVBoxLayout *main = new QVBoxLayout(this);
    main->setContentsMargins(16, 14, 16, 12);
    main->setSpacing(12);

    // ---- En-tete ----
    QHBoxLayout *header = new QHBoxLayout();
    QLabel *lTitle = new QLabel(tr_("VDP2"), this);
    lTitle->setObjectName("lTitle");
    lChipRes = new QLabel(this);   lChipRes->setObjectName("lChip");
    lChipCram = new QLabel(this);  lChipCram->setObjectName("lChip");
    lChipState = new QLabel(this); lChipState->setObjectName("lChip");
    header->addWidget(lTitle);
    header->addStretch(1);
    header->addWidget(lChipRes);
    header->addWidget(lChipCram);
    header->addWidget(lChipState);
    main->addLayout(header);

    lInactive = new QLabel(tr_("VDP2 not initialised: start a game to inspect it."), this);
    lInactive->setProperty("dim", true);
    lInactive->setAlignment(Qt::AlignCenter);
    main->addWidget(lInactive);

    body = new QWidget(this);
    QVBoxLayout *bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(12);
    main->addWidget(body, 1);

    // ---- Carte Couches ----
    QFrame *layerCard = new QFrame(body);
    layerCard->setObjectName("panel");
    QVBoxLayout *lc = new QVBoxLayout(layerCard);
    lc->setContentsMargins(16, 12, 16, 10);
    lc->setSpacing(6);
    QHBoxLayout *lh = new QHBoxLayout();
    QLabel *lLayers = new QLabel(tr_("Layers"), layerCard);
    lLayers->setObjectName("lSection");
    lStripHint = new QLabel(tr_("Bar height = priority. Click a bar for details, the box under a name to show or hide the layer."), layerCard);
    lStripHint->setProperty("dim", true);
    lStripHint->setWordWrap(true);
    pbShowAll = new QPushButton(tr_("Show all"), layerCard);
    pbShowAll->setObjectName("pbFlat");
    pbShowAll->setCursor(Qt::PointingHandCursor);
    lh->addWidget(lLayers);
    lh->addSpacing(8);
    lh->addWidget(lStripHint, 1);
    lh->addWidget(pbShowAll);
    lc->addLayout(lh);
    strip = new Vdp2LayerStrip(layerCard);
    lc->addWidget(strip, 1);
    QLabel *lBadges = new QLabel(tr_("Badges: CC color calculation, OFS color offset, W window, Z zoom, LC line color insertion, SH shadow, MZ mosaic."), layerCard);
    lBadges->setProperty("dim", true);
    lBadges->setWordWrap(true);
    lc->addWidget(lBadges);

    // ---- Carte Detail + carte VRAM, separees par un splitter ----
    QSplitter *split = new QSplitter(Qt::Horizontal, body);
    split->setChildrenCollapsible(false);

    QFrame *detailCard = new QFrame(split);
    detailCard->setObjectName("panel");
    QVBoxLayout *dc = new QVBoxLayout(detailCard);
    dc->setContentsMargins(16, 12, 16, 12);
    dc->setSpacing(8);
    lDetailTitle = new QLabel(detailCard);
    lDetailTitle->setObjectName("lSection");
    dc->addWidget(lDetailTitle);
    QHBoxLayout *tabRow = new QHBoxLayout();
    tabRow->setSpacing(4);
    tabs = new QButtonGroup(this);
    tabs->setExclusive(true);
    for (int i = 0; i < 8; i++)
    {
        QPushButton *b = new QPushButton(QString::fromLatin1(kTabNames[i]), detailCard);
        b->setObjectName("pbTab");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        tabs->addButton(b, i);
        tabRow->addWidget(b);
        // Lambda plutot que QButtonGroup::buttonClicked(int), obsolete
        // depuis Qt 5.15 (remplace par idClicked, absent avant).
        connect(b, &QPushButton::clicked, this, [this, i]() { onTabClicked(i); });
    }
    tabRow->addStretch(1);
    dc->addLayout(tabRow);
    tree = new QTreeWidget(detailCard);
    tree->setColumnCount(2);
    tree->setHeaderLabels(QStringList() << tr_("Field") << tr_("Value"));
    tree->setRootIsDecorated(true);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setStretchLastSection(true);
    tree->setFrameShape(QFrame::NoFrame);
    dc->addWidget(tree, 1);

    QFrame *vramCard = new QFrame(split);
    vramCard->setObjectName("panel");
    QVBoxLayout *vc = new QVBoxLayout(vramCard);
    vc->setContentsMargins(16, 12, 16, 12);
    vc->setSpacing(8);
    QLabel *lVram = new QLabel(tr_("VRAM access (cycle patterns)"), vramCard);
    lVram->setObjectName("lSection");
    vc->addWidget(lVram);
    vram = new Vdp2VramCycleWidget(vramCard);
    vc->addWidget(vram);
    QLabel *lVramLegend = new QLabel(tr_("PN pattern name (light), CP character data (solid), VCS vertical cell scroll (hatched), CPU access (grey), PROHIB setting not allowed (red)."), vramCard);
    lVramLegend->setProperty("dim", true);
    lVramLegend->setWordWrap(true);
    vc->addWidget(lVramLegend);
    lVramInfo = new QLabel(vramCard);
    lVramInfo->setWordWrap(true);
    lVramInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lVramInfo->setFont(monoFont());
    vc->addWidget(lVramInfo);
    vc->addStretch(1);

    split->addWidget(detailCard);
    split->addWidget(vramCard);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);

    bl->addWidget(layerCard, 2);
    bl->addWidget(split, 3);

    // ---- Pied ----
    QHBoxLayout *footer = new QHBoxLayout();
    pbViewer = new QPushButton(tr_("Viewer && Registers..."), this);
    pbViewer->setObjectName("pbViewer");
    pbViewer->setToolTip(tr_("Open the VDP2 viewer"));
    pbExport = new QPushButton(tr_("Export (.txt)..."), this);
    pbNextButton = new QPushButton(tr_("Next Frame"), this);
    pbNextButton->setObjectName("pbNextButton");
    pbNextButton->setToolTip(tr_("Advance one frame"));
    pbLive = new QPushButton(tr_("Live"), this);
    pbLive->setCheckable(true);
    pbLive->setToolTip(tr_("Let the game run and refresh this window 10 times per second"));
    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    footer->addWidget(pbViewer);
    footer->addWidget(pbExport);
    footer->addStretch(1);
    footer->addWidget(pbNextButton);
    footer->addWidget(pbLive);
    footer->addSpacing(12);
    footer->addWidget(bb);
    main->addLayout(footer);

    // Boutons crees apres setupUi() : connexions explicites (des slots
    // nommes on_<objet>_<signal> feraient avertir connectSlotsByName).
    connect(pbViewer, SIGNAL(clicked()), this, SLOT(onViewerClicked()));
    connect(pbNextButton, SIGNAL(clicked()), this, SLOT(onNextFrame()));
    connect(pbLive, SIGNAL(toggled(bool)), this, SLOT(onLiveToggled(bool)));
    connect(pbExport, SIGNAL(clicked()), this, SLOT(onExport()));
    connect(pbShowAll, SIGNAL(clicked()), this, SLOT(onShowAll()));
    connect(bb, SIGNAL(rejected()), this, SLOT(reject()));

    strip->onSelect = [this](int lane) { onTabClicked(lane); };
    strip->onToggle = [this](int lane) {
        if (lane >= 0 && lane <= 5)  Vdp2External.disptoggle ^= (1 << lane);   // meme bit que ToggleNBG0..ToggleRBG1
        else if (lane == 6)          Vdp1External.disptoggle ^= 1;            // ToggleVDP1
        updateAll();
    };

    liveTimer = new QTimer(this);
    liveTimer->setInterval(100);
    connect(liveTimer, SIGNAL(timeout()), this, SLOT(onLiveTick()));

    tabs->button(0)->setChecked(true);
    pbLive->setEnabled(mLock != NULL && emuThread() != NULL);
    pbNextButton->setEnabled(mLock != NULL);
}

void UIDebugVDP2::applyStyle()
{
    setStyleSheet(QString::fromLatin1("QLabel[dim=\"true\"] { color: %1; }").arg(dimColor(this).name()) + QString::fromLatin1(
        "QFrame#panel { background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }"
        "QLabel#lTitle { font-size: 18pt; font-weight: 600; }"
        "QLabel#lSection { font-weight: 600; font-size: 11pt; }"
        "QLabel#lChip { border: 1px solid palette(mid); border-radius: 10px; padding: 2px 10px; font-weight: 600; }"
        "QLabel#lChip[live=\"true\"] { border-color: #46a758; color: #46a758; }"
        "QPushButton#pbFlat, QPushButton#pbTab { border: 1px solid palette(mid); border-radius: 8px; padding: 4px 12px; background: transparent; }"
        "QPushButton#pbFlat:hover, QPushButton#pbTab:hover { border-color: palette(highlight); }"
        "QPushButton#pbTab:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
        "QPushButton#pbTab:disabled, QPushButton#pbFlat:disabled { color: palette(mid); }"
        "QTreeWidget { background: transparent; }"));
}

// ------------------------------------------------------------
//  Etat des couches. Regles de isEnabled() (vidcs.c, ST-058-R2 sec.4.1 et
//  p.61) : RBG0+RBG1 coupent NBG0-3 ; RBG1 sans RBG0 est interdit ; NBG0
//  en 16M coupe NBG1-3, en 2048+ coupe NBG2 ; NBG1 en 2048+ coupe NBG3.
//  Priorite 0 = ecran transparent (le renderer ne le dessine pas).
// ------------------------------------------------------------
void UIDebugVDP2::computeLanes(Vdp2LaneInfo *L) const
{
    for (int i = 0; i < Vdp2LayerStrip::LaneCount; i++)
    {
        L[i] = Vdp2LaneInfo();
        L[i].name = QString::fromLatin1(kLaneNames[i]);
    }
    if (!Vdp2Regs) return;
    const Vdp2 *r = Vdp2Regs;

    const bool r0on = r->BGON & 0x10, r1on = r->BGON & 0x20;
    const bool n0on = r->BGON & 0x1, n1on = r->BGON & 0x2;
    const int n0color = (r->CHCTLA >> 4) & 7, n1color = (r->CHCTLA >> 12) & 3;
    const int prio[6] = { prioLo(r->PRINA), prioHi(r->PRINA), prioLo(r->PRINB), prioHi(r->PRINB),
                          prioLo(r->PRIR), prioLo(r->PRINA) };
    // bits CCCTL / CLOFEN / LNCLEN / SDCTL / MZCTL de chaque ecran (RBG1 partage ceux de NBG0)
    const int bit[6] = { 0, 1, 2, 3, 4, 0 };
    // octet de WCTLx : NBG0 WCTLA bas, NBG1 haut, NBG2 WCTLB bas, NBG3 haut, RBG0 WCTLC bas, RBG1 = NBG0
    const u16 wctl[6] = { (u16)(r->WCTLA & 0xFF), (u16)(r->WCTLA >> 8), (u16)(r->WCTLB & 0xFF),
                          (u16)(r->WCTLB >> 8), (u16)(r->WCTLC & 0xFF), (u16)(r->WCTLA & 0xFF) };

    for (int i = 0; i < 6; i++)
    {
        Vdp2LaneInfo &l = L[i];
        l.priority = prio[i];
        l.togglable = true;
        l.userVisible = (Vdp2External.disptoggle >> i) & 1;
        const bool on = (r->BGON >> i) & 1;
        QStringList why;
        if (!on) why << tr_("BGON bit off");
        if (on && i <= 3 && r0on && r1on) why << tr_("RBG0 and RBG1 are both on: NBG0-3 are not displayed");
        if (on && i == 5 && !r0on) why << tr_("RBG1 without RBG0 is not allowed");
        if (on && i >= 1 && i <= 3 && n0on && n0color >= 4) why << tr_("NBG0 uses 16M colors");
        else if (on && i == 2 && n0on && n0color >= 2) why << tr_("NBG0 uses 2048 colors or more");
        if (on && i == 3 && n1on && n1color >= 2) why << tr_("NBG1 uses 2048 colors or more");

        if (!on) l.state = Vdp2LaneInfo::Off;
        else if (why.size() > 0) l.state = Vdp2LaneInfo::Masked;
        else if (prio[i] == 0) { l.state = Vdp2LaneInfo::Prio0; why << tr_("priority 0: transparent"); }
        else if (!l.userVisible) { l.state = Vdp2LaneInfo::UserHidden; why << tr_("hidden from the emulator output by you"); }
        else l.state = Vdp2LaneInfo::Shown;

        if (r->CCCTL & (1 << bit[i]))  l.badges << QLatin1String("CC");
        if (r->CLOFEN & (1 << bit[i])) l.badges << QLatin1String("OFS");
        if (wctl[i] & 0x2A)            l.badges << QLatin1String("W");
        if (r->LNCLEN & (1 << bit[i])) l.badges << QLatin1String("LC");
        if (r->SDCTL & (1 << bit[i]))  l.badges << QLatin1String("SH");
        if (r->MZCTL & (1 << bit[i]))  l.badges << QLatin1String("MZ");
        if ((i == 0 && ((r->ZMXN0.all & 0x7FF00) != 0x10000 || (r->ZMYN0.all & 0x7FF00) != 0x10000)) ||
            (i == 1 && ((r->ZMXN1.all & 0x7FF00) != 0x10000 || (r->ZMYN1.all & 0x7FF00) != 0x10000)))
            l.badges << QLatin1String("Z");

        l.why = tr_("Priority %1").arg(prio[i]) + QLatin1Char('\n') + why.join(QLatin1Char('\n'));
    }

    // Sprite : 8 registres de priorite PRISA-PRISD (2 champs chacun)
    {
        Vdp2LaneInfo &l = L[6];
        const u16 pr[4] = { r->PRISA, r->PRISB, r->PRISC, r->PRISD };
        int mx = 0, mn = 7;
        QStringList vals;
        for (int k = 0; k < 8; k++)
        {
            const int v = (k & 1) ? prioHi(pr[k >> 1]) : prioLo(pr[k >> 1]);
            mx = qMax(mx, v); mn = qMin(mn, v);
            vals << QString::number(v);
        }
        l.priority = mx;
        l.prioMin = mn;
        l.togglable = true;
        l.userVisible = Vdp1External.disptoggle != 0;
        l.state = !l.userVisible ? Vdp2LaneInfo::UserHidden : (mx == 0 ? Vdp2LaneInfo::Prio0 : Vdp2LaneInfo::Shown);
        if (r->CCCTL & 0x40)   l.badges << QLatin1String("CC");
        if (r->CLOFEN & 0x40)  l.badges << QLatin1String("OFS");
        if ((r->WCTLC >> 8) & 0x2A) l.badges << QLatin1String("W");
        if (r->LNCLEN & 0x20)  l.badges << QLatin1String("LC");
        l.why = tr_("Sprite type %1, priorities PR0-PR7: %2 (bar: lowest to highest)")
                    .arg(r->SPCTL & 0xF, 0, 16).arg(vals.join(QLatin1String(" ")));
    }

    // Ecran de fond : toujours derriere tout le reste
    {
        Vdp2LaneInfo &l = L[7];
        l.state = Vdp2LaneInfo::Shown;
        l.priority = 0;
        if (r->CLOFEN & 0x20) l.badges << QLatin1String("OFS");
        if (r->SDCTL & 0x20)  l.badges << QLatin1String("SH");
        l.why = (r->BKTAU & 0x8000) ? tr_("Back screen: one color per line") : tr_("Back screen: single color");
        l.why += QLatin1Char('\n') + tr_("Always behind every other layer. Click for the general registers.");
    }
}

void UIDebugVDP2::updateHeader()
{
    if (!Vdp2Regs)
    {
        lChipRes->setText(tr_("inactive"));
        lChipCram->clear(); lChipCram->hide();
        return;
    }
    static const int hres[8] = { 320, 352, 640, 704, 320, 352, 640, 704 };
    static const int vres[4] = { 224, 240, 256, 0 };
    static const char *lsmd[4] = { "NI", "?", "SDI", "DDI" };
    const int v = vres[(Vdp2Regs->TVMD >> 4) & 3];
    lChipRes->setText(QString::fromLatin1("%1x%2 %3 %4%5")
        .arg(hres[Vdp2Regs->TVMD & 7]).arg(v ? QString::number(v) : QLatin1String("?"))
        .arg((Vdp2Regs->TVSTAT & 1) ? QLatin1String("PAL") : QLatin1String("NTSC"))
        .arg(QLatin1String(lsmd[(Vdp2Regs->TVMD >> 6) & 3]))
        .arg((Vdp2Regs->TVMD & 0x8000) ? QString() : QString::fromLatin1("  ") + tr_("display off")));
    lChipCram->show();
    lChipCram->setText(tr_("CRAM mode %1").arg((Vdp2Regs->RAMCTL >> 12) & 3));
}

QString UIDebugVDP2::statsText(int tab, bool *enabled) const
{
    char buf[VDP2_DEBUG_STRING_SIZE];
    memset(buf, 0, sizeof(buf));
    int en = 0;
    if (Vdp2Regs && tab >= 0 && tab < 8) kStats[tab](buf, &en);
    buf[sizeof(buf) - 1] = '\0';
    if (enabled) *enabled = en != 0;
    return QString::fromUtf8(buf).remove(QLatin1Char('\r'));
}

// ------------------------------------------------------------
//  Arbre de detail. Reconstruit seulement si le texte a change (en Live, 10
//  fois par seconde, il change rarement) ; la position de defilement est
//  conservee.
// ------------------------------------------------------------
void UIDebugVDP2::showDetail(int tab)
{
    bool en = false;
    const QString text = statsText(tab, &en);
    lDetailTitle->setText(tr_("Detail - %1").arg(QString::fromLatin1(kTabNames[tab])));
    if (tab == mShownTab && text == mShownText) return;

    const int scroll = tree->verticalScrollBar()->value();
    const bool sameTab = (tab == mShownTab);
    mShownTab = tab;
    mShownText = text;

    tree->setUpdatesEnabled(false);
    tree->clear();
    if (!en)
    {
        QTreeWidgetItem *it = new QTreeWidgetItem(tree);
        it->setText(0, tr_("Layer disabled (BGON bit off)"));
        it->setFirstColumnSpanned(true);
        it->setForeground(0, dimColor(this));
        tree->setUpdatesEnabled(true);
        return;
    }

    // Regroupement partage avec l'onglet Debug du viewer (UIDebugVDP2Viewer.cpp).
    Vdp2FillStatsTree(tree, NULL, text, tab == 7);
    tree->setUpdatesEnabled(true);
    if (sameTab) tree->verticalScrollBar()->setValue(scroll);
}

void UIDebugVDP2::updateViewerLayers()
{
    // Les couches dont la fonction de stats dit qu'elles sont actives.
    // La liste du viewer n'est reconstruite que si cet ensemble change :
    // chaque clearItems()/addItem() change l'index de cbScreen, et le
    // viewer relit alors la texture de la couche (glGetTexImage) meme
    // quand il est ferme -- deux fois par image avec l'ancien code, et
    // ce serait 10 fois par seconde en Live.
    int mask = 0;
    if (Vdp2Regs)
        for (int i = 0; i < 7; i++)
        {
            bool en = false;
            statsText(i, &en);
            if (en) mask |= 1 << i;
        }
    if (mask == mViewerMask) return;
    mViewerMask = mask;
    viewer->clearItems();
    for (int i = 0; i < 7; i++)
        if (mask & (1 << i)) viewer->addItem(i);   // 0-5 = NBG0..RBG1, 6 = SPRITE (enBG)
}

void UIDebugVDP2::updateAll()
{
    const bool active = (Vdp2Regs != NULL);
    lInactive->setVisible(!active);
    body->setVisible(active);
    pbViewer->setEnabled(active);
    pbExport->setEnabled(active);
    pbViewer->setToolTip(active ? tr_("Open the VDP2 viewer") : tr_("VDP2 not initialised."));
    setWindowTitle(active ? tr_("VDP2 Debug") : tr_("VDP2 Debug - inactive"));
    lChipState->setText(mLive ? tr_("Live") : tr_("Paused"));
    lChipState->setProperty("live", mLive);
    lChipState->style()->unpolish(lChipState);
    lChipState->style()->polish(lChipState);
    updateHeader();
    if (!active) { viewer->clearItems(); mViewerMask = -1; return; }

    Vdp2LaneInfo lanes[Vdp2LayerStrip::LaneCount];
    computeLanes(lanes);
    strip->setLanes(lanes);

    bool anyHidden = !(Vdp1External.disptoggle);
    for (int i = 0; i < 6; i++) if (!((Vdp2External.disptoggle >> i) & 1)) anyHidden = true;
    pbShowAll->setEnabled(anyHidden);

    for (int i = 0; i < 8; i++)
    {
        bool en = (i >= 6);
        if (i < 6) statsText(i, &en);
        QAbstractButton *b = tabs->button(i);
        QFont f = b->font(); f.setBold(en); b->setFont(f);
    }
    showDetail(mTab);

    vram->refreshFromRegs();
    {
        const Vdp2 *r = Vdp2Regs;
        static const char *crmd[4] = { "RGB555 x1024", "RGB555 x2048", "RGB888 x1024", "prohibited" };
        lVramInfo->setText(QString::fromLatin1("RAMCTL %1  CRAM %2%3\nVRSIZE %4  VRAM-A %5  VRAM-B %6")
            .arg(r->RAMCTL, 4, 16, QLatin1Char('0'))
            .arg(QLatin1String(crmd[(r->RAMCTL >> 12) & 3]))
            .arg((r->RAMCTL & 0x8000) ? QString::fromLatin1(", ") + tr_("coef. in CRAM (CRKTE)") : QString())
            .arg((r->VRSIZE & 0x8000) ? QLatin1String("8Mbit") : QLatin1String("4Mbit"))
            .arg((r->RAMCTL & 0x100) ? tr_("split A0/A1") : tr_("single"))
            .arg((r->RAMCTL & 0x200) ? tr_("split B0/B1") : tr_("single")));
    }

    updateViewerLayers();
}

void UIDebugVDP2::onTabClicked(int id)
{
    if (id < 0 || id > 7) return;
    mTab = id;
    if (tabs->button(id)) tabs->button(id)->setChecked(true);
    strip->setSelected(id);   // couloirs 0-7 : NBG0..RBG1, SPR, BACK (= General)
    showDetail(id);
}

void UIDebugVDP2::onShowAll()
{
    Vdp2External.disptoggle |= 0x3F;
    Vdp1External.disptoggle = 1;
    updateAll();
}

void UIDebugVDP2::onViewerClicked()
{
    // show() non bloquant : le viewer reste ouvert pendant Next Frame / Live.
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
}

void UIDebugVDP2::onNextFrame()
{
    if (mLock == NULL || mLive) return;
    pbNextButton->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    mLock->step();
    updateAll();
    if (viewer->isVisible()) viewer->refresh();
    QApplication::restoreOverrideCursor();
    pbNextButton->setEnabled(true);
}

// ------------------------------------------------------------
//  Live : l'emulation tourne sur le thread GUI (YabauseGL::event), la boucle
//  modale de cette fenetre continue donc a la faire avancer, comme pour
//  UIDebugSCSPChan. On ne fait que lever puis remettre la pause.
// ------------------------------------------------------------
void UIDebugVDP2::setLive(bool live)
{
    YabauseThread *t = emuThread();
    if (t == NULL || !t->emulationRunning()) live = false;
    if (t != NULL && t->emulationRunning()) t->pauseEmulation(!live, false);
    mLive = live;
    if (live) liveTimer->start(); else liveTimer->stop();
    pbNextButton->setEnabled(!live && mLock != NULL);
    if (pbLive->isChecked() != live) { pbLive->blockSignals(true); pbLive->setChecked(live); pbLive->blockSignals(false); }
    pbLive->setText(live ? tr_("Pause") : tr_("Live"));
}

void UIDebugVDP2::onLiveToggled(bool live)
{
    setLive(live);
    updateAll();
    if (!live && viewer->isVisible()) viewer->refresh();
}

void UIDebugVDP2::onLiveTick()
{
    updateAll();
    // Le viewer relit des textures GL : 2 fois par seconde suffit.
    if (viewer->isVisible() && (++mTick % 5) == 0) viewer->refresh();
}

QString UIDebugVDP2::fullReport() const
{
    QString out;
    QTextStream ts(&out);
    ts << "Kronos VDP2 debug report - " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << lChipRes->text() << "  " << lChipCram->text() << "\n\n";

    Vdp2LaneInfo lanes[Vdp2LayerStrip::LaneCount];
    computeLanes(lanes);
    static const char *st[] = { "off", "displayed", "hidden by a rule", "priority 0", "hidden by user" };
    ts << "=== Layers ===\n";
    for (int i = 0; i < Vdp2LayerStrip::LaneCount; i++)
        ts << QString::fromLatin1("  %1  prio %2  %3  [%4]  %5\n").arg(lanes[i].name, -5).arg(lanes[i].priority)
              .arg(QLatin1String(st[lanes[i].state]), -17).arg(lanes[i].badges.join(QLatin1Char(' ')))
              .arg(lanes[i].why.split(QLatin1Char('\n'), Qt::SkipEmptyParts).join(QLatin1String(" / ")));

    if (Vdp2Regs)
    {
        static const char *bn[4] = { "A0", "A1", "B0", "B1" };
        const u16 lo[4] = { Vdp2Regs->CYCA0L, Vdp2Regs->CYCA1L, Vdp2Regs->CYCB0L, Vdp2Regs->CYCB1L };
        const u16 hi[4] = { Vdp2Regs->CYCA0U, Vdp2Regs->CYCA1U, Vdp2Regs->CYCB0U, Vdp2Regs->CYCB1U };
        ts << "\n=== VRAM cycle patterns (RAMCTL " << QString::number(Vdp2Regs->RAMCTL, 16) << ") ===\n";
        for (int b = 0; b < 4; b++)
        {
            ts << "  " << bn[b] << ":";
            if ((b & 1) && !((Vdp2Regs->RAMCTL >> (8 + (b >> 1))) & 1))
            { ts << " (not partitioned, register unused)\n"; continue; }
            if ((Vdp2Regs->BGON & 0x10) && ((Vdp2Regs->RAMCTL >> (2 * b)) & 3))
                ts << " [reserved for RBG0, ignored]";
            for (int t = 0; t < 8; t++)
            {
                const int c = (((t < 4) ? lo[b] : hi[b]) >> (12 - 4 * (t & 3))) & 0xF;
                const QString n = Vdp2VramCycleWidget::commandName(c);
                ts << " " << QString(n.isEmpty() ? QLatin1String("--") : n).leftJustified(6);
            }
            ts << "\n";
        }
    }
    for (int i = 0; i < 8; i++)
    {
        bool en = false;
        const QString t = statsText(i, &en);
        ts << "\n=== " << kTabNames[i] << " ===\n" << (en ? t : QString::fromLatin1("(disabled)\n"));
    }
    return out;
}

void UIDebugVDP2::onExport()
{
    const QString suggested = QString::fromLatin1("vdp2_debug_%1.txt")
        .arg(QDateTime::currentDateTime().toString(QLatin1String("yyyyMMdd_hhmmss")));
    QString path = CommonDialogs::getSaveFileName(suggested, tr_("Choose a location for the VDP2 report"),
                                                  tr_("Text files (*.txt)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)) path += QLatin1String(".txt");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        CommonDialogs::error(tr_("An error occured while writing file."));
        return;
    }
    QTextStream ts(&f);
    ts << fullReport();
}
