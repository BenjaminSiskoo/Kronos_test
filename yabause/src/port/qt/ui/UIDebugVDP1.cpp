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
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QApplication>
#include <sstream>
#include <iomanip>
#include <cstring>

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

    pteVdp1Regs->setPlainText(QString::fromStdString(s.str()));
}

void UIDebugVDP1::fillCommandList()
{
    Vdp1CommandsCount cmdCount;
    lwCommandList->clear();
    lwCommandRaw->clear();

    if (Vdp1Ram)
    {
        // Plafond de sécurité : une liste de commandes VDP1 corrompue (jeu
        // buggé, RAM non initialisée, etc.) pourrait ne jamais renvoyer de
        // marqueur de fin (nameStr == NULL), ce qui bloquerait cette boucle
        // et gèlerait toute l'interface de Yabause. Une vraie table de
        // commandes ne s'approche jamais de cette taille.
        const int kMaxCommands = 65536;
        for (int i = 0; i < kMaxCommands; i++)
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
    // Garantir la null-termination même si Vdp1DebugCommand remplit le
    // buffer en entier ou ne le termine pas explicitement (même précaution
    // que celle déjà appliquée côté UIDebugVDP2::updateInfoDisplay()).
    memset(tempstr, 0, sizeof(tempstr));
    lwCommandRaw->setCurrentRow(cursel);
    lwCommandList->setCurrentRow(cursel);

    Vdp1DebugCommand(cursel, tempstr);
    tempstr[sizeof(tempstr) - 1] = '\0';
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
    ts << "########## VDP1 RAM 0x00000-0x000C0 et 0x01000-0x010C0 (raw) ##########\n\n";
    if (Vdp1Ram) {
        /* Deux fenetres : la tete de liste, et 0x1000 -- la cible du stub
           "call" que le jeu depose parfois en 0x00000. Sans la seconde on ne
           peut pas dire si la liste reelle vit la-bas ou si l'adresse ne
           contient qu'un END. */
        static const u32 kWindows[2] = { 0x00000, 0x01000 };
        for (int w = 0; w < 2; w++) {
        for (u32 a = kWindows[w]; a < kWindows[w] + 0xC0; a += 16) {
            ts << QString("%1  ").arg(a, 5, 16, QChar('0')).toUpper();
            for (int i = 0; i < 16; i += 2)
                ts << QString("%1 ").arg(T1ReadWord(Vdp1Ram, a + i),
                                         4, 16, QChar('0')).toUpper();
            if ((a & 0x1F) == 0) {
                u16 ctrl = T1ReadWord(Vdp1Ram, a);
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
        ts << "Vdp1Ram is NULL.\n\n";
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
