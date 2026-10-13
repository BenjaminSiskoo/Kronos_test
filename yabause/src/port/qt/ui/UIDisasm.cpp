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
#include "UIDisasm.h"

#include <cstring>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPolygon>
#include <QRegularExpression>
#include <QScrollBar>

namespace {

const int GutterWidth = 30;

int DisasmInstructionNull(void *context, u32 address, char *string)
{
   Q_UNUSED(context);
   Q_UNUSED(address);
   strcpy(string, " ");
   return 0;
}

QColor mixColors(const QColor &a, const QColor &b, double t)
{
   return QColor((int)(a.red() * t + b.red() * (1.0 - t)),
                 (int)(a.green() * t + b.green() * (1.0 - t)),
                 (int)(a.blue() * t + b.blue() * (1.0 - t)));
}

enum InsnKind { KindNormal, KindFlow, KindReturn, KindNop, KindBad };

// Classes the mnemonic of an SH2 instruction so the control flow stands out
InsnKind classify(const QString &mnemonic)
{
   const QString m = mnemonic.toLower();
   if (m == QLatin1String("rts") || m == QLatin1String("rte"))
      return KindReturn;
   if (m == QLatin1String("nop"))
      return KindNop;
   if (m.startsWith(QLatin1String("unrecognized")) || m.startsWith(QLatin1String("illegal")))
      return KindBad;

   static const char *flow[] = { "bra", "braf", "bsr", "bsrf", "jmp", "jsr", "bt", "bf",
                                 "bt/s", "bf/s", "trapa", 0 };
   for (const char **f = flow; *f; f++)
      if (m == QLatin1String(*f))
         return KindFlow;
   return KindNormal;
}

} // namespace

UIDisasm::UIDisasm( QWidget* p )
	: QAbstractScrollArea( p ), fontWidth(8), fontHeight(14), rowHeight(18),
     context(NULL), disassembleFunction(DisasmInstructionNull), address(0), vCenter(true),
     endAddress(0), pc(0xFFFFFFFF), instructionSize(1), fixedSize(false)
{
   // Columns only line up with a fixed-pitch font
   QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
   f.setPointSize(f.pointSize() > 0 ? qMax(9, f.pointSize()) : 10);
   setFont(f);

   setFocusPolicy(Qt::StrongFocus);
   viewport()->setBackgroundRole(QPalette::Base);
   viewport()->setAutoFillBackground(true);

   setSelectionColor(QColor(0x6d, 0x9e, 0xff, 0xff));

   adjustSettings();
}

void UIDisasm::setSelectionColor(const QColor &color)
{
   selectionColor = color;
   viewport()->update();
}

void UIDisasm::setDisassembleFunction(int (*func)(void *, u32, char *))
{
   disassembleFunction = func;
   viewport()->update();
}

void UIDisasm::setEndAddress(u32 address)
{
   endAddress = address;
   adjustSettings();
}

void UIDisasm::setContext(void *context)
{
   this->context = context;
}

void UIDisasm::setFixedInstructionSize(bool fixed)
{
   fixedSize = fixed;
}

void UIDisasm::setBreakpoints(const QSet<u32> &addresses)
{
   breakpoints = addresses;
   viewport()->update();
}

int UIDisasm::visibleRows() const
{
   return qMax(1, viewport()->height() / rowHeight);
}

u32 UIDisasm::topAddress() const
{
   const u32 pos = (u32)verticalScrollBar()->value();
   return pos / instructionSize * instructionSize;
}

void UIDisasm::goToAddress(u32 address, bool vCenter)
{
   this->address = address;
   this->vCenter = vCenter;

   // Already on screen (and not on the very last row): leave the view alone, so
   // that stepping through straight-line code moves the highlight instead of
   // re-scrolling the whole listing at every step.
   const int rows = visibleRows();
   u32 cur = topAddress();
   for (int r = 0; r < rows - 1; r++)
   {
      if (cur == address)
      {
         viewport()->update();
         return;
      }
      char text[256];
      text[0] = 0;
      int off = disassembleFunction(context, cur, text);
      cur += (off > 0) ? off : instructionSize;
   }

   u32 target = address;
   if (vCenter && fixedSize)
   {
      const u32 back = (u32)(rows / 3) * (u32)instructionSize;
      target = (address > back) ? address - back : 0;
   }
   verticalScrollBar()->setValue((int)target);
   viewport()->update();
}

void UIDisasm::setPC(u32 address)
{
   this->pc = address;
   viewport()->update();
}

void UIDisasm::setMinimumInstructionSize(int instructionSize)
{
   this->instructionSize = instructionSize > 0 ? instructionSize : 1;
   adjustSettings();
}

void UIDisasm::adjustSettings()
{
   verticalScrollBar()->setRange(0, endAddress);
   verticalScrollBar()->setSingleStep(instructionSize);

   const QFontMetrics fm(font());
   fontWidth = qMax(1, fm.boundingRect(QLatin1Char('9')).width());
   fontHeight = fm.height();
   rowHeight = fontHeight + 4;

   verticalScrollBar()->setPageStep(qMax(1, visibleRows() - 1) * instructionSize);
   viewport()->update();
}

void UIDisasm::changeEvent(QEvent *event)
{
   QAbstractScrollArea::changeEvent(event);
   if (event->type() == QEvent::FontChange)
      adjustSettings();
   else if (event->type() == QEvent::PaletteChange)
      viewport()->update();
}

void UIDisasm::resizeEvent(QResizeEvent *event)
{
   QAbstractScrollArea::resizeEvent(event);
   verticalScrollBar()->setPageStep(qMax(1, visibleRows() - 1) * instructionSize);
}

// Address of the instruction drawn at vertical position y of the viewport
u32 UIDisasm::addressAtY(int y) const
{
   const int line = qMax(0, y) / rowHeight;
   u32 cur = topAddress();
   for (int i = 0; i < line; i++)
   {
      char text[256];
      text[0] = 0;
      int off = disassembleFunction(context, cur, text);
      cur += (off > 0) ? off : instructionSize;
   }
   return cur;
}

void UIDisasm::mouseDoubleClickEvent( QMouseEvent * event )
{
   if (event->button() == Qt::LeftButton)
      emit toggleCodeBreakpoint(addressAtY(event->pos().y()));
}

void UIDisasm::mousePressEvent( QMouseEvent * event )
{
   // One click on the margin toggles a breakpoint, like in most debuggers
   if (event->button() == Qt::LeftButton && event->pos().x() < GutterWidth)
   {
      emit toggleCodeBreakpoint(addressAtY(event->pos().y()));
      return;
   }
   QAbstractScrollArea::mousePressEvent(event);
}

void UIDisasm::contextMenuEvent( QContextMenuEvent * event )
{
   const u32 addr = addressAtY(event->pos().y());
   char text[256];
   text[0] = 0;
   disassembleFunction(context, addr, text);

   QMenu menu(this);
   QAction *toggle = menu.addAction(breakpoints.contains(addr) ? tr("Remove breakpoint") : tr("Add breakpoint"));
   menu.addSeparator();
   QAction *copyAddr = menu.addAction(tr("Copy address"));
   QAction *copyLine = menu.addAction(tr("Copy line"));

   QAction *chosen = menu.exec(event->globalPos());
   if (chosen == toggle)
      emit toggleCodeBreakpoint(addr);
   else if (chosen == copyAddr)
      QApplication::clipboard()->setText(QString("%1").arg(addr, 8, 16, QChar('0')).toUpper());
   else if (chosen == copyLine)
      QApplication::clipboard()->setText(QString::fromLatin1(text).trimmed());
}

void UIDisasm::drawRow(QPainter &painter, int y, const QString &text, bool isPC)
{
   const QPalette pal = palette();
   const QColor textColor = pal.color(QPalette::Text);
   const QColor baseColor = pal.color(QPalette::Base);
   const QColor dim = mixColors(textColor, baseColor, 0.5);
   const bool dark = baseColor.lightness() < 128;

   const int baseline = y + (rowHeight - fontHeight) / 2 + painter.fontMetrics().ascent();
   int x = GutterWidth + 8;

   // "0x06004000: mov.l @r15+, r14 ; comment"
   static const QRegularExpression re(QString::fromLatin1("^(0x[0-9A-Fa-f]+:)\\s*(.*)$"));
   QRegularExpressionMatch m = re.match(text);
   if (!m.hasMatch())
   {
      painter.setPen(textColor);
      painter.drawText(x, baseline, text.trimmed());
      return;
   }

   // Column positions come from measured text widths, never from "characters
   // times the width of a digit" (the ink width of a glyph is smaller than its
   // advance, so that estimate made the columns collide).
   const QFontMetrics fm(painter.font());
   QFont boldFont = painter.font();
   boldFont.setBold(true);
   const QFontMetrics fmBold(boldFont);
   QFont italicFont = painter.font();
   italicFont.setItalic(true);
   const QString oneSpace = QString::fromLatin1(" ");

   // Address column
   painter.setPen(isPC ? textColor : dim);
   const QString addr = m.captured(1);
   painter.drawText(x, baseline, addr);
   x += qMax(fm.size(Qt::TextSingleLine, QString::fromLatin1("0x00000000: ")).width(),
             fm.size(Qt::TextSingleLine, addr + oneSpace).width());

   // Split code and trailing comment
   QString code = m.captured(2);
   QString comment;
   const int semi = code.indexOf(QLatin1Char(';'));
   if (semi >= 0)
   {
      comment = code.mid(semi + 1).trimmed();
      code = code.left(semi).trimmed();
   }

   const int space = code.indexOf(QLatin1Char(' '));
   const QString mnemonic = (space < 0) ? code : code.left(space);
   const QString operands = (space < 0) ? QString() : code.mid(space + 1).trimmed();

   QColor mnemonicColor = textColor;
   switch (classify(mnemonic))
   {
      case KindFlow:   mnemonicColor = pal.color(QPalette::Highlight); break;
      case KindReturn: mnemonicColor = dark ? QColor(190, 140, 255) : QColor(130, 80, 223); break;
      case KindNop:    mnemonicColor = dim; break;
      case KindBad:    mnemonicColor = QColor(209, 52, 56); break;
      default: break;
   }

   painter.setFont(boldFont);
   painter.setPen(mnemonicColor);
   painter.drawText(x, baseline, mnemonic);
   painter.setFont(font());

   // Operands start in a fixed column (wide enough for "mov.l", "dmuls.l"...)
   // so that they line up from one row to the next
   x += qMax(fmBold.size(Qt::TextSingleLine, QString::fromLatin1("mmmmmmm ")).width(),
             fmBold.size(Qt::TextSingleLine, mnemonic).width() + fm.size(Qt::TextSingleLine, oneSpace).width());
   painter.setPen(textColor);
   painter.drawText(x, baseline, operands);

   if (!comment.isEmpty())
   {
      x += fm.size(Qt::TextSingleLine, operands + QString::fromLatin1("   ")).width();
      painter.setFont(italicFont);
      painter.setPen(dim);
      painter.drawText(x, baseline, QString::fromLatin1("; ") + comment);
      painter.setFont(font());
   }
}

void UIDisasm::paintEvent(QPaintEvent *event)
{
   Q_UNUSED(event);

   // Always repaint the whole viewport from its top: the rows are drawn from
   // the scroll position, so starting at the top of a partial dirty rectangle
   // (as this used to do) shifted the listing whenever only part of it was
   // redrawn.
   QPainter painter(viewport());
   painter.setFont(font());

   const QPalette pal = palette();
   const QColor textColor = pal.color(QPalette::Text);
   const QColor baseColor = pal.color(QPalette::Base);
   const QColor accent = pal.color(QPalette::Highlight);
   const QColor breakColor(209, 52, 56);
   const int w = viewport()->width();
   const int h = viewport()->height();

   // Margin
   painter.fillRect(QRect(0, 0, GutterWidth, h), pal.color(QPalette::AlternateBase));
   painter.setPen(mixColors(textColor, baseColor, 0.12));
   painter.drawLine(GutterWidth, 0, GutterWidth, h);

   u32 cur = topAddress();
   for (int row = 0; row * rowHeight < h; row++)
   {
      const int y = row * rowHeight;

      char text[256];
      text[0] = 0;
      int off = disassembleFunction(context, cur, text);
      if (off <= 0)
         off = instructionSize;

      const bool isPC = (pc != 0xFFFFFFFF && cur == pc);
      const bool isBP = breakpoints.contains(cur);

      if (isPC)
      {
         QColor c = accent;
         c.setAlpha(55);
         painter.fillRect(QRect(GutterWidth + 1, y, w - GutterWidth - 1, rowHeight), c);
         painter.fillRect(QRect(GutterWidth + 1, y, 3, rowHeight), accent);
      }
      else if (isBP)
      {
         QColor c = breakColor;
         c.setAlpha(28);
         painter.fillRect(QRect(GutterWidth + 1, y, w - GutterWidth - 1, rowHeight), c);
      }

      painter.setRenderHint(QPainter::Antialiasing, true);
      if (isBP)
      {
         painter.setPen(Qt::NoPen);
         painter.setBrush(breakColor);
         painter.drawEllipse(QPoint(10, y + rowHeight / 2), 5, 5);
      }
      if (isPC)
      {
         const int cy = y + rowHeight / 2;
         QPolygon arrow;
         arrow << QPoint(17, cy - 5) << QPoint(26, cy) << QPoint(17, cy + 5);
         painter.setPen(Qt::NoPen);
         painter.setBrush(accent);
         painter.drawPolygon(arrow);
      }
      painter.setRenderHint(QPainter::Antialiasing, false);

      drawRow(painter, y, QString::fromLatin1(text), isPC);

      cur += off;
   }
}
