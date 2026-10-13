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
#include <QVector>

#ifdef HAVE_QT_MULTIMEDIA
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif

class YabauseThread;
class QPushButton;
class QTimer;

class UIDebugSCSP : public QDialog, public Ui::UIDebugSCSP
{
	Q_OBJECT
private:
	// Needed to open UIDebugSCSPDSP (its constructor requires a YabauseThread*)
	YabauseThread *mYabauseThread;

	QPushButton *slotButtons[32];
	bool emulationReady;

#ifdef HAVE_QT_MULTIMEDIA
	QTimer *audioBufferTimer;
	QAudioDeviceInfo audioDeviceInfo;
	QAudioOutput *audioOutput;
	QIODevice *outputDevice;
	QAudioFormat audioFormat;

	bool playing;        // true while a slot is being previewed
	bool audioUsable;    // false when the device cannot do 44.1 kHz stereo 16-bit
	u32 chunkFrames;     // stereo frames generated per call to ScspSlotDebugAudio()

	// ScspSlotDebugAudio() needs 2 * frames u32 of work space and writes
	// 2 * frames s16 (interleaved stereo)
	QVector<u32> workBuf;
	QVector<s16> slotBuf;
#endif

	void setupSlotGrid();
	void applyStyle();
	void refreshSlot(int slot);
	void refreshCommon();
	void updateSlotButtons();
	void fillSlotInfo(const QString &text);
	void fillCommonInfo(const QString &text);
	void applyFilter();

public:
	UIDebugSCSP( YabauseThread *mYabauseThread, QWidget* parent = 0 );
	~UIDebugSCSP();

#ifdef HAVE_QT_MULTIMEDIA
protected:
	void initAudio();
	void startPlayingSlot(int slot);
	void stopPlayback(const QString &status);
#endif

protected slots:
   void on_sbSlotNumber_valueChanged ( int i );
   void on_leFilter_textChanged ( const QString &text );
   void on_pbSaveAsWav_clicked ();
   void on_pbSaveSlotRegisters_clicked ();
   void on_pbOpenDSPDebugger_clicked ();
   void on_pbOpenChannelViewer_clicked ();
#ifdef HAVE_QT_MULTIMEDIA
	void on_pbPlaySlot_clicked ();
	void audioBufferRefill();
	void stateChanged(QAudio::State state);
#endif
};

#endif // UIDEBUGSCSP_H
