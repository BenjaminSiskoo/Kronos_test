/* Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/
#include "UIDebugVDP1.h"
#include "UIDebugGfxEditor.h"
#include "UIEditsExport.h"
#include "CommonDialogs.h"

#include <QImageWriter>
#include <QMessageBox>
#include <QScrollBar>
#include <QGraphicsPixmapItem>
#include <QListWidgetItem>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QApplication>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QGraphicsRectItem>
#include <QGraphicsView>
#include <QVBoxLayout>
#include <QLabel>
#include <QFont>
#include <QStringList>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPalette>
#include <QFontMetrics>
#include <QToolTip>
#include <QHelpEvent>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <algorithm>

// VDP1 system headers
extern "C" {
#include "vdp1.h"
#include "vdpedit.h"
}

namespace {

struct Vdp1CommandsCount
{
    size_t distortedSprites = 0;
    size_t polygons = 0;
    size_t polylines = 0;
    size_t normalSprites = 0;
    size_t scaledSprites = 0;
    size_t lines = 0;
};

// Prend le type déjà lu plutôt qu'un rang : Vdp1DebugGetCommandType(rang)
// reparcourait toute la table depuis l'adresse 0 à chaque appel.
void Vdp1CountCommandType(Vdp1CommandType commandType, Vdp1CommandsCount& cmdCount)
{
    switch (commandType) {
    case VDPCT_DISTORTED_SPRITE:
    case VDPCT_DISTORTED_SPRITEN:
        cmdCount.distortedSprites++;
        break;
    case VDPCT_NORMAL_SPRITE:
        cmdCount.normalSprites++;
        break;
    case VDPCT_SCALED_SPRITE:
        cmdCount.scaledSprites++;
        break;
    case VDPCT_POLYGON:
        cmdCount.polygons++;
        break;
    case VDPCT_POLYLINE:
    case VDPCT_POLYLINEN:
        cmdCount.polylines++;
        break;
    case VDPCT_LINE:
        cmdCount.lines++;
        break;
    default: break;
    }
}

// Calcul couleur des commandes de tracé, lu dans CMDPMOD (+04h) :
//   bit 15    = MON  (MSB On : seul le bit 15 du pixel du frame buffer est écrit)
//   bit 8     = Mesh
//   bits 2-0  = CCB  0 Replace, 1 Shadow, 2 Half-luminance, 3 Half-transparency,
//                    4 Gouraud, 5 interdit, 6 Gouraud + Half-luminance,
//                    7 Gouraud + Half-transparency
// (même codage que Vdp1DebugCommandAtAddr dans vdp1.c).
struct Vdp1ColorCalcCount
{
    size_t ccb[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    size_t msbOn = 0;
    size_t mesh = 0;
};

bool isVdp1DrawCommand(Vdp1CommandType type)
{
    switch (type) {
    case VDPCT_NORMAL_SPRITE:
    case VDPCT_SCALED_SPRITE:
    case VDPCT_DISTORTED_SPRITE:
    case VDPCT_DISTORTED_SPRITEN:
    case VDPCT_POLYGON:
    case VDPCT_POLYLINE:
    case VDPCT_POLYLINEN:
    case VDPCT_LINE:
        return true;
    default:
        return false;
    }
}

const char *const kCcbTag[8]  = { "", "SH", "HL", "HT", "G", "CC5!", "G+HL", "G+HT" };
const char *const kCcbName[8] = { "Replace", "Shadow", "Half-luminance", "Half-transparency",
                                  "Gouraud", "Setting prohibited (5)",
                                  "Gouraud + Half-luminance", "Gouraud + Half-transparency" };

// Teinte de fond par mode : alpha faible pour rester lisible en thème clair
// comme en thème sombre ; la couleur du texte reste celle du type.
QColor ccbBackground(int ccb)
{
    switch (ccb) {
    case 1:  return QColor(128, 128, 128, 70); // Shadow
    case 2:  return QColor(255, 190,   0, 70); // Half-luminance
    case 3:  return QColor(160,  80, 255, 70); // Half-transparency
    case 4:  return QColor(  0, 180, 255, 60); // Gouraud
    case 5:  return QColor(255,   0,   0, 80); // interdit
    case 6:  return QColor(140, 220,   0, 70); // Gouraud + HL
    case 7:  return QColor(255,  80, 170, 70); // Gouraud + HT
    default: return QColor();                  // Replace : pas de teinte
    }
}

// Étiquette ajoutée au nom de la commande, ex. "[G+HT, MSB]". Vide si rien
// d'autre que Replace sans MSB On ni Mesh.
QString colorCalcTag(u16 pmod)
{
    QStringList parts;
    const int ccb = pmod & 0x7;
    if (ccb != 0) parts << QString::fromLatin1(kCcbTag[ccb]);
    if (pmod & 0x8000) parts << QStringLiteral("MSB");
    if (pmod & 0x0100) parts << QStringLiteral("Mesh");
    return parts.isEmpty() ? QString() : QStringLiteral("[") + parts.join(QStringLiteral(", ")) + QStringLiteral("]");
}

std::string buildColorCalcLabel(const Vdp1ColorCalcCount &cc)
{
    bool previous = false;
    std::stringstream out;
    auto addStat = [&](const char *label, size_t count) {
        if (count > 0) {
            if (previous) out << ", ";
            out << label << ": " << count;
            previous = true;
        }
    };
    // Toujours affiché, Replace compris : une liste entièrement en Replace
    // donnait un résumé vide, impossible à distinguer d'une absence de
    // relevé (aucune commande de tracé lue).
    size_t drawCmds = 0;
    for (int i = 0; i < 8; i++) drawCmds += cc.ccb[i];
    if (drawCmds == 0) return "Color calc: no draw command";
    addStat("Replace", cc.ccb[0]);
    addStat("HL", cc.ccb[2]);
    addStat("HT", cc.ccb[3]);
    addStat("Gouraud", cc.ccb[4]);
    addStat("G+HL", cc.ccb[6]);
    addStat("G+HT", cc.ccb[7]);
    addStat("Shadow", cc.ccb[1]);
    addStat("Prohibited CC", cc.ccb[5]);
    addStat("MSB On", cc.msbOn);
    addStat("Mesh", cc.mesh);
    return "Color calc: " + out.str();
}

std::string buildInfoLabel(Vdp1CommandsCount& cmdCount)
{
    bool previous = false;
    std::stringstream infoLabel;

    auto addStat = [&](const char* label, size_t count) {
        if (count > 0) {
            if (previous) infoLabel << ", ";
            infoLabel << label << ": " << count;
            previous = true;
        }
    };

    addStat("Distorted", cmdCount.distortedSprites);
    addStat("Polygons", cmdCount.polygons);
    addStat("PolyLines", cmdCount.polylines);
    addStat("Normal", cmdCount.normalSprites);
    addStat("Scaled", cmdCount.scaledSprites);
    addStat("Lines", cmdCount.lines);

    return infoLabel.str();
}

// Charge VDP1 d'une période (vdp1.c, Vdp1DebugGetCycleStats) : cycles des
// commandes + cycles volés par le CPU, rapportés à la capacité théorique
// (cycles/ligne x lignes). Deux chiffres :
//  - modèle de l'émulateur : celui qui règle réellement le tracé de Kronos ;
//  - estimation : modèle détaillé de Mednafen (lignes, relecture du frame
//    buffer, antialiasing, pixels hors zone), observationnel uniquement.
double vdp1LoadPercent(const Vdp1CycleStats &st)
{
    if (st.budgetCycles == 0) return 0.0;
    return 100.0 * (double)(st.cmdCycles + st.cpuCycles) / (double)st.budgetCycles;
}

double vdp1EstLoadPercent(const Vdp1CycleStats &st)
{
    if (st.budgetCycles == 0) return 0.0;
    return 100.0 * (double)(st.estCycles + st.cpuCycles) / (double)st.budgetCycles;
}

void appendCyclePeriod(std::stringstream &s, const char *title, const Vdp1CycleStats &st, bool isFrame)
{
    s << "  " << title << ": ";
    if (st.budgetCycles == 0) {
        s << "n/a (no complete period yet)\n";
        return;
    }
    s << st.fields << (st.fields > 1 ? " fields, " : " field, ")
      << st.lines << " lines x " << st.cyclesPerLine << " cyc\n";
    s << "    budget         = " << st.budgetCycles << " cyc\n";
    s << std::fixed << std::setprecision(1);
    s << "    estimate       = " << st.estCycles << " (commands) + " << st.cpuCycles
      << " (CPU stolen)  ->  " << vdp1EstLoadPercent(st) << " %\n";
    s << "      " << st.estLines << " lines, " << st.estPixels << " pixels";
    if (st.estPixels)
        s << ", " << st.estRmwPixels << " with frame buffer read ("
          << (100.0 * (double)st.estRmwPixels / (double)st.estPixels) << " %)";
    s << "\n";
    s << "    emulator model = " << st.cmdCycles << " (commands) + " << st.cpuCycles
      << " (CPU stolen)  ->  " << vdp1LoadPercent(st) << " %\n";
    s.unsetf(std::ios::floatfield);
    s << "    commands: " << st.commands << " executed, " << st.skipped << " skipped"
      << "   lists: " << st.lists << " started, " << st.listsEnded << " ended, "
      << st.listsAborted << " aborted\n";
    if (st.blockedLines)
        s << "    blocked lines (DMA concurrency, no budget granted): " << st.blockedLines << "\n";
    if (isFrame)
        s << "    list still drawing at frame change: "
          << (st.runningAtSwap ? "YES (overrun)" : "no") << "\n";
}

// QFontMetrics::horizontalAdvance n'existe qu'à partir de Qt 5.11.
int textWidth(const QFontMetrics &fm, const QString &t)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return fm.horizontalAdvance(t);
#else
    return fm.width(t);
#endif
}

QString groupedNumber(qulonglong v)
{
    // 1234567 -> "1 234 567" : lisible dans la liste sans dépendre de la locale.
    QString d = QString::number(v);
    for (int i = d.size() - 3; i > 0; i -= 3) d.insert(i, QChar(' '));
    return d;
}

// Graphe de charge image par image (Vdp1DebugGetLoadHistory) : barres =
// estimation, ligne = modèle de l'émulateur, repère rouge = liste encore en
// cours au changement de frame buffer. Échelle 0-100 %, étendue si une image
// dépasse 100 %. Pas de Q_OBJECT : aucun signal ni slot, donc pas de moc.
class Vdp1LoadGraph : public QWidget
{
public:
    explicit Vdp1LoadGraph(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(220, 96);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMouseTracking(true);
    }

    void refresh()
    {
        mKronos.assign(VDP1_CYCLE_HISTORY, 0);
        mEst.assign(VDP1_CYCLE_HISTORY, 0);
        mOverrun.assign(VDP1_CYCLE_HISTORY, 0);
        const int n = Vdp1DebugGetLoadHistory(mKronos.data(), mEst.data(), mOverrun.data(), VDP1_CYCLE_HISTORY);
        mKronos.resize(n);
        mEst.resize(n);
        mOverrun.resize(n);
        update();
    }

    QSize sizeHint() const override { return QSize(420, 110); }

protected:
    QRect plotRect() const
    {
        const QFontMetrics fm(font());
        const int left = textWidth(fm, QStringLiteral("100%")) + 6;
        // Marge basse d'une demi-ligne : l'étiquette "0%" est centrée sur l'axe.
        return QRect(left, fm.height() + 4, width() - left - 4, height() - fm.height() - 8 - fm.height() / 2);
    }

    int scaleMax() const
    {
        int top = 1000;
        for (size_t i = 0; i < mEst.size(); i++) top = std::max(top, std::max(mEst[i], mKronos[i]));
        return ((top + 249) / 250) * 250;   // pas de 25 %
    }

    int frameAt(int x) const
    {
        const QRect pr = plotRect();
        if (mEst.empty() || x < pr.left() || x > pr.right()) return -1;
        const double slot = (double)pr.width() / VDP1_CYCLE_HISTORY;
        // Les images les plus récentes sont à droite.
        const int fromRight = (int)((pr.right() - x) / slot);
        const int idx = (int)mEst.size() - 1 - fromRight;
        return (idx >= 0 && idx < (int)mEst.size()) ? idx : -1;
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            QHelpEvent *he = static_cast<QHelpEvent *>(e);
            const int i = frameAt(he->pos().x());
            if (i >= 0) {
                const int age = (int)mEst.size() - 1 - i;
                QToolTip::showText(he->globalPos(),
                    QStringLiteral("Frame -%1\nEstimate: %2.%3 %\nEmulator model: %4.%5 %%6")
                        .arg(age)
                        .arg(mEst[i] / 10).arg(mEst[i] % 10)
                        .arg(mKronos[i] / 10).arg(mKronos[i] % 10)
                        .arg(mOverrun[i] ? QStringLiteral("\nList still drawing at frame change") : QString()),
                    this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QPalette pal = palette();
        const QColor text = pal.color(QPalette::WindowText);
        QColor grid = text; grid.setAlpha(50);
        const QColor estColor(230, 140, 30);
        const QColor kronosColor(60, 130, 230);
        const QRect pr = plotRect();
        const QFontMetrics fm(font());

        p.fillRect(pr, pal.color(QPalette::Base));
        const int top = scaleMax();
        auto yOf = [&](int permille) {
            return pr.bottom() - (int)((double)permille * pr.height() / top);
        };

        // Grille tous les 25 %, étiquettes 0/50/100... à gauche.
        for (int v = 0; v <= top; v += 250) {
            const int y = yOf(v);
            p.setPen(v == 1000 ? QPen(text, 1, Qt::DashLine) : QPen(grid));
            p.drawLine(pr.left(), y, pr.right(), y);
            if (v % 500 == 0) {
                p.setPen(text);
                const QString lab = QString::number(v / 10) + QLatin1Char('%');
                p.drawText(QRect(0, y - fm.height() / 2, pr.left() - 4, fm.height()),
                           Qt::AlignRight | Qt::AlignVCenter, lab);
            }
        }

        const int n = (int)mEst.size();
        const double slot = (double)pr.width() / VDP1_CYCLE_HISTORY;
        auto xOf = [&](int i) { return pr.right() - (n - 1 - i + 1) * slot; };

        // Barres : estimation.
        for (int i = 0; i < n; i++) {
            const double x0 = xOf(i);
            const int y = yOf(std::min(mEst[i], top));
            const QRectF bar(x0 + 0.5, y, std::max(1.0, slot - 1.0), pr.bottom() - y + 1);
            p.fillRect(bar, mEst[i] > 1000 ? QColor(220, 50, 50) : estColor);
            if (mOverrun[i])
                p.fillRect(QRectF(x0 + 0.5, pr.top(), std::max(1.0, slot - 1.0), 3), QColor(220, 0, 0));
        }

        // Ligne : modèle de l'émulateur.
        if (n > 0) {
            QPainterPath path;
            for (int i = 0; i < n; i++) {
                const QPointF pt(xOf(i) + slot / 2, yOf(std::min(mKronos[i], top)));
                if (i == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(kronosColor, 1.5));
            p.drawPath(path);
            p.setRenderHint(QPainter::Antialiasing, false);
        }

        // Légende sur la ligne du haut.
        int lx = pr.left();
        const int ly = 1;
        auto legend = [&](const QColor &c, const QString &t) {
            p.fillRect(lx, ly + fm.height() / 2 - 4, 10, 8, c);
            lx += 13;
            p.setPen(text);
            p.drawText(lx, ly + fm.ascent(), t);
            lx += textWidth(fm, t) + 12;
        };
        if (n > 0) {
            legend(estColor, QStringLiteral("Estimate %1.%2 %").arg(mEst[n - 1] / 10).arg(mEst[n - 1] % 10));
            legend(kronosColor, QStringLiteral("Emulator model %1.%2 %").arg(mKronos[n - 1] / 10).arg(mKronos[n - 1] % 10));
            legend(QColor(220, 0, 0), QStringLiteral("Overrun"));
        } else {
            p.setPen(text);
            p.drawText(pr, Qt::AlignCenter, QStringLiteral("No frame recorded yet"));
        }
        p.setPen(QPen(grid));
        p.drawRect(pr.adjusted(0, 0, -1, -1));
    }

private:
    std::vector<int> mKronos, mEst;
    std::vector<u8> mOverrun;
};

} // anonymous namespace

void UIDebugVDP1::updateVdp1Registers()
{
    if (!Vdp1Regs) {
        pteVdp1Regs->setPlainText(QtYabause::translate("VDP1 not initialised."));
        return;
    }

    std::stringstream s;
    s << std::hex << std::uppercase << std::setfill('0');

    // TVMR — RE-VÉRIFIÉ contre VDP1 Manual §4.1 p.36-37 : bit0=profondeur
    // couleur (0=16bpp,1=8bpp), bit1=rotation (0=off,1=on), bit2=HDTV
    // (0=NTSC/PAL,1=HDTV/31KC), bit3=VBE. Le "DIE" affiché précédemment
    // (lu au bit 4 de TVMR) N'EXISTE PAS dans TVMR — DIE est en réalité le
    // bit 3 de FBCR (voir plus bas). Bug corrigé.
    u16 tvmr = Vdp1Regs->TVMR;
    int tvm_bpp8 = tvmr & 0x1;           // bit0 : 0=16bpp, 1=8bpp
    int tvm_rotate = (tvmr >> 1) & 0x1;  // bit1 : rotation
    int tvm_hdtv = (tvmr >> 2) & 0x1;    // bit2 : HDTV/31KC
    s << "TVMR    = 0x" << std::setw(4) << tvmr << "\n";
    s << "  TVM=" << (tvmr & 0x7) << " (" << (tvm_bpp8 ? "8bpp" : "16bpp")
      << (tvm_rotate ? " rotation" : "") << (tvm_hdtv ? " HDTV/31KC" : " NTSC/PAL")
      << ")  VBE:" << ((tvmr >> 3) & 1) << "\n";

    // FBCR : le mode réel (érasure/bascule manuelle vs auto) dépend de
    // VBE (TVMR bit 3) + FCM (FBCR bit 1) + FCT (FBCR bit 0) combinés, pas
    // du seul bit 0 de FBCR — table exacte reprise de decodeFBCRMode()
    // (vdp1.c, VDP1 Manual §4.2 Table 4.3 p.41-42) :
    //   VBE=1                -> (VBlankErase, ManualChange)
    //   VBE=0, FCM=0         -> (OneCycleErase, OneCycleChange)
    //   VBE=0, FCM=1, FCT=0  -> ManualErase
    //   VBE=0, FCM=1, FCT=1  -> ManualChange
    u16 fbcr = Vdp1Regs->FBCR;
    int fbcr_vbe = (tvmr >> 3) & 0x1;
    int fbcr_fcm = (fbcr >> 1) & 0x1;
    int fbcr_fct =  fbcr       & 0x1;
    // EOS (bit4) / DIE (bit3) / DIL (bit2) — RE-VÉRIFIÉS §4.2 p.43, absents
    // de l'affichage jusqu'ici.
    int fbcr_eos = (fbcr >> 4) & 0x1;
    int fbcr_die = (fbcr >> 3) & 0x1;
    int fbcr_dil = (fbcr >> 2) & 0x1;
    int vblankErase=0, manualErase=0, oneCycleErase=0, manualChange=0, oneCycleChange=0;
    if (fbcr_vbe == 1)        { vblankErase = 1; manualChange = 1; }
    else if (fbcr_fcm == 0)   { oneCycleErase = 1; oneCycleChange = 1; }
    else if (fbcr_fct == 0)   { manualErase = 1; }
    else                      { manualChange = 1; }
    s << "\nFBCR    = 0x" << std::setw(4) << fbcr << "\n";
    s << "  FCM:" << fbcr_fcm << " FCT:" << fbcr_fct << " VBE(TVMR.3):" << fbcr_vbe
      << " EOS:" << fbcr_eos << " DIE:" << fbcr_die << " DIL:" << fbcr_dil << "\n";
    s << "  -> VBlankErase:" << vblankErase << " ManualErase:" << manualErase
      << " OneCycleErase:" << oneCycleErase << " ManualChange:" << manualChange
      << " OneCycleChange:" << oneCycleChange << "\n";

    // PTMR (Plot Trigger Mode Register) : pilote le déclenchement du
    // dessin des commandes. Bits 1:0 uniquement significatifs.
    u16 ptmr = Vdp1Regs->PTMR;
    static const char *ptmrModes[4] = { "Idle", "Start (1-shot)", "Start each frame", "Prohibited" };
    s << "\nPTMR    = 0x" << std::setw(4) << ptmr << "\n";
    s << "  Mode: " << (ptmr & 0x3) << " (" << ptmrModes[ptmr & 0x3] << ")\n";

    // EDSR (End/Draw Status Register, lecture seule) : reflète l'état de
    // fin de dessin de la trame courante (CEF) et de la précédente (BEF).
    // RE-VÉRIFIÉ contre VDP1 User's Manual ST-013-R3 : "Current End Bit
    // Fetch Status (CEF): bit 1" / "Before End Bit Fetch Status (BEF): bit 0"
    // — CEF et BEF étaient INVERSÉS dans la version précédente (j'avais mis
    // CEF=bit0, BEF=bit1 ; c'est l'inverse). Le décalage Vdp1Regs->EDSR>>=1
    // vu dans vdp1.c reste cohérent avec cette correction : à la bascule de
    // trame, l'ancien CEF (bit1) devient le nouveau BEF (bit0).
    u16 edsr = Vdp1Regs->EDSR;
    s << "\nEDSR    = 0x" << std::setw(4) << edsr << "\n";
    s << "  CEF (Current End): " << ((edsr >> 1) & 0x1)
      << "   BEF (Before End): " << (edsr & 0x1) << "\n";

    // COPR/LOPR (Current/Last Operand Pointer Register, lecture seule) :
    // confirmé par vdp1.c -> regs->COPR = (regs->addr & 0x7FFFF) >> 3;
    // donc l'adresse octet réelle en VRAM VDP1 = valeur registre << 3.
    // RE-CONFIRMÉ mot pour mot par le manuel VDP1 (§4.7 p.55) : "The value
    // resulting from dividing the command table address ... by 8H is
    // written to this register."
    u16 copr = Vdp1Regs->COPR;
    u32 coprAddr = (u32)copr << 3;
    s << "\nCOPR    = 0x" << std::setw(4) << copr
      << "  (command table addr = 0x" << std::setw(5) << coprAddr << ")\n";

    u16 lopr = Vdp1Regs->LOPR;
    u32 loprAddr = (u32)lopr << 3;
    s << "\nLOPR    = 0x" << std::setw(4) << lopr
      << "  (command table addr = 0x" << std::setw(5) << loprAddr << ")\n";

    // MODR (Mode Status Register, lecture seule, §4.9 p.56-57) : ce n'est
    // pas juste un "statut" opaque — c'est un miroir documenté bit-à-bit de
    // plusieurs registres write-only (utile car on ne peut pas relire
    // TVMR/FBCR/PTMR directement). Décodage complet ajouté (raw hex
    // seulement jusqu'ici) :
    //   bits 15-12 = VER (version, doit valoir 1)
    //   bit 8  = PTM1  (miroir de PTMR bit1)
    //   bit 7  = EOS   (miroir de FBCR bit4)
    //   bit 6  = DIE   (miroir de FBCR bit3)
    //   bit 5  = DIL   (miroir de FBCR bit2)
    //   bit 4  = FCM   (miroir de FBCR bit1)
    //   bit 3  = VBE   (miroir de TVMR bit3)
    //   bits 2-0 = TVM (miroir de TVMR bits2-0)
    u16 modr = Vdp1Regs->MODR;
    s << "\nMODR    = 0x" << std::setw(4) << modr << "\n";
    s << "  VER:" << ((modr >> 12) & 0xF) << " PTM1:" << ((modr >> 8) & 1)
      << " EOS:" << ((modr >> 7) & 1) << " DIE:" << ((modr >> 6) & 1)
      << " DIL:" << ((modr >> 5) & 1) << " FCM:" << ((modr >> 4) & 1)
      << " VBE:" << ((modr >> 3) & 1) << " TVM:" << (modr & 0x7) << "\n";

    // EWDR/EWLR/EWRR/ENDR : registres d'effacement du framebuffer.
    // Layout EWLR/EWRR confirmé par vdp1.c ET le manuel VDP1 (§4.4 p.47) :
    //   EWLR bits 14-9 = X1 (6 bits), bits 8-0 = Y1 (9 bits)
    //   EWRR bits 15-9 = X3 (7 bits), bits 8-0 = Y3 (9 bits)
    // Coordonnées pixel réelles (manuel p.47) : X1 = valeur x8 (16bpp) ou
    // x16 (8bpp) ; X3 = valeur x(8 ou 16) - 1. Le facteur dépend de TVMR
    // bit0 (tvm_bpp8, décodé plus haut). Y est en unités de ligne 1:1 en
    // mode normal (le manuel signale un doublement en double-interlace,
    // non pris en compte ici — DIE/DIL sont affichés ci-dessus si besoin).
    u16 ewdr = Vdp1Regs->EWDR;
    s << "\nEWDR    = 0x" << std::setw(4) << ewdr << "  (erase/write fill color)\n";

    u16 ewlr = Vdp1Regs->EWLR;
    u16 ewrr = Vdp1Regs->EWRR;
    int ewX1raw = (ewlr >> 9) & 0x3F, ewY1 = ewlr & 0x1FF;
    int ewX3raw = (ewrr >> 9) & 0x7F, ewY3 = ewrr & 0x1FF;
    int ewXfactor = tvm_bpp8 ? 16 : 8;
    int ewX1px = ewX1raw * ewXfactor;
    int ewX3px = ewX3raw * ewXfactor - 1;
    s << "EWLR    = 0x" << std::setw(4) << ewlr << "\n";
    s << "EWRR    = 0x" << std::setw(4) << ewrr << "\n";
    s << "  Erase area: (" << std::dec << ewX1px << "," << ewY1 << ") - ("
      << ewX3px << "," << ewY3 << ") px  [x" << ewXfactor << " per TVMR bpp, Y=line units]"
      << std::hex << "\n";

    u16 endr = Vdp1Regs->ENDR;
    s << "\nENDR    = 0x" << std::setw(4) << endr << "  (forced draw termination, write-only)\n";

    s << "\n--- Clipping ---\n" << std::dec;
    s << "  System: (" << Vdp1Regs->systemclipX1 << "," << Vdp1Regs->systemclipY1 << ") - (" 
      << Vdp1Regs->systemclipX2 << "," << Vdp1Regs->systemclipY2 << ")\n";
    s << "  Local : (" << Vdp1Regs->localX << "," << Vdp1Regs->localY << ")\n";
    // User clipping (CMDPMOD bit 9 Cmod : 0=dedans, 1=dehors), absent de
    // l'affichage jusqu'ici alors que le registre existe (vdp1.h).
    s << "  User  : (" << Vdp1Regs->userclipX1 << "," << Vdp1Regs->userclipY1 << ") - ("
      << Vdp1Regs->userclipX2 << "," << Vdp1Regs->userclipY2 << ")"
      << "  Mode:" << Vdp1Regs->userclipMode
      /* vdp1_compute.c : userclipMode = clip_enable ? Cmod : 2, ou 2 signifie
       * "pas de clipping utilisateur". Trois etats, pas deux -- le test
       * booleen affichait "outside" pour la valeur 2, c'est-a-dire quand le
       * clipping utilisateur est justement inactif. */
      << " (" << (Vdp1Regs->userclipMode == 0 ? "inside drawing"
                 : Vdp1Regs->userclipMode == 1 ? "outside drawing"
                 : "disabled") << ")\n";

    // --- État interne moteur (Vdp1External) : PAS des registres matériel,
    // section clairement distincte pour ne pas les confondre avec l'état
    // du VDP1 réel — mais très utile pour déboguer le comportement de
    // l'émulateur lui-même (état de la state machine de dessin, etc.)
    s << "\n--- Engine State (internal, not hardware registers) ---\n";
    int stMask = Vdp1External.status & VDP1_STATUS_MASK;
    s << "  status: " << (stMask == VDP1_STATUS_RUNNING ? "RUNNING" : "IDLE")
      << "  switching:" << ((Vdp1External.status & VDP1_SWITCHING) ? 1 : 0)
      << "  switchRequest:" << ((Vdp1External.status & VDP1_SWITCH_REQUEST) ? 1 : 0) << "\n";
    s << "  blocked:" << Vdp1External.blocked
      << "  disptoggle:" << Vdp1External.disptoggle
      << "  checkEDSR:" << Vdp1External.checkEDSR
      << "  current_frame:" << Vdp1External.current_frame
      << "  updateVdp1Ram:" << Vdp1External.updateVdp1Ram << "\n";
    s << "  manualErase:" << Vdp1External.manualerase
      << "  manualChange:" << Vdp1External.manualchange
      << "  oneCycleErase:" << Vdp1External.onecycleerase
      << "  oneLastErase:" << Vdp1External.onelasterase
      << "  oneCycleChange:" << Vdp1External.onecyclechange
      << "  useVBlankErase:" << Vdp1External.useVBlankErase << "\n";
    // Curseur de lecture courant (adresse VRAM octet, pas COPR) : utile
    // pour voir en direct où en est l'interpréteur de commandes.
    s << std::hex;
    s << "  current cmd cursor (addr): 0x" << std::setw(5) << Vdp1Regs->addr << std::dec << "\n";

    // --- Charge VDP1 : total des cycles utilisés par image, comparé à la
    // capacité de tracé de la période. Modèle de cycles de l'émulateur
    // (vdp1.c getNormalCycles/getScaledCycles/...), donc une orientation,
    // pas une mesure exacte du matériel.
    {
        Vdp1CycleStats frame, field;
        int avgPm = 0, peakPm = 0, histCount = 0;
        Vdp1DebugGetCycleStats(&frame, &field, &avgPm, &peakPm, &histCount);
        s << std::dec << std::setfill(' ');
        s << "\n--- VDP1 cycle load ---\n"
          << "  estimate       : detailed model (Mednafen): per-line setup, 6 cycles per pixel\n"
          << "                   when the frame buffer is read (MSB On, shadow, half-transparency),\n"
          << "                   antialiasing pixels, pixels walked outside the clip area.\n"
          << "  emulator model : what Kronos actually charges (vdp1.c getNormalCycles...).\n"
          << "  Neither is a hardware measurement.\n";
        appendCyclePeriod(s, "Last frame (between FB changes)", frame, true);
        appendCyclePeriod(s, "Last field", field, false);
        if (histCount > 0) {
            std::vector<int> est(VDP1_CYCLE_HISTORY);
            const int n = Vdp1DebugGetLoadHistory(nullptr, est.data(), nullptr, VDP1_CYCLE_HISTORY);
            long sum = 0; int peak = 0;
            for (int i = 0; i < n; i++) { sum += est[i]; peak = std::max(peak, est[i]); }
            const int avgEst = n ? (int)((sum + n / 2) / n) : 0;
            s << "  Last " << histCount << " frames:\n"
              << "    estimate       average " << (avgEst / 10) << "." << (avgEst % 10)
              << " %, peak " << (peak / 10) << "." << (peak % 10) << " %\n"
              << "    emulator model average " << (avgPm / 10) << "." << (avgPm % 10)
              << " %, peak " << (peakPm / 10) << "." << (peakPm % 10) << " %\n";
        }
        if (!mTopCostText.empty())
            s << mTopCostText;
    }

    pteVdp1Regs->setPlainText(QString::fromStdString(s.str()));
}

void UIDebugVDP1::fillCommandList()
{
    Vdp1CommandsCount cmdCount;
    Vdp1ColorCalcCount ccCount;
    // Budget de la dernière image, pour la part de chaque commande, et
    // classement des commandes les plus coûteuses.
    double frameBudget = 0.0;
    {
        Vdp1CycleStats lastFrame;
        Vdp1DebugGetCycleStats(&lastFrame, nullptr, nullptr, nullptr, nullptr);
        frameBudget = (double)lastFrame.budgetCycles;
    }
    std::vector<std::pair<u32, int>> costRank;
    // Ligne sélectionnée avant le rafraîchissement : "Image suivante"
    // ramenait systématiquement la sélection sur la commande 0, ce qui
    // obligeait à re-cliquer le sprite étudié à chaque trame.
    const int previousRow = lwCommandList->currentRow();

    {
        // Vider/remplir les listes émet itemSelectionChanged ; sans blocage,
        // syncOnVdp1Entry était appelé avec des rangs qui ne correspondaient
        // plus à rien pendant la reconstruction.
        QSignalBlocker blockList(lwCommandList);
        QSignalBlocker blockRaw(lwCommandRaw);
        lwCommandList->clear();
        lwCommandRaw->clear();
        mCmdAddrs.clear();

        if (Vdp1DebugGetRam())
        {
            // Un seul parcours de la table (au plus 16384 tables de 32 octets
            // dans 512 Ko de VRAM). L'ancien remplissage appelait
            // Vdp1DebugGetCommandAddr(i) puis Vdp1DebugGetCommandType(i) pour
            // chaque i, soit deux parcours complets depuis l'adresse 0 par
            // ligne (O(n^2)), plafonnés en silence à 2000 commandes.
            mCmdAddrs.resize(16384);
            const int n = Vdp1DebugGetCommandList(mCmdAddrs.data(), (int)mCmdAddrs.size());
            mCmdAddrs.resize(n > 0 ? n : 0);

            for (size_t i = 0; i < mCmdAddrs.size(); i++)
            {
                const u32 addr = mCmdAddrs[i];
                char *nameStr = Vdp1DebugGetCommandNumberName(addr);
                if (nameStr == NULL) { mCmdAddrs.resize(i); break; }

                const Vdp1CommandType type = Vdp1DebugGetCommandTypeAtAddr(addr);
                Vdp1CountCommandType(type, cmdCount);

                QString itemText = QtYabause::translate(nameStr);
                QString ccTooltip;
                int itemCcb = 0;
                bool itemMsb = false;
                if (isVdp1DrawCommand(type)) {
                    const u16 pmod = T1ReadWord(Vdp1DebugGetRam(), (addr + 4) & 0x7FFFF);
                    itemCcb = pmod & 0x7;
                    itemMsb = (pmod & 0x8000) != 0;
                    ccCount.ccb[itemCcb]++;
                    if (itemMsb) ccCount.msbOn++;
                    if (pmod & 0x0100) ccCount.mesh++;
                    const QString tag = colorCalcTag(pmod);
                    if (!tag.isEmpty()) itemText += QStringLiteral("  ") + tag;
                    ccTooltip = QStringLiteral("\nColor calc: ") + QString::fromLatin1(kCcbName[itemCcb]);
                    if (itemMsb) ccTooltip += QStringLiteral("\nMSB On");
                    if (pmod & 0x0100) ccTooltip += QStringLiteral("\nMesh");
                }

                // Coût de la commande dans le tracé capturé (vdp1.c,
                // Vdp1DebugGetCommandCost) : estimation détaillée, et le
                // modèle de l'émulateur en infobulle.
                QString costTooltip;
                {
                    Vdp1CommandCost cost;
                    // Seulement pour la liste capturée : avant le premier "Image
                    // suivante", la liste affichée est la VRAM vivante et la
                    // table des coûts ne couvre qu'une partie du tracé en cours.
                    if (Vdp1DebugGetFrameRam() && Vdp1DebugGetCommandCost(addr, &cost)) {
                        itemText += QStringLiteral("   ") + groupedNumber(cost.estCycles) + QStringLiteral(" cyc");
                        if (frameBudget)
                            itemText += QStringLiteral(" (%1 %)").arg(100.0 * cost.estCycles / frameBudget, 0, 'f', 1);
                        costTooltip = QStringLiteral("\nCost in this draw: estimate %1 cyc, emulator model %2 cyc"
                                                     "\n%3 line(s), %4 pixel(s), %5 with frame buffer read%6")
                            .arg(cost.estCycles).arg(cost.kronosCycles)
                            .arg(cost.lines).arg(cost.pixels).arg(cost.rmwPixels)
                            .arg(cost.executions > 1 ? QStringLiteral("\nExecuted %1 times").arg(cost.executions) : QString());
                        if (isVdp1DrawCommand(type))
                            costRank.push_back(std::make_pair(cost.estCycles, (int)i));
                    } else if (Vdp1DebugGetFrameRam()) {
                        costTooltip = QStringLiteral("\nNot reached in this draw");
                    }
                }

                QListWidgetItem *item = new QListWidgetItem(itemText);
                if (itemCcb != 0) item->setBackground(ccbBackground(itemCcb));
                if (itemMsb) {
                    QFont f = item->font();
                    f.setBold(true);
                    item->setFont(f);
                }

                // Couleur par type réel (enum Vdp1CommandType, vdp1.h).
                switch (type)
                {
                    case VDPCT_NORMAL_SPRITE:
                    case VDPCT_SCALED_SPRITE:
                    case VDPCT_DISTORTED_SPRITE:
                    case VDPCT_DISTORTED_SPRITEN:
                        item->setForeground(Qt::darkGreen); // Sprites
                        break;
                    case VDPCT_POLYGON:
                        item->setForeground(Qt::blue); // Polygon
                        break;
                    case VDPCT_POLYLINE:
                    case VDPCT_POLYLINEN:
                        item->setForeground(Qt::darkCyan); // Polyline
                        break;
                    case VDPCT_LINE:
                        item->setForeground(QColor(180, 90, 0)); // Line (dark orange)
                        break;
                    case VDPCT_USER_CLIPPING_COORDINATES:
                    case VDPCT_USER_CLIPPING_COORDINATESN:
                    case VDPCT_SYSTEM_CLIPPING_COORDINATES:
                    case VDPCT_LOCAL_COORDINATES:
                        item->setForeground(Qt::gray); // Clipping / local coords
                        break;
                    case VDPCT_INVALID:
                        item->setForeground(Qt::red); // Invalid command word
                        break;
                    default:
                        break; // VDPCT_DRAW_END: leave at the default text color
                }

                // Adresse VRAM en infobulle : permet de rapprocher une ligne
                // de l'export brut ou d'un point d'arrêt mémoire.
                item->setToolTip(QString("0x%1").arg(addr, 5, 16, QChar('0')).toUpper() + ccTooltip + costTooltip);
                lwCommandList->addItem(item);

                // Une ligne par commande dans les DEUX listes, toujours : les
                // barres de défilement et les rangs sont synchronisés.
                char *rawStr = Vdp1DebugGetCommandRaw(addr);
                lwCommandRaw->addItem(rawStr ? QString::fromLatin1(rawStr) : QString());
                free(rawStr);
            }
        }
    }

    // Commandes les plus coûteuses du tracé capturé (panneau des registres).
    {
        std::stringstream top;
        std::sort(costRank.begin(), costRank.end(),
                  [](const std::pair<u32, int> &a, const std::pair<u32, int> &b) { return a.first > b.first; });
        if (!costRank.empty()) {
            top << "  Heaviest commands in the captured draw (estimate):\n";
            for (size_t k = 0; k < costRank.size() && k < 8; k++) {
                const int row = costRank[k].second;
                const QString name = (row < lwCommandList->count() && lwCommandList->item(row))
                    ? lwCommandList->item(row)->text().section(QStringLiteral("   "), 0, 0) : QString();
                top << "    #" << row << " @0x" << std::hex << std::uppercase << std::setw(5) << std::setfill('0')
                    << mCmdAddrs[row] << std::dec << std::setfill(' ') << "  " << costRank[k].first << " cyc";
                if (frameBudget)
                    top << " (" << std::fixed << std::setprecision(1) << 100.0 * costRank[k].first / frameBudget << " %)";
                top.unsetf(std::ios::floatfield);
                top << "  " << name.toStdString() << "\n";
            }
        }
        mTopCostText = top.str();
    }

    // Une ligne par rubrique (types, calcul couleur, charge, source) : le
    // bandeau occupe désormais toute la largeur, sous les panneaux.
    QStringList infoLines;
    {
        const std::string types = buildInfoLabel(cmdCount);
        if (!types.empty()) infoLines << QString::fromStdString(types);
    }
    infoLines << QString::fromStdString(buildColorCalcLabel(ccCount));
    QString info;
    {
        // Résumé de la charge VDP1 de la dernière image (détail dans le
        // panneau des registres).
        Vdp1CycleStats frame;
        Vdp1DebugGetCycleStats(&frame, nullptr, nullptr, nullptr, nullptr);
        if (frame.budgetCycles != 0) {
            info += QtYabause::translate("VDP1 load: %1 % estimated (%2 / %3 cycles, %4 field(s)), emulator model %5 %")
                        .arg(vdp1EstLoadPercent(frame), 0, 'f', 1)
                        .arg(groupedNumber(frame.estCycles + frame.cpuCycles))
                        .arg(groupedNumber(frame.budgetCycles))
                        .arg(frame.fields)
                        .arg(vdp1LoadPercent(frame), 0, 'f', 1);
            if (frame.runningAtSwap)
                info += QtYabause::translate(" OVERRUN");
            infoLines << info;
        }
    }
    infoLines << (Vdp1DebugGetFrameRam()
        ? QtYabause::translate("Source: VRAM captured at draw start")
        : QtYabause::translate("Source: live VRAM (Next frame captures the drawn list)"));
    lVDP1Info->setText(infoLines.join(QStringLiteral("\n")));
    if (mLoadGraph)
        static_cast<Vdp1LoadGraph *>(mLoadGraph)->refresh();

    // Avant syncOnVdp1Entry : celui-ci place le cadre de sélection dans
    // l'aperçu, qui doit déjà correspondre à la nouvelle liste.
    if (mFrameDlg && mFrameDlg->isVisible())
        refreshFramePreview();

    if (lwCommandList->count() > 0)
        syncOnVdp1Entry((previousRow >= 0 && previousRow < lwCommandList->count()) ? previousRow : 0);
    else
        clearVdp1Display();

    updateVdp1Registers();
}

void UIDebugVDP1::clearVdp1Display()
{
    pteCommandInfo->clear();
    if (vdp1texture) { free(vdp1texture); vdp1texture = NULL; }
    if (vdp1RawTexture) { free(vdp1RawTexture); vdp1RawTexture = NULL; }
    vdp1RawNumBytes = 0;
    vdp1RawW = vdp1RawH = 0;
    mCurIsSprite = false;
    currentTextureImage = QImage();
    lTextureInfo->setText(QtYabause::translate("No texture"));
    lTexturePixelInfo->setText(QtYabause::translate("Move the mouse over the texture to inspect a pixel."));
    pbSaveBitmap->setEnabled(false);
    pbSaveRawSprite->setEnabled(false);
    if (gvTexture->scene()) gvTexture->scene()->clear();
    mTexPixItem = nullptr;   // detruit par clear()
}

UIDebugVDP1::UIDebugVDP1(QWidget* p, YabauseLocker* lock) : QDialog(p), mLock(lock)
{
    setupUi(this);
    QGraphicsScene *scene = new QGraphicsScene(this);
    // Damier de transparence.
    //
    // Il était posé en fond de SCÈNE (setBackgroundBrush(Qt::Dense7Pattern)).
    // Le fond de scène est peint en coordonnées de scène, donc agrandi par
    // le zoom comme la texture elle-même : avec fitInView sur un sprite 8x8,
    // chaque point du motif Dense7 devenait un gros carré noir de la taille
    // d'un texel, sur toute la vue -- y compris hors de la texture. On ne
    // distinguait plus le motif des pixels du sprite.
    //
    // Le damier est maintenant peint en coordonnées ÉCRAN, dans le filtre
    // Paint du viewport (eventFilter), AVANT que QGraphicsView ne dessine la
    // scène : cases de 8 px fixes quel que soit le zoom, gris clair/gris
    // moyen, jamais confondables avec un texel noir. Le styleSheet du .ui
    // ("background-color: transparent") empêche de passer par la palette du
    // viewport, d'où le filtre plutôt que QPalette::Base.
    {
        QPixmap tile(16, 16);
        tile.fill(QColor(0xCC, 0xCC, 0xCC));
        QPainter tp(&tile);
        tp.fillRect(0, 0, 8, 8, QColor(0x99, 0x99, 0x99));
        tp.fillRect(8, 8, 8, 8, QColor(0x99, 0x99, 0x99));
        tp.end();
        mCheckerBrush = QBrush(tile);
    }
    // Le styleSheet de gvTexture (.ui) est restreint au selecteur
    // "QGraphicsView { ... }". Ecrit sans selecteur, il s'appliquait aussi a
    // l'info-bulle du widget ("Ctrl+wheel to zoom"), dont le fond devenait
    // transparent : sous Windows 7 sans composition, une barre noire
    // apparaissait apres 2 s de survol du sprite.
    scene->setBackgroundBrush(Qt::NoBrush);
    gvTexture->setBackgroundBrush(Qt::NoBrush);
    gvTexture->setScene(scene);
    // Un défilement par blit (viewport->scroll) déplacerait le damier avec
    // le contenu et le désalignerait de la grille écran : tout repeindre.
    gvTexture->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    // Texels nets : pas de lissage quand on agrandit un sprite.
    gvTexture->setRenderHint(QPainter::SmoothPixmapTransform, false);

    // Capture de la VRAM VDP1 au début de chaque tracé tant que la fenêtre
    // est ouverte (vdp1.c, Vdp1DebugCaptureFrame). Toutes les fonctions
    // Vdp1Debug* lisent cet instantané dès qu'il existe -- c'est-à-dire
    // après le premier "Image suivante" -- au lieu de la VRAM vivante.
    Vdp1DebugSetCapture(1);

    // Graphe de charge à droite du bandeau (horizontalLayout_info, .ui).
    mLoadGraph = new Vdp1LoadGraph(this);
    mLoadGraph->setToolTip(QString());
    horizontalLayout_info->addWidget(mLoadGraph, 2);

    connect(lwCommandList->verticalScrollBar(), &QScrollBar::valueChanged,
            lwCommandRaw->verticalScrollBar(), &QScrollBar::setValue);
    connect(lwCommandRaw->verticalScrollBar(), &QScrollBar::valueChanged,
            lwCommandList->verticalScrollBar(), &QScrollBar::setValue);

    // Filtre d'événements plutôt que surcharger mouseMoveEvent : la cible
    // réelle des mouvements de souris est gvTexture->viewport(), pas
    // gvTexture lui-même ni ce QDialog. Alimente l'inspecteur de pixel
    // (lTexturePixelInfo) -- même schéma que UIDebugVDP2Viewer.
    gvTexture->viewport()->installEventFilter(this);
    gvTexture->viewport()->setMouseTracking(true);

    // Hauteur fixe des deux lignes d'information sous la texture. Le texte
    // de l'inspecteur passe de 1 a 3 lignes (et davantage avec le retour a
    // la ligne) selon le texel survole : chaque changement redimensionnait
    // gvTexture, qui recentrait la scene -- le sprite "bougeait" sous le
    // curseur. La largeur est ignoree pour la meme raison (une longue ligne
    // de couleur elargissait la colonne Texture). Le texte ne pilote plus
    // la mise en page.
    {
        const int ls = lTexturePixelInfo->fontMetrics().lineSpacing();
        lTextureInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        lTextureInfo->setFixedHeight(2 * ls + 4);
        lTextureInfo->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        lTexturePixelInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        lTexturePixelInfo->setFixedHeight(4 * ls + 4);
        lTexturePixelInfo->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }

    fillCommandList();

    // was missing: the dialog stayed in English whatever the language
    QtYabause::retranslateWidget(this);
}

UIDebugVDP1::~UIDebugVDP1()
{
    // Détruire l'aperçu AVANT la fin de ce destructeur : son viewport a ce
    // dialogue pour filtre d'événements, et la destruction automatique des
    // enfants (après ~UIDebugVDP1) lui enverrait encore des événements
    // alors que l'objet est déjà à moitié détruit.
    if (mFrameDlg) {
        mFrameView->viewport()->removeEventFilter(this);
        delete mFrameDlg;
        mFrameDlg = nullptr;
        mFrameView = nullptr;
        mFrameSel = nullptr;
        mFrameInfo = nullptr;
    }
    clearVdp1Display();
    // L'émulation est encore suspendue par le YabauseLocker de l'appelant
    // (UIYabause : UIDebugVDP1(NULL, &locker).exec()) : libérer l'instantané
    // ici ne peut pas croiser un Vdp1DebugCaptureFrame().
    Vdp1DebugSetCapture(0);
}

void UIDebugVDP1::syncOnVdp1Entry(int cursel)
{
    if (cursel < 0 || cursel >= lwCommandList->count() ||
        cursel >= (int)mCmdAddrs.size()) return;

    const u32 addr = mCmdAddrs[cursel];

    char tempstr[2048];
    // Garantir la null-termination même si Vdp1DebugCommandAtAddr remplit
    // le buffer en entier ou ne le termine pas explicitement.
    memset(tempstr, 0, sizeof(tempstr));
    {
        QSignalBlocker blockList(lwCommandList);
        QSignalBlocker blockRaw(lwCommandRaw);
        lwCommandRaw->setCurrentRow(cursel);
        lwCommandList->setCurrentRow(cursel);
    }

    // Variantes adressées : l'adresse relevée par fillCommandList, pas un
    // rang re-résolu en reparcourant la table à chaque clic.
    Vdp1DebugCommandAtAddr(addr, tempstr);
    tempstr[sizeof(tempstr) - 1] = '\0';
    QString info = QString("Command address = 0x%1\n").arg(addr, 5, 16, QChar('0')).toUpper()
                 + QtYabause::translate(tempstr);
    {
        Vdp1CommandCost cost;
        if (Vdp1DebugGetFrameRam() && Vdp1DebugGetCommandCost(addr, &cost)) {
            info += QStringLiteral("\nCost in the captured draw:\n"
                                   "  estimate       = %1 cyc (table fetch included)\n"
                                   "  emulator model = %2 cyc\n"
                                   "  lines %3, pixels %4 (%5 with frame buffer read)\n")
                        .arg(cost.estCycles).arg(cost.kronosCycles)
                        .arg(cost.lines).arg(cost.pixels).arg(cost.rmwPixels);
            if (cost.executions > 1)
                info += QStringLiteral("  executed %1 times (CALL)\n").arg(cost.executions);
        } else if (Vdp1DebugGetFrameRam()) {
            info += QStringLiteral("\nCost: not reached in the captured draw.\n");
        }
    }
    pteCommandInfo->setPlainText(info);

    // Nettoyage avant nouvelle allocation
    if (vdp1texture) { free(vdp1texture); vdp1texture = NULL; }
    if (vdp1RawTexture) { free(vdp1RawTexture); vdp1RawTexture = NULL; }
    vdp1RawNumBytes = 0;
    vdp1RawW = vdp1RawH = 0;

    vdp1texture    = Vdp1DebugTextureAtAddr(addr, &vdp1texturew, &vdp1textureh);
    vdp1RawTexture = Vdp1DebugRawTextureAtAddr(addr, &vdp1RawW, &vdp1RawH, &vdp1RawNumBytes);

    // Paramètres utiles à l'inspecteur de pixel (CMDCTRL +0, CMDPMOD +4).
    mCurIsSprite = false;
    u8 *ram = Vdp1DebugGetRam();
    if (ram && addr != 0xFFFFFFFF) {
        const u16 ctrl = T1ReadWord(ram, addr);
        const u16 pmod = T1ReadWord(ram, addr + 4);
        mCurIsSprite  = !(ctrl & 0xC000) && ((ctrl & 0xF) <= 3);
        mCurColorMode = (pmod >> 3) & 0x7;
        mCurEndCode   = !(pmod & 0x0080);
        mCurSPD       =  (pmod & 0x0040) != 0;
    }

    pbSaveBitmap->setEnabled(vdp1texture != NULL);
    pbSaveRawSprite->setEnabled(vdp1RawTexture != NULL);

    QGraphicsScene *sc = gvTexture->scene();
    sc->clear();
    mTexPixItem = nullptr;   // detruit par clear()

    if (vdp1texture) {
        QImage img((uchar *)vdp1texture, vdp1texturew, vdp1textureh, QImage::Format_ARGB32);
        // .copy() : vdp1texture est libéré au prochain syncOnVdp1Entry (ou
        // dans le destructeur) -- sans copie profonde, currentTextureImage
        // pointerait sur de la mémoire libérée dès la sélection suivante.
        currentTextureImage = img.rgbSwapped().copy();

        const QRectF r(0, 0, vdp1texturew, vdp1textureh);
        // Cadre autour de l'emprise de la texture, sous le pixmap : sans lui
        // un sprite entièrement transparent (code 0 partout, SPD = 0) ne
        // laissait voir que le fond, sans aucune idée de sa taille.
        QPen framePen(QColor(255, 0, 255));
        framePen.setCosmetic(true);
        framePen.setWidth(2);
        QGraphicsRectItem *frame = sc->addRect(r, framePen, Qt::NoBrush);
        frame->setZValue(-1);

        QGraphicsPixmapItem *pix = sc->addPixmap(texturePixmap());
        pix->setTransformationMode(Qt::FastTransformation); // texels nets
        mTexPixItem = pix;
        sc->setSceneRect(r);
        fitTexture();

        int transparent = 0;
        for (int y = 0; y < currentTextureImage.height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(currentTextureImage.constScanLine(y));
            for (int x = 0; x < currentTextureImage.width(); x++)
                if (qAlpha(line[x]) == 0) transparent++;
        }
        const int total = vdp1texturew * vdp1textureh;
        QString t = QtYabause::translate("%1 x %2 px").arg(vdp1texturew).arg(vdp1textureh);
        if (mCurIsSprite) {
            t += "  -  " + QtYabause::translate("%1 / %2 transparent pixels").arg(transparent).arg(total);
            if (transparent == total)
                t += "\n" + QtYabause::translate("Fully transparent texture (every pixel is a transparent or end code)");
        }
        lTextureInfo->setText(t);
    } else {
        currentTextureImage = QImage();
        sc->setSceneRect(QRectF());
        gvTexture->resetTransform();
        lTextureInfo->setText(QtYabause::translate(
            "No texture for this command (draw end, skipped, clipping,\n"
            "local coordinates, or an invalid command word)"));
    }
    lTexturePixelInfo->setText(QtYabause::translate("Move the mouse over the texture to inspect a pixel."));
    updateFrameSelection();
}

// Image affichee dans gvTexture. Case Opaque : les texels transparents
// (alpha 0) gardent leur couleur au lieu de laisser voir le damier, comme
// la case Opaque du VDP2 Viewer. currentTextureImage, lue par
// l'inspecteur de pixel, garde toujours son canal alpha.
QPixmap UIDebugVDP1::texturePixmap() const
{
    if (currentTextureImage.isNull()) return QPixmap();
    if (cbOpaque->isChecked())
        return QPixmap::fromImage(currentTextureImage.convertToFormat(QImage::Format_RGB32));
    return QPixmap::fromImage(currentTextureImage);
}

void UIDebugVDP1::on_cbOpaque_toggled(bool)
{
    // Remplace seulement l'image : zoom et position de la vue conserves.
    if (mTexPixItem) mTexPixItem->setPixmap(texturePixmap());
}

// Zoom d'ajustement : facteur ENTIER dès que la texture tient au moins une
// fois dans la vue, pour que chaque texel occupe exactement N x N pixels
// écran (fitInView donnait un facteur fractionnaire, d'où des texels de
// tailles inégales).
void UIDebugVDP1::fitTexture()
{
    gvTexture->resetTransform();
    if (!gvTexture->scene() || currentTextureImage.isNull()) return;

    const QRectF r(0, 0, currentTextureImage.width(), currentTextureImage.height());
    const QSize vp = gvTexture->viewport()->size();
    const double margin = 6.0;
    const double sx = (vp.width()  - 2 * margin) / r.width();
    const double sy = (vp.height() - 2 * margin) / r.height();
    double s = std::min(sx, sy);
    if (s >= 1.0) s = std::floor(s);
    else if (s <= 0.0) s = 1.0;
    gvTexture->scale(s, s);
    gvTexture->centerOn(r.center());
}

void UIDebugVDP1::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    fitTexture();
}

// Code couleur brut (tel que lu en VRAM) du texel (x, y), à partir du buffer
// renvoyé par Vdp1DebugRawTextureAtAddr. 4 bpp : deux texels par octet, le
// premier dans le quartet fort ; 16 bpp : mot big-endian.
bool UIDebugVDP1::rawCodeAt(int x, int y, u32 *code) const
{
    if (!mCurIsSprite || !vdp1RawTexture || x < 0 || y < 0 ||
        x >= vdp1RawW || y >= vdp1RawH) return false;

    const int idx = y * vdp1RawW + x;
    switch (mCurColorMode) {
    case 0:
    case 1: {
        const int b = idx / 2;
        if (b >= vdp1RawNumBytes) return false;
        *code = (idx & 1) ? (vdp1RawTexture[b] & 0xF) : (vdp1RawTexture[b] >> 4);
        return true;
    }
    case 2:
    case 3:
    case 4:
        if (idx >= vdp1RawNumBytes) return false;
        *code = vdp1RawTexture[idx];
        return true;
    case 5:
        if (2 * idx + 1 >= vdp1RawNumBytes) return false;
        *code = ((u32)vdp1RawTexture[2 * idx] << 8) | vdp1RawTexture[2 * idx + 1];
        return true;
    default:
        return false;
    }
}

void UIDebugVDP1::wheelEvent(QWheelEvent *event)
{
    // Même schéma que UIDebugVDP2Viewer::wheelEvent : Ctrl+molette zoome,
    // seulement quand le curseur est au-dessus de la vue texture. Les
    // sprites VDP1 vont de 8x8 à 504x255 px -- les petits sont difficiles
    // à inspecter en détail sans zoom, qui n'existait pas du tout avant.
    if ((event->modifiers() & Qt::ControlModifier) && gvTexture->underMouse())
    {
        const double scaleFactor = 1.15;
        if (event->angleDelta().y() > 0)
            gvTexture->scale(scaleFactor, scaleFactor);
        else
            gvTexture->scale(1.0 / scaleFactor, 1.0 / scaleFactor);
        event->accept();
        return;
    }
    QDialog::wheelEvent(event);
}

void UIDebugVDP1::on_pbResetZoom_clicked()
{
    fitTexture();
}

// ============================================================
//  Pixel inspector : même principe que UIDebugVDP2Viewer -- survoler la
//  texture affiche ses coordonnées et sa couleur RGBA exacte, lues sur
//  currentTextureImage (donc cohérentes avec le zoom appliqué).
// ============================================================
bool UIDebugVDP1::eventFilter(QObject *watched, QEvent *event)
{
    if (mFrameView && watched == mFrameView->viewport())
    {
        switch (event->type())
        {
        case QEvent::Paint:
        {
            QPainter p(mFrameView->viewport());
            p.fillRect(static_cast<QPaintEvent*>(event)->rect(), mCheckerBrush);
            break;
        }
        case QEvent::Resize:
            fitFrame();
            break;
        case QEvent::Wheel:
        {
            QWheelEvent *we = static_cast<QWheelEvent*>(event);
            if (we->modifiers() & Qt::ControlModifier) {
                const double f = (we->angleDelta().y() > 0) ? 1.15 : 1.0 / 1.15;
                mFrameView->scale(f, f);
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            const QPointF sp = mFrameView->mapToScene(me->pos());
            const int x = (int)std::floor(sp.x()), y = (int)std::floor(sp.y());
            const int idx = commandAtFramePoint(x, y);
            if (event->type() == QEvent::MouseButtonPress) {
                if (me->button() == Qt::LeftButton && idx >= 0)
                    syncOnVdp1Entry(idx);
            } else {
                QString t = QString("(%1, %2)").arg(x).arg(y);
                if (idx >= 0 && idx < lwCommandList->count())
                    t += "   " + QtYabause::translate("Command #%1 at 0x%2: %3")
                            .arg(idx)
                            .arg(mCmdAddrs[idx], 5, 16, QChar('0'))
                            .arg(lwCommandList->item(idx)->text());
                mFrameInfo->setText(t);
            }
            break;
        }
        default:
            break;
        }
        return QDialog::eventFilter(watched, event);
    }

    if (watched == gvTexture->viewport())
    {
        if (event->type() == QEvent::Paint)
        {
            // Damier en coordonnées écran, peint avant la scène (le
            // gestionnaire Paint de QGraphicsView s'exécute après ce filtre
            // puisqu'on renvoie false). Voir le constructeur.
            QPainter p(gvTexture->viewport());
            p.fillRect(static_cast<QPaintEvent*>(event)->rect(), mCheckerBrush);
        }
        else if (event->type() == QEvent::MouseMove)
        {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            QPointF scenePos = gvTexture->mapToScene(me->pos());
            int px = (int)std::floor(scenePos.x());
            int py = (int)std::floor(scenePos.y());

            if (!currentTextureImage.isNull() && px >= 0 && py >= 0 &&
                px < currentTextureImage.width() && py < currentTextureImage.height())
            {
                QColor c = currentTextureImage.pixelColor(px, py);
                QString t = QtYabause::translate("Pixel (%1, %2)  R=%3 G=%4 B=%5 A=%6")
                    .arg(px).arg(py)
                    .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
                // Code brut lu en VRAM : indispensable pour savoir si un texel
                // transparent l'est par son code (0, ou MSB = 0 en RGB, avec
                // SPD = 0), par un end code, ou parce qu'il suit le second end
                // code de sa ligne.
                u32 code;
                if (rawCodeAt(px, py, &code)) {
                    const int digits = (mCurColorMode <= 1) ? 1 : (mCurColorMode <= 4 ? 2 : 4);
                    t += "\n" + QtYabause::translate("Raw code = 0x%1")
                                    .arg(code, digits, 16, QChar('0')).toUpper();
                    const u32 endCode = (mCurColorMode <= 1) ? 0xF
                                      : (mCurColorMode <= 4 ? 0xFF : 0x7FFF);
                    // Même règle que Vdp1DebugTextureAtAddr et le moteur
                    // (vdp1_prog_compute*.h) : code 0 transparent en modes
                    // palette, MSB = 0 transparent en mode RGB.
                    const bool transp = !mCurSPD &&
                        ((mCurColorMode == 5) ? !(code & 0x8000) : (code == 0));
                    if (mCurEndCode && code == endCode)
                        t += "  " + QtYabause::translate("(end code)");
                    else if (transp)
                        t += "  " + QtYabause::translate("(transparent code)");
                    else if (c.alpha() == 0)
                        t += "  " + QtYabause::translate("(after the 2nd end code of the line)");
                    // Chemin complet jusqu'à la couleur : banque ou LUT,
                    // décalage CRAOFB, index et contenu de la Color RAM.
                    const int row = lwCommandList->currentRow();
                    if (!transp && !(mCurEndCode && code == endCode) &&
                        row >= 0 && row < (int)mCmdAddrs.size()) {
                        char ctext[256];
                        if (Vdp1DebugTexelColorText(mCmdAddrs[row], code, ctext, sizeof(ctext)) > 0)
                            t += "\n" + QString::fromLatin1(ctext);
                    }
                }
                lTexturePixelInfo->setText(t);
            }
            else
            {
                lTexturePixelInfo->setText(QtYabause::translate("Move the mouse over the texture to inspect a pixel."));
            }
        }
        else if (event->type() == QEvent::Leave)
        {
            lTexturePixelInfo->setText(QtYabause::translate("Move the mouse over the texture to inspect a pixel."));
        }
    }
    return QDialog::eventFilter(watched, event);
}

// ============================================================
//  Aperçu de la trame
//
//  Chaque ligne de la liste n'est qu'un morceau de l'image : un personnage
//  est souvent découpé en plusieurs sprites 8x8 ou 8x32 (Mickey : 10 à 19
//  commandes selon l'animation). Cette fenêtre assemble toutes les commandes
//  à leurs coordonnées, avec la commande sélectionnée encadrée, pour voir à
//  quoi correspond chaque ligne. Cliquer un sprite sélectionne sa commande.
//
//  Ce n'est pas le rendu du VDP1 : pas de calcul de couleur (demi-
//  transparence, ombre, Gouraud), ni mesh, ni clipping utilisateur ; les
//  scaled/distorted sprites sont plaqués par échantillonnage direct. Voir
//  Vdp1DebugRenderFrame (vdp1.c).
// ============================================================
void UIDebugVDP1::on_pbFramePreview_clicked()
{
    if (!mFrameDlg)
    {
        mFrameDlg = new QDialog(this);
        mFrameDlg->setWindowTitle(QtYabause::translate("VDP1 Frame Preview"));
        mFrameDlg->resize(700, 560);

        QVBoxLayout *lay = new QVBoxLayout(mFrameDlg);
        mFrameView = new QGraphicsView(mFrameDlg);
        mFrameView->setScene(new QGraphicsScene(mFrameDlg));
        mFrameView->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        mFrameView->setRenderHint(QPainter::SmoothPixmapTransform, false);
        mFrameView->setMouseTracking(true);
        mFrameView->viewport()->setMouseTracking(true);
        mFrameView->viewport()->installEventFilter(this);
        mFrameView->setToolTip(QtYabause::translate("Ctrl+wheel to zoom, click a sprite to select its command"));
        lay->addWidget(mFrameView, 1);

        mFrameInfo = new QLabel(mFrameDlg);
        mFrameInfo->setWordWrap(true);
        lay->addWidget(mFrameInfo);
    }

    refreshFramePreview();
    mFrameDlg->show();
    mFrameDlg->raise();
    mFrameDlg->activateWindow();
    fitFrame();
}

void UIDebugVDP1::refreshFramePreview()
{
    if (!mFrameDlg) return;

    QGraphicsScene *sc = mFrameView->scene();
    sc->clear();
    mFrameSel = nullptr;
    mFrameImage = QImage();
    mFrameBoxes.assign(mCmdAddrs.size() * 4, 0);

    int w = 0, h = 0;
    u32 *fb = mCmdAddrs.empty() ? NULL
        : Vdp1DebugRenderFrame(mCmdAddrs.data(), (int)mCmdAddrs.size(), &w, &h, mFrameBoxes.data());
    if (fb)
    {
        QImage img((uchar *)fb, w, h, QImage::Format_ARGB32);
        mFrameImage = img.rgbSwapped().copy(); // fb libéré juste après
        free(fb);

        const QRectF r(0, 0, w, h);
        QPen border(QColor(128, 128, 128));
        border.setCosmetic(true);
        sc->addRect(r, border, Qt::NoBrush)->setZValue(-1);
        QGraphicsPixmapItem *pix = sc->addPixmap(QPixmap::fromImage(mFrameImage));
        pix->setTransformationMode(Qt::FastTransformation);

        QPen selPen(QColor(255, 0, 255));
        selPen.setCosmetic(true);
        selPen.setWidth(2);
        mFrameSel = sc->addRect(QRectF(), selPen, Qt::NoBrush);
        mFrameSel->setZValue(10);
        mFrameSel->setVisible(false);
        sc->setSceneRect(r);

        mFrameInfo->setText(QtYabause::translate("%1 x %2 px (system clipping)").arg(w).arg(h)
            + "   |   "
            + QtYabause::translate("Approximation: no color calculation, mesh or user clipping"));
    }
    else
    {
        sc->setSceneRect(QRectF());
        mFrameInfo->setText(QtYabause::translate("Nothing to draw"));
    }
    updateFrameSelection();
}

void UIDebugVDP1::updateFrameSelection()
{
    if (!mFrameDlg || !mFrameSel) return;
    const int row = lwCommandList->currentRow();
    if (row < 0 || (size_t)(4 * row + 3) >= mFrameBoxes.size() ||
        mFrameBoxes[4 * row + 2] < mFrameBoxes[4 * row])
    {
        mFrameSel->setVisible(false);
        return;
    }
    const int *b = &mFrameBoxes[4 * row];
    mFrameSel->setRect(QRectF(b[0], b[1], b[2] - b[0] + 1, b[3] - b[1] + 1));
    mFrameSel->setVisible(true);
    // Sprite hors du clipping système : le cadre reste visible, la vue
    // s'étend pour l'inclure plutôt que de le perdre hors champ.
    mFrameView->scene()->setSceneRect(
        QRectF(0, 0, mFrameImage.width(), mFrameImage.height()).united(mFrameSel->rect()));
    mFrameView->ensureVisible(mFrameSel->rect(), 8, 8);
}

void UIDebugVDP1::fitFrame()
{
    if (!mFrameView) return;
    mFrameView->resetTransform();
    if (mFrameImage.isNull()) return;
    const QSize vp = mFrameView->viewport()->size();
    const double s0 = std::min((vp.width() - 12.0) / mFrameImage.width(),
                               (vp.height() - 12.0) / mFrameImage.height());
    const double s = (s0 >= 1.0) ? std::floor(s0) : (s0 > 0.0 ? s0 : 1.0);
    mFrameView->scale(s, s);
    mFrameView->centerOn(mFrameImage.width() / 2.0, mFrameImage.height() / 2.0);
}

// Commande dessinée en dernier (donc visible au-dessus) dont l'emprise
// contient (x, y). -1 si aucune.
int UIDebugVDP1::commandAtFramePoint(int x, int y) const
{
    for (int i = (int)mFrameBoxes.size() / 4 - 1; i >= 0; i--)
    {
        const int *b = &mFrameBoxes[4 * i];
        if (b[2] >= b[0] && x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3])
            return i;
    }
    return -1;
}

void UIDebugVDP1::on_lwCommandRaw_itemSelectionChanged()
{
    syncOnVdp1Entry(lwCommandRaw->currentRow());
}

void UIDebugVDP1::on_lwCommandList_itemSelectionChanged()
{
    syncOnVdp1Entry(lwCommandList->currentRow());
}

void UIDebugVDP1::on_pbSaveRawSprite_clicked()
{
    const QString answer = CommonDialogs::getSaveFileName(
        QString(), 
        QtYabause::translate("Save Raw Data"), 
        "*.bin");

    if (!answer.isEmpty() && vdp1RawTexture) {
        QFile outputFile(answer);
        if (outputFile.open(QIODevice::WriteOnly)) {
            outputFile.write((const char*)vdp1RawTexture, vdp1RawNumBytes);
            outputFile.close();
        }
    }
}

// ============================================================
//  Edition : l'editeur graphique s'ouvre sur le sprite selectionne (ou sur
//  la liste des sprites si la commande n'en est pas un). Il travaille sur la
//  meme memoire que ce dialogue (instantane pris au debut du trace) et y
//  recopie ses edits, d'ou le rafraichissement au retour.
// ============================================================
void UIDebugVDP1::on_pbEditSprite_clicked()
{
    const int row = lwCommandList->currentRow();
    {
        UIDebugGfxEditor ed(this);
        if (row >= 0 && row < (int)mCmdAddrs.size() && mCurIsSprite)
            ed.openSpriteCommand(mCmdAddrs[row]);
        ed.exec();
    }
    // L'editeur peut avoir fait tourner le jeu : la liste a change.
    fillCommandList();
}

void UIDebugVDP1::on_pbSaveEdits_clicked()
{
    UIEditsExport dlg(this);
    dlg.exec();
    syncOnVdp1Entry(lwCommandList->currentRow());
}

void UIDebugVDP1::on_pbRevertEdits_clicked()
{
    if (VdpEditCount(-1) == 0)
        return;
    if (QMessageBox::question(this, QtYabause::translate("Revert all edits"),
            QtYabause::translate("Put every original graphic and color back and forget all %1 edit(s)?")
                .arg(VdpEditCount(-1)), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    VdpEditRevertAll();
    syncOnVdp1Entry(lwCommandList->currentRow());
}

void UIDebugVDP1::on_pbSaveBitmap_clicked()
{
    const QString s = CommonDialogs::getSaveFileName(
        QString(), 
        QtYabause::translate("Save Bitmap"), 
        "*.png;;*.bmp");

    if (!s.isEmpty() && !currentTextureImage.isNull()) {
        // Réutilise l'image déjà affichée (voir syncOnVdp1Entry) au lieu de
        // reconvertir vdp1texture indépendamment : les deux ne peuvent plus
        // diverger, et ça n'exige plus vdp1texture (qui pourrait déjà avoir
        // été libéré) pour rester valide.
        if (!currentTextureImage.save(s))
            CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
    }
}

void UIDebugVDP1::on_pbNextButton_clicked()
{
    if (mLock) {
        // Désactiver le bouton pendant l'exécution pour éviter les
        // double-clics (même précaution que UIDebugVDP2::on_pbNextButton_clicked)
        pbNextButton->setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);

        mLock->step();
        fillCommandList();

        QApplication::restoreOverrideCursor();
        pbNextButton->setEnabled(true);
    }
}

// ============================================================
//  on_pbExportDebugInfo_clicked
//  Sauvegarde dans un unique fichier .txt : les registres VDP1, le résumé
//  de la table de commandes, la liste complète des commandes (jump list +
//  liste détaillée), et le détail de la commande actuellement sélectionnée.
//  Miroir de UIDebugVDP2Viewer::on_pbExportDebugInfo_clicked().
// ============================================================
void UIDebugVDP1::on_pbExportDebugInfo_clicked()
{
    // S'assurer que les registres affichés sont à jour avant export
    updateVdp1Registers();

    const QString suggested = QString("vdp1_debug_%1.txt")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QString s = CommonDialogs::getSaveFileName(suggested,
        QtYabause::translate("Choose a location for the text file"),
        QtYabause::translate("Text files (*.txt)"));
    if (s.isEmpty())
        return;

    // Certains dialogues natifs n'ajoutent pas automatiquement l'extension
    // du filtre sélectionné : on la garantit nous-mêmes.
    if (!s.endsWith(".txt", Qt::CaseInsensitive))
        s += ".txt";

    QFile f(s);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
        return;
    }

    QTextStream ts(&f);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    ts.setCodec("UTF-8");
#endif

    ts << "Yabause VDP1 Debug Export\n";
    ts << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "==================================================\n\n";

    ts << "########## VDP1 REGISTERS ##########\n\n";
    ts << pteVdp1Regs->toPlainText() << "\n\n";

    ts << "########## SUMMARY ##########\n\n";
    ts << lVDP1Info->text() << "\n\n";

    const int n = lwCommandList->count();
    ts << "########## COMMAND LIST (" << n << " entries) ##########\n\n";
    for (int i = 0; i < n; ++i) {
        const QString name = lwCommandList->item(i) ? lwCommandList->item(i)->text() : QString();
        const QString raw  = (i < lwCommandRaw->count() && lwCommandRaw->item(i))
                                  ? lwCommandRaw->item(i)->text() : QString();
        ts << "[" << i << "] " << name;
        if (!raw.isEmpty())
            ts << "  -  " << raw;
        ts << "\n";
    }
    ts << "\n";

    /* Raw dump of the head of the VDP1 command table. The decoded list above
       follows the JP field and therefore stops at the first END, which hides
       the case where a game leaves a "draw nothing" stub at address 0 while
       the real list it used to build sits further on -- or is simply absent.
       0xC0 = six 32-byte command tables, the size of a typical FMV list
       (system clip, user clip, local coords, background sprite, frame sprite,
       draw end). */
    ts << "########## VDP1 RAM 0x00000-0x00200 et 0x01000-0x010C0 (raw) ##########\n\n";
    // Même mémoire que la liste affichée : l'instantané pris au début du
    // tracé s'il existe, la VRAM vivante sinon.
    u8 *dumpRam = Vdp1DebugGetRam();
    ts << (Vdp1DebugGetFrameRam() ? "Source: VRAM captured at draw start\n\n"
                                  : "Source: live VRAM\n\n");
    if (dumpRam) {
        /* Deux fenetres : la tete de liste, et 0x1000 -- la cible du stub
           "call" que le jeu depose parfois en 0x00000. Sans la seconde on ne
           peut pas dire si la liste reelle vit la-bas ou si l'adresse ne
           contient qu'un END. */
        /* first window widened to 0x200 (16 command tables): lists with
           more than six commands (Zero Divide title menu: 5 sprites after
           the clipping/local commands) were cut before their sprites. */
        static const u32 kWindows[2] = { 0x00000, 0x01000 };
        static const u32 kSizes[2] = { 0x200, 0xC0 };
        for (int w = 0; w < 2; w++) {
        for (u32 a = kWindows[w]; a < kWindows[w] + kSizes[w]; a += 16) {
            ts << QString("%1  ").arg(a, 5, 16, QChar('0')).toUpper();
            for (int i = 0; i < 16; i += 2)
                ts << QString("%1 ").arg(T1ReadWord(dumpRam, a + i),
                                         4, 16, QChar('0')).toUpper();
            if ((a & 0x1F) == 0) {
                u16 ctrl = T1ReadWord(dumpRam, a);
                ts << "  <- cmd @" << QString("%1").arg(a, 5, 16, QChar('0')).toUpper();
                if (ctrl & 0x8000)
                    ts << "  END";
                else {
                    const char *jp;
                    switch ((ctrl >> 12) & 3) {
                        case 0:  jp = "next";   break;
                        case 1:  jp = "assign"; break;
                        case 2:  jp = "call";   break;
                        default: jp = "return"; break;
                    }
                    ts << QString("  type=%1 JP=%2")
                            .arg(ctrl & 0xF, 1, 16).toUpper()
                            .arg(QString::fromLatin1(jp));
                }
            }
            ts << "\n";
        }
        if (w == 0) ts << "\n";
        }
        ts << "\nEach command table is 0x20 bytes: CMDCTRL CMDLINK CMDPMOD CMDCOLR\n"
              "CMDSRCA CMDSIZE CMDXA CMDYA CMDXB CMDYB CMDXC CMDYC CMDXD CMDYD\n"
              "CMDGRDA (dummy). CMDCTRL bit15 = END, bits 13-12 = JP\n"
              "(0 next, 1 assign, 2 call, 3 return), bits 3-0 = command type.\n\n";
    } else {
        ts << "VDP1 RAM is NULL.\n\n";
    }

    const int cur = lwCommandList->currentRow();
    ts << "########## SELECTED COMMAND DETAIL";
    if (cur >= 0)
        ts << " (#" << cur << ")";
    ts << " ##########\n\n";
    ts << pteCommandInfo->toPlainText() << "\n";

    ts.flush();
    f.close();

    if (f.error() != QFile::NoError)
        CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}
