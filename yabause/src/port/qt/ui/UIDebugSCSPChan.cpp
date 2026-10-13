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
#include "UIDebugSCSPChan.h"
#include "CommonDialogs.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QColor mixColors(const QColor &a, const QColor &b, double t)
{
   return QColor((int)(a.red() * t + b.red() * (1.0 - t)),
                 (int)(a.green() * t + b.green() * (1.0 - t)),
                 (int)(a.blue() * t + b.blue() * (1.0 - t)));
}

QColor dimColor(const QWidget *w)
{
   return mixColors(w->palette().color(QPalette::WindowText), w->palette().color(QPalette::Window), 0.6);
}

// Sample start address as 5 upper case hex digits. toUpper() used to be applied
// to the whole translated sentence, which turned the tooltip into capitals.
QString hexSa(u32 sa)
{
   return QString::number(sa, 16).rightJustified(5, QLatin1Char('0')).toUpper();
}

} // namespace

// ============================================================
//  SlotMeterWidget
// ============================================================

SlotMeterWidget::SlotMeterWidget(QWidget *p)
   : QWidget(p)
{
   for (int i = 0; i < 32; i++)
   {
      env_[i] = 0x3FF;
      state_[i] = 0;
   }
   setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SlotMeterWidget::setSlot(int slot, int env, int state)
{
   if (slot < 0 || slot >= 32)
      return;
   env_[slot] = env;
   state_[slot] = state;
}

// Same criterion as the core: a slot in release with a nearly full attenuation
// (>= 0x3BF) is finished. EnvelopeStates: ATTACK = 1, DECAY1, DECAY2, RELEASE.
bool SlotMeterWidget::isSounding(int env, int state)
{
   if (state < 1 || state > 4)
      return false;
   return !(state == 4 && env >= 0x3BF);
}

// The envelope state enum starts at 1 (ATTACK) while the colors are indexed
// from 0, so the raw state must be shifted. Anything outside 1-4 is inactive.
QColor SlotMeterWidget::colorForState(int state, const QPalette &pal)
{
   switch (state)
   {
      case 1: return QColor(229, 72, 77);    // attack
      case 2: return QColor(70, 167, 88);    // decay 1
      case 3: return QColor(62, 99, 221);    // decay 2
      case 4: return QColor(18, 165, 148);   // release
      default: return pal.color(QPalette::Mid);
   }
}

QSize SlotMeterWidget::sizeHint() const { return QSize(760, 240); }
QSize SlotMeterWidget::minimumSizeHint() const { return QSize(520, 170); }

void SlotMeterWidget::paintEvent(QPaintEvent *)
{
   QPainter painter(this);
   painter.setRenderHint(QPainter::Antialiasing);

   const QPalette pal = palette();
   const QFontMetrics fm(font());

   const int margin = 8;
   const int legendH = fm.height() + 10;
   const int labelH = fm.height() + 4;
   const QRect area(margin, margin, width() - 2 * margin, height() - 2 * margin - labelH - legendH);
   if (area.width() < 64 || area.height() < 16)
      return;

   const double slotW = (double)area.width() / 32.0;
   const double barW = qMax(3.0, slotW * 0.68);

   // Faint guide lines at 25 / 50 / 75 % of the envelope
   painter.setPen(QPen(mixColors(pal.color(QPalette::WindowText), pal.color(QPalette::Base), 0.12), 1));
   for (int q = 1; q < 4; q++)
   {
      const int y = area.top() + area.height() * q / 4;
      painter.drawLine(area.left(), y, area.right(), y);
   }

   for (int i = 0; i < 32; i++)
   {
      const double x = area.left() + i * slotW + (slotW - barW) / 2.0;
      const bool on = isSounding(env_[i], state_[i]);

      // Track behind the bar
      painter.setPen(Qt::NoPen);
      painter.setBrush(pal.color(QPalette::AlternateBase));
      painter.drawRoundedRect(QRectF(x, area.top(), barW, area.height()), 2, 2);

      double ratio = (1023.0 - env_[i]) / 1023.0;
      ratio = qBound(0.0, ratio, 1.0);
      const double h = on ? qMax(2.0, area.height() * ratio) : 2.0;

      painter.setBrush(on ? colorForState(state_[i], pal) : pal.color(QPalette::Mid));
      painter.drawRoundedRect(QRectF(x, area.bottom() - h, barW, h), 2, 2);

      // Slot number under each bar
      painter.setPen(on ? pal.color(QPalette::WindowText) : dimColor(this));
      painter.drawText(QRectF(area.left() + i * slotW, area.bottom() + 3, slotW, labelH),
                       Qt::AlignHCenter | Qt::AlignTop, QString::number(i));
   }

   // Legend
   struct { const char *text; int state; } legend[] = {
      { "Attack", 1 }, { "Decay 1", 2 }, { "Decay 2", 3 }, { "Release", 4 }, { "Inactive slot", 0 }
   };
   int lx = area.left();
   const int ly = height() - margin - legendH + 4;
   for (size_t k = 0; k < sizeof(legend) / sizeof(legend[0]); k++)
   {
      painter.setPen(Qt::NoPen);
      painter.setBrush(colorForState(legend[k].state, pal));
      painter.drawRoundedRect(QRect(lx, ly + 2, 10, 10), 2, 2);

      const QString text = tr_(legend[k].text);
      const int tw = fm.boundingRect(text).width();   // width() is deprecated since Qt 5.11
      painter.setPen(pal.color(QPalette::WindowText));
      painter.drawText(lx + 14, ly, tw + 4, fm.height(), Qt::AlignLeft | Qt::AlignVCenter, text);
      lx += 14 + tw + 22;
   }
}

// ============================================================
//  UIDebugSCSPChan
// ============================================================

UIDebugSCSPChan::UIDebugSCSPChan(QWidget* p)
   : QDialog(p), meter(NULL), timer(NULL), muteGrid(NULL), lMuteEmpty(NULL), lMuteCount(NULL), pbUnmuteAll(NULL)
{
   setWindowTitle(tr_("Debug SCSP Channels"));

   // Start recording the sounds. The list is emptied first: turning debug mode
   // on records the samples already playing (a music loop keyed on before the
   // window opened), and clearing after that, as was done here, lost them.
   scsp_debug_instrument_clear();
   scsp_debug_set_mode(1);

   QVBoxLayout *main = new QVBoxLayout(this);
   main->setContentsMargins(16, 14, 16, 12);
   main->setSpacing(12);

   // Header
   QHBoxLayout *header = new QHBoxLayout();
   QLabel *lTitle = new QLabel(tr_("Channel Viewer"), this);
   lTitle->setObjectName("lTitle");
   QLabel *lLive = new QLabel(tr_("Live"), this);
   lLive->setObjectName("lChip");
   header->addWidget(lTitle);
   header->addStretch(1);
   header->addWidget(lLive);
   main->addLayout(header);

   // Envelope meter
   QFrame *meterCard = new QFrame(this);
   meterCard->setObjectName("panel");
   QVBoxLayout *mc = new QVBoxLayout(meterCard);
   mc->setContentsMargins(16, 14, 16, 12);
   mc->setSpacing(8);
   QLabel *lEnv = new QLabel(tr_("Slot envelopes"), meterCard);
   lEnv->setObjectName("lSection");
   meter = new SlotMeterWidget(meterCard);
   mc->addWidget(lEnv);
   mc->addWidget(meter, 1);
   main->addWidget(meterCard, 1);

   // Mute list
   QFrame *muteCard = new QFrame(this);
   muteCard->setObjectName("panel");
   QVBoxLayout *mu = new QVBoxLayout(muteCard);
   mu->setContentsMargins(16, 14, 16, 14);
   mu->setSpacing(8);

   QHBoxLayout *muteHead = new QHBoxLayout();
   QLabel *lMute = new QLabel(tr_("Mute individual sounds"), muteCard);
   lMute->setObjectName("lSection");
   lMuteCount = new QLabel(muteCard);
   lMuteCount->setProperty("dim", true);
   pbUnmuteAll = new QPushButton(tr_("Unmute all"), muteCard);
   pbUnmuteAll->setObjectName("pbUnmuteAll");
   pbUnmuteAll->setCursor(Qt::PointingHandCursor);
   muteHead->addWidget(lMute);
   muteHead->addSpacing(8);
   muteHead->addWidget(lMuteCount);
   muteHead->addStretch(1);
   muteHead->addWidget(pbUnmuteAll);
   mu->addLayout(muteHead);

   QLabel *lHint = new QLabel(tr_("Each entry is a sample, identified by its start address, that was playing when this window opened or has been played since. Entries in bold are playing now (hover one to see its slots). Check one to silence every slot playing it."), muteCard);
   lHint->setWordWrap(true);
   lHint->setProperty("dim", true);
   mu->addWidget(lHint);

   lMuteEmpty = new QLabel(tr_("No sound has been played yet."), muteCard);
   lMuteEmpty->setProperty("dim", true);
   mu->addWidget(lMuteEmpty);

   // The list grows with every new sample a game plays: keep it in a scroll
   // area, otherwise it pushed the dialog taller than the screen
   QScrollArea *muteScroll = new QScrollArea(muteCard);
   muteScroll->setObjectName("muteScroll");
   muteScroll->setWidgetResizable(true);
   muteScroll->setFrameShape(QFrame::NoFrame);
   muteScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
   muteScroll->setMinimumHeight(90);
   QWidget *muteHost = new QWidget(muteScroll);
   muteHost->setObjectName("muteHost");
   muteGrid = new QGridLayout(muteHost);
   muteGrid->setContentsMargins(0, 0, 0, 0);
   muteGrid->setHorizontalSpacing(16);
   muteGrid->setVerticalSpacing(6);
   muteGrid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
   muteScroll->setWidget(muteHost);
   mu->addWidget(muteScroll, 1);
   main->addWidget(muteCard, 1);

   // Footer
   QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
   connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
   main->addWidget(buttons);

   applyStyle();
   resize(860, 680);

   connect(pbUnmuteAll, SIGNAL(clicked()), this, SLOT(unmuteAll()));
   pbUnmuteAll->setEnabled(false);

   timer = new QTimer(this);
   timer->setInterval(50);   // 20 fps
   connect(timer, SIGNAL(timeout()), SLOT(update_window()));
   timer->start();

   // retranslate widgets
   QtYabause::retranslateWidget(this);
}

UIDebugSCSPChan::~UIDebugSCSPChan()
{
   // Leaving debug mode also releases every mute: the core only applies them
   // while debug mode is on. Children (timer, widgets) are deleted by Qt.
   scsp_debug_set_mode(0);
}

void UIDebugSCSPChan::applyStyle()
{
   setStyleSheet(QString::fromLatin1("QLabel[dim=\"true\"] { color: %1; }").arg(dimColor(this).name()) + QString::fromLatin1(
      "QFrame#panel { background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }"
      "QLabel#lTitle { font-size: 18pt; font-weight: 600; }"
      "QLabel#lSection { font-weight: 600; font-size: 11pt; }"
      "QLabel#lChip { border: 1px solid palette(mid); border-radius: 10px; padding: 2px 10px; font-weight: 600; }"
      "QPushButton#pbUnmuteAll { border: 1px solid palette(mid); border-radius: 8px; padding: 5px 14px; background: transparent; }"
      "QPushButton#pbUnmuteAll:hover { border-color: palette(highlight); }"
      "QPushButton#pbUnmuteAll:disabled { color: palette(mid); }"
      "QScrollArea#muteScroll, QWidget#muteHost { background: transparent; }"));
}

void UIDebugSCSPChan::addMuteBox(u32 sa, int muted)
{
   QCheckBox *cb = new QCheckBox(QLatin1String("0x") + hexSa(sa), this);
   cb->setProperty("sa", (uint)sa);
   cb->setToolTip(tr_("Sample start address 0x%1").arg(hexSa(sa)));
   cb->setChecked(muted != 0);
   connect(cb, SIGNAL(toggled(bool)), this, SLOT(onMuteToggled(bool)));

   // Six entries per row
   const int idx = muteBoxes.count();
   muteGrid->addWidget(cb, idx / 6, idx % 6);
   muteBoxes.append(cb);
   playingMask.append(0);

   lMuteEmpty->setVisible(false);
   pbUnmuteAll->setEnabled(true);
}

void UIDebugSCSPChan::onMuteToggled(bool checked)
{
   QCheckBox *cb = qobject_cast<QCheckBox*>(sender());
   if (!cb)
      return;
   scsp_debug_instrument_set_mute(cb->property("sa").toUInt(), checked ? 1 : 0);
}

void UIDebugSCSPChan::unmuteAll()
{
   for (int i = 0; i < muteBoxes.count(); i++)
      muteBoxes[i]->setChecked(false);
}

void UIDebugSCSPChan::update_window()
{
   for (int i = 0; i < 32; i++)
   {
      int env = 0x3FF, state = 0;
      scsp_debug_get_envelope(i, &env, &state);
      meter->setSlot(i, env, state);
   }
   meter->update();

   // The core appends instruments as new samples get keyed on; add one entry per
   // new sample instead of rewriting every mute flag at each tick.
   const int count = scsp_debug_instrument_count();
   const bool added = muteBoxes.count() < count;
   while (muteBoxes.count() < count)
   {
      u32 sa = 0;
      int muted = 0;
      scsp_debug_instrument_get_data(muteBoxes.count(), &sa, &muted);
      addMuteBox(sa, muted);
   }

   if (added || lMuteCount->text().isEmpty())
   {
      const int capacity = scsp_debug_instrument_capacity();
      if (count >= capacity)
         lMuteCount->setText(tr_("%1 / %2 - list full, later samples are not listed").arg(count).arg(capacity));
      else
         lMuteCount->setText(tr_("%1 / %2").arg(count).arg(capacity));
   }

   updatePlayingSamples();
}

// Shows in bold the samples that some slot is playing now, with those slots in
// the tooltip, so that a sound heard in the game can be found in the list.
void UIDebugSCSPChan::updatePlayingSamples()
{
   u32 slotSa[32];
   bool sounding[32];
   for (int s = 0; s < 32; s++)
   {
      int env = 0x3FF, state = 0;
      scsp_debug_get_envelope(s, &env, &state);
      sounding[s] = SlotMeterWidget::isSounding(env, state);
      slotSa[s] = scsp_debug_get_slot_sa(s);
   }

   for (int i = 0; i < muteBoxes.count(); i++)
   {
      const u32 sa = muteBoxes[i]->property("sa").toUInt();
      u32 mask = 0;
      for (int s = 0; s < 32; s++)
         if (sounding[s] && slotSa[s] == sa)
            mask |= 1u << s;

      if (mask == playingMask[i])
         continue;   // only touch the widgets that changed
      playingMask[i] = mask;

      QFont f = muteBoxes[i]->font();
      f.setBold(mask != 0);
      muteBoxes[i]->setFont(f);

      QString tip = tr_("Sample start address 0x%1").arg(hexSa(sa));
      if (mask)
      {
         QStringList slotList;
         for (int s = 0; s < 32; s++)
            if (mask & (1u << s))
               slotList << QString::number(s);
         tip += QLatin1Char('\n') + tr_("Playing on slot(s) %1").arg(slotList.join(QLatin1String(", ")));
      }
      muteBoxes[i]->setToolTip(tip);
   }
}
