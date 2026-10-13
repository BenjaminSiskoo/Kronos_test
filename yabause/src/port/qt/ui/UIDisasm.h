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
#ifndef UIDISASM_H
#define UIDISASM_H

#include <QAbstractScrollArea>
#include <QSet>
#include "../QtYabause.h"

// Scrollable disassembly listing shared by every CPU debugger.
//
// The scroll bar value is the address of the first visible instruction. Each
// row has a left margin that shows the code breakpoints (red dot, click it to
// toggle) and the current PC (arrow). Lines that look like the SH2 disassembler
// output ("0x06004000: mov.l @r15+, r14 ; comment") are drawn in columns and
// colored by kind of instruction; any other text is drawn as is.
class UIDisasm : public QAbstractScrollArea
{
	Q_OBJECT
public:
	UIDisasm( QWidget* parent = 0 );

   void setSelectionColor(const QColor &color);
   void setDisassembleFunction(int (*func)(void *, u32, char *));
   void setEndAddress(u32 address);
   void setContext(void *context);
   void goToAddress(u32 address, bool vCenter=true);
   void setPC(u32 address);
   void setMinimumInstructionSize(int instructionSize);

   // When every instruction has the same size, going to an address that is not
   // on screen puts it about a third from the top, with some context above.
   // With variable-size instructions it is put on the first row, since
   // decoding cannot safely start in the middle of an unknown instruction.
   void setFixedInstructionSize(bool fixed);

   // Addresses drawn with a breakpoint marker
   void setBreakpoints(const QSet<u32> &addresses);

private:
   void adjustSettings();
   int  visibleRows() const;
   u32  topAddress() const;
   u32  addressAtY(int y) const;
   void drawRow(QPainter &painter, int y, const QString &text, bool isPC);

   int fontWidth, fontHeight, rowHeight;
   QColor selectionColor;
   void *context;

   int (*disassembleFunction)(void *context, u32 address, char *string);
   u32 address;
   bool vCenter;
   u32 endAddress;
   u32 pc;
   int instructionSize;
   bool fixedSize;
   QSet<u32> breakpoints;

protected:
   void mouseDoubleClickEvent( QMouseEvent * event );
   void mousePressEvent( QMouseEvent * event );
   void contextMenuEvent( QContextMenuEvent * event );
   void paintEvent(QPaintEvent *event);
   void changeEvent(QEvent *event);
   void resizeEvent(QResizeEvent *event);
protected slots:

signals:
      void toggleCodeBreakpoint(u32 addr);
};

#endif // UIDISASM_H
