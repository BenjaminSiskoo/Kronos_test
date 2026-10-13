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
#include "UIDebugSCSP.h"
#include "CommonDialogs.h"
#include "UIDebugSCSPDSP.h"
#include "UIDebugSCSPChan.h"
#include "../QtYabause.h"
#include "../YabauseThread.h"

#include <QDebug>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QHeaderView>
#include <QIODevice>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidgetItem>

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QColor mixColors(const QColor &a, const QColor &b, double t)
{
	return QColor((int)(a.red() * t + b.red() * (1.0 - t)),
	              (int)(a.green() * t + b.green() * (1.0 - t)),
	              (int)(a.blue() * t + b.blue() * (1.0 - t)));
}

// Secondary text color that follows the current theme, light or dark
QColor dimColor(const QWidget *w)
{
	return mixColors(w->palette().color(QPalette::WindowText), w->palette().color(QPalette::Window), 0.6);
}

// Same criterion as the core (see the RELEASE test next to NUM_DEBUG_INSTRUMENTS):
// a slot in release with a nearly full attenuation is finished and silent.
bool slotIsSounding(int slot)
{
	int env = 0x3FF, state = 0;
	scsp_debug_get_envelope(slot, &env, &state);
	if (state < 1 || state > 4)
		return false;
	return !(state == 4 && env >= 0x3BF);
}

// The text of ScspSlotDebugStats() is a flat list. Group it so that the
// 40-odd lines of a slot are easy to scan.
QString groupForSlotLabel(const QString &label)
{
	static const char *sample[]   = { "Sound Source", "Source bit", "Loop Mode", "Sample size",
	                                  "Start Address", "Loop Start Address", "Loop End Address", 0 };
	static const char *envelope[] = { "Attack Rate", "Decay 1 Rate", "Decay 2 Rate", "Decay Level",
	                                  "Release Rate", "EG Hold", "Loop Start Link", "Key rate scaling",
	                                  "Total Level", 0 };
	static const char *pitch[]    = { "Octave", "Frequency Number", 0 };
	static const char *mixer[]    = { "Input mix", "Input Select", "Direct data", "Effect data", 0 };
	static const char *output[]   = { "Stack Write", "Sound Direct", 0 };

	struct { const char *name; const char **keys; } groups[] = {
		{ "Sample", sample }, { "Envelope", envelope }, { "Pitch", pitch }, { "Mixer", mixer }, { "Output", output }
	};

	if (label.startsWith("LFO"))
		return tr_("LFO");
	if (label.startsWith("Modulation"))
		return tr_("Modulation");

	for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
		for (const char **k = groups[g].keys; *k; k++)
			if (label.startsWith(QString::fromLatin1(*k)))
				return tr_(groups[g].name);

	return tr_("Other");
}

QTreeWidgetItem *makeGroup(QTreeWidget *tree, const QString &name)
{
	QTreeWidgetItem *g = new QTreeWidgetItem(tree);
	g->setText(0, name);
	QFont f = g->font(0);
	f.setBold(true);
	g->setFont(0, f);
	g->setForeground(0, tree->palette().color(QPalette::Highlight));
	g->setFirstColumnSpanned(true);
	g->setFlags(Qt::ItemIsEnabled);
	return g;
}

QTreeWidgetItem *makeRow(QTreeWidgetItem *group, const QString &label, const QString &value)
{
	QTreeWidgetItem *it = new QTreeWidgetItem(group);
	it->setText(0, label);
	it->setText(1, value);
	it->setToolTip(1, value);   // the column can be too narrow for long values
	it->setToolTip(0, label);

	// Monospace for numbers and addresses so digits line up, normal font for words
	if (!value.isEmpty() && (value.at(0).isDigit() || value.at(0) == QLatin1Char('-')))
	{
		QFont mono(QString::fromLatin1("Courier New"));
		mono.setStyleHint(QFont::Monospace);
		it->setFont(1, mono);
	}
	return it;
}

} // namespace

UIDebugSCSP::UIDebugSCSP( YabauseThread *yt, QWidget* p )
	: QDialog( p ), mYabauseThread(yt), emulationReady(HighWram != NULL)
#ifdef HAVE_QT_MULTIMEDIA
	, audioBufferTimer(NULL), audioOutput(NULL), outputDevice(NULL)
	, playing(false), audioUsable(false), chunkFrames(0)
#endif
{
	for (int i = 0; i < 32; i++)
		slotButtons[i] = NULL;

	// setup dialog
	setupUi( this );

	tabInfo->setDocumentMode(true);
	tabInfo->tabBar()->setDrawBase(false);   // no stray line above the tabs
	twSlotInfo->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	twSlotInfo->header()->setStretchLastSection(true);
	twCommonInfo->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	twCommonInfo->header()->setStretchLastSection(true);
	twSlotInfo->setHeaderHidden(true);
	twCommonInfo->setHeaderHidden(true);

	setupSlotGrid();
	applyStyle();

	lChipEmu->setText(emulationReady ? tr_("Emulation paused") : tr_("No game running"));
	pbSaveAsWav->setEnabled(emulationReady);
	pbSaveSlotRegisters->setEnabled(emulationReady);
	pbPlaySlot->setEnabled(emulationReady);

#ifdef HAVE_QT_MULTIMEDIA
	audioBufferTimer = new QTimer(this);
	audioDeviceInfo = QAudioDeviceInfo::defaultOutputDevice();
	initAudio();
#else
	// Without Qt Multimedia there is nothing to play the slot with
	pbPlaySlot->setVisible(false);
	lPlayStatus->setText(tr_("Audio preview is not available in this build."));
#endif

	refreshCommon();
	refreshSlot(sbSlotNumber->value());

	// retranslate widgets
	QtYabause::retranslateWidget( this );
}

UIDebugSCSP::~UIDebugSCSP()
{
#ifdef HAVE_QT_MULTIMEDIA
	// Stop everything before the buffers go away
	if (audioBufferTimer)
		audioBufferTimer->stop();
	if (audioOutput)
		audioOutput->stop();
#endif
}

void UIDebugSCSP::setupSlotGrid()
{
	const int columns = 8;
	for (int i = 0; i < 32; i++)
	{
		QPushButton *b = new QPushButton(QString::number(i), wSlotGrid);
		b->setCheckable(true);
		b->setAutoExclusive(true);
		b->setProperty("slotbtn", true);
		b->setProperty("sounding", false);
		b->setFixedSize(34, 30);
		b->setCursor(Qt::PointingHandCursor);
		glSlots->addWidget(b, i / columns, i % columns);
		slotButtons[i] = b;
		connect(b, &QPushButton::clicked, this, [this, i]() { sbSlotNumber->setValue(i); });
	}
}

void UIDebugSCSP::applyStyle()
{
	// Only palette() colors are used, so the dialog follows light and dark themes
	setStyleSheet(QString::fromLatin1("QLabel[dim=\"true\"] { color: %1; }").arg(dimColor(this).name()) + QString::fromLatin1(
		"QFrame#panelLeft, QFrame#panelRight {"
		"  background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }"

		"QLabel#lChipSlot, QLabel#lChipEmu { border: 1px solid palette(mid); border-radius: 10px;"
		"  padding: 2px 10px; font-weight: 600; }"

		"QPushButton[slotbtn=\"true\"] { border: 1px solid palette(mid); border-radius: 6px;"
		"  background: transparent; padding: 0; }"
		"QPushButton[slotbtn=\"true\"]:hover { border-color: palette(highlight); }"
		"QPushButton[slotbtn=\"true\"][sounding=\"true\"] { border: 2px solid palette(highlight); font-weight: 700; }"
		"QPushButton[slotbtn=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text);"
		"  border-color: palette(highlight); }"

		"QPushButton#pbPlaySlot { background: palette(highlight); color: palette(highlighted-text);"
		"  border: none; border-radius: 8px; padding: 8px 18px; font-weight: 600; }"
		"QPushButton#pbPlaySlot:disabled { background: palette(midlight); color: palette(mid); }"

		"QPushButton#pbSaveAsWav, QPushButton#pbSaveSlotRegisters, QPushButton#pbOpenDSPDebugger,"
		"QPushButton#pbOpenChannelViewer { border: 1px solid palette(mid); border-radius: 8px;"
		"  padding: 6px 14px; background: transparent; }"
		"QPushButton#pbSaveAsWav:hover, QPushButton#pbSaveSlotRegisters:hover,"
		"QPushButton#pbOpenDSPDebugger:hover, QPushButton#pbOpenChannelViewer:hover { border-color: palette(highlight); }"
		"QPushButton:disabled { color: palette(mid); }"

		"QLineEdit { border: 1px solid palette(mid); border-radius: 6px; padding: 6px 8px; background: palette(base); }"
		"QLineEdit:focus { border-color: palette(highlight); }"

		"QTreeWidget { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
		"QTreeWidget::item { padding: 3px 4px; }"
		"QTabWidget::pane { border: none; }"
		"QTabBar::tab { padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }"
		"QTabBar::tab:selected { border-bottom: 2px solid palette(highlight); color: palette(highlight); }"));

	QFont f = lTitle->font();
	f.setPointSize(f.pointSize() + 6);
	f.setBold(true);
	lTitle->setFont(f);

	f = lSlotTitle->font();
	f.setBold(true);
	f.setPointSize(f.pointSize() + 1);
	lSlotTitle->setFont(f);
	lPreviewTitle->setFont(f);
	lRelatedTitle->setFont(f);

	lSlotLegend->setProperty("dim", true);
	lPlayStatus->setProperty("dim", true);
	lFootNote->setProperty("dim", true);
}

void UIDebugSCSP::updateSlotButtons()
{
	for (int i = 0; i < 32; i++)
	{
		const bool on = emulationReady && slotIsSounding(i);
		slotButtons[i]->setProperty("sounding", on);
		slotButtons[i]->setToolTip(on ? tr_("Slot %1 is sounding").arg(i) : tr_("Slot %1").arg(i));
		// Re-polish so the stylesheet picks the new property value up
		slotButtons[i]->style()->unpolish(slotButtons[i]);
		slotButtons[i]->style()->polish(slotButtons[i]);
	}
}

void UIDebugSCSP::refreshSlot(int slot)
{
	lChipSlot->setText(tr_("Slot %1").arg(slot));
	tabInfo->setTabText(0, tr_("Slot %1").arg(slot));
	if (slotButtons[slot])
		slotButtons[slot]->setChecked(true);

	updateSlotButtons();

	twSlotInfo->clear();
	if (!emulationReady)
	{
		QTreeWidgetItem *g = makeGroup(twSlotInfo, tr_("No game running"));
		makeRow(g, tr_("Start a game to inspect the sound slots."), QString());
		g->setExpanded(true);
		return;
	}

	char tempstr[4096];
	tempstr[0] = 0;
	ScspSlotDebugStats((u8)slot, tempstr);
	fillSlotInfo(QString::fromLatin1(tempstr));
}

void UIDebugSCSP::refreshCommon()
{
	twCommonInfo->clear();
	if (!emulationReady)
	{
		QTreeWidgetItem *g = makeGroup(twCommonInfo, tr_("No game running"));
		makeRow(g, tr_("Start a game to inspect the common control registers."), QString());
		g->setExpanded(true);
		return;
	}

	char tempstr[4096];
	tempstr[0] = 0;
	ScspCommonControlRegisterDebugStats(tempstr);
	fillCommonInfo(QString::fromLatin1(tempstr));
}

void UIDebugSCSP::fillSlotInfo(const QString &text)
{
	QStringList order;
	order << tr_("Sample") << tr_("Envelope") << tr_("Pitch") << tr_("LFO") << tr_("Modulation")
	      << tr_("Mixer") << tr_("Output") << tr_("Other");

	QHash<QString, QTreeWidgetItem*> groups;
	for (int i = 0; i < order.count(); i++)
		groups.insert(order[i], makeGroup(twSlotInfo, order[i]));

	// "Enabled" / "Inhibited" lines have no " = ": split them anyway
	QRegularExpression flag(QString::fromLatin1("^(.*) (Enabled|Inhibited)$"));

	const QStringList lines = text.split(QLatin1Char('\n'));
	for (int i = 0; i < lines.count(); i++)
	{
		QString line = lines[i].trimmed();
		if (line.isEmpty())
			continue;

		QString label, value;
		const int eq = line.indexOf(QString::fromLatin1(" = "));
		QRegularExpressionMatch m = flag.match(line);
		if (eq > 0)
		{
			label = line.left(eq).trimmed();
			value = line.mid(eq + 3).trimmed();
		}
		else if (m.hasMatch())
		{
			label = m.captured(1);
			value = m.captured(2);
		}
		else if (line.endsWith(QString::fromLatin1(" samples")))
		{
			label = tr_("Sample size");
			value = line.left(line.length() - 8);
		}
		else
		{
			label = line;
		}

		makeRow(groups.value(groupForSlotLabel(label)), label, value);
	}

	// Drop the groups that stayed empty (e.g. no LFO line when the LFO is off)
	for (int i = order.count() - 1; i >= 0; i--)
	{
		QTreeWidgetItem *g = groups.value(order[i]);
		if (g->childCount() == 0)
			delete g;
		else
			g->setExpanded(true);
	}

	applyFilter();
}

void UIDebugSCSP::fillCommonInfo(const QString &text)
{
	// Sections are a title line followed by a line of dashes
	QTreeWidgetItem *group = makeGroup(twCommonInfo, tr_("General"));
	const QStringList lines = text.split(QLatin1Char('\n'));
	QRegularExpression dashes(QString::fromLatin1("^-+$"));

	for (int i = 0; i < lines.count(); i++)
	{
		QString line = lines[i].trimmed();
		if (line.isEmpty())
			continue;

		if (i + 1 < lines.count() && dashes.match(lines[i + 1].trimmed()).hasMatch())
		{
			if (group->childCount() == 0)
				delete group;
			group = makeGroup(twCommonInfo, line);
			i++;   // skip the dashes
			continue;
		}

		const int sep = line.indexOf(QString::fromLatin1(": "));
		if (sep > 0)
			makeRow(group, line.left(sep), line.mid(sep + 2).trimmed());
		else
			makeRow(group, line, QString());
	}
	if (group->childCount() == 0)
		delete group;

	for (int i = 0; i < twCommonInfo->topLevelItemCount(); i++)
		twCommonInfo->topLevelItem(i)->setExpanded(true);
}

void UIDebugSCSP::applyFilter()
{
	const QString f = leFilter->text().trimmed();
	for (int g = 0; g < twSlotInfo->topLevelItemCount(); g++)
	{
		QTreeWidgetItem *group = twSlotInfo->topLevelItem(g);
		bool any = false;
		for (int c = 0; c < group->childCount(); c++)
		{
			QTreeWidgetItem *row = group->child(c);
			const bool match = f.isEmpty()
				|| row->text(0).contains(f, Qt::CaseInsensitive)
				|| row->text(1).contains(f, Qt::CaseInsensitive);
			row->setHidden(!match);
			any = any || match;
		}
		group->setHidden(!any);
		if (any && !f.isEmpty())
			group->setExpanded(true);
	}
}

void UIDebugSCSP::on_leFilter_textChanged(const QString &)
{
	applyFilter();
}

#ifdef HAVE_QT_MULTIMEDIA
void UIDebugSCSP::initAudio()
{
	connect(audioBufferTimer, SIGNAL(timeout()), SLOT(audioBufferRefill()));

	// The core always renders 44.1 kHz stereo 16-bit little endian samples
	audioFormat.setSampleRate(44100);
	audioFormat.setChannelCount(2);
	audioFormat.setSampleSize(16);
	audioFormat.setCodec("audio/pcm");
	audioFormat.setByteOrder(QAudioFormat::LittleEndian);
	audioFormat.setSampleType(QAudioFormat::SignedInt);

	QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
	if (info.isNull())
	{
		pbPlaySlot->setEnabled(false);
		lPlayStatus->setText(tr_("No audio output device found."));
		return;
	}

	// The slot is rendered at 44.1 kHz and cannot be resampled here: if the
	// device only offers another format, previewing would play at the wrong
	// pitch, so say so instead of silently doing it.
	if (!info.isFormatSupported(audioFormat))
	{
		qWarning() << "UIDebugSCSP: 44.1 kHz stereo 16-bit output is not supported by the audio device";
		pbPlaySlot->setEnabled(false);
		lPlayStatus->setText(tr_("The audio device does not support 44.1 kHz stereo 16-bit output, so the slot cannot be previewed."));
		return;
	}

	audioOutput = new QAudioOutput(audioDeviceInfo, audioFormat, this);
	connect(audioOutput, SIGNAL(stateChanged(QAudio::State)), SLOT(stateChanged(QAudio::State)));
	audioUsable = true;
	lPlayStatus->setText(tr_("Plays the slot from its key on, with the same sound generator as the game (without the DSP effects and the other slots)."));
}

void UIDebugSCSP::audioBufferRefill()
{
	if (!playing || !audioOutput || !outputDevice || chunkFrames == 0)
		return;

	// One chunk = chunkFrames stereo frames of 16-bit samples
	const int chunkBytes = (int)(chunkFrames * 4);
	int chunks = audioOutput->bytesFree() / chunkBytes;
	while (chunks > 0)
	{
		if (ScspSlotDebugAudio(workBuf.data(), slotBuf.data(), chunkFrames) == 0)
		{
			// The envelope finished (or the slot has nothing to play)
			stopPlayback(tr_("Finished."));
			return;
		}
		outputDevice->write((const char *)slotBuf.constData(), chunkBytes);
		--chunks;
	}
}

void UIDebugSCSP::stateChanged(QAudio::State state)
{
	// Everything queued has been played once playback was ended: release the device
	if (state == QAudio::IdleState && !playing && audioOutput)
		audioOutput->stop();
}

void UIDebugSCSP::stopPlayback(const QString &status)
{
	audioBufferTimer->stop();
	playing = false;
	pbPlaySlot->setText(tr_("Play Slot"));
	lPlayStatus->setText(status);
	// The device is stopped from stateChanged() once its buffer has drained,
	// so the end of the sound is not cut off
}

void UIDebugSCSP::startPlayingSlot(int slot)
{
	if (!audioUsable)
		return;

	audioBufferTimer->stop();
	audioOutput->stop();

	ScspSlotResetDebug((u8)slot);

	outputDevice = audioOutput->start();
	if (!outputDevice)
	{
		stopPlayback(tr_("Could not open the audio device."));
		return;
	}

	// Frames per chunk: the device period is in bytes (4 bytes per stereo frame)
	chunkFrames = (u32)(audioOutput->periodSize() / 4);
	if (chunkFrames == 0)
		chunkFrames = 512;
	workBuf.resize((int)(chunkFrames * 2));
	slotBuf.resize((int)(chunkFrames * 2));

	playing = true;
	pbPlaySlot->setText(tr_("Stop Slot"));
	lPlayStatus->setText(tr_("Playing slot %1...").arg(slot));
	audioBufferTimer->start(20);

	// A slot whose sound source is zero data (SSCTL = 2/3) makes the core return
	// 0 right away. (The preview restarts the envelope, so a slot that is silent
	// in the game right now still plays.)
	audioBufferRefill();
	if (!playing)
		lPlayStatus->setText(tr_("Nothing to play: the sound source of this slot is zero data (SSCTL = 2 or 3)."));
}

void UIDebugSCSP::on_pbPlaySlot_clicked ()
{
	if (!playing)
		startPlayingSlot(sbSlotNumber->value());
	else
	{
		audioOutput->stop();
		stopPlayback(tr_("Stopped."));
	}
}
#endif

void UIDebugSCSP::on_sbSlotNumber_valueChanged ( int i )
{
	refreshSlot(i);

#ifdef HAVE_QT_MULTIMEDIA
	// Changing the slot while playing switches the preview to the new slot,
	// like changing track in a player
	if (playing)
		startPlayingSlot(i);
#endif
}

void UIDebugSCSP::on_pbOpenDSPDebugger_clicked()
{
   UIDebugSCSPDSP( mYabauseThread, this ).exec();
}

void UIDebugSCSP::on_pbOpenChannelViewer_clicked()
{
#ifdef HAVE_QT_MULTIMEDIA
	// The preview and the channel viewer must not run together: stop it
	if (playing)
	{
		audioOutput->stop();
		stopPlayback(tr_("Stopped."));
	}
#endif

	// This dialog is opened with the emulation paused (YabauseLocker in
	// UIYabause), so the channel viewer used to show frozen envelopes, and no
	// new sound could ever be listed or muted. Let the game run while the viewer
	// is open, and pause it again when it is closed.
	const bool resume = emulationReady && mYabauseThread
		&& mYabauseThread->emulationRunning() && mYabauseThread->emulationPaused();
	if (resume)
		mYabauseThread->pauseEmulation(false, false);

	UIDebugSCSPChan( this ).exec();

	if (resume)
		mYabauseThread->pauseEmulation(true, false);

	// The slots have moved on while the game was running
	refreshCommon();
	refreshSlot(sbSlotNumber->value());
}

void UIDebugSCSP::on_pbSaveAsWav_clicked ()
{
	// request a file to save to to user
	const QString suggested = QString::asprintf("channel%02d.wav", sbSlotNumber->value());
	const QString s = CommonDialogs::getSaveFileName(suggested, QtYabause::translate( "Choose a location for your wav file" ), QtYabause::translate( "WAV Files (*.wav)" ) );

	if ( s.isEmpty() )
		return;

#ifdef HAVE_QT_MULTIMEDIA
	// The WAV export renders through the same preview state as Play Slot
	if (playing)
	{
		audioOutput->stop();
		stopPlayback(tr_("Stopped."));
	}
#endif

	// A loop end address of 0 is not "no sample" (it is a 64K sample buffer):
	// the core only refuses a slot whose sound source is zero data
	const int r = ScspSlotDebugAudioSaveWav(sbSlotNumber->value(), QFile::encodeName(s).constData());
	if (r == -2)
		CommonDialogs::information( QtYabause::translate( "The sound source of this slot is zero data (SSCTL = 2 or 3), so there is nothing to save." ) );
	else if (r != 0)
		CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}

void UIDebugSCSP::on_pbSaveSlotRegisters_clicked ()
{
	const QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your binary file" ), QtYabause::translate( "Binary Files (*.bin)" ) );
	if ( !s.isEmpty() )
		if (ScspSlotDebugSaveRegisters(sbSlotNumber->value(), QFile::encodeName(s).constData()) != 0)
			CommonDialogs::error( QtYabause::translate( "An error occured while writing file." ) );
}
