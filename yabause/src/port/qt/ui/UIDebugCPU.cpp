/*	Copyright 2012-2013 Theo Berkau <cwx@cyberwarriorx.com>

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

#include "UIDebugCPU.h"
#include "UIHexInput.h"
#include "UIMemoryEditor.h"
#include "UIMemoryTransfer.h"
#include "../CommonDialogs.h"
#include <QFont>
#include <QFontDatabase>
#include <QKeySequence>
#include <QPalette>
#include <QSet>
#include <QTabBar>

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QColor mixColors(const QColor &a, const QColor &b, double t)
{
	return QColor((int)(a.red() * t + b.red() * (1.0 - t)),
	              (int)(a.green() * t + b.green() * (1.0 - t)),
	              (int)(a.blue() * t + b.blue() * (1.0 - t)));
}

} // namespace

UIDebugCPU::UIDebugCPU( PROCTYPE proc, YabauseThread *mYabauseThread, QWidget* p )
	: QDialog( p )
{
	// set up dialog
	setupUi( this );
	if ( p && !p->isFullScreen() )
		setWindowFlags( Qt::Sheet | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );

	// Disable unimplemented functions
	gbBackTrace->setVisible( false );
	pbStepOver->setVisible( false );
	pbStepOut->setVisible( false );
	pbReserved1->setVisible( false );
	pbReserved2->setVisible( false );
	pbReserved3->setVisible( false );
	pbReserved4->setVisible( false );
	pbReserved5->setVisible( false );
	pbLoadCode->setVisible( false );

	// The button was shown by every debugger but no slot was ever connected to
	// it, so clicking it did nothing
	pbTraceLogging->setVisible( false );
	lProcBadge->setVisible( false );
	lPcChip->setVisible( false );

	pbAddCodeBreakpoint->setEnabled( false );
	pbDelCodeBreakpoint->setEnabled( false );
	pbAddMemoryBreakpoint->setEnabled( false );
	pbDelMemoryBreakpoint->setEnabled( false );

	// Clear code/memory breakpoint text fields
	leCodeBreakpoint->setValidator(new HexValidator(0x00000000, 0xFFFFFFFF, leCodeBreakpoint));
	leMemoryBreakpoint->setValidator(new HexValidator(0x00000000, 0xFFFFFFFF, leMemoryBreakpoint));
	leCodeBreakpoint->setText("");
	leMemoryBreakpoint->setText("");

	connect( lwDisassembledCode, SIGNAL( toggleCodeBreakpoint(u32) ), this, SLOT( toggleCodeBreakpoint(u32) ) );

	// Same keys as most debuggers (a hidden button never triggers its shortcut)
	pbStepInto->setShortcut( QKeySequence( Qt::Key_F11 ) );
	pbStepOver->setShortcut( QKeySequence( Qt::Key_F10 ) );
	pbStepOut->setShortcut( QKeySequence( Qt::SHIFT + Qt::Key_F11 ) );
	pbLoadCode->setShortcut( QKeySequence( Qt::CTRL + Qt::Key_G ) );

	// Fixed pitch font for everything that shows registers and addresses
	const QFont mono = QFontDatabase::systemFont( QFontDatabase::FixedFont );
	lwRegisters->setFont( mono );
	lwRegisters->setWordWrap( true );   // long lines (the SR decode) wrap instead of needing a scroll bar
	lwRegisters->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
	lwBackTrace->setFont( mono );
	lwCodeBreakpoints->setFont( mono );
	lwMemoryBreakpoints->setFont( mono );
	twTrackInfLoop->setFont( mono );
	codeBrowser->setFont( mono );
	leCodeBreakpoint->setFont( mono );
	leMemoryBreakpoint->setFont( mono );
	lPcChip->setFont( mono );

	lStatus->setProperty( "dim", true );
	codeTab->tabBar()->setDrawBase( false );   // no stray line above the tabs
	tabOtherDebug->tabBar()->setDrawBase( false );
	applyStyle();

	this->context = proc;
	this->mYabauseThread = mYabauseThread;

	// retranslate widgets
	QtYabause::retranslateWidget( this );
}

void UIDebugCPU::applyStyle()
{
	// Only palette() colors are used, so the dialog follows light and dark themes
	const QColor dim = mixColors( palette().color( QPalette::WindowText ), palette().color( QPalette::Window ), 0.6 );
	setStyleSheet( QString::fromLatin1( "QLabel[dim=\"true\"] { color: %1; }" ).arg( dim.name() ) + QString::fromLatin1(
		"QGroupBox { background: palette(base); border: 1px solid palette(mid); border-radius: 10px;"
		"  margin-top: 22px; padding-top: 6px; }"
		"QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 12px; padding: 0 4px; font-weight: 600; }"
		"QGroupBox QGroupBox { background: transparent; }"

		"QLabel#lPcChip { border: 1px solid palette(mid); border-radius: 11px; padding: 3px 14px; font-weight: 600; }"

		"QListWidget, QTextBrowser { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
		"QListWidget::item { padding: 2px 4px; }"
		"QTableWidget { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
		"QHeaderView::section { background: transparent; border: none; border-bottom: 1px solid palette(mid);"
		"  padding: 4px 8px; font-weight: 600; }"

		"QTabWidget::pane { border: none; }"
		"QTabBar::tab { padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }"
		"QTabBar::tab:selected { border-bottom: 2px solid palette(highlight); color: palette(highlight); }"

		"QLineEdit { border: 1px solid palette(mid); border-radius: 6px; padding: 5px 8px; background: palette(base); }"
		"QLineEdit:focus { border-color: palette(highlight); }"

		"QPushButton { border: 1px solid palette(mid); border-radius: 8px; padding: 5px 14px; background: transparent; }"
		"QPushButton:hover { border-color: palette(highlight); }"
		"QPushButton:disabled { color: palette(mid); }"
		"QPushButton#pbStepInto, QPushButton#pbStepOver, QPushButton#pbStepOut {"
		"  background: palette(highlight); color: palette(highlighted-text); border: none; padding: 6px 20px; font-weight: 600; }"
		"QPushButton#pbStepInto:disabled, QPushButton#pbStepOver:disabled, QPushButton#pbStepOut:disabled {"
		"  background: palette(midlight); color: palette(mid); }"
		"QPushButton#pbAddCodeBreakpoint, QPushButton#pbDelCodeBreakpoint,"
		"QPushButton#pbAddMemoryBreakpoint, QPushButton#pbDelMemoryBreakpoint { padding: 4px 12px; border-radius: 6px; }" ) );
}

void UIDebugCPU::setProcBadge(const QString &text, const QColor &color)
{
	lProcBadge->setText( text );
	lProcBadge->setVisible( !text.isEmpty() );
	lProcBadge->setStyleSheet( QString::fromLatin1( "background: %1; color: white; border-radius: 11px;"
		" padding: 3px 14px; font-weight: 700;" ).arg( color.name() ) );
}

void UIDebugCPU::setPcChip(u32 pc)
{
	lPcChip->setText( QString::fromLatin1( "PC  " ) + hex8( pc ) );
	lPcChip->setVisible( true );
}

void UIDebugCPU::setStatus(const QString &text)
{
	lStatus->setText( text );
}

bool UIDebugCPU::parseHex(const QString &text, u32 *value)
{
	// toInt() cannot read anything above 0x7FFFFFFF: addresses such as the
	// cache-through area (0xA0000000...) came out as 0 and the breakpoint was
	// silently set on address 0 while the list showed the typed address.
	QString t = text.trimmed();
	if ( t.startsWith( QLatin1String( "0x" ), Qt::CaseInsensitive ) )
		t = t.mid( 2 );
	bool ok = false;
	const u32 v = t.toUInt( &ok, 16 );
	if ( ok && value )
		*value = v;
	return ok;
}

QString UIDebugCPU::hex8(u32 value)
{
	return QString( "%1" ).arg( value, 8, 16, QChar( '0' ) ).toUpper();
}

// The list can hold the same address written differently (typed by hand, or
// added by a derived class), so compare the numbers and not the text
QListWidgetItem *UIDebugCPU::findBreakpointItem(QListWidget *list, u32 addr) const
{
	for ( int i = 0; i < list->count(); i++ )
	{
		u32 v;
		if ( parseHex( list->item( i )->text(), &v ) && v == addr )
			return list->item( i );
	}
	return NULL;
}

void UIDebugCPU::updateBreakpointMarkers()
{
	QSet<u32> addresses;
	for ( int i = 0; i < lwCodeBreakpoints->count(); i++ )
	{
		u32 v;
		if ( parseHex( lwCodeBreakpoints->item( i )->text(), &v ) )
			addresses.insert( v );
	}
	lwDisassembledCode->setBreakpoints( addresses );
}

void UIDebugCPU::on_lwCodeBreakpoints_itemDoubleClicked ( QListWidgetItem * item )
{
	u32 addr;
	if ( item && parseHex( item->text(), &addr ) )
		updateCodeList( addr );
}

int UIDebugCPU::exec() {
	updateBreakpointMarkers();
	ScspMuteAudio(SCSP_MUTE_SYSTEM);
	return QDialog::exec();
}

void UIDebugCPU::done(int r){
	ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
	QDialog::done(r);
}

void UIDebugCPU::on_lwRegisters_itemDoubleClicked ( QListWidgetItem * item )
{
	int size;

	u32 value = getRegister(lwRegisters->row(item), &size);

	UIHexInput hex(value, size, this);
	if (hex.exec() == QDialog::Accepted)
	{
		value = hex.getValue();
		setRegister(lwRegisters->row(item), value);
	}

	updateRegList();
}

void UIDebugCPU::on_lwBackTrace_itemDoubleClicked ( QListWidgetItem * item )
{
	updateCodeList(item->text().toUInt(NULL, 16));
	updateCodePage(item->text().toUInt(NULL, 16));
}

void UIDebugCPU::on_twTrackInfLoop_itemDoubleClicked ( QTableWidgetItem * item )
{
	if (item->column() == 0)
		updateCodeList(item->text().toUInt(NULL, 16));
}

void UIDebugCPU::on_leCodeBreakpoint_textChanged( const QString & text )
{
   pbAddCodeBreakpoint->setEnabled(text.length() > 0);
}

bool UIDebugCPU::isReadWriteButtonAndTextOK()
{
	if (leMemoryBreakpoint->text().length() > 0)
	{
		if (cbRead->isChecked() &&
		(cbReadByte->isChecked() || cbReadWord->isChecked() || cbReadLong->isChecked()))
			return true;
		else if (cbWrite->isChecked() &&
			(cbWriteByte->isChecked() || cbWriteWord->isChecked() || cbWriteLong->isChecked()))
			return true;
	}
	return false;
}

void UIDebugCPU::on_leMemoryBreakpoint_textChanged( const QString & text )
{
	// Only enable Memory breakpoint add button if address is valid and read/write and size is checked
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_lwCodeBreakpoints_itemSelectionChanged ()
{
	pbDelCodeBreakpoint->setEnabled(!lwCodeBreakpoints->selectedItems().isEmpty());
}

void UIDebugCPU::on_pbAddCodeBreakpoint_clicked()
{
	u32 addr;
	if (!parseHex(leCodeBreakpoint->text(), &addr))
	{
		setStatus(tr_("Invalid address."));
		return;
	}

	const QString text = hex8(addr);
	if (findBreakpointItem(lwCodeBreakpoints, addr))
	{
		setStatus(tr_("A breakpoint already exists at %1.").arg(text));
		return;
	}

	// Only add breakpoint to list if all goes well with emulator
	if (addCodeBreakpoint(addr))
	{
		lwCodeBreakpoints->addItem(text);
		leCodeBreakpoint->clear();
		updateBreakpointMarkers();
		setStatus(tr_("Breakpoint added at %1.").arg(text));
	}
	else
		setStatus(tr_("Could not add a breakpoint at %1: the list is full or the address is not supported.").arg(text));
}

void UIDebugCPU::on_pbDelCodeBreakpoint_clicked()
{
	QList<QListWidgetItem *> list=lwCodeBreakpoints->selectedItems ();
	for (int i = 0; i < list.count(); i++)
	{
		QListWidgetItem *item=list.value(i);
		u32 addr;
		if (parseHex(item->text(), &addr))
			delCodeBreakpoint(addr);
	}
	qDeleteAll(lwCodeBreakpoints->selectedItems ());
	pbDelCodeBreakpoint->setEnabled(!lwCodeBreakpoints->selectedItems().isEmpty());
	updateBreakpointMarkers();
}

void UIDebugCPU::on_lwMemoryBreakpoints_itemSelectionChanged ()
{
	pbDelMemoryBreakpoint->setEnabled(!lwMemoryBreakpoints->selectedItems().isEmpty());

	// Fix: the Read/Write/Byte/Word/Long checkboxes used to just keep
	// whatever state they last had (or the .ui file's default on a fresh
	// window), completely disconnected from what's actually configured
	// for a breakpoint already in the list. That made it easy to think a
	// breakpoint was set one way (e.g. Write+Long) when it was really
	// something else, especially after reopening the debug window.
	// Reflect the real, currently-selected breakpoint's flags instead.
	QList<QListWidgetItem *> list = lwMemoryBreakpoints->selectedItems();
	if (list.count() != 1)
		return;

	bool ok = false;
	u32 addr = list.value(0)->text().toUInt(&ok, 16);
	if (!ok)
		return;

	u32 flags = getMemoryBreakpointFlags(addr);

	cbReadByte->setChecked((flags & BREAK_BYTEREAD) != 0);
	cbReadWord->setChecked((flags & BREAK_WORDREAD) != 0);
	cbReadLong->setChecked((flags & BREAK_LONGREAD) != 0);
	cbRead->setChecked((flags & (BREAK_BYTEREAD | BREAK_WORDREAD | BREAK_LONGREAD)) != 0);

	cbWriteByte->setChecked((flags & BREAK_BYTEWRITE) != 0);
	cbWriteWord->setChecked((flags & BREAK_WORDWRITE) != 0);
	cbWriteLong->setChecked((flags & BREAK_LONGWRITE) != 0);
	cbWrite->setChecked((flags & (BREAK_BYTEWRITE | BREAK_WORDWRITE | BREAK_LONGWRITE)) != 0);
}

u32 UIDebugCPU::getMemoryBreakpointFlags(u32 addr)
{
	return 0;
}

void UIDebugCPU::on_pbAddMemoryBreakpoint_clicked()
{
	u32 flags=0;

	// Get RW flags
	if (cbRead->checkState() == Qt::Checked)
	{
		if (cbReadByte->checkState() == Qt::Checked)
			flags |= BREAK_BYTEREAD;
		if (cbReadWord->checkState() == Qt::Checked)
			flags |= BREAK_WORDREAD;
		if (cbReadLong->checkState() == Qt::Checked)
			flags |= BREAK_LONGREAD;
	}

	if (cbWrite->checkState() == Qt::Checked)
	{
		if (cbWriteByte->checkState() == Qt::Checked)
			flags |= BREAK_BYTEWRITE;
		if (cbWriteWord->checkState() == Qt::Checked)
			flags |= BREAK_WORDWRITE;
		if (cbWriteLong->checkState() == Qt::Checked)
			flags |= BREAK_LONGWRITE;
	}

	u32 addr;
	if (!parseHex(leMemoryBreakpoint->text(), &addr))
	{
		setStatus(tr_("Invalid address."));
		return;
	}

	// Only add breakpoint to list if all goes well with emulator
	const QString text = hex8(addr);
	if (addMemoryBreakpoint(addr, flags))
	{
		if (!findBreakpointItem(lwMemoryBreakpoints, addr))
			lwMemoryBreakpoints->addItem(text);
		leMemoryBreakpoint->clear();
		setStatus(tr_("Memory breakpoint set at %1.").arg(text));
	}
	else
		setStatus(tr_("Could not add a memory breakpoint at %1: the list is full or the address is not supported.").arg(text));
}

void UIDebugCPU::on_pbDelMemoryBreakpoint_clicked()
{
	QList<QListWidgetItem *> list=lwMemoryBreakpoints->selectedItems ();
	for (int i = 0; i < list.count(); i++)
	{
		QListWidgetItem *item=list.value(i);
		u32 addr;
		if (parseHex(item->text(), &addr))
			delMemoryBreakpoint(addr);
	}
	qDeleteAll(lwMemoryBreakpoints->selectedItems ());
	pbDelMemoryBreakpoint->setEnabled(!lwMemoryBreakpoints->selectedItems().isEmpty());
}

void UIDebugCPU::on_cbRead_toggled(bool enable)
{
	// Enable Byte/Word/Long if Read is checked
	cbReadByte->setEnabled(enable);
	cbReadWord->setEnabled(enable);
	cbReadLong->setEnabled(enable);

	// Enable Add button if address is valid and read/write and size is checked
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbWrite_toggled(bool enable)
{
	// Enable Byte/Word/Long if Write is checked
	cbWriteByte->setEnabled(enable);
	cbWriteWord->setEnabled(enable);
	cbWriteLong->setEnabled(enable);

	// Enable Add button if address is valid and read/write and size is checked
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbReadByte_toggled(bool enable)
{
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbReadWord_toggled(bool enable)
{
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbReadLong_toggled(bool enable)
{
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbWriteByte_toggled(bool enable)
{
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbWriteWord_toggled(bool enable)
{
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_cbWriteLong_toggled(bool enable)
{
	pbAddMemoryBreakpoint->setEnabled(isReadWriteButtonAndTextOK());
}

void UIDebugCPU::on_pbStepInto_clicked()
{
	stepInto();
}

void UIDebugCPU::on_pbStepOver_clicked()
{
	stepOver();
}

void UIDebugCPU::on_pbStepOut_clicked()
{
	stepOut();
}

void UIDebugCPU::on_pbMemoryTransfer_clicked()
{
	UIMemoryTransfer( NULL, this ).exec();
}

void UIDebugCPU::on_pbMemoryEditor_clicked()
{
	UIMemoryEditor( context, mYabauseThread, this ).exec();
}

void UIDebugCPU::on_pbReserved1_clicked()
{
	reserved1();
}

void UIDebugCPU::on_pbReserved2_clicked()
{
	reserved2();
}

void UIDebugCPU::on_pbReserved3_clicked()
{
	reserved3();
}

void UIDebugCPU::on_pbReserved4_clicked()
{
	reserved4();
}

void UIDebugCPU::on_pbReserved5_clicked()
{
	reserved5();
}

void UIDebugCPU::updateRegList()
{
}

void UIDebugCPU::updateCodeList(u32 addr)
{
}

void UIDebugCPU::updateCodePage(u32 addr)
{
}

u32 UIDebugCPU::getRegister(int index, int *size)
{
	*size = 4;
	return 0;
}

void UIDebugCPU::setRegister(int index, u32 value)
{
}

bool UIDebugCPU::addCodeBreakpoint(u32 addr)
{
	return true;
}

void UIDebugCPU::toggleCodeBreakpoint(u32 addr)
{
	const QString text = hex8(addr);

	// Compared by value: a breakpoint typed by hand ("6004000") used to be
	// missed here, so toggling it added a duplicate instead of removing it
	QListWidgetItem *item = findBreakpointItem(lwCodeBreakpoints, addr);

	if (item)
	{
		// Remove breakpoint
		delete item;
		delCodeBreakpoint(addr);
		setStatus(tr_("Breakpoint removed at %1.").arg(text));
	}
	else
	{
		// Add breakpoint
		if (addCodeBreakpoint(addr))
		{
			lwCodeBreakpoints->addItem(text);
			setStatus(tr_("Breakpoint added at %1.").arg(text));
		}
		else
			setStatus(tr_("Could not add a breakpoint at %1: the list is full or the address is not supported.").arg(text));
	}
	pbDelCodeBreakpoint->setEnabled(!lwCodeBreakpoints->selectedItems().isEmpty());
	updateBreakpointMarkers();
}

bool UIDebugCPU::delCodeBreakpoint(u32 addr)
{
	return true;
}

bool UIDebugCPU::addMemoryBreakpoint(u32 addr, u32 flags)
{
	return true;
}

bool UIDebugCPU::delMemoryBreakpoint(u32 addr)
{
	return true;
}

void UIDebugCPU::stepInto()
{
}

void UIDebugCPU::stepOver()
{
}

void UIDebugCPU::stepOut()
{
}

void UIDebugCPU::reserved1()
{
}

void UIDebugCPU::reserved2()
{
}

void UIDebugCPU::reserved3()
{
}

void UIDebugCPU::reserved4()
{
}

void UIDebugCPU::reserved5()
{
}
