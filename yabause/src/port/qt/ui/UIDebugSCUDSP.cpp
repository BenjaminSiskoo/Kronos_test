/*  Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

    This file is part of Yabause. (GPL v2+)
*/
#include "UIDebugSCUDSP.h"
#include "../CommonDialogs.h"
#include "UIYabause.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QLabel>
#include <QSplitter>
#include <sstream>
#include <iomanip>

// ============================================================
//  Helper local : répétition de caractère (MSVC-safe)
// ============================================================
static std::string repeatChar(char c, int n)
{
    std::string s;
    for (int i = 0; i < n; i++) s += c;
    return s;
}
int SCUDSPDis(void *context, u32 addr, char *string)
{
    (void)context;
    ScuDspDisasm((u8)addr, string);
    return 1;
}

// ============================================================
//  Breakpoint callback — runs in emulation thread
// ============================================================
void SCUDSPBreakpointHandler(u32 addr)
{
    (void)addr;
    UIYabause *ui = QtYabause::mainWindow(false);
    if (ui)
        emit ui->breakpointHandlerSCUDSP();}

// ============================================================
//  Constructor
// ============================================================
UIDebugSCUDSP::UIDebugSCUDSP(YabauseThread *mYabauseThread, QWidget *p)
    : UIDebugCPU(PROC_SCUDSP, mYabauseThread, p)
    , m_tabExtra(NULL)
    , m_pteDMA(NULL)
    , m_pteIRQ(NULL)
    , m_pteMD(NULL)
    , m_pteTimers(NULL)
{
    setWindowTitle(QtYabause::translate("Debug SCU DSP"));
    gbRegisters->setTitle(QtYabause::translate("DSP Registers"));

    // Masquer les widgets non pertinents pour le DSP SCU
    pbMemoryTransfer->setVisible(false);
    gbMemoryBreakpoints->setVisible(false);

    // Boutons réservés → fonctions Save
    pbReserved1->setText(QtYabause::translate("Save Program"));
    pbReserved2->setText(QtYabause::translate("Save MD0"));
    pbReserved3->setText(QtYabause::translate("Save MD1"));
    pbReserved4->setText(QtYabause::translate("Save MD2"));
    pbReserved5->setText(QtYabause::translate("Save MD3"));
    pbReserved1->setVisible(true);
    pbReserved2->setVisible(true);
    pbReserved3->setVisible(true);
    pbReserved4->setVisible(true);
    pbReserved5->setVisible(true);
    pbReserved1->setToolTip(QtYabause::translate("Save SCU DSP Program RAM (256 x 32-bit words) to .bin"));
    pbReserved2->setToolTip(QtYabause::translate("Save Data RAM bank 0 (64 x 32-bit words) to .bin"));
    pbReserved3->setToolTip(QtYabause::translate("Save Data RAM bank 1 to .bin"));
    pbReserved4->setToolTip(QtYabause::translate("Save Data RAM bank 2 to .bin"));
    pbReserved5->setToolTip(QtYabause::translate("Save Data RAM bank 3 to .bin"));

    // Dimensionnement minimal des listes
    {
        QSize s = lwRegisters->minimumSize();
        s.setWidth(lwRegisters->fontMetrics().averageCharWidth() * 32);
        lwRegisters->setMinimumSize(s);
    }
    {
        QSize s = lwDisassembledCode->minimumSize();
        s.setWidth(lwRegisters->fontMetrics().averageCharWidth() * 80);
        lwDisassembledCode->setMinimumSize(s);
    }

    // ── Onglets supplémentaires injectés dans le layout existant ──
    // On cherche le widget parent du lwDisassembledCode pour y insérer
    // un QTabWidget contenant DMA / IRQ / MD / Timers.
    m_tabExtra = new QTabWidget(this);
    m_tabExtra->setTabPosition(QTabWidget::South);

    auto mkPTE = [&]() {
        auto *pte = new QPlainTextEdit(m_tabExtra);
        pte->setReadOnly(true);
        pte->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont f("Courier New"); f.setPointSize(9);
        pte->setFont(f);
        return pte;
    };

    m_pteDMA    = mkPTE();
    m_pteIRQ    = mkPTE();
    m_pteMD     = mkPTE();
    m_pteTimers = mkPTE();

    m_tabExtra->addTab(m_pteDMA,    "DMA");
    m_tabExtra->addTab(m_pteIRQ,    "IRQ / IMS");
    m_tabExtra->addTab(m_pteMD,     "Data RAM");
    m_tabExtra->addTab(m_pteTimers, "Timers / Misc");

    // Insérer l'onglet extra dans le layout principal
    // (UIDebugCPU expose son QVBoxLayout principal ou on l'ajoute à la fin)
    if (auto *vl = qobject_cast<QVBoxLayout*>(layout())) {
        vl->addWidget(m_tabExtra);
    } else {
        // Fallback : on crée un QSplitter vertical wrappant tout
        auto *split = new QSplitter(Qt::Vertical, this);
        split->addWidget(m_tabExtra);
    }

    connect(m_tabExtra, &QTabWidget::currentChanged, this, &UIDebugSCUDSP::onTabChanged);

    // ── Initialisation breakpoints ──
    if (ScuRegs) {
        const scucodebreakpoint_struct *cbp = ScuDspGetBreakpointList();
        for (int i = 0; i < MAX_BREAKPOINTS; i++) {
            if (cbp[i].addr != 0xFFFFFFFF) {
                QString text;
                text.sprintf("%08X", (int)cbp[i].addr);
                lwCodeBreakpoints->addItem(text);
            }
        }

        lwDisassembledCode->setDisassembleFunction(SCUDSPDis);
        lwDisassembledCode->setEndAddress(0x100);   // DSP a 256 instructions max
        lwDisassembledCode->setMinimumInstructionSize(1);
        ScuDspSetBreakpointCallBack(SCUDSPBreakpointHandler);
    }

    updateAll();
}

// ============================================================
//  updateRegList — registres DSP dans lwRegisters
//  BUGS CORRIGÉS :
//    - RY affichait regs.RX au lieu de regs.RY
//    - sprintf() déprécié → utilisation de QString::asprintf()
//    - Ajout ALU et MUL (manquants)
//    - Ajout PC explicite, jmpaddr, delayed, DataRamPage
// ============================================================
void UIDebugSCUDSP::updateRegList()
{
    if (!ScuRegs) return;

    scudspregs_struct regs;
    memset(&regs, 0, sizeof(regs));
    ScuDspGetRegisters(&regs);
    lwRegisters->clear();

    lwRegisters->addItem("--- Program Control Port ---");
    lwRegisters->addItem(QString::asprintf("PR=%d  EP=%d  EX=%d  ES=%d",
        regs.ProgControlPort.part.PR, regs.ProgControlPort.part.EP,
        regs.ProgControlPort.part.EX, regs.ProgControlPort.part.ES));
    lwRegisters->addItem(QString::asprintf("LE=%d  E =%d  T0=%d",
        regs.ProgControlPort.part.LE, regs.ProgControlPort.part.E,
        regs.ProgControlPort.part.T0));
    lwRegisters->addItem(QString::asprintf("Z =%d  C =%d  V =%d  S =%d",
        regs.ProgControlPort.part.Z,  regs.ProgControlPort.part.C,
        regs.ProgControlPort.part.V,  regs.ProgControlPort.part.S));

    // -- Pointeurs de programme --
    lwRegisters->addItem("--- Program Pointers ---");
    lwRegisters->addItem(QString::asprintf("PC  =       %02X", regs.PC));
    lwRegisters->addItem(QString::asprintf("TOP =       %02X", regs.TOP));
    lwRegisters->addItem(QString::asprintf("LOP =     %04X", regs.LOP));
    lwRegisters->addItem(QString::asprintf("JMP =  %08X%s",
        (u32)regs.jmpaddr, regs.delayed ? "  (delayed)" : ""));

    // -- Compteurs circulaires --
    lwRegisters->addItem("--- Circular Counters ---");
    lwRegisters->addItem(QString::asprintf("CT0=%02X  CT1=%02X  CT2=%02X  CT3=%02X",
        regs.CT[0], regs.CT[1], regs.CT[2], regs.CT[3]));
    lwRegisters->addItem(QString::asprintf("DataRamPage=%d  ReadAddr=%02X",
        regs.DataRamPage, regs.DataRamReadAddress));

    // -- Adresses DMA DSP --
    lwRegisters->addItem("--- DSP DMA Addresses ---");
    lwRegisters->addItem(QString::asprintf("RA0 =  %08X", regs.RA0));
    lwRegisters->addItem(QString::asprintf("WA0 =  %08X", regs.WA0));

    // -- Registres X/Y multiplicateur --
    lwRegisters->addItem("--- Multiplier ---");
    lwRegisters->addItem(QString::asprintf("RX  =  %08X  (%d)", (u32)regs.RX, regs.RX));
    // BUG CORRIGE : etait regs.RX
    lwRegisters->addItem(QString::asprintf("RY  =  %08X  (%d)", (u32)regs.RY, regs.RY));

    // -- Registre P (produit, 48 bits) --
    lwRegisters->addItem("--- P Register (48-bit) ---");
    lwRegisters->addItem(QString::asprintf("PH  =     %04X", (u16)(regs.P.part.H & 0xFFFF)));
    lwRegisters->addItem(QString::asprintf("PL  =  %08X", (u32)(regs.P.part.L & 0xFFFFFFFF)));

    // -- Accumulateur AC (48 bits) --
    lwRegisters->addItem("--- Accumulator AC (48-bit) ---");
    lwRegisters->addItem(QString::asprintf("ACH =     %04X", (u16)(regs.AC.part.H & 0xFFFF)));
    lwRegisters->addItem(QString::asprintf("ACL =  %08X", (u32)(regs.AC.part.L & 0xFFFFFFFF)));

    // -- ALU et MUL (manquants dans l'original) --
    lwRegisters->addItem("--- ALU / MUL (48-bit) ---");
    lwRegisters->addItem(QString::asprintf("ALUH=     %04X", (u16)(regs.ALU.part.H & 0xFFFF)));
    lwRegisters->addItem(QString::asprintf("ALUL=  %08X", (u32)(regs.ALU.part.L & 0xFFFFFFFF)));
    lwRegisters->addItem(QString::asprintf("MULH=     %04X", (u16)(regs.MUL.part.H & 0xFFFF)));
    lwRegisters->addItem(QString::asprintf("MULL=  %08X", (u32)(regs.MUL.part.L & 0xFFFFFFFF)));
}

// ============================================================
//  updateCodeList
// ============================================================
void UIDebugSCUDSP::updateCodeList(u32 addr)
{
    lwDisassembledCode->goToAddress(addr);
    lwDisassembledCode->setPC(addr);
}

// ============================================================
//  updateAll — appelé après chaque step ou breakpoint
// ============================================================
void UIDebugSCUDSP::updateAll()
{
    updateRegList();
    if (ScuRegs) {
        scudspregs_struct regs;
        memset(&regs, 0, sizeof(regs));
        ScuDspGetRegisters(&regs);
        updateCodeList(regs.PC);
    }
    // Rafraîchir l'onglet actif des infos supplémentaires
    onTabChanged(m_tabExtra ? m_tabExtra->currentIndex() : 0);
}

// ============================================================
//  Helpers — décodage DMA
// ============================================================
QString UIDebugSCUDSP::decodeDMAChannel(u32 R, u32 W, u32 C,
                                        u32 AD, u32 EN, u32 MD, int ch) const
{
    // SCU HW Manual §7 : DMA mode register bits
    u32 mode     = MD & 0x7;          // bits 2-0 : start factor
    u32 readAdd  = (AD >> 8) & 0xFF;  // bits 15-8 : read address add
    u32 writeAdd = AD & 0xFF;         // bits 7-0  : write address add
    bool enabled  = (EN & 0x1) != 0;
    bool indirect = ((MD >> 24) & 0x1) != 0;

    // Start factor names (SCU HW manual §7.4)
    static const char *sf[16] = {
        "VBlank-IN","VBlank-OUT","HBlank-IN","Timer0",
        "Timer1","Sound Req","Sprite Draw End","DMA-illegal",
        "Ext0","Ext1","Ext2","Ext3","Ext4","Ext5","Ext6","Sprite"
    };
    const char *sfName = (mode < 16) ? sf[mode] : "?";

    std::ostringstream o;
    o << "--- DMA Channel " << ch << (enabled?" [ENABLED]":" [disabled]") << " ---\n";
    o << "  Src  : 0x" << std::hex << std::uppercase
      << std::setw(8) << std::setfill('0') << R
      << "  +=" << std::dec << readAdd << "\n";
    o << "  Dst  : 0x" << std::hex << std::setw(8) << std::setfill('0') << W
      << "  +=" << std::dec << writeAdd << "\n";
    o << "  Count: " << std::dec << C << " (0x" << std::hex << C << ")\n";
    o << "  Mode : " << (indirect ? "Indirect" : "Direct")
      << "  StartFactor=" << sfName << " (" << std::dec << mode << ")\n";
    o << "  EN=0x" << std::hex << std::setw(8) << std::setfill('0') << EN
      << "  MD=0x" << std::setw(8) << MD
      << "  AD=0x" << std::setw(8) << AD << "\n";
    return QString::fromStdString(o.str());
}

// ============================================================
//  Helpers — décodage IRQ / IMS / IST
// ============================================================
QString UIDebugSCUDSP::decodeInterruptMask(u32 IMS, u32 IST) const
{
    // SCU HW Manual §8 — Interrupt mask/status bits
    struct { int bit; const char *name; } irqs[] = {
        {0,"VBlank-IN"},{1,"VBlank-OUT"},{2,"HBlank-IN"},{3,"Timer0"},
        {4,"Timer1"},{5,"DSP End"},{6,"Sound Req"},{7,"Sys Manager"},
        {8,"Pad"},{9,"Lv2DMA"},{10,"Lv1DMA"},{11,"Lv0DMA"},
        {12,"DMA Illegal"},{13,"Sprite Draw"},{14,"(rsvd)"},{15,"A-Bus Int"},
        {16,"ExtInt0"},{17,"ExtInt1"},{18,"ExtInt2"},{19,"ExtInt3"},
        {20,"ExtInt4"},{21,"ExtInt5"},{22,"ExtInt6"},{23,"ExtInt7"},
        {24,"ExtInt8"},{25,"ExtInt9"},{26,"ExtIntA"},{27,"ExtIntB"},
        {28,"ExtIntC"},{29,"ExtIntD"},{30,"ExtIntE"},{31,"ExtIntF"}
    };
    std::ostringstream o;
    o << "IMS = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << IMS << "\n";
    o << "IST = 0x" << std::setw(8) << IST << "\n\n";
    o << std::left << std::setw(20) << "Interrupt"
      << std::setw(8) << "Masked" << std::setw(8) << "Pending" << "\n";
    o << repeatChar('-', 36) << "\n";
    for (auto &ir : irqs) {
        bool masked  = (IMS >> ir.bit) & 1;
        bool pending = (IST >> ir.bit) & 1;
        if (!masked || pending) { // n'afficher que les lignes significatives
            o << std::left << std::setw(20) << ir.name
              << std::setw(8) << (masked  ? "yes" : "NO ")
              << std::setw(8) << (pending ? "YES" : "no") << "\n";
        }
    }
    o << "\n(Only unmasked or pending interrupts shown)\n";
    return QString::fromStdString(o.str());
}

// ============================================================
//  Helpers — timers et registres divers
// ============================================================
QString UIDebugSCUDSP::decodeTimers() const
{
    if (!ScuRegs) return "SCU not initialized";
    std::ostringstream o;
    o << "--- Timer 0 ---\n";
    o << "  T0C  = " << std::dec << ScuRegs->T0C
      << " (compare at line " << ScuRegs->T0C << ")\n";
    o << "  timer0 counter = " << ScuRegs->timer0 << "\n\n";
    o << "--- Timer 1 ---\n";
    o << "  T1S  = " << std::dec << ScuRegs->T1S << "\n";
    o << "  T1MD = 0x" << std::hex << std::uppercase
      << std::setw(8) << std::setfill('0') << ScuRegs->T1MD;
    o << "  (EN=" << std::dec << (ScuRegs->T1MD & 0x1)
      << "  MODE=" << ((ScuRegs->T1MD >> 7) & 0x1) << ")\n";
    o << "  timer1 counter = " << std::dec << ScuRegs->timer1 << "\n\n";
    o << "--- SCU Misc ---\n";
    o << "  VER  = " << std::dec << ScuRegs->VER << "  (SCU hardware version)\n";
    o << "  RSEL = 0x" << std::hex << ScuRegs->RSEL << "\n";
    o << "  AIACK= 0x" << std::hex << ScuRegs->AIACK << "\n";
    o << "  ASR0 = 0x" << std::hex << std::setw(8) << std::setfill('0') << ScuRegs->ASR0 << "\n";
    o << "  ASR1 = 0x" << std::hex << std::setw(8) << std::setfill('0') << ScuRegs->ASR1 << "\n";
    o << "  AREF = 0x" << std::hex << std::setw(8) << std::setfill('0') << ScuRegs->AREF << "\n\n";
    o << "--- DSP DMA internal ---\n";
    o << "  PPAF = 0x" << std::hex << std::setw(8) << std::setfill('0') << ScuRegs->PPAF << "\n";
    o << "  PPD  = 0x" << std::setw(8) << ScuRegs->PPD << "\n";
    o << "  PDA  = 0x" << std::setw(8) << ScuRegs->PDA << "\n";
    o << "  PDD  = 0x" << std::setw(8) << ScuRegs->PDD << "\n";
    o << "  DSTP = 0x" << std::setw(8) << ScuRegs->DSTP << "\n";
    o << "  DSTA = 0x" << std::setw(8) << ScuRegs->DSTA << "\n";
    return QString::fromStdString(o.str());
}

// ============================================================
//  Helpers — dump MD bank
// ============================================================
QString UIDebugSCUDSP::formatMDBank(int bank, const u32 *md) const
{
    std::ostringstream o;
    o << "--- Data RAM MD" << bank << " (64 x 32-bit words) ---\n";
    o << "Addr  Value       Dec\n";
    o << repeatChar('-', 32) << "\n";
    for (int i = 0; i < 64; i++) {
        o << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << i
          << ":   " << std::setw(8) << md[i]
          << "  " << std::dec << std::setw(10) << (s32)md[i] << "\n";
    }
    return QString::fromStdString(o.str());
}

// ============================================================
//  Slot — rafraîchir l'onglet actif
// ============================================================
void UIDebugSCUDSP::onTabChanged(int idx)
{
    if (!ScuRegs) return;

    switch (idx) {
        case 0: { // DMA
            QString s;
            s += decodeDMAChannel(ScuRegs->D0R,ScuRegs->D0W,ScuRegs->D0C,
                                  ScuRegs->D0AD,ScuRegs->D0EN,ScuRegs->D0MD, 0);
            s += "\n";
            s += decodeDMAChannel(ScuRegs->D1R,ScuRegs->D1W,ScuRegs->D1C,
                                  ScuRegs->D1AD,ScuRegs->D1EN,ScuRegs->D1MD, 1);
            s += "\n";
            s += decodeDMAChannel(ScuRegs->D2R,ScuRegs->D2W,ScuRegs->D2C,
                                  ScuRegs->D2AD,ScuRegs->D2EN,ScuRegs->D2MD, 2);
            m_pteDMA->setPlainText(s);
            break;
        }
        case 1: { // IRQ
            m_pteIRQ->setPlainText(decodeInterruptMask(ScuRegs->IMS, ScuRegs->IST));
            break;
        }
        case 2: { // Data RAM
            scudspregs_struct regs;
            memset(&regs, 0, sizeof(regs));
            ScuDspGetRegisters(&regs);
            QString s;
            for (int b = 0; b < 4; b++)
                s += formatMDBank(b, regs.MD[b]) + "\n";
            m_pteMD->setPlainText(s);
            break;
        }
        case 3: { // Timers / Misc
            m_pteTimers->setPlainText(decodeTimers());
            break;
        }
        default: break;
    }
}

// ============================================================
//  UIDebugCPU interface
// ============================================================
u32 UIDebugSCUDSP::getRegister(int /*index*/, int *size)
{
    *size = 0;
    return 0;
}

void UIDebugSCUDSP::setRegister(int /*index*/, u32 /*value*/) {}

bool UIDebugSCUDSP::addCodeBreakpoint(u32 addr)
{
    if (!ScuRegs) return false;
    return ScuDspAddCodeBreakpoint(addr) == 0;
}

bool UIDebugSCUDSP::delCodeBreakpoint(u32 addr)
{
    if (!ScuRegs) return false;
    return ScuDspDelCodeBreakpoint(addr) == 0;
}

void UIDebugSCUDSP::stepInto()
{
    ScuDspStep();
    updateAll();
}

// ============================================================
//  Save helpers -- guard NULL before calling SCU functions
//  BUG CORRIGE : l'original testait !ScuRegs APRES getSaveFileName
//  → on vérifie maintenant AVANT d'ouvrir la boîte de dialogue
// ============================================================
static QString askSaveFile(const QString &title)
{
    return CommonDialogs::getSaveFileName(
        QString(), title,
        QtYabause::translate("Binary Files (*.bin)"));
}

void UIDebugSCUDSP::reserved1()
{
    if (!ScuRegs) return;
    const QString s = askSaveFile(QtYabause::translate("Save SCU DSP Program RAM"));
    if (!s.isNull()) ScuDspSaveProgram(QFile::encodeName(s));
}

void UIDebugSCUDSP::reserved2()
{
    if (!ScuRegs) return;
    const QString s = askSaveFile(QtYabause::translate("Save SCU DSP Data RAM MD0"));
    if (!s.isNull()) ScuDspSaveMD(QFile::encodeName(s), 0);
}

void UIDebugSCUDSP::reserved3()
{
    if (!ScuRegs) return;
    const QString s = askSaveFile(QtYabause::translate("Save SCU DSP Data RAM MD1"));
    if (!s.isNull()) ScuDspSaveMD(QFile::encodeName(s), 1);
}

void UIDebugSCUDSP::reserved4()
{
    if (!ScuRegs) return;
    const QString s = askSaveFile(QtYabause::translate("Save SCU DSP Data RAM MD2"));
    if (!s.isNull()) ScuDspSaveMD(QFile::encodeName(s), 2);
}

void UIDebugSCUDSP::reserved5()
{
    if (!ScuRegs) return;
    const QString s = askSaveFile(QtYabause::translate("Save SCU DSP Data RAM MD3"));
    if (!s.isNull()) ScuDspSaveMD(QFile::encodeName(s), 3);
}
