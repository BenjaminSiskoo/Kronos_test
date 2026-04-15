/* Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/
#include "UIDebugVDP1.h"
#include "CommonDialogs.h"

#include <QImageWriter>
#include <QScrollBar>
#include <QGraphicsPixmapItem>
#include <QListWidgetItem>
#include <sstream>
#include <iomanip>

// VDP1 system headers
extern "C" {
#include "vdp1.h"
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
    default: break;
    }
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

} // anonymous namespace

void UIDebugVDP1::updateVdp1Registers()
{
    if (!Vdp1Regs) {
        pteVdp1Regs->setPlainText("VDP1 not initialised");
        return;
    }

    std::stringstream s;
    s << std::hex << std::uppercase << std::setfill('0');

    u16 tvmr = Vdp1Regs->TVMR;
    s << "TVMR    = 0x" << std::setw(4) << tvmr << "\n";
    s << "  Mode: " << (tvmr & 0x7) << " (VBE:" << ((tvmr >> 3) & 1) << " DIE:" << ((tvmr >> 4) & 1) << ")\n";

    u16 fbcr = Vdp1Regs->FBCR;
    s << "\nFBCR    = 0x" << std::setw(4) << fbcr << "\n";
    s << "  Change: " << (fbcr & 1 ? "Manual" : "Auto") << "\n";

    s << "\n--- Clipping ---\n" << std::dec;
    s << "  System: (" << Vdp1Regs->systemclipX1 << "," << Vdp1Regs->systemclipY1 << ") - (" 
      << Vdp1Regs->systemclipX2 << "," << Vdp1Regs->systemclipY2 << ")\n";
    s << "  Local : (" << Vdp1Regs->localX << "," << Vdp1Regs->localY << ")\n";

    pteVdp1Regs->setPlainText(QString::fromStdString(s.str()));
}

void UIDebugVDP1::fillCommandList()
{
    Vdp1CommandsCount cmdCount;
    lwCommandList->clear();
    lwCommandRaw->clear();

    if (Vdp1Ram)
    {
        for (int i = 0;; i++)
        {
            u32 addr = Vdp1DebugGetCommandAddr(i);
            char *nameStr = Vdp1DebugGetCommandNumberName(addr);
            if (nameStr == NULL) break;

            Vdp1CountCommands(i, cmdCount);
            
            QListWidgetItem *item = new QListWidgetItem(QtYabause::translate(nameStr));
            int type = (int)Vdp1DebugGetCommandType(i); // Cast en int pour comparaison matérielle
            
            // Coloration robuste utilisant les ID matériels
            if (type >= 0x00 && type <= 0x05) 
                item->setForeground(Qt::darkGreen); // Sprites
            else if (type == 0x06) 
                item->setForeground(Qt::blue);      // Polygons
            else if (type == 0x08 || type == 0x09) 
                item->setForeground(Qt::gray);      // User/System Clipping
                
            lwCommandList->addItem(item);

            char *rawStr = Vdp1DebugGetCommandRaw(addr);
            if (rawStr) {
                lwCommandRaw->addItem(rawStr);
                free(rawStr);
            }
        }
    }

    lVDP1Info->setText(QString::fromStdString(buildInfoLabel(cmdCount)));
    
    if (lwCommandList->count() > 0) 
        syncOnVdp1Entry(0);
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
    pbSaveBitmap->setEnabled(false);
    pbSaveRawSprite->setEnabled(false);
    if (gvTexture->scene()) gvTexture->scene()->clear();
}

UIDebugVDP1::UIDebugVDP1(QWidget* p, YabauseLocker* lock) : QDialog(p), mLock(lock)
{
    setupUi(this);
    gvTexture->setScene(new QGraphicsScene(this));

    connect(lwCommandList->verticalScrollBar(), &QScrollBar::valueChanged,
            lwCommandRaw->verticalScrollBar(), &QScrollBar::setValue);
    connect(lwCommandRaw->verticalScrollBar(), &QScrollBar::valueChanged,
            lwCommandList->verticalScrollBar(), &QScrollBar::setValue);

    fillCommandList();
}

UIDebugVDP1::~UIDebugVDP1()
{
    clearVdp1Display();
}

void UIDebugVDP1::syncOnVdp1Entry(int cursel)
{
    if (cursel < 0 || cursel >= lwCommandList->count()) return;

    char tempstr[2048];
    lwCommandRaw->setCurrentRow(cursel);
    lwCommandList->setCurrentRow(cursel);

    Vdp1DebugCommand(cursel, tempstr);
    pteCommandInfo->setPlainText(QtYabause::translate(tempstr));

    // Nettoyage avant nouvelle allocation
    if (vdp1texture) { free(vdp1texture); vdp1texture = NULL; }
    if (vdp1RawTexture) { free(vdp1RawTexture); vdp1RawTexture = NULL; }

    vdp1texture    = Vdp1DebugTexture(cursel, &vdp1texturew, &vdp1textureh);
    vdp1RawTexture = Vdp1DebugRawTexture(cursel, &vdp1texturew, &vdp1textureh, &vdp1RawNumBytes);

    pbSaveBitmap->setEnabled(vdp1texture != NULL);
    pbSaveRawSprite->setEnabled(vdp1RawTexture != NULL);

    if (vdp1texture) {
        QImage img((uchar *)vdp1texture, vdp1texturew, vdp1textureh, QImage::Format_ARGB32);
        QPixmap pixmap = QPixmap::fromImage(img.rgbSwapped());
        gvTexture->scene()->clear();
        gvTexture->scene()->addPixmap(pixmap);
        gvTexture->fitInView(gvTexture->scene()->itemsBoundingRect(), Qt::KeepAspectRatio);
    }
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

void UIDebugVDP1::on_pbSaveBitmap_clicked()
{
    const QString s = CommonDialogs::getSaveFileName(
        QString(), 
        QtYabause::translate("Save Bitmap"), 
        "*.png;;*.bmp");

    if (!s.isEmpty() && vdp1texture) {
        QImage img((uchar *)vdp1texture, vdp1texturew, vdp1textureh, QImage::Format_ARGB32);
        if (!img.rgbSwapped().save(s))
            CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
    }
}

void UIDebugVDP1::on_pbNextButton_clicked()
{
    if (mLock) {
        mLock->step();
        fillCommandList();
    }
}
