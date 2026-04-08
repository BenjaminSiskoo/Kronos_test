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
#include "UIDebugVDP1.h"
#include "CommonDialogs.h"

#include <QImageWriter>
#include <QScrollBar>
#include <QGraphicsPixmapItem>
#include <sstream>

// VDP1 system headers
extern "C" {
#include "vdp1.h"
}

namespace {

struct Vdp1CommandsCount
{
    size_t distortedSprites;
    size_t polygons;
    size_t polylines;
    size_t normalSprites;
    size_t scaledSprites;
    size_t lines;
};

void Vdp1CountCommands(u32 index, Vdp1CommandsCount& cmdCount)
{
    Vdp1CommandType commandType = Vdp1DebugGetCommandType(index);
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
    }
}

std::string buildInfoLabel(Vdp1CommandsCount& cmdCount)
{
    bool previous = false;
    std::stringstream infoLabel;

    if (cmdCount.distortedSprites > 0) {
        infoLabel << "Distorted Sprites: " << cmdCount.distortedSprites;
        previous = true;
    }
    if (cmdCount.polygons > 0) {
        infoLabel << (previous ? ", " : "") << "Polygons: " << cmdCount.polygons;
        previous = true;
    }
    if (cmdCount.polylines > 0) {
        infoLabel << (previous ? ", " : "") << "PolyLines: " << cmdCount.polylines;
        previous = true;
    }
    if (cmdCount.normalSprites > 0) {
        infoLabel << (previous ? ", " : "") << "Normal Sprites: " << cmdCount.normalSprites;
        previous = true;
    }
    if (cmdCount.scaledSprites > 0) {
        infoLabel << (previous ? ", " : "") << "Scaled Sprites: " << cmdCount.scaledSprites;
        previous = true;
    }
    if (cmdCount.lines > 0) {
        infoLabel << (previous ? ", " : "") << "Lines: " << cmdCount.lines;
    }

    return infoLabel.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// updateVdp1Registers
//   Reads Vdp1Regs and formats every register with its decoded meaning,
//   exactly like the Mednafen VDP viewer panel.
// ---------------------------------------------------------------------------
void UIDebugVDP1::updateVdp1Registers()
{
    if (!Vdp1Regs) {
        pteVdp1Regs->setPlainText("VDP1 not initialised");
        return;
    }

    std::stringstream s;

    // ---- TVMR  (TV Mode Register)  0x25D00000 ----
    u16 tvmr = Vdp1Regs->TVMR;
    s << "TVMR    = 0x" << std::hex << std::uppercase << tvmr << "\n";
    s << "  TVM[1:0] = " << (tvmr & 0x7) << "  -> ";
    switch (tvmr & 0x7) {
        case 0: s << "Normal (512x256 or 512x224)\n"; break;
        case 1: s << "Hi-res (1024x256 or 1024x224)\n"; break;
        case 2: s << "Exclusive Normal (512x480 or 512x448)\n"; break;
        case 3: s << "Exclusive Hi-res (1024x480 or 1024x448)\n"; break;
        case 6: s << "Exclusive 31kHz Normal (512x480)\n"; break;
        case 7: s << "Exclusive 31kHz Hi-res (1024x480)\n"; break;
        default: s << "Reserved\n"; break;
    }
    s << "  VBE (VBlank Erase)  = " << ((tvmr >> 3) & 1) << "\n";
    s << "  DIE (Display Input) = " << ((tvmr >> 4) & 1) << "\n";

    // ---- FBCR  (Frame Buffer Change Mode Register)  0x25D00002 ----
    u16 fbcr = Vdp1Regs->FBCR;
    s << "\nFBCR    = 0x" << std::hex << fbcr << "\n";
    s << "  FCM (FB Change Mode) = " << (fbcr & 1)
      << (fbcr & 1 ? "  (Manual change)\n" : "  (Auto change at VBlank-Out)\n");
    s << "  FCT (FB Change Trig) = " << ((fbcr >> 1) & 1) << "\n";
    s << "  DIL (Display Field) = " << ((fbcr >> 2) & 1) << "\n";
    s << "  DIE (Display Interlace Enable) = " << ((fbcr >> 3) & 1) << "\n";
    s << "  EOS (Even/Odd Coord Select) = " << ((fbcr >> 4) & 1) << "\n";

    // ---- PTMR  (Plot Trigger Mode Register)  0x25D00006 ----
    u16 ptmr = Vdp1Regs->PTMR;
    s << "\nPTMR    = 0x" << std::hex << ptmr << "\n";
    s << "  PTM[1:0] = " << (ptmr & 0x3) << "  -> ";
    switch (ptmr & 0x3) {
        case 0: s << "Idle (no draw)\n"; break;
        case 1: s << "Draw once (then idle)\n"; break;
        case 2: s << "Draw automatically each frame\n"; break;
        default: s << "Reserved\n"; break;
    }

    // ---- EWDR  (Erase/Write Data Register)  0x25D00008 ----
    u16 ewdr = Vdp1Regs->EWDR;
    s << "\nEWDR    = 0x" << std::hex << ewdr
      << "  (Erase/Write fill colour = RGB555 " << std::dec
      << ((ewdr >> 10) & 0x1F) << "," << ((ewdr >> 5) & 0x1F) << "," << (ewdr & 0x1F) << ")\n";

    // ---- EWLR  (Erase/Write Upper-Left)  0x25D0000A ----
    u16 ewlr = Vdp1Regs->EWLR;
    s << "\nEWLR    = 0x" << std::hex << ewlr
      << "  (X1=" << std::dec << ((ewlr >> 9) & 0x3F) * 8
      << "  Y1=" << (ewlr & 0x1FF) << ")\n";

    // ---- EWRR  (Erase/Write Lower-Right)  0x25D0000C ----
    u16 ewrr = Vdp1Regs->EWRR;
    s << "\nEWRR    = 0x" << std::hex << ewrr
      << "  (X2=" << std::dec << ((ewrr >> 9) & 0x3F) * 8 + 7
      << "  Y2=" << (ewrr & 0x1FF) << ")\n";

    // ---- EDSR  (Transfer End Status Register)  0x25D00010 ----
    u16 edsr = Vdp1Regs->EDSR;
    s << "\nEDSR    = 0x" << std::hex << edsr << "\n";
    s << "  CEF (Cartridge End Flag)  = " << ((edsr >> 1) & 1) << "\n";
    s << "  BEF (FB End Flag)         = " << (edsr & 1) << "\n";

    // ---- LOPR  (Last Operation Command Address)  0x25D00012 ----
    u32 lopr = (u32)Vdp1Regs->LOPR << 3;
    s << "\nLOPR    = 0x" << std::hex << Vdp1Regs->LOPR
      << "  (addr 0x" << lopr << ")\n";

    // ---- COPR  (Current Operation Command Address)  0x25D00014 ----
    u32 copr = (u32)Vdp1Regs->COPR << 3;
    s << "\nCOPR    = 0x" << std::hex << Vdp1Regs->COPR
      << "  (addr 0x" << copr << ")\n";

    // ---- MODR  (Mode Status Register, read-only)  0x25D00016 ----
    u16 modr = Vdp1Regs->MODR;
    s << "\nMODR    = 0x" << std::hex << modr << "  [READ-ONLY]\n";
    s << "  VER[3:0]  = " << std::dec << ((modr >> 12) & 0xF)
      << "  (VDP1 version)\n";
    s << "  PTM[1:0]  = " << ((modr >> 8) & 0x3) << "\n";
    s << "  EOS       = " << ((modr >> 4) & 1) << "\n";
    s << "  DIE       = " << ((modr >> 3) & 1) << "\n";
    s << "  DIL       = " << ((modr >> 2) & 1) << "\n";
    s << "  FCT       = " << ((modr >> 1) & 1) << "\n";
    s << "  FCM       = " << (modr & 1) << "\n";

    // ---- Internal / derived state ----
    s << "\n--- Clipping ---\n";
    s << std::dec;
    s << "  System clip  : (" << Vdp1Regs->systemclipX1 << "," << Vdp1Regs->systemclipY1
      << ") - (" << Vdp1Regs->systemclipX2 << "," << Vdp1Regs->systemclipY2 << ")\n";
    s << "  User clip    : (" << Vdp1Regs->userclipX1 << "," << Vdp1Regs->userclipY1
      << ") - (" << Vdp1Regs->userclipX2 << "," << Vdp1Regs->userclipY2 << ")\n";
    s << "  Clip mode    : " << Vdp1Regs->userclipMode
      << (Vdp1Regs->userclipMode ? "  (outside)" : "  (inside)") << "\n";
    s << "  Local offset : (" << Vdp1Regs->localX << "," << Vdp1Regs->localY << ")\n";

    // ---- External status ----
    s << "\n--- External state ---\n";
    s << "  disptoggle   = " << Vdp1External.disptoggle << "\n";
    s << "  manualerase  = " << Vdp1External.manualerase << "\n";
    s << "  manualchange = " << Vdp1External.manualchange << "\n";
    s << "  status       = 0x" << std::hex << Vdp1External.status << "\n";

    pteVdp1Regs->setPlainText(QString::fromStdString(s.str()));
}

// ---------------------------------------------------------------------------

void UIDebugVDP1::fillCommandList()
{
    Vdp1CommandsCount cmdCount;
    memset(&cmdCount, 0, sizeof(Vdp1CommandsCount));

    lwCommandList->clear();
    lwCommandRaw->clear();
    if (Vdp1Ram)
    {
        for (int i = 0;; i++)
        {
            char *string;
            u32 addr = Vdp1DebugGetCommandAddr(i);
            if ((string = Vdp1DebugGetCommandNumberName(addr)) == NULL)
                break;

            Vdp1CountCommands(i, cmdCount);
            lwCommandList->addItem(QtYabause::translate(string));
            string = Vdp1DebugGetCommandRaw(addr);
            lwCommandRaw->addItem(string);
            free(string);
        }
    }

    QString infoLabelText(QString::fromStdString(buildInfoLabel(cmdCount)));
    lVDP1Info->setText(infoLabelText);
    lVDP1Info->setToolTip(infoLabelText);

    if (lwCommandList->count() > 0) {
        syncOnVdp1Entry(0);
    } else {
        pteCommandInfo->clear();
        if (vdp1texture)    free(vdp1texture);
        if (vdp1RawTexture) free(vdp1RawTexture);
        vdp1texture     = NULL;
        vdp1RawTexture  = NULL;
        vdp1RawNumBytes = 0;
        vdp1texturew = vdp1textureh = 1;
        pbSaveBitmap->setEnabled(false);
        pbSaveRawSprite->setEnabled(false);
    }

    updateVdp1Registers();

    QtYabause::retranslateWidget(this);
}

UIDebugVDP1::UIDebugVDP1( QWidget* p, YabauseLocker* lock)
    : QDialog( p )
{
    setupUi(this);
    mLock = lock;

    QGraphicsScene *scene = new QGraphicsScene(this);
    gvTexture->setScene(scene);

    connect(lwCommandList->verticalScrollBar(), &QScrollBar::valueChanged,
            lwCommandRaw->verticalScrollBar(),  &QScrollBar::setValue);
    connect(lwCommandRaw->verticalScrollBar(),  &QScrollBar::valueChanged,
            lwCommandList->verticalScrollBar(), &QScrollBar::setValue);

    fillCommandList();
}

UIDebugVDP1::~UIDebugVDP1()
{
    if (vdp1texture)    free(vdp1texture);
    if (vdp1RawTexture) free(vdp1RawTexture);
}

void UIDebugVDP1::syncOnVdp1Entry(int cursel)
{
    char tempstr[1024];

    lwCommandRaw->setCurrentRow(cursel);
    lwCommandList->setCurrentRow(cursel);

    Vdp1DebugCommand(cursel, tempstr);
    pteCommandInfo->clear();
    pteCommandInfo->appendPlainText(QtYabause::translate(tempstr));
    pteCommandInfo->moveCursor(QTextCursor::Start);

    if (vdp1texture)    free(vdp1texture);
    if (vdp1RawTexture) free(vdp1RawTexture);

    vdp1texture    = Vdp1DebugTexture(cursel, &vdp1texturew, &vdp1textureh);
    vdp1RawTexture = Vdp1DebugRawTexture(cursel, &vdp1texturew, &vdp1textureh, &vdp1RawNumBytes);

    pbSaveBitmap->setEnabled(vdp1texture    ? true : false);
    pbSaveRawSprite->setEnabled(vdp1RawTexture ? true : false);

    QGraphicsScene *scene = gvTexture->scene();
    QImage img((uchar *)vdp1texture, vdp1texturew, vdp1textureh, QImage::Format_ARGB32);
    QPixmap pixmap = QPixmap::fromImage(img.rgbSwapped());
    scene->clear();
    scene->addPixmap(pixmap);
    scene->setSceneRect(scene->itemsBoundingRect());
    gvTexture->fitInView(scene->sceneRect());
    gvTexture->invalidateScene();
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
    QStringList filters( QString::fromUtf8("*.bin") );
    const QString answer = CommonDialogs::getSaveFileName(
        QString(), QtYabause::translate("Choose a location for your raw data"),
        filters.join(";;"));

    if (!answer.isEmpty()) {
        bool fileWritten = false;
        QFile outputFile(answer);
        if (outputFile.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
            if (outputFile.write(reinterpret_cast<const char*>(vdp1RawTexture),
                                 vdp1RawNumBytes) == vdp1RawNumBytes)
                fileWritten = true;
            outputFile.close();
        }
        if (!fileWritten)
            CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
    }
}

void UIDebugVDP1::on_pbSaveBitmap_clicked()
{
    QStringList filters;
    foreach (QByteArray ba, QImageWriter::supportedImageFormats())
        if (!filters.contains(ba, Qt::CaseInsensitive))
            filters << QString(ba).toLower();
    for (int i = 0; i < filters.count(); i++)
        filters[i] = QtYabause::translate("%1 Images (*.%2)")
                         .arg(filters[i].toUpper()).arg(filters[i]);

    QImage img((uchar *)vdp1texture, vdp1texturew, vdp1textureh, QImage::Format_ARGB32);
    img = img.rgbSwapped();

    const QString s = CommonDialogs::getSaveFileName(
        QString(), QtYabause::translate("Choose a location for your bitmap"),
        filters.join(";;"));

    if (!s.isEmpty())
        if (!img.save(s))
            CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}

void UIDebugVDP1::on_pbNextButton_clicked()
{
    if (mLock != NULL) {
        mLock->step();
        fillCommandList();
    }
}
