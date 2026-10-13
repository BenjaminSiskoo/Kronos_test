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
#ifndef UIDebugSCSPChan_H
#define UIDebugSCSPChan_H

#include "../QtYabause.h"
#include <QDialog>
#include <QCheckBox>
#include <QLabel>
#include <QVector>

#ifdef HAVE_QT_MULTIMEDIA
#include <QAudioOutput>
#endif

class QTimer;
class QGridLayout;
class QPushButton;

// Draws the 32 slot envelopes as bars (height = loudness, color = envelope
// phase) with the slot number under each bar and a color legend. It paints
// with the dialog palette, so it follows light and dark themes.
class SlotMeterWidget : public QWidget
{
public:
   explicit SlotMeterWidget(QWidget *parent = 0);

   void setSlot(int slot, int env, int state);

   // A slot that is not keyed on, or that finished its release, is silent
   static bool isSounding(int env, int state);
   static QColor colorForState(int state, const QPalette &pal);

   QSize sizeHint() const;
   QSize minimumSizeHint() const;

protected:
   void paintEvent(QPaintEvent *event);

private:
   int env_[32];
   int state_[32];
};

class UIDebugSCSPChan : public QDialog
{
   Q_OBJECT
private:
   SlotMeterWidget *meter;
   QTimer *timer;

   QVector<QCheckBox*> muteBoxes;
   QVector<u32> playingMask;     // per mute entry: slots playing that sample (bit n = slot n)
   QGridLayout *muteGrid;
   QLabel *lMuteEmpty;
   QLabel *lMuteCount;
   QPushButton *pbUnmuteAll;

   void addMuteBox(u32 sa, int muted);
   void updatePlayingSamples();
   void applyStyle();

public:
   UIDebugSCSPChan(QWidget* parent = 0);
   ~UIDebugSCSPChan();

protected slots:
   void update_window();
   void onMuteToggled(bool checked);
   void unmuteAll();
};

#endif // UIDebugSCSPChan_H
