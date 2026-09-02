/*	Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

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
//
// Etait un stub : updateRegList/updateCodeList/getRegister/setRegister/
// addCodeBreakpoint/delCodeBreakpoint/stepInto ne faisaient tous rien.
// scspdsp.c n'exposait d'ailleurs aucune fonction de breakpoint/step (à la
// différence de scu.c pour le DSP SCU) : ces fonctions ont été ajoutées à
// scspdsp.c/.h pour que cette fenêtre soit réellement utilisable. Le menu
// "Debug SCSP DSP" (aViewDebugSCSPDSP) n'avait par ailleurs aucun slot
// connecté dans UIYabause : corrigé dans UIYabause.cpp/.h.
//
#include "UIDebugSCSPDSP.h"
#include "../CommonDialogs.h"
#include "UIYabause.h"
#include <QVBoxLayout>
#include <QSplitter>
#include <QPushButton>
#include <sstream>
#include <iomanip>

static std::string repeatChar(char c, int n)
{
   std::string s;
   for (int i = 0; i < n; i++) s += c;
   return s;
}

// Adapte ScspDspDisasm() à la signature attendue par lwDisassembledCode
static int SCSPDSPDis(void *context, u32 addr, char *string)
{
   (void)context;
   ScspDspDisasm((u8)addr, string);
   return 1;
}

// Callback de breakpoint -- appelé depuis le thread audio (boucle
// d'exécution DSP par échantillon dans scsp.c / ScspDspCheckBreakpoints).
// On ne fait qu'émettre un signal Qt, exactement comme pour M68K/SCU DSP :
// c'est UIYabause::breakpointHandlerSCSPDSP() (thread UI) qui se charge
// de verrouiller l'émulateur et d'ouvrir la fenêtre.
static void SCSPDSPBreakpointHandler(u32 addr)
{
   (void)addr;
   UIYabause *ui = QtYabause::mainWindow(false);
   if (ui)
      emit ui->breakpointHandlerSCSPDSP();
}

UIDebugSCSPDSP::UIDebugSCSPDSP( YabauseThread *mYabauseThread, QWidget* p )
   : UIDebugCPU( PROC_SCSPDSP, mYabauseThread, p )
   , m_tabExtra(NULL)
   , m_pteCoefMadrs(NULL)
   , m_pteTemp(NULL)
   , m_pteMems(NULL)
{
   setWindowTitle(QtYabause::translate("Debug SCSP DSP"));
   gbRegisters->setTitle(QtYabause::translate("DSP Registers"));

   // Widgets non pertinents pour un DSP (pas de bus mémoire adressable
   // séparément, pas de breakpoints mémoire au sens SH2/M68K)
   pbMemoryTransfer->setVisible(false);
   pbMemoryEditor->setVisible(false);
   gbMemoryBreakpoints->setVisible(false);

   // Boutons réservés -> Save
   pbReserved1->setText(QtYabause::translate("Save Program"));
   pbReserved2->setText(QtYabause::translate("Save COEF"));
   pbReserved3->setText(QtYabause::translate("Save MADRS"));
   pbReserved4->setText(QtYabause::translate("Save TEMP"));
   pbReserved5->setText(QtYabause::translate("Save MEMS"));
   pbReserved1->setVisible(true);
   pbReserved2->setVisible(true);
   pbReserved3->setVisible(true);
   pbReserved4->setVisible(true);
   pbReserved5->setVisible(true);
   pbReserved1->setToolTip(QtYabause::translate("Save the 128-step DSP program (MPRO, 64-bit words) to .bin"));
   pbReserved2->setToolTip(QtYabause::translate("Save the 64 COEF coefficients (16-bit) to .bin"));
   pbReserved3->setToolTip(QtYabause::translate("Save the 32 MADRS base addresses (16-bit) to .bin"));
   pbReserved4->setToolTip(QtYabause::translate("Save the 128-word TEMP ring buffer (32-bit) to .bin"));
   pbReserved5->setToolTip(QtYabause::translate("Save the 32-word MEMS registers (32-bit) to .bin"));

   {
      QSize s = lwRegisters->minimumSize();
      s.setWidth(lwRegisters->fontMetrics().averageCharWidth() * 30);
      lwRegisters->setMinimumSize(s);
   }
   {
      QSize s = lwDisassembledCode->minimumSize();
      s.setWidth(lwRegisters->fontMetrics().averageCharWidth() * 80);
      lwDisassembledCode->setMinimumSize(s);
   }

   // Onglets supplémentaires : COEF/MADRS, TEMP, MEMS/MIXS/EFREG/EXTS
   m_tabExtra = new QTabWidget(this);
   m_tabExtra->setTabPosition(QTabWidget::South);

   auto mkPTE = [&]() {
      QPlainTextEdit *pte = new QPlainTextEdit(m_tabExtra);
      pte->setReadOnly(true);
      pte->setLineWrapMode(QPlainTextEdit::NoWrap);
      QFont f("Courier New"); f.setPointSize(9);
      pte->setFont(f);
      return pte;
   };

   m_pteCoefMadrs = mkPTE();
   m_pteTemp      = mkPTE();
   m_pteMems      = mkPTE();

   m_tabExtra->addTab(m_pteCoefMadrs, "COEF / MADRS");
   m_tabExtra->addTab(m_pteTemp,      "TEMP");
   m_tabExtra->addTab(m_pteMems,      "MEMS / MIXS / EFREG");

   if (QVBoxLayout *vl = qobject_cast<QVBoxLayout*>(layout())) {
      vl->addWidget(m_tabExtra);
   } else {
      QSplitter *split = new QSplitter(Qt::Vertical, this);
      split->addWidget(m_tabExtra);
   }

   connect(m_tabExtra, &QTabWidget::currentChanged, this, &UIDebugSCSPDSP::onTabChanged);

   // Raccourci pratique : le rapport complet (registres communs + 32 slots +
   // DSP) est aussi accessible ici, pas seulement depuis la fenetre
   // "Debug SCSP" -- evite d'avoir a rouvrir l'autre fenetre en cours
   // d'investigation. Meme fonction core que UIDebugSCSP::on_pbExportFullReport_clicked().
   {
      QPushButton *pbExportFullReport = new QPushButton(QtYabause::translate("Export Full Debug Report..."), this);
      pbExportFullReport->setToolTip(QtYabause::translate(
         "Export common control registers + all 32 slots + full DSP state into a single text file"));
      connect(pbExportFullReport, &QPushButton::clicked, this, [this]() {
         const QString s = CommonDialogs::getSaveFileName(
            QString(), QtYabause::translate("Choose a location for your report"),
            QtYabause::translate("Text Files (*.txt)"));
         if (!s.isEmpty())
            if (ScspSaveFullDebugReport(s.toLatin1()) != 0)
               CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
      });
      if (QVBoxLayout *vl = qobject_cast<QVBoxLayout*>(layout()))
         vl->addWidget(pbExportFullReport);
   }

   // Breakpoints existants + branchement disassembleur/step
   const scspdspcodebreakpoint_struct *cbp = ScspDspGetBreakpointList();
   for (int i = 0; i < SCSPDSP_MAX_BREAKPOINTS; i++) {
      if (cbp[i].addr != 0xFFFFFFFF) {
         QString text = QString::asprintf("%08X", (int)cbp[i].addr);
         lwCodeBreakpoints->addItem(text);
      }
   }

   lwDisassembledCode->setDisassembleFunction(SCSPDSPDis);
   lwDisassembledCode->setEndAddress(0x80);   // 128 pas de programme max
   lwDisassembledCode->setMinimumInstructionSize(1);
   ScspDspSetBreakpointCallBack(SCSPDSPBreakpointHandler);

   updateAll();
}

void UIDebugSCSPDSP::updateAll()
{
   updateRegList();
   updateCodeList(ScspDspGetPC());
   onTabChanged(m_tabExtra ? m_tabExtra->currentIndex() : 0);
}

void UIDebugSCSPDSP::updateCodeList(u32 addr)
{
   lwDisassembledCode->goToAddress(addr);
   lwDisassembledCode->setPC(addr);
}

QString UIDebugSCSPDSP::formatRegisterList() const
{
   const ScspDsp &d = scsp_dsp;
   std::ostringstream o;
   o << std::hex << std::uppercase << std::setfill('0');

   o << "--- Program ---\n";
   o << "Debug PC   = " << std::setw(2) << ScspDspGetPC()
     << " (next step executed by \"Step Into\")\n";
   o << "last_step  = " << std::setw(2) << d.last_step
     << " (active steps run per sample, HW max 128)\n";
   o << "mdec_ct    = " << std::setw(8) << d.mdec_ct << "\n\n";

   o << "--- Multiply / ALU pipeline ---\n";
   o << "INPUTS = " << std::setw(8) << (u32)d.inputs << std::dec << "  (" << d.inputs << ")\n" << std::hex;
   o << "B      = " << std::setw(8) << (u32)d.b      << std::dec << "  (" << d.b << ")\n" << std::hex;
   o << "X      = " << std::setw(8) << (u32)d.x      << std::dec << "  (" << d.x << ")\n" << std::hex;
   o << "Y      = " << std::setw(4) << (u16)d.y       << std::dec << "  (" << d.y << ")\n" << std::hex;
   o << "Y_REG  = " << std::setw(8) << (u32)d.y_reg   << std::dec << "  (" << d.y_reg << ")\n" << std::hex;
   o << "MUL_OUT= " << std::setw(8) << (u32)d.mul_out << std::dec << "  (" << d.mul_out << ")\n" << std::hex;
   o << "ACC    = " << std::setw(8) << (u32)d.acc     << std::dec << "  (" << d.acc << ")\n" << std::hex;
   o << "SHIFTED= " << std::setw(8) << (u32)d.shifted << std::dec << "  (" << d.shifted << ")\n\n" << std::hex;

   o << "--- Ring buffer memory access (MRD/MWT) ---\n";
   o << "FRC_REG  = " << std::setw(4) << d.frc_reg << "\n";
   o << "ADRS_REG = " << std::setw(4) << d.adrs_reg << "\n";
   o << "RBP      = " << std::setw(8) << (u32)d.rbp << "  (ring buffer base)\n";
   o << "RBL      = " << std::setw(8) << (u32)d.rbl << "  (ring buffer length code)\n";
   o << "MRD_VALUE= " << std::setw(8) << d.mrd_value << "\n";
   o << "SHIFT_REG= " << std::setw(8) << d.shift_reg << "\n\n";

   o << "--- Pending I/O (EWA/EWT, effect memory access) ---\n";
   o << "io_addr     = " << std::setw(8) << d.io_addr << "\n";
   o << "need_read   = " << std::dec << d.need_read  << std::hex << "\n";
   o << "need_write  = " << std::dec << d.need_write << std::hex << "\n";
   o << "write_data  = " << std::setw(4) << d.write_data << "\n";
   o << "need_nofl   = " << std::dec << d.need_nofl  << std::hex << "\n";
   o << "read_pending  = " << std::dec << d.read_pending  << std::hex << "\n";
   o << "write_pending = " << std::dec << d.write_pending << std::hex << "\n";
   o << "read_value    = " << std::setw(8) << d.read_value  << "\n";
   o << "write_value   = " << std::setw(8) << d.write_value << "\n";
   o << "updated       = " << std::dec << d.updated << std::hex << "\n";
   o << "product (64b) = 0x" << std::setw(16) << (unsigned long long)d.product << std::dec << "\n";

   return QString::fromStdString(o.str());
}

void UIDebugSCSPDSP::updateRegList()
{
   lwRegisters->clear();
   QString s = formatRegisterList();
   const QStringList lines = s.split('\n');
   for (const QString &line : lines)
      lwRegisters->addItem(line);
}

QString UIDebugSCSPDSP::formatCoefMadrs() const
{
   const ScspDsp &d = scsp_dsp;
   std::ostringstream o;
   o << std::hex << std::uppercase << std::setfill('0');

   o << "--- COEF (64 x 16-bit filter/mix coefficients) ---\n";
   o << "Idx  Hex    Signed\n" << repeatChar('-', 26) << "\n";
   for (int i = 0; i < 64; i++)
      o << std::dec << std::setfill(' ') << std::setw(3) << i << "  0x"
        << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << d.coef[i]
        << "  " << std::dec << (s16)d.coef[i] << "\n";

   o << "\n--- MADRS (32 x 16-bit base addresses, one per MPRO opcode's MASA/ADREB) ---\n";
   o << "Idx  Hex\n" << repeatChar('-', 12) << "\n";
   for (int i = 0; i < 32; i++)
      o << std::dec << std::setfill(' ') << std::setw(3) << i << "  0x"
        << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << d.madrs[i] << "\n";

   return QString::fromStdString(o.str());
}

QString UIDebugSCSPDSP::formatTemp() const
{
   const ScspDsp &d = scsp_dsp;
   std::ostringstream o;
   o << "--- TEMP (128 x 32-bit DSP working ring buffer, addressed by TRA/TWA) ---\n";
   o << std::hex << std::uppercase << std::setfill('0');
   for (int i = 0; i < 128; i++)
   {
      o << std::dec << std::setfill(' ') << std::setw(3) << i << ": "
        << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << (u32)d.temp[i];
      o << ((i % 4 == 3) ? "\n" : "   ");
   }
   o << "\n";
   return QString::fromStdString(o.str());
}

QString UIDebugSCSPDSP::formatMemsMixsEfreg() const
{
   const ScspDsp &d = scsp_dsp;
   std::ostringstream o;
   o << std::hex << std::uppercase << std::setfill('0');

   o << "--- MEMS (32 x 32-bit persistent \"M\" registers, kept across samples) ---\n";
   for (int i = 0; i < 32; i++)
   {
      o << std::dec << std::setfill(' ') << std::setw(2) << i << ": "
        << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << (u32)d.mems[i];
      o << ((i % 4 == 3) ? "\n" : "   ");
   }

   o << "\n\n--- MIXS (16 x 32-bit, per-slot audio mixed into the DSP, ISEL/IMXL) ---\n";
   for (int i = 0; i < 16; i++)
      o << std::dec << std::setfill(' ') << std::setw(2) << i << ": "
        << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << (u32)d.mixs[i] << "\n";

   o << "\n--- EFREG (16 x 16-bit DSP effect output, mixed back into the final stereo output) ---\n";
   for (int i = 0; i < 16; i++)
      o << std::dec << std::setfill(' ') << std::setw(2) << i << ": "
        << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << (u16)d.efreg[i]
        << std::dec << "  (" << d.efreg[i] << ")\n" << std::hex;

   o << "\n--- EXTS (2 x 16-bit external/CD input to the DSP) ---\n";
   o << "EXTS[0] = " << std::setw(4) << (u16)d.exts[0] << std::dec << "  (" << d.exts[0] << ")\n" << std::hex;
   o << "EXTS[1] = " << std::setw(4) << (u16)d.exts[1] << std::dec << "  (" << d.exts[1] << ")\n";

   return QString::fromStdString(o.str());
}

void UIDebugSCSPDSP::onTabChanged(int idx)
{
   switch (idx)
   {
      case 0: m_pteCoefMadrs->setPlainText(formatCoefMadrs()); break;
      case 1: m_pteTemp->setPlainText(formatTemp()); break;
      case 2: m_pteMems->setPlainText(formatMemsMixsEfreg()); break;
      default: break;
   }
}

u32 UIDebugSCSPDSP::getRegister(int /*index*/, int *size)
{
   // Comme UIDebugSCUDSP: la liste de registres mélange en-têtes et
   // valeurs formatées en une seule chaîne, sans mapping 1:1 index->champ
   // fiable ; on ne permet donc pas l'édition depuis cette liste (même
   // convention que le débogueur SCU DSP).
   *size = 0;
   return 0;
}

void UIDebugSCSPDSP::setRegister(int /*index*/, u32 /*value*/)
{
}

bool UIDebugSCSPDSP::addCodeBreakpoint(u32 addr)
{
   return ScspDspAddCodeBreakpoint(addr) == 0;
}

bool UIDebugSCSPDSP::delCodeBreakpoint(u32 addr)
{
   return ScspDspDelCodeBreakpoint(addr) == 0;
}

void UIDebugSCSPDSP::stepInto()
{
   ScspDspStep();
   updateAll();
}

static QString askSaveFile(const QString &title)
{
   return CommonDialogs::getSaveFileName(
      QString(), title,
      QtYabause::translate("Binary Files (*.bin)"));
}

void UIDebugSCSPDSP::reserved1()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP Program (MPRO)"));
   if (!s.isEmpty())
      if (ScspDspSaveProgram(s.toLatin1()) != 0)
         CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}

void UIDebugSCSPDSP::reserved2()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP COEF"));
   if (!s.isEmpty())
      if (ScspDspSaveCoef(s.toLatin1()) != 0)
         CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}

void UIDebugSCSPDSP::reserved3()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP MADRS"));
   if (!s.isEmpty())
      if (ScspDspSaveMadrs(s.toLatin1()) != 0)
         CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}

void UIDebugSCSPDSP::reserved4()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP TEMP"));
   if (!s.isEmpty())
      if (ScspDspSaveTemp(s.toLatin1()) != 0)
         CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}

void UIDebugSCSPDSP::reserved5()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP MEMS"));
   if (!s.isEmpty())
      if (ScspDspSaveMems(s.toLatin1()) != 0)
         CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}
