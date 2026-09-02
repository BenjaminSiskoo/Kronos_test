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

// Etait un stub quasi vide (updateRegList/getRegister/etc. tous no-op,
// pas de breakpoints, pas de step, aucun onglet mémoire) alors que le
// debugger SCU DSP voisin (UIDebugSCUDSP) est complet. Réécrit sur le
// même modèle : liste de registres DSP réels, onglets COEF/MADRS/TEMP/
// MEMS-MIXS-EFREG, breakpoints et step branchés sur la nouvelle API
// ScspDsp* de scspdsp.c/.h, boutons Save.
class UIDebugSCSPDSP : public UIDebugCPU
{
	Q_OBJECT

public:
	explicit UIDebugSCSPDSP( YabauseThread *mYabauseThread, QWidget* parent = 0 );

	void updateRegList();
	void updateCodeList(u32 addr);
	void updateAll();          // pas virtuelle dans UIDebugCPU, appelée explicitement
	u32 getRegister(int index, int *size);
	void setRegister(int index, u32 value);
	bool addCodeBreakpoint(u32 addr);
	bool delCodeBreakpoint(u32 addr);
	void stepInto();

	// Boutons réservés -> fonctions Save (pbReserved1-5)
	void reserved1();   // Save Program (MPRO)
	void reserved2();   // Save COEF
	void reserved3();   // Save MADRS
	void reserved4();   // Save TEMP
	void reserved5();   // Save MEMS

private:
	QString formatRegisterList() const;
	QString formatCoefMadrs() const;
	QString formatTemp() const;
	QString formatMemsMixsEfreg() const;

	QTabWidget     *m_tabExtra;
	QPlainTextEdit *m_pteCoefMadrs;
	QPlainTextEdit *m_pteTemp;
	QPlainTextEdit *m_pteMems;

protected slots:
	void onTabChanged(int idx);
};

#endif // UIDEBUGSCSPDSP_H
