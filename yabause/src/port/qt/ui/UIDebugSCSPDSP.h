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
#ifndef UIDEBUGSCSPDSP_H
#define UIDEBUGSCSPDSP_H

#include "UIDebugCPU.h"
#include "../QtYabause.h"
#include <QTabWidget>
#include <QPlainTextEdit>

// Le noyau (scspdsp.c/.h) expose deja toute l'API necessaire (registres,
// step, breakpoints code, dumps) -- voir le commentaire de scspdsp.h :
// "so the Qt debugger windows (UIDebugSCSPDSP) can be filled in the same
// way the M68K and SCU DSP ones already are." Cette classe etait restee un
// squelette : les 7 methodes ci-dessous existaient toutes mais avec un
// corps vide (ou "return true;" pour les breakpoints), donc ouvrir cette
// fenetre affichait une liste de registres vide en permanence et aucun
// bouton (Step Into, Add/Del Breakpoint...) n'avait d'effet. Implementee
// ici en suivant exactement le meme schema que UIDebugSCUDSP (deja
// complete), y compris l'onglet supplementaire pour les tableaux internes
// (COEF/MADRS/TEMP/MEMS/MIXS/EFREG) que lwRegisters n'a pas vocation a
// afficher en entier.
class UIDebugSCSPDSP : public UIDebugCPU
{
	Q_OBJECT
public:
	UIDebugSCSPDSP( YabauseThread *mYabauseThread, QWidget* parent = 0 );
   void updateRegList();
   void updateCodeList(u32 addr);
   void updateAll();           // pas virtuelle dans UIDebugCPU (comme UIDebugSCUDSP)
   u32 getRegister(int index, int *size);
   void setRegister(int index, u32 value);
   bool addCodeBreakpoint(u32 addr);
   bool delCodeBreakpoint(u32 addr);
   void stepInto();

   // Boutons reserved1-5 -> sauvegardes brutes (mirroring UIDebugSCUDSP)
   void reserved1();   // Save Program (MPRO)
   void reserved2();   // Save COEF
   void reserved3();   // Save MADRS
   void reserved4();   // Save TEMP
   void reserved5();   // Save MEMS

private:
   QString formatWords16(const u16 *data, int count, int perLine) const;
   QString formatWords32(const s32 *data, int count, int perLine) const;

   QTabWidget     *m_tabExtra;
   QPlainTextEdit *m_pteCoefMadrs;
   QPlainTextEdit *m_pteTemp;
   QPlainTextEdit *m_pteMemsMixs;
   QPlainTextEdit *m_pteEfregExts;

protected slots:
   void onTabChanged(int idx);
};

#endif // UIDEBUGSCSPDSP_H
