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
#ifndef UIDEBUGSCSP_H
#define UIDEBUGSCSP_H

#include "ui_UIDebugSCSP.h"
#include "../QtYabause.h"

#ifdef HAVE_QT_MULTIMEDIA
#include <QAudioOutput>
#endif

class YabauseThread;

class UIDebugSCSP : public QDialog, public Ui::UIDebugSCSP
{
	Q_OBJECT
private:
	// Necessaire pour ouvrir UIDebugSCSPDSP/UIDebugCPU (constructeur exige
	// un YabauseThread*) et pour le verrouiller le temps d'ouvrir cette
	// fenetre, comme les autres boutons de navigation inter-debuggers.
	YabauseThread *mYabauseThread;

#ifdef HAVE_QT_MULTIMEDIA
	QTimer *audioBufferTimer = nullptr;

	QAudioDeviceInfo audioDeviceInfo;
	// Defauts explicites : on_sbSlotNumber_valueChanged() (auto-connecte
	// par setupUi(), donc potentiellement declenche par les
	// sbSlotNumber->setValue(...) tout au debut du constructeur, avant
	// qu'initAudio() n'ait tourne) lit audioOutput/isPlaying pour savoir
	// s'il doit redemarrer la lecture sur le nouveau slot. Sans ces
	// initialisateurs, ce sont des pointeurs/bool non initialises a ce
	// moment-la -- un member par defaut garantit un etat sur, avant meme
	// le corps du constructeur.
	QAudioOutput *audioOutput = nullptr;
	QIODevice *outputDevice = nullptr;
	QAudioFormat audioFormat;
	bool isPlaying = true;

	u32 *slot_workbuf = nullptr;
	s16 *slot_buf = nullptr;
#endif

public:
	UIDebugSCSP( YabauseThread *mYabauseThread, QWidget* parent = 0 );
	~UIDebugSCSP();

#ifdef HAVE_QT_MULTIMEDIA
protected:
	void initAudio();
	// Factorise le (re)demarrage de la lecture, utilise par
	// on_pbPlaySlot_clicked() et par le changement de slot pendant la
	// lecture (voir on_sbSlotNumber_valueChanged).
	void startPlayingSlot(int slot);
#endif

protected slots:
   void on_sbSlotNumber_valueChanged ( int i );
   void on_pbSaveAsWav_clicked ();
   void on_pbSaveSlotRegisters_clicked ();
   void on_pbOpenDSPDebugger_clicked ();
   void on_pbOpenChannelViewer_clicked ();
#ifdef HAVE_QT_MULTIMEDIA
	void on_pbPlaySlot_clicked ();
	void notified();
	void audioBufferRefill();
	void stateChanged(QAudio::State state);
#endif
};

#endif // UIDEBUGSCSP_H
