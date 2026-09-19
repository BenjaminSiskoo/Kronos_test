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
#include "UIDebugSCSPDSP.h"
#include "../CommonDialogs.h"
#include "UIYabause.h"
#include <QVBoxLayout>
#include <QFile>

extern "C" {
#include "scspdsp.h"
}

int SCSPDSPDis(void *context, u32 addr, char *string)
{
   (void)context;
   ScspDspDisasm((u8)addr, string);
   return 1;
}

// ============================================================
//  Breakpoint callback -- runs from inside the main emulation loop (the
//  SCSP per-sample DSP step is cycle-driven from the same thread as the
//  rest of the core, same as the M68K/SCU DSP breakpoint callbacks this
//  mirrors), so calling straight into Qt here is the same, already-used
//  pattern as SCUDSPBreakpointHandler/M68KBreakpointHandler.
//
//  BUG CORRIGE : cette fonction existait deja (l'action de menu "Debug
//  SCSP DSP" et breakpointHandlerSCSPDSP() en dependent), mais son corps
//  etait vide -- un breakpoint DSP qui se declenchait ne faisait donc
//  rien du tout, silencieusement. Fixe en meme temps que le bug plus
//  profond (ScspDspCheckBreakpoints() jamais appelee, voir scsp.c).
// ============================================================
void SCSPDSPBreakpointHandler(u32 addr)
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
   , m_pteMemsMixs(NULL)
   , m_pteEfregExts(NULL)
{
   this->setWindowTitle(QtYabause::translate("Debug SCSP DSP"));
   gbRegisters->setTitle(QtYabause::translate("DSP Registers"));
	// BUG CORRIGE : etait "setVisible(true)". "Memory Transfer" copie une
	// plage entre deux zones adressables par le CPU en cours de debug ;
	// le DSP SCSP n'a pas de bus memoire de ce genre (juste son propre
	// programme MPRO + ses tableaux internes), donc ce bouton n'avait
	// jamais eu de sens ici -- UIDebugSCUDSP (l'autre DSP) le masque.
	pbMemoryTransfer->setVisible( false );
	gbMemoryBreakpoints->setVisible( false );

   // Boutons reserves -> sauvegardes brutes (memes fonctions que celles
   // deja utilisees par ScspSaveFullDebugReport, cf. scspdsp.h)
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
   pbReserved1->setToolTip(QtYabause::translate("Save the DSP program (MPRO, 128 x 64-bit instructions) to .bin"));
   pbReserved2->setToolTip(QtYabause::translate("Save the COEF table (64 x 16-bit filter coefficients) to .bin"));
   pbReserved3->setToolTip(QtYabause::translate("Save the MADRS table (32 x 16-bit ring buffer addresses) to .bin"));
   pbReserved4->setToolTip(QtYabause::translate("Save the TEMP delay-line memory (128 x 32-bit words) to .bin"));
   pbReserved5->setToolTip(QtYabause::translate("Save the MEMS sound-memory work area (32 x 32-bit words) to .bin"));

   QSize size = lwRegisters->minimumSize();
   size.setWidth(size.width() + lwRegisters->fontMetrics().averageCharWidth());
   lwRegisters->setMinimumSize(size);

   size = lwDisassembledCode->minimumSize();
   size.setWidth(lwRegisters->fontMetrics().averageCharWidth() * 80);
   lwDisassembledCode->setMinimumSize(size);
   lwDisassembledCode->setDisassembleFunction(SCSPDSPDis);
   lwDisassembledCode->setEndAddress(128);

   // ── Onglet supplementaire pour les tableaux internes ──
   // lwRegisters est une liste a plat (un registre par ligne, editable par
   // double-clic) : y caser les 128 mots de TEMP ou les 64 de COEF s'y
   // serait mal prete. Meme solution que UIDebugSCUDSP pour ses propres
   // tableaux DMA/MD : un QTabWidget ajoute au layout existant.
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

   m_pteCoefMadrs = mkPTE();
   m_pteTemp      = mkPTE();
   m_pteMemsMixs  = mkPTE();
   m_pteEfregExts = mkPTE();

   m_tabExtra->addTab(m_pteCoefMadrs, "COEF / MADRS");
   m_tabExtra->addTab(m_pteTemp,      "TEMP");
   m_tabExtra->addTab(m_pteMemsMixs,  "MEMS / MIXS");
   m_tabExtra->addTab(m_pteEfregExts, "EFREG / EXTS");

   if (auto *vl = qobject_cast<QVBoxLayout*>(layout())) {
      vl->addWidget(m_tabExtra);
   }

   connect(m_tabExtra, &QTabWidget::currentChanged, this, &UIDebugSCSPDSP::onTabChanged);

   // ── Breakpoints deja poses (rouvrir la fenetre ne doit pas les perdre) ──
   const scspdspcodebreakpoint_struct *cbp = ScspDspGetBreakpointList();
   for (int i = 0; i < SCSPDSP_MAX_BREAKPOINTS; i++) {
      if (cbp[i].addr != 0xFFFFFFFF) {
         QString text = QString::asprintf("%02X", (int)cbp[i].addr);
         lwCodeBreakpoints->addItem(text);
      }
   }
   ScspDspSetBreakpointCallBack(SCSPDSPBreakpointHandler);

   updateAll();
}

// ============================================================
//  updateRegList -- registres scalaires du DSP dans lwRegisters. Les gros
//  tableaux (COEF/MADRS/TEMP/MEMS/MIXS/EFREG/EXTS) vont dans m_tabExtra
//  (voir onTabChanged) plutot qu'ici : les y melanger aurait noye les
//  quelques registres qu'on regarde vraiment pas a pas (ACC, X, Y, PC...)
//  au milieu de 128+ lignes de tableau.
// ============================================================
void UIDebugSCSPDSP::updateRegList()
{
   lwRegisters->clear();

   lwRegisters->addItem("--- Program ---");
   lwRegisters->addItem(QString::asprintf("PC        =    %02X", ScspDspGetPC()));
   lwRegisters->addItem(QString::asprintf("last_step =    %d (max 128)", scsp_dsp.last_step));
   lwRegisters->addItem(QString::asprintf("mdec_ct   =  %08X", (unsigned int)scsp_dsp.mdec_ct));

   lwRegisters->addItem("--- Multiply / ALU pipeline ---");
   lwRegisters->addItem(QString::asprintf("INPUTS  =  %08X  (%d)", (unsigned int)scsp_dsp.inputs, scsp_dsp.inputs));
   lwRegisters->addItem(QString::asprintf("B       =  %08X  (%d)", (unsigned int)scsp_dsp.b, scsp_dsp.b));
   lwRegisters->addItem(QString::asprintf("X       =  %08X  (%d)", (unsigned int)scsp_dsp.x, scsp_dsp.x));
   lwRegisters->addItem(QString::asprintf("Y       =      %04X  (%d)", (unsigned short)scsp_dsp.y, scsp_dsp.y));
   lwRegisters->addItem(QString::asprintf("Y_REG   =  %08X  (%d)", (unsigned int)scsp_dsp.y_reg, scsp_dsp.y_reg));
   lwRegisters->addItem(QString::asprintf("MUL_OUT =  %08X  (%d)", (unsigned int)scsp_dsp.mul_out, scsp_dsp.mul_out));
   lwRegisters->addItem(QString::asprintf("ACC     =  %08X  (%d)", (unsigned int)scsp_dsp.acc, scsp_dsp.acc));
   lwRegisters->addItem(QString::asprintf("SHIFTED =  %08X  (%d)", (unsigned int)scsp_dsp.shifted, scsp_dsp.shifted));

   lwRegisters->addItem("--- Ring buffer access (MRD/MWT) ---");
   lwRegisters->addItem(QString::asprintf("FRC_REG   =      %04X", scsp_dsp.frc_reg));
   lwRegisters->addItem(QString::asprintf("ADRS_REG  =      %04X", scsp_dsp.adrs_reg));
   lwRegisters->addItem(QString::asprintf("RBP       =  %08X", (unsigned int)scsp_dsp.rbp));
   lwRegisters->addItem(QString::asprintf("RBL       =  %08X", (unsigned int)scsp_dsp.rbl));
   lwRegisters->addItem(QString::asprintf("MRD_VALUE =  %08X", scsp_dsp.mrd_value));
   lwRegisters->addItem(QString::asprintf("SHIFT_REG =  %08X", scsp_dsp.shift_reg));

   lwRegisters->addItem("--- Pending I/O (EWA/EWT) ---");
   lwRegisters->addItem(QString::asprintf("io_addr    =  %08X", scsp_dsp.io_addr));
   lwRegisters->addItem(QString::asprintf("need_read  =  %d   need_write = %d", scsp_dsp.need_read, scsp_dsp.need_write));
   lwRegisters->addItem(QString::asprintf("write_data =      %04X", scsp_dsp.write_data));
   lwRegisters->addItem(QString::asprintf("read_value =  %08X", scsp_dsp.read_value));
}

// ============================================================
//  updateCodeList
// ============================================================
void UIDebugSCSPDSP::updateCodeList(u32 addr)
{
   lwDisassembledCode->goToAddress(addr);
   lwDisassembledCode->setPC(addr);
}

// ============================================================
//  updateAll -- appele apres chaque step ou breakpoint (voir le
//  commentaire de UIDebugSCUDSP::updateAll : non-virtuelle dans la classe
//  de base, donc seuls les appels emis depuis les methodes de CETTE
//  classe -- stepInto() etc, elles bien virtuelles -- rafraichissent
//  m_tabExtra ; meme limitation deja acceptee pour UIDebugSCUDSP).
// ============================================================
void UIDebugSCSPDSP::updateAll()
{
   updateRegList();
   updateCodeList(ScspDspGetPC());
   onTabChanged(m_tabExtra ? m_tabExtra->currentIndex() : 0);
}

// ============================================================
//  Helpers de formatage tableau (16 et 32 bits)
// ============================================================
QString UIDebugSCSPDSP::formatWords16(const u16 *data, int count, int perLine) const
{
   QString s;
   for (int i = 0; i < count; i++) {
      s += QString::asprintf("%3d: %04X%s", i, data[i], ((i % perLine) == perLine - 1) ? "\n" : "   ");
   }
   if (count % perLine != 0) s += "\n";
   return s;
}

QString UIDebugSCSPDSP::formatWords32(const s32 *data, int count, int perLine) const
{
   QString s;
   for (int i = 0; i < count; i++) {
      s += QString::asprintf("%3d: %08X  (%d)%s", i, (unsigned int)data[i], data[i],
                              ((i % perLine) == perLine - 1) ? "\n" : "   ");
   }
   if (count % perLine != 0) s += "\n";
   return s;
}

// ============================================================
//  onTabChanged -- rafraichit uniquement l'onglet visible
// ============================================================
void UIDebugSCSPDSP::onTabChanged(int idx)
{
   switch (idx) {
      case 0: { // COEF / MADRS
         QString s = "--- COEF (64 x 16-bit filter coefficients) ---\n";
         s += formatWords16(scsp_dsp.coef, 64, 8);
         s += "\n--- MADRS (32 x 16-bit ring buffer addresses) ---\n";
         s += formatWords16(scsp_dsp.madrs, 32, 8);
         m_pteCoefMadrs->setPlainText(s);
         break;
      }
      case 1: { // TEMP
         QString s = "--- TEMP (128 x 32-bit delay-line memory) ---\n";
         s += formatWords32(scsp_dsp.temp, 128, 4);
         m_pteTemp->setPlainText(s);
         break;
      }
      case 2: { // MEMS / MIXS
         QString s = "--- MEMS (32 x 32-bit sound-memory work area) ---\n";
         s += formatWords32(scsp_dsp.mems, 32, 4);
         s += "\n--- MIXS (16 x 32-bit mix bus accumulators) ---\n";
         s += formatWords32(scsp_dsp.mixs, 16, 4);
         m_pteMemsMixs->setPlainText(s);
         break;
      }
      case 3: { // EFREG / EXTS
         QString s = "--- EFREG (16 x 16-bit effect output registers) ---\n";
         for (int i = 0; i < 16; i++)
            s += QString::asprintf("%2d: %04X  (%d)\n", i, (unsigned short)scsp_dsp.efreg[i], scsp_dsp.efreg[i]);
         s += "\n--- EXTS (2 x 16-bit external CD audio input) ---\n";
         s += QString::asprintf("L: %04X  (%d)\n", (unsigned short)scsp_dsp.exts[0], scsp_dsp.exts[0]);
         s += QString::asprintf("R: %04X  (%d)\n", (unsigned short)scsp_dsp.exts[1], scsp_dsp.exts[1]);
         m_pteEfregExts->setPlainText(s);
         break;
      }
      default: break;
   }
}

// ============================================================
//  UIDebugCPU interface
//  getRegister/setRegister : comme UIDebugSCUDSP, l'edition par
//  double-clic n'est pas cablee pour ce DSP -- la plupart de ces
//  registres sont recalcules a chaque step de toute facon, les editer a
//  la main n'a pas grand sens (contrairement a un vrai CPU a usage
//  general).
// ============================================================
u32 UIDebugSCSPDSP::getRegister(int /*index*/, int *size)
{
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

// ============================================================
//  Save helpers
// ============================================================
static QString askSaveFile(const QString &title)
{
   return CommonDialogs::getSaveFileName(
      QString(), title,
      QtYabause::translate("Binary Files (*.bin)"));
}

void UIDebugSCSPDSP::reserved1()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP Program (MPRO)"));
   if (!s.isNull()) ScspDspSaveProgram(QFile::encodeName(s));
}

void UIDebugSCSPDSP::reserved2()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP COEF"));
   if (!s.isNull()) ScspDspSaveCoef(QFile::encodeName(s));
}

void UIDebugSCSPDSP::reserved3()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP MADRS"));
   if (!s.isNull()) ScspDspSaveMadrs(QFile::encodeName(s));
}

void UIDebugSCSPDSP::reserved4()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP TEMP"));
   if (!s.isNull()) ScspDspSaveTemp(QFile::encodeName(s));
}

void UIDebugSCSPDSP::reserved5()
{
   const QString s = askSaveFile(QtYabause::translate("Save SCSP DSP MEMS"));
   if (!s.isNull()) ScspDspSaveMems(QFile::encodeName(s));
}
