/*  Copyright 2026 Kronos fork contributors

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kronos is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kronos; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "UIDebugGfxEditor.h"
#include "UIEditsExport.h"
#include "UIYabause.h"
#include "CommonDialogs.h"
#include "../YabauseThread.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

extern "C" {
#include "vdp1.h"
#include "vdp2.h"
}

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QString hex(u32 v, int digits) { return QString("0x%1").arg(v, digits, 16, QChar('0')).toUpper().replace("0X", "0x"); }

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

QBrush checkerBrush(int cell)
{
    QPixmap tile(cell * 2, cell * 2);
    tile.fill(QColor(0xCC, 0xCC, 0xCC));
    QPainter p(&tile);
    p.fillRect(0, 0, cell, cell, QColor(0x99, 0x99, 0x99));
    p.fillRect(cell, cell, cell, cell, QColor(0x99, 0x99, 0x99));
    p.end();
    return QBrush(tile);
}

QPoint wheelPos(QWheelEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return e->position().toPoint();
#else
    return e->pos();
#endif
}

quint64 fnv(quint64 h, const void *data, int n)
{
    const unsigned char *p = (const unsigned char *)data;
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
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
    g->setExpanded(true);
    return g;
}

void makeRow(QTreeWidgetItem *g, const QString &label, const QString &value)
{
    QTreeWidgetItem *it = new QTreeWidgetItem(g);
    it->setText(0, label);
    it->setText(1, value);
    it->setToolTip(0, label);
    it->setToolTip(1, value);
    if (!value.isEmpty() && (value.at(0).isDigit() || value.at(0) == QLatin1Char('-'))) {
        QFont mono(QString::fromLatin1("Courier New"));
        mono.setStyleHint(QFont::Monospace);
        it->setFont(1, mono);
    }
}

const char *spriteModeName(int mode)
{
    static const char *n[8] = { "4 bpp bank (16)", "4 bpp LUT (16)", "8 bpp bank (64)", "8 bpp bank (128)",
                                "8 bpp bank (256)", "16 bpp RGB", "invalid", "invalid" };
    return n[mode & 7];
}

const char *bgFormatName(int cn)
{
    static const char *n[5] = { "16 colors", "256 colors", "2048 colors", "RGB 32768", "RGB 16M" };
    return n[(cn >= 0 && cn <= 4) ? cn : 0];
}

const int kSourceLayer[UIDebugGfxEditor::SrcCount] = {
    -1, -1, VDPEDIT_NBG0, VDPEDIT_NBG1, VDPEDIT_NBG2, VDPEDIT_NBG3, VDPEDIT_RBG0, VDPEDIT_RBG0B, VDPEDIT_RBG1
};

// Sprite picture in the game's look (editor marks off) or with the marks the
// editor shows: end codes in magenta, dots after the second end code of a
// line (not drawn by the VDP1) faded.
QImage renderSprite(const VdpEditSprite &spr, const QByteArray &lut, int w, int h,
                    const std::vector<u32> &codes, bool marks)
{
    QImage img(w, h, QImage::Format_ARGB32);
    int mode = 0;
    VdpEditSpriteGeometry(&spr, NULL, NULL, &mode, NULL, NULL);
    const int n = VdpEditSpritePaletteSize(&spr);
    const u8 *l = lut.size() >= 32 ? (const u8 *)lut.constData() : NULL;
    QRgb pal[256];
    for (int k = 0; k < n; k++)
        pal[k] = VdpEditSpriteDataArgb(VdpEditSpriteCodeData(&spr, l, (u32)k));

    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        int ends = 0;
        for (int x = 0; x < w; x++) {
            const size_t i = (size_t)y * w + x;
            const u32 c = i < codes.size() ? codes[i] : 0;
            const int kind = VdpEditSpriteCodeKind(&spr, c);
            QRgb col;
            if (kind == 2) {
                ends++;
                col = marks ? qRgba(255, 0, 255, 150) : 0;
            } else if (kind == 1) {
                col = 0;
            } else {
                col = (n > 0) ? pal[c % (u32)n] : (QRgb)VdpEditSpriteDataArgb(c);
            }
            if (ends >= 2 && kind != 2)
                col = marks ? ((col & 0x00FFFFFF) | (qAlpha(col) ? 0x40000000u : 0)) : 0;
            else if (ends >= 2 && kind == 2 && !marks)
                col = 0;
            line[x] = col;
        }
    }
    return img;
}

} // namespace

// ===========================================================================
// GfxCanvas
// ===========================================================================

GfxCanvas::GfxCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
}

QSize GfxCanvas::sizeHint() const
{
    return QSize(qMax(1, mW * mZoom), qMax(1, mH * mZoom));
}

void GfxCanvas::setPicture(int w, int h, const std::vector<u32> &codes)
{
    mW = w; mH = h;
    mCodes = codes;
    mCodes.resize((size_t)w * h, 0);
    mUndo.clear(); mRedo.clear();
    mDrawing = false;
    mLast = QPoint(-1, -1);
    rerender();
    setFixedSize(sizeHint());
    emit historyChanged();
}

void GfxCanvas::clearPicture()
{
    mW = mH = 0;
    mCodes.clear();
    mImage = QImage();
    mUndo.clear(); mRedo.clear();
    setFixedSize(1, 1);
    update();
    emit historyChanged();
}

void GfxCanvas::replaceCodes(const std::vector<u32> &codes, bool undoable)
{
    if (codes.size() != mCodes.size()) return;
    if (undoable && codes != mCodes) {
        mUndo.push_back(mCodes);
        if (mUndo.size() > 200) mUndo.erase(mUndo.begin());
        mRedo.clear();
    }
    mCodes = codes;
    rerender();
    emit historyChanged();
}

void GfxCanvas::rerender()
{
    if (mRender && mW > 0) mImage = mRender(mCodes);
    update();
}

void GfxCanvas::setZoom(int z)
{
    mZoom = qBound(1, z, 64);
    setFixedSize(sizeHint());
    update();
}

void GfxCanvas::undo()
{
    if (mUndo.empty()) return;
    mRedo.push_back(mCodes);
    mCodes = mUndo.back();
    mUndo.pop_back();
    rerender();
    emit historyChanged();
    emit edited();
}

void GfxCanvas::redo()
{
    if (mRedo.empty()) return;
    mUndo.push_back(mCodes);
    mCodes = mRedo.back();
    mRedo.pop_back();
    rerender();
    emit historyChanged();
    emit edited();
}

QPoint GfxCanvas::toPic(const QPoint &p) const
{
    return QPoint((int)std::floor((double)p.x() / mZoom), (int)std::floor((double)p.y() / mZoom));
}

void GfxCanvas::plot(int x, int y, u32 c)
{
    const int r0 = -(mBrush - 1) / 2, r1 = mBrush / 2;
    for (int dy = r0; dy <= r1; dy++)
        for (int dx = r0; dx <= r1; dx++) {
            const int px = x + dx, py = y + dy;
            if (px >= 0 && py >= 0 && px < mW && py < mH)
                mCodes[(size_t)py * mW + px] = c;
        }
}

void GfxCanvas::line(QPoint a, QPoint b, u32 c)
{
    int x0 = a.x(), y0 = a.y();
    const int x1 = b.x(), y1 = b.y();
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void GfxCanvas::rect(QPoint a, QPoint b, u32 c, bool filled)
{
    const int x0 = qMin(a.x(), b.x()), x1 = qMax(a.x(), b.x());
    const int y0 = qMin(a.y(), b.y()), y1 = qMax(a.y(), b.y());
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (filled || y == y0 || y == y1 || x == x0 || x == x1)
                if (x >= 0 && y >= 0 && x < mW && y < mH)
                    mCodes[(size_t)y * mW + x] = c;
}

void GfxCanvas::flood(QPoint p, u32 c)
{
    if (p.x() < 0 || p.y() < 0 || p.x() >= mW || p.y() >= mH) return;
    const u32 target = mCodes[(size_t)p.y() * mW + p.x()];
    if (target == c) return;
    std::vector<QPoint> stack;
    stack.push_back(p);
    while (!stack.empty()) {
        const QPoint q = stack.back();
        stack.pop_back();
        if (q.x() < 0 || q.y() < 0 || q.x() >= mW || q.y() >= mH) continue;
        u32 &v = mCodes[(size_t)q.y() * mW + q.x()];
        if (v != target) continue;
        v = c;
        stack.push_back(QPoint(q.x() + 1, q.y()));
        stack.push_back(QPoint(q.x() - 1, q.y()));
        stack.push_back(QPoint(q.x(), q.y() + 1));
        stack.push_back(QPoint(q.x(), q.y() - 1));
    }
}

void GfxCanvas::beginStroke()
{
    mStrokeStart = mCodes;
    mDrawing = true;
}

void GfxCanvas::endStroke()
{
    mDrawing = false;
    if (mStrokeStart != mCodes) {
        mUndo.push_back(mStrokeStart);
        if (mUndo.size() > 200) mUndo.erase(mUndo.begin());
        mRedo.clear();
        emit historyChanged();
        emit edited();
    }
    mStrokeStart.clear();
}

void GfxCanvas::mousePressEvent(QMouseEvent *e)
{
    if (!hasPicture()) return;
    const QPoint p = toPic(e->pos());
    if (e->button() == Qt::MiddleButton) { e->ignore(); return; }
    const bool primary = (e->button() == Qt::LeftButton);
    if (!primary && e->button() != Qt::RightButton) return;
    mStrokeCode = primary ? mPrimary : mSecondary;

    if (mTool == Picker || (e->modifiers() & Qt::AltModifier)) {
        if (p.x() >= 0 && p.y() >= 0 && p.x() < mW && p.y() < mH)
            emit picked(mCodes[(size_t)p.y() * mW + p.x()], primary);
        return;
    }
    if (mTool == Eraser) mStrokeCode = mTransparent;

    beginStroke();
    mAnchor = p;
    switch (mTool) {
    case Pencil:
    case Eraser:
        if ((e->modifiers() & Qt::ShiftModifier) && mLast.x() >= 0)
            line(mLast, p, mStrokeCode);
        else
            plot(p.x(), p.y(), mStrokeCode);
        mLast = p;
        rerender();
        break;
    case Fill:
        flood(p, mStrokeCode);
        rerender();
        endStroke();
        break;
    default:
        break;
    }
}

void GfxCanvas::mouseMoveEvent(QMouseEvent *e)
{
    if (!hasPicture()) return;
    const QPoint p = toPic(e->pos());
    if (p != mHover) {
        mHover = p;
        if (p.x() >= 0 && p.y() >= 0 && p.x() < mW && p.y() < mH)
            emit hovered(p.x(), p.y());
        else
            emit hovered(-1, -1);
    }
    if (!mDrawing) { update(); return; }
    switch (mTool) {
    case Pencil:
    case Eraser:
        line(mLast, p, mStrokeCode);
        mLast = p;
        rerender();
        break;
    case Line:
        mCodes = mStrokeStart;
        line(mAnchor, p, mStrokeCode);
        rerender();
        break;
    case Rect:
    case FillRect:
        mCodes = mStrokeStart;
        rect(mAnchor, p, mStrokeCode, mTool == FillRect);
        rerender();
        break;
    default:
        break;
    }
}

void GfxCanvas::mouseReleaseEvent(QMouseEvent *)
{
    if (mDrawing) endStroke();
}

void GfxCanvas::leaveEvent(QEvent *)
{
    mHover = QPoint(-1, -1);
    emit hovered(-1, -1);
    update();
}

void GfxCanvas::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        emit zoomRequest(e->angleDelta().y() > 0 ? 1 : -1, wheelPos(e));
        e->accept();
        return;
    }
    e->ignore();
}

void GfxCanvas::paintEvent(QPaintEvent *ev)
{
    QPainter p(this);
    p.fillRect(ev->rect(), checkerBrush(8));
    if (!hasPicture()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(QRect(0, 0, mW * mZoom, mH * mZoom), mImage);

    if (mGrid && mZoom >= 6) {
        QPen pen(QColor(0, 0, 0, 40));
        pen.setWidth(1);
        p.setPen(pen);
        for (int x = 1; x < mW; x++) p.drawLine(x * mZoom, 0, x * mZoom, mH * mZoom);
        for (int y = 1; y < mH; y++) p.drawLine(0, y * mZoom, mW * mZoom, y * mZoom);
    }
    if (mCellGrid > 0) {
        QPen pen(QColor(0, 160, 255, 140));
        pen.setWidth(1);
        p.setPen(pen);
        for (int x = mCellGrid; x < mW; x += mCellGrid) p.drawLine(x * mZoom, 0, x * mZoom, mH * mZoom);
        for (int y = mCellGrid; y < mH; y += mCellGrid) p.drawLine(0, y * mZoom, mW * mZoom, y * mZoom);
    }
    if (!mHighlight.isNull()) {
        QPen pen(QColor(255, 0, 255));
        pen.setWidth(2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(mHighlight.x() * mZoom, mHighlight.y() * mZoom,
                         mHighlight.width() * mZoom, mHighlight.height() * mZoom));
    }
    if (mHover.x() >= 0 && mHover.y() >= 0 && mHover.x() < mW && mHover.y() < mH) {
        const int r0 = -(mBrush - 1) / 2;
        QPen pen(palette().color(QPalette::Highlight));
        pen.setWidth(1);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect((mHover.x() + r0) * mZoom, (mHover.y() + r0) * mZoom, mBrush * mZoom - 1, mBrush * mZoom - 1);
    }
}

// ===========================================================================
// GfxPalette
// ===========================================================================

GfxPalette::GfxPalette(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void GfxPalette::setEntries(const QVector<QRgb> &colors, const QVector<u32> &codes,
                            const QVector<QString> &tips, int columns)
{
    mColors = colors;
    mCodes = codes;
    mTips = tips;
    mColumns = qMax(1, columns);
    updateGeometry();
    update();
}

int GfxPalette::cell() const
{
    return qBound(8, (width() - 1) / mColumns, 32);
}

QSize GfxPalette::sizeHint() const
{
    return QSize(16 * 16 + 1, heightForWidth(16 * 16 + 1));
}

int GfxPalette::heightForWidth(int w) const
{
    const int c = qBound(8, (w - 1) / mColumns, 32);
    const int rows = (mColors.size() + mColumns - 1) / mColumns;
    return qMax(c, rows * c + 1);
}

int GfxPalette::entryAt(const QPoint &p) const
{
    const int c = cell();
    const int col = p.x() / c, row = p.y() / c;
    if (col < 0 || col >= mColumns || row < 0) return -1;
    const int i = row * mColumns + col;
    return (i < mColors.size()) ? i : -1;
}

void GfxPalette::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int c = cell();
    const QBrush checker = checkerBrush(qMax(2, c / 4));
    for (int i = 0; i < mColors.size(); i++) {
        const QRect r((i % mColumns) * c, (i / mColumns) * c, c, c);
        if (qAlpha(mColors[i]) < 255) p.fillRect(r, checker);
        p.fillRect(r, QColor::fromRgba(mColors[i]));
        p.setPen(QColor(0, 0, 0, 60));
        p.drawRect(r.adjusted(0, 0, -1, -1));
    }
    for (int i = 0; i < mColors.size(); i++) {
        const bool pri = mCodes[i] == mPrimary, sec = mCodes[i] == mSecondary;
        if (!pri && !sec) continue;
        const QRect r((i % mColumns) * c, (i / mColumns) * c, c, c);
        QPen pen(pri ? QColor(255, 255, 255) : QColor(0, 0, 0));
        pen.setWidth(2);
        p.setPen(pen);
        p.drawRect(r.adjusted(1, 1, -2, -2));
        QPen pen2(pri ? QColor(0, 0, 0) : QColor(255, 255, 255));
        pen2.setWidth(1);
        p.setPen(pen2);
        p.drawRect(r.adjusted(0, 0, -1, -1));
        if (pri && sec) {
            p.setPen(QColor(0, 0, 0));
            p.drawLine(r.topLeft(), r.bottomRight());
        }
    }
}

void GfxPalette::mousePressEvent(QMouseEvent *e)
{
    const int i = entryAt(e->pos());
    if (i < 0) return;
    if (e->button() == Qt::LeftButton) emit chosen(mCodes[i], true);
    else if (e->button() == Qt::RightButton) emit chosen(mCodes[i], false);
}

void GfxPalette::mouseDoubleClickEvent(QMouseEvent *e)
{
    const int i = entryAt(e->pos());
    if (i >= 0 && e->button() == Qt::LeftButton) emit editRequested(i);
}

bool GfxPalette::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        QHelpEvent *he = static_cast<QHelpEvent *>(e);
        const int i = entryAt(he->pos());
        if (i >= 0 && i < mTips.size()) QToolTip::showText(he->globalPos(), mTips[i], this);
        else QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

// ===========================================================================
// UIDebugGfxEditor -- construction
// ===========================================================================

UIDebugGfxEditor::UIDebugGfxEditor(QWidget *parent) : QDialog(parent)
{
    memset(&mItem.spr, 0, sizeof(mItem.spr));
    memset(&mItem.bg, 0, sizeof(mItem.bg));
    memset(&mOverviewBg, 0, sizeof(mOverviewBg));

    setWindowTitle(tr_("Graphics Editor"));
    resize(1320, 820);
    setSizeGripEnabled(true);

    // The VDP1 command list and textures are read from the copy of the VRAM
    // taken when the frame started drawing (vdp1.c). Reference counted: the
    // VDP1 debugger may be open at the same time.
    Vdp1DebugSetCapture(1);

    buildUi();
    applyStyle();
    mUiReady = true;

    mTimer = new QTimer(this);
    connect(mTimer, SIGNAL(timeout()), this, SLOT(onTick()));
    mTimer->start(250);

    refreshSource(false);
    updateChips();
    updateActionState();
    QtYabause::retranslateWidget(this);
}

UIDebugGfxEditor::~UIDebugGfxEditor()
{
    mUiReady = false;
    if (gvOverview) gvOverview->viewport()->removeEventFilter(this);
    if (mCanvasArea) mCanvasArea->viewport()->removeEventFilter(this);
    Vdp1DebugSetCapture(0);
}

QWidget *UIDebugGfxEditor::makePanel(const QString &name)
{
    QFrame *f = new QFrame(this);
    f->setObjectName(name);
    return f;
}

QToolButton *UIDebugGfxEditor::makeTool(const QString &text, const QString &tip, int id)
{
    QToolButton *b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setProperty("toolbtn", true);
    b->setCursor(Qt::PointingHandCursor);
    b->setMinimumSize(30, 28);
    mTools->addButton(b, id);
    return b;
}

void UIDebugGfxEditor::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(12);

    // ---- header ----
    QHBoxLayout *hdr = new QHBoxLayout();
    hdr->setSpacing(8);
    lTitle = new QLabel(tr_("Graphics Editor"), this);
    lTitle->setObjectName("lTitle");
    hdr->addWidget(lTitle);
    hdr->addStretch(1);
    lChipSource = new QLabel(this); lChipSource->setObjectName("chip");
    lChipEdits = new QLabel(this);  lChipEdits->setObjectName("chip");
    lChipEmu = new QLabel(this);    lChipEmu->setObjectName("chip");
    hdr->addWidget(lChipSource);
    hdr->addWidget(lChipEdits);
    hdr->addWidget(lChipEmu);
    root->addLayout(hdr);

    QSplitter *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(12);
    root->addWidget(split, 1);

    // ---- left panel: source ----
    QWidget *left = makePanel("panelLeft");
    left->setMinimumWidth(300);
    QVBoxLayout *vl = new QVBoxLayout(left);
    vl->setContentsMargins(14, 12, 14, 12);
    vl->setSpacing(8);
    QLabel *ls = new QLabel(tr_("Source"), left); ls->setProperty("section", true);
    vl->addWidget(ls);
    cbSource = new QComboBox(left);
    cbSource->addItem(tr_("VDP1 sprites"));
    cbSource->addItem(tr_("Sprite layer (VDP1 frame)"));
    cbSource->addItem(tr_("NBG0"));
    cbSource->addItem(tr_("NBG1"));
    cbSource->addItem(tr_("NBG2"));
    cbSource->addItem(tr_("NBG3"));
    cbSource->addItem(tr_("RBG0"));
    cbSource->addItem(tr_("RBG0 (rotation parameter B)"));
    cbSource->addItem(tr_("RBG1"));
    cbSource->setToolTip(tr_("What to edit: the sprites drawn by the VDP1, the VDP1 frame as the VDP2 sprite layer shows it, or a VDP2 background"));
    vl->addWidget(cbSource);

    stLeft = new QStackedWidget(left);
    vl->addWidget(stLeft, 1);

    // page 0: sprite list
    QWidget *pgList = new QWidget(stLeft);
    QVBoxLayout *pl = new QVBoxLayout(pgList);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(6);
    leFilter = new QLineEdit(pgList);
    leFilter->setPlaceholderText(tr_("Filter (address, size, mode)..."));
    leFilter->setClearButtonEnabled(true);
    pl->addWidget(leFilter);
    lwSprites = new QListWidget(pgList);
    lwSprites->setIconSize(QSize(48, 48));
    lwSprites->setUniformItemSizes(true);
    lwSprites->setAlternatingRowColors(true);
    pl->addWidget(lwSprites, 1);
    cbLibrary = new QCheckBox(tr_("Keep sprites seen in earlier frames"), pgList);
    cbLibrary->setToolTip(tr_("Builds a library of every sprite drawn while the game runs (or while you step frames), "
                              "so animation frames and sprites that are not on screen right now can be edited too"));
    pl->addWidget(cbLibrary);
    pbClearLibrary = new QPushButton(tr_("Clear the library"), pgList);
    pl->addWidget(pbClearLibrary);
    stLeft->addWidget(pgList);

    // page 1: overview (VDP1 frame or VDP2 map)
    QWidget *pgView = new QWidget(stLeft);
    QVBoxLayout *pv = new QVBoxLayout(pgView);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(6);
    lLayerInfo = new QLabel(pgView);
    lLayerInfo->setWordWrap(true);
    lLayerInfo->setProperty("dim", true);
    pv->addWidget(lLayerInfo);
    QHBoxLayout *rr = new QHBoxLayout();
    QLabel *lr = new QLabel(tr_("Edit area"), pgView);
    rr->addWidget(lr);
    cbRegion = new QComboBox(pgView);
    cbRegion->setToolTip(tr_("Size of the part of the map opened in the editor when you click it"));
    rr->addWidget(cbRegion, 1);
    pbFitOverview = new QPushButton(tr_("Fit"), pgView);
    pbFitOverview->setToolTip(tr_("Fit the whole map in the view (Ctrl+wheel zooms)"));
    rr->addWidget(pbFitOverview);
    pv->addLayout(rr);
    gvOverview = new QGraphicsView(pgView);
    mOverviewScene = new QGraphicsScene(gvOverview);
    gvOverview->setScene(mOverviewScene);
    gvOverview->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    gvOverview->setRenderHint(QPainter::SmoothPixmapTransform, false);
    gvOverview->setMouseTracking(true);
    gvOverview->viewport()->setMouseTracking(true);
    gvOverview->viewport()->installEventFilter(this);
    gvOverview->setToolTip(tr_("Click to open that part in the editor. Ctrl+wheel to zoom."));
    gvOverview->setStyleSheet("background: transparent;");
    pv->addWidget(gvOverview, 1);
    lOverviewInfo = new QLabel(pgView);
    lOverviewInfo->setWordWrap(true);
    lOverviewInfo->setProperty("dim", true);
    lOverviewInfo->setMinimumHeight(34);
    pv->addWidget(lOverviewInfo);
    stLeft->addWidget(pgView);

    cbLive = new QCheckBox(tr_("Follow the game while it runs"), left);
    cbLive->setChecked(true);
    cbLive->setToolTip(tr_("Refresh this list / view about once a second while the emulation runs"));
    vl->addWidget(cbLive);
    split->addWidget(left);

    // ---- center: canvas ----
    QWidget *center = makePanel("panelCenter");
    QVBoxLayout *vc = new QVBoxLayout(center);
    vc->setContentsMargins(14, 12, 14, 12);
    vc->setSpacing(8);
    lItemTitle = new QLabel(tr_("Nothing open"), center);
    lItemTitle->setProperty("section", true);
    vc->addWidget(lItemTitle);

    QHBoxLayout *tb = new QHBoxLayout();
    tb->setSpacing(4);
    mTools = new QButtonGroup(this);
    mTools->setExclusive(true);
    tb->addWidget(makeTool(tr_("Pencil"), tr_("Pencil (B). Left button: primary color, right button: secondary. Shift+click draws a line from the last point."), GfxCanvas::Pencil));
    tb->addWidget(makeTool(tr_("Line"), tr_("Line (L)"), GfxCanvas::Line));
    tb->addWidget(makeTool(tr_("Rect"), tr_("Rectangle outline (R)"), GfxCanvas::Rect));
    tb->addWidget(makeTool(tr_("Box"), tr_("Filled rectangle (Shift+R)"), GfxCanvas::FillRect));
    tb->addWidget(makeTool(tr_("Fill"), tr_("Flood fill of the touching dots of the same code (F)"), GfxCanvas::Fill));
    tb->addWidget(makeTool(tr_("Pick"), tr_("Color picker (I, or Alt+click with any tool)"), GfxCanvas::Picker));
    tb->addWidget(makeTool(tr_("Erase"), tr_("Paints the transparent code (E)"), GfxCanvas::Eraser));
    mTools->button(GfxCanvas::Pencil)->setChecked(true);
    tb->addSpacing(8);
    QLabel *lb = new QLabel(tr_("Size"), center);
    tb->addWidget(lb);
    sbBrush = new QSpinBox(center);
    sbBrush->setRange(1, 16);
    sbBrush->setToolTip(tr_("Brush size of the pencil and the eraser"));
    tb->addWidget(sbBrush);
    tb->addStretch(1);
    tbUndo = new QToolButton(center); tbUndo->setText(tr_("Undo")); tbUndo->setToolTip(tr_("Undo (Ctrl+Z)"));
    tbRedo = new QToolButton(center); tbRedo->setText(tr_("Redo")); tbRedo->setToolTip(tr_("Redo (Ctrl+Y)"));
    tbUndo->setProperty("toolbtn", true); tbRedo->setProperty("toolbtn", true);
    tb->addWidget(tbUndo);
    tb->addWidget(tbRedo);
    vc->addLayout(tb);

    QHBoxLayout *zb = new QHBoxLayout();
    zb->setSpacing(6);
    QToolButton *zout = new QToolButton(center); zout->setText("-"); zout->setProperty("toolbtn", true);
    QToolButton *zin = new QToolButton(center);  zin->setText("+");  zin->setProperty("toolbtn", true);
    zout->setToolTip(tr_("Zoom out (Ctrl+wheel, -)"));
    zin->setToolTip(tr_("Zoom in (Ctrl+wheel, +)"));
    lZoom = new QLabel(center);
    lZoom->setMinimumWidth(44);
    lZoom->setAlignment(Qt::AlignCenter);
    zb->addWidget(zout);
    zb->addWidget(lZoom);
    zb->addWidget(zin);
    cbGrid = new QCheckBox(tr_("Dot grid"), center);
    cbGrid->setChecked(true);
    cbCellGrid = new QCheckBox(tr_("Cell grid"), center);
    cbCellGrid->setChecked(true);
    cbCellGrid->setToolTip(tr_("Outlines the 8x8 cells (VDP2) / 8-dot columns (VDP1)"));
    zb->addSpacing(8);
    zb->addWidget(cbGrid);
    zb->addWidget(cbCellGrid);
    zb->addStretch(1);
    vc->addLayout(zb);

    mCanvas = new GfxCanvas();
    mCanvasArea = new QScrollArea(center);
    mCanvasArea->setWidget(mCanvas);
    mCanvasArea->setAlignment(Qt::AlignCenter);
    mCanvasArea->setWidgetResizable(false);
    mCanvasArea->setFrameShape(QFrame::NoFrame);
    mCanvasArea->viewport()->installEventFilter(this);
    vc->addWidget(mCanvasArea, 1);
    mCanvas->setRenderer([this](const std::vector<u32> &c) { return renderCodes(c); });

    lHover = new QLabel(center);
    lHover->setProperty("dim", true);
    lHover->setMinimumHeight(34);
    lHover->setWordWrap(true);
    lHover->setTextInteractionFlags(Qt::TextSelectableByMouse);
    vc->addWidget(lHover);
    split->addWidget(center);

    // ---- right panel: colors + item ----
    QWidget *right = makePanel("panelRight");
    right->setMinimumWidth(330);
    right->setMaximumWidth(420);
    QVBoxLayout *vr = new QVBoxLayout(right);
    vr->setContentsMargins(14, 12, 14, 12);
    vr->setSpacing(8);
    QLabel *lc = new QLabel(tr_("Colors"), right); lc->setProperty("section", true);
    vr->addWidget(lc);
    QHBoxLayout *sw = new QHBoxLayout();
    lPrimary = new QLabel(right);   lPrimary->setObjectName("swatch");
    lSecondary = new QLabel(right); lSecondary->setObjectName("swatch");
    lPrimary->setFixedSize(40, 28);
    lSecondary->setFixedSize(40, 28);
    lPrimary->setToolTip(tr_("Primary color (left button)"));
    lSecondary->setToolTip(tr_("Secondary color (right button). X swaps them."));
    sw->addWidget(lPrimary);
    sw->addWidget(lSecondary);
    pbCustomColor = new QPushButton(tr_("Any color..."), right);
    pbCustomColor->setToolTip(tr_("RGB formats: choose any color as the primary color"));
    sw->addWidget(pbCustomColor, 1);
    vr->addLayout(sw);
    mPalette = new GfxPalette();
    mPaletteArea = new QScrollArea(right);
    mPaletteArea->setWidget(mPalette);
    mPaletteArea->setWidgetResizable(true);
    mPaletteArea->setFrameShape(QFrame::NoFrame);
    mPaletteArea->setMinimumHeight(140);
    vr->addWidget(mPaletteArea, 2);
    lPaletteInfo = new QLabel(right);
    lPaletteInfo->setWordWrap(true);
    lPaletteInfo->setProperty("dim", true);
    vr->addWidget(lPaletteInfo);

    QFrame *sep = new QFrame(right);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    vr->addWidget(sep);

    QLabel *li = new QLabel(tr_("Item"), right); li->setProperty("section", true);
    vr->addWidget(li);
    twInfo = new QTreeWidget(right);
    twInfo->setColumnCount(2);
    twInfo->setHeaderHidden(true);
    twInfo->setRootIsDecorated(true);
    twInfo->setAlternatingRowColors(true);
    twInfo->setUniformRowHeights(true);
    twInfo->setFrameShape(QFrame::NoFrame);
    twInfo->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    twInfo->header()->setStretchLastSection(true);
    vr->addWidget(twInfo, 2);

    pbApply = new QPushButton(tr_("Apply to the game"), right);
    pbApply->setObjectName("pbApply");
    pbApply->setMinimumHeight(38);
    pbApply->setCursor(Qt::PointingHandCursor);
    pbApply->setToolTip(tr_("Writes the picture into VRAM (Ctrl+Enter). The edit stays active while the game runs: "
                            "it is written again each time the game reloads the original graphic."));
    vr->addWidget(pbApply);
    cbAutoApply = new QCheckBox(tr_("Apply after every stroke"), right);
    cbAutoApply->setChecked(true);
    vr->addWidget(cbAutoApply);
    lApplyStatus = new QLabel(right);
    lApplyStatus->setWordWrap(true);
    lApplyStatus->setProperty("dim", true);
    vr->addWidget(lApplyStatus);
    QGridLayout *ga = new QGridLayout();
    ga->setSpacing(6);
    pbRevertItem = new QPushButton(tr_("Revert item"), right);
    pbRevertItem->setToolTip(tr_("Puts the original graphic of this item back and forgets its edits"));
    pbImportPng = new QPushButton(tr_("Import PNG..."), right);
    pbImportPng->setToolTip(tr_("Loads a picture of the same size; colors are mapped to the nearest color of the palette"));
    pbExportPng = new QPushButton(tr_("Export PNG..."), right);
    ga->addWidget(pbRevertItem, 0, 0, 1, 2);
    ga->addWidget(pbImportPng, 1, 0);
    ga->addWidget(pbExportPng, 1, 1);
    vr->addLayout(ga);
    for (QPushButton *b : { pbRevertItem, pbImportPng, pbExportPng, pbCustomColor, pbClearLibrary, pbFitOverview }) {
        b->setProperty("flat2", true);
        b->setCursor(Qt::PointingHandCursor);
    }
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes(QList<int>() << 330 << 620 << 350);

    // ---- footer ----
    QHBoxLayout *ft = new QHBoxLayout();
    ft->setSpacing(8);
    pbRun = new QPushButton(this);
    pbRun->setObjectName("pbRun");
    pbRun->setMinimumHeight(32);
    pbRun->setCursor(Qt::PointingHandCursor);
    pbStep = new QPushButton(tr_("Next frame"), this);
    pbStep->setToolTip(tr_("Runs the emulation for one frame (F10)"));
    pbRefresh = new QPushButton(tr_("Refresh"), this);
    pbRefresh->setToolTip(tr_("Reads the sprite list / the map again (F5)"));
    pbEdits = new QPushButton(tr_("Save / load edits..."), this);
    pbEdits->setToolTip(tr_("Edit pack (.kmod), patched disc image, BPS or IPS patch"));
    pbRevertAll = new QPushButton(tr_("Revert all edits"), this);
    pbExportAll = new QPushButton(tr_("Export all sprites..."), this);
    pbExportAll->setToolTip(tr_("Writes every sprite of the list as PNG (+ index.csv) to edit them in another program"));
    pbImportFolder = new QPushButton(tr_("Import sprite folder..."), this);
    pbImportFolder->setToolTip(tr_("Re-imports the PNG files of an exported folder that were modified since the export"));
    for (QPushButton *b : { pbStep, pbRefresh, pbEdits, pbRevertAll, pbExportAll, pbImportFolder }) {
        b->setProperty("flat2", true);
        b->setMinimumHeight(32);
        b->setCursor(Qt::PointingHandCursor);
    }
    ft->addWidget(pbRun);
    ft->addWidget(pbStep);
    ft->addWidget(pbRefresh);
    ft->addSpacing(12);
    ft->addWidget(pbExportAll);
    ft->addWidget(pbImportFolder);
    ft->addWidget(pbEdits);
    ft->addWidget(pbRevertAll);
    ft->addStretch(1);
    lFootNote = new QLabel(tr_("Edits are kept every frame and saved with \"Save / load edits\"."), this);
    lFootNote->setProperty("dim", true);
    ft->addWidget(lFootNote);
    QPushButton *pbClose = new QPushButton(tr_("Close"), this);
    pbClose->setProperty("flat2", true);
    pbClose->setMinimumHeight(32);
    ft->addWidget(pbClose);
    root->addLayout(ft);

    // ---- connections ----
    connect(cbSource, SIGNAL(currentIndexChanged(int)), this, SLOT(onSourceChanged(int)));
    connect(lwSprites, SIGNAL(itemSelectionChanged()), this, SLOT(onSpriteSelected()));
    connect(leFilter, SIGNAL(textChanged(QString)), this, SLOT(onFilterChanged(QString)));
    connect(cbLibrary, &QCheckBox::toggled, this, [this](bool) { refreshSource(true); });
    connect(pbClearLibrary, &QPushButton::clicked, this, [this]() { mSprites.clear(); refreshSource(true); });
    connect(cbRegion, SIGNAL(currentIndexChanged(int)), this, SLOT(onRegionChanged(int)));
    connect(pbFitOverview, &QPushButton::clicked, this, [this]() { fitOverview(); });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(mTools, &QButtonGroup::idClicked, this, [this](int id) { mCanvas->setTool((GfxCanvas::Tool)id); });
#else
    connect(mTools, static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked),
            this, [this](int id) { mCanvas->setTool((GfxCanvas::Tool)id); });
#endif
    connect(sbBrush, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this](int v) { mCanvas->setBrush(v); });
    connect(tbUndo, &QToolButton::clicked, mCanvas, &GfxCanvas::undo);
    connect(tbRedo, &QToolButton::clicked, mCanvas, &GfxCanvas::redo);
    connect(zout, &QToolButton::clicked, this, [this]() { onZoom(-1); });
    connect(zin, &QToolButton::clicked, this, [this]() { onZoom(1); });
    connect(cbGrid, &QCheckBox::toggled, mCanvas, &GfxCanvas::setGrid);
    connect(cbCellGrid, &QCheckBox::toggled, this, [this](bool on) {
        mCanvas->setCellGrid(on ? (mItem.kind == Item::Cells ? mItem.bg.csize : 8) : 0);
    });
    connect(mCanvas, SIGNAL(edited()), this, SLOT(onCanvasEdited()));
    connect(mCanvas, SIGNAL(hovered(int, int)), this, SLOT(onCanvasHover(int, int)));
    connect(mCanvas, SIGNAL(picked(u32, bool)), this, SLOT(onPicked(u32, bool)));
    connect(mCanvas, &GfxCanvas::historyChanged, this, [this]() {
        tbUndo->setEnabled(mCanvas->canUndo());
        tbRedo->setEnabled(mCanvas->canRedo());
    });
    connect(mCanvas, &GfxCanvas::zoomRequest, this, [this](int d, QPoint) { onZoom(d); });
    connect(mPalette, SIGNAL(chosen(u32, bool)), this, SLOT(onPaletteChosen(u32, bool)));
    connect(mPalette, SIGNAL(editRequested(int)), this, SLOT(onPaletteEdit(int)));
    connect(pbCustomColor, SIGNAL(clicked()), this, SLOT(onCustomColor()));
    connect(pbApply, SIGNAL(clicked()), this, SLOT(onApplyClicked()));
    connect(pbRevertItem, SIGNAL(clicked()), this, SLOT(onRevertItemClicked()));
    connect(pbImportPng, SIGNAL(clicked()), this, SLOT(onImportPng()));
    connect(pbExportPng, SIGNAL(clicked()), this, SLOT(onExportPng()));
    connect(pbRun, SIGNAL(clicked()), this, SLOT(onRunClicked()));
    connect(pbStep, SIGNAL(clicked()), this, SLOT(onStepClicked()));
    connect(pbRefresh, SIGNAL(clicked()), this, SLOT(onRefreshClicked()));
    connect(pbEdits, SIGNAL(clicked()), this, SLOT(onEditsDialog()));
    connect(pbRevertAll, SIGNAL(clicked()), this, SLOT(onRevertAllClicked()));
    connect(pbExportAll, SIGNAL(clicked()), this, SLOT(onExportAllSprites()));
    connect(pbImportFolder, SIGNAL(clicked()), this, SLOT(onImportSpriteFolder()));
    connect(pbClose, SIGNAL(clicked()), this, SLOT(reject()));

    // ---- shortcuts ----
    struct { const char *key; int tool; } tk[] = {
        { "B", GfxCanvas::Pencil }, { "L", GfxCanvas::Line }, { "R", GfxCanvas::Rect },
        { "Shift+R", GfxCanvas::FillRect }, { "F", GfxCanvas::Fill }, { "I", GfxCanvas::Picker },
        { "E", GfxCanvas::Eraser }
    };
    for (size_t i = 0; i < sizeof(tk) / sizeof(tk[0]); i++) {
        const int tool = tk[i].tool;
        QShortcut *s = new QShortcut(QKeySequence(QString::fromLatin1(tk[i].key)), this);
        connect(s, &QShortcut::activated, this, [this, tool]() {
            if (mTools->button(tool)) mTools->button(tool)->setChecked(true);
            mCanvas->setTool((GfxCanvas::Tool)tool);
        });
    }
    connect(new QShortcut(QKeySequence::Undo, this), &QShortcut::activated, mCanvas, &GfxCanvas::undo);
    connect(new QShortcut(QKeySequence::Redo, this), &QShortcut::activated, mCanvas, &GfxCanvas::redo);
    connect(new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+Y")), this), &QShortcut::activated, mCanvas, &GfxCanvas::redo);
    connect(new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+Return")), this), &QShortcut::activated, this, &UIDebugGfxEditor::onApplyClicked);
    connect(new QShortcut(QKeySequence(QString::fromLatin1("F5")), this), &QShortcut::activated, this, &UIDebugGfxEditor::onRefreshClicked);
    connect(new QShortcut(QKeySequence(QString::fromLatin1("F10")), this), &QShortcut::activated, this, &UIDebugGfxEditor::onStepClicked);
    connect(new QShortcut(QKeySequence(QString::fromLatin1("+")), this), &QShortcut::activated, this, [this]() { onZoom(1); });
    connect(new QShortcut(QKeySequence(QString::fromLatin1("-")), this), &QShortcut::activated, this, [this]() { onZoom(-1); });
    connect(new QShortcut(QKeySequence(QString::fromLatin1("G")), this), &QShortcut::activated, cbGrid, &QCheckBox::toggle);
    connect(new QShortcut(QKeySequence(QString::fromLatin1("X")), this), &QShortcut::activated, this, [this]() {
        const u32 p = mPrimaryCode;
        setPrimary(mSecondaryCode);
        setSecondary(p);
    });

    mCanvas->clearPicture();
    lZoom->setText(QString("%1x").arg(mCanvas->zoom()));
}

void UIDebugGfxEditor::applyStyle()
{
    // Same look as the SCSP debugger: palette() colors only, so light and dark
    // themes both work.
    setStyleSheet(QString::fromLatin1("QLabel[dim=\"true\"] { color: %1; }").arg(dimColor(this).name()) + QString::fromLatin1(
        "QFrame#panelLeft, QFrame#panelCenter, QFrame#panelRight {"
        "  background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }"
        "QLabel#chip { border: 1px solid palette(mid); border-radius: 10px; padding: 2px 10px; font-weight: 600; }"
        "QLabel#swatch { border: 1px solid palette(mid); border-radius: 4px; }"
        "QLabel[section=\"true\"] { font-weight: 700; }"
        "QToolButton[toolbtn=\"true\"] { border: 1px solid palette(mid); border-radius: 6px; padding: 3px 8px; background: transparent; }"
        "QToolButton[toolbtn=\"true\"]:hover { border-color: palette(highlight); }"
        "QToolButton[toolbtn=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
        "QToolButton:disabled { color: palette(mid); }"
        "QPushButton#pbApply, QPushButton#pbRun { background: palette(highlight); color: palette(highlighted-text);"
        "  border: none; border-radius: 8px; padding: 6px 18px; font-weight: 600; }"
        "QPushButton#pbApply:disabled, QPushButton#pbRun:disabled { background: palette(midlight); color: palette(mid); }"
        "QPushButton[flat2=\"true\"] { border: 1px solid palette(mid); border-radius: 8px; padding: 5px 12px; background: transparent; }"
        "QPushButton[flat2=\"true\"]:hover { border-color: palette(highlight); }"
        "QPushButton:disabled { color: palette(mid); }"
        "QLineEdit, QComboBox, QSpinBox { border: 1px solid palette(mid); border-radius: 6px; padding: 4px 8px; background: palette(base); }"
        "QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: palette(highlight); }"
        "QListWidget, QTreeWidget { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
        "QTreeWidget::item { padding: 2px 4px; }"
        "QScrollArea { background: transparent; }"));

    QFont f = lTitle->font();
    f.setPointSize(f.pointSize() + 6);
    f.setBold(true);
    lTitle->setFont(f);
}

// ===========================================================================
// Emulation state
// ===========================================================================

YabauseThread *UIDebugGfxEditor::thread() const
{
    UIYabause *w = QtYabause::mainWindow(false);
    return w ? w->mYabauseThread : NULL;
}

bool UIDebugGfxEditor::emuReady() const
{
    // The VDP memories exist only while a game is loaded (Vdp1Init/Vdp2Init).
    YabauseThread *t = thread();
    if (t && !t->emulationRunning()) return false;
    return Vdp1Ram && Vdp2Ram && Vdp2Regs && Vdp2ColorRam && Vdp1Regs;
}

void UIDebugGfxEditor::updateChips()
{
    YabauseThread *t = thread();
    const bool ready = emuReady();
    const bool paused = !t || t->emulationPaused();
    lChipEmu->setText(!ready ? tr_("No game running") : paused ? tr_("Emulation paused") : tr_("Emulation running"));
    pbRun->setText(paused ? tr_("Run") : tr_("Pause"));
    pbRun->setToolTip(paused ? tr_("Runs the game with this window open: the edits show on screen as you draw")
                             : tr_("Pauses the game"));
    pbRun->setEnabled(ready);
    pbStep->setEnabled(ready && paused);
    lChipSource->setText(cbSource->currentText());
    const int n = VdpEditCount(-1);
    lChipEdits->setText(n == 1 ? tr_("1 edit") : tr_("%1 edits").arg(n));
}

void UIDebugGfxEditor::updateActionState()
{
    const bool open = mItem.kind != Item::None;
    pbApply->setEnabled(open && emuReady());
    pbRevertItem->setEnabled(open && emuReady());
    pbImportPng->setEnabled(open);
    pbExportPng->setEnabled(open);
    pbCustomColor->setEnabled(open && itemIsRgb());
    const bool sprites = cbSource->currentIndex() <= SrcSpriteLayer;
    pbExportAll->setEnabled(sprites && !mSprites.empty());
    pbImportFolder->setEnabled(emuReady());
    pbRevertAll->setEnabled(VdpEditCount(-1) > 0);
    tbUndo->setEnabled(mCanvas->canUndo());
    tbRedo->setEnabled(mCanvas->canRedo());
}

void UIDebugGfxEditor::onRunClicked()
{
    YabauseThread *t = thread();
    if (!t || !emuReady()) return;
    const bool paused = t->emulationPaused();
    if (paused && mDirty && !cbAutoApply->isChecked()) applyItem();
    t->pauseEmulation(!paused, false);
    if (paused) mResumedEmulation = true;
    updateChips();
    if (!paused) refreshSource(true);   // just paused: show the frame we stopped on
}

void UIDebugGfxEditor::onStepClicked()
{
    YabauseThread *t = thread();
    if (!t || !emuReady() || !t->emulationPaused()) return;
    if (mDirty) applyItem();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    YabauseExec();
    QApplication::restoreOverrideCursor();
    refreshSource(true);
    refreshPalette();
    mCanvas->rerender();
    updateChips();
}

void UIDebugGfxEditor::onRefreshClicked()
{
    refreshSource(true);
    refreshPalette();
    mCanvas->rerender();
    updateChips();
}

void UIDebugGfxEditor::onTick()
{
    mTickCount++;
    YabauseThread *t = thread();
    const bool running = emuReady() && t && !t->emulationPaused();
    if (mLastGen != VdpEditGeneration() || (mTickCount % 4) == 0) {
        mLastGen = VdpEditGeneration();
        updateChips();
        updateActionState();
    }
    // The game was closed (or another one started): what is open belongs to
    // memories that do not exist any more.
    if (!emuReady()) {
        if (mItem.kind != Item::None || !mSprites.empty() || !mOverview.isNull()) {
            mDirty = false;
            closeItem();
            mSprites.clear();
            mOverview = QImage();
            refreshSource(false);
        }
        return;
    }
    if (!running) return;
    // Colors may be animated by the game (fades, palette cycling).
    if (mItem.kind != Item::None) {
        refreshPalette();
        mCanvas->rerender();
    }
    // Large rotation maps (up to 4096 x 4096) are not redrawn every second.
    const bool bigMap = cbSource->currentIndex() >= SrcNBG0 && mOverviewIsMap &&
                        (qint64)mOverviewBg.mapW * mOverviewBg.mapH > 2048 * 2048;
    if (cbLive->isChecked() && (mTickCount % 4) == 0 && !bigMap)
        refreshSource(true);
}

void UIDebugGfxEditor::done(int r)
{
    if (mDirty && mItem.kind != Item::None && !cbAutoApply->isChecked() && emuReady()) {
        const QMessageBox::StandardButton b = QMessageBox::question(this, windowTitle(),
            tr_("The picture has changes that were not applied to the game. Apply them?"),
            QMessageBox::Apply | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Apply);
        if (b == QMessageBox::Cancel) return;
        if (b == QMessageBox::Apply) applyItem();
    }
    // Opened from a debugger that paused the game: leave it paused as found.
    YabauseThread *t = thread();
    if (isModal() && mResumedEmulation && t && t->emulationRunning() && !t->emulationPaused())
        t->pauseEmulation(true, false);
    mTimer->stop();
    QDialog::done(r);
}

void UIDebugGfxEditor::closeEvent(QCloseEvent *e)
{
    QDialog::closeEvent(e);
}

void UIDebugGfxEditor::keyPressEvent(QKeyEvent *e)
{
    // Enter must not close the dialog (default button behaviour).
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { e->accept(); return; }
    QDialog::keyPressEvent(e);
}

// ===========================================================================
// Sources
// ===========================================================================

void UIDebugGfxEditor::onSourceChanged(int)
{
    refreshSource(false);
    updateChips();
}

void UIDebugGfxEditor::refreshSource(bool keepSelection)
{
    const int src = cbSource->currentIndex();
    if (!emuReady()) {
        stLeft->setCurrentIndex(src == SrcSprites ? 0 : 1);
        lwSprites->clear();
        mOverviewScene->clear();
        mOverviewSel = nullptr;
        lLayerInfo->setText(tr_("Start a game to edit its graphics."));
        updateActionState();
        return;
    }
    if (src == SrcSprites) {
        stLeft->setCurrentIndex(0);
        cbRegion->setEnabled(false);
        const size_t before = mSprites.size();
        scanSprites();
        // the library only grows: redraw the list (thumbnails) when it did
        if (!keepSelection || !cbLibrary->isChecked() || mSprites.size() != before)
            rebuildSpriteList();
    } else {
        stLeft->setCurrentIndex(1);
        if (src == SrcSpriteLayer) scanSprites();
        if (!keepSelection || (src >= SrcNBG0 && mOverviewIsMap && mOverviewBg.layer != kSourceLayer[src]) ||
            (src == SrcSpriteLayer) != !mOverviewIsMap) {
            // fill the edit-area choices for this kind of layer
            QSignalBlocker blk(cbRegion);
            cbRegion->clear();
            VdpEditBg bg;
            const int rc = (src >= SrcNBG0) ? VdpEditBgGet(kSourceLayer[src], &bg) : -1;
            if (src == SrcSpriteLayer) {
                cbRegion->addItem(tr_("The sprite clicked"));
                cbRegion->setEnabled(false);
            } else if (rc != -1 && bg.bitmap) {
                cbRegion->addItem(tr_("16 x 16 dots"), 16);
                cbRegion->addItem(tr_("32 x 32 dots"), 32);
                cbRegion->addItem(tr_("64 x 64 dots"), 64);
                cbRegion->addItem(tr_("128 x 128 dots"), 128);
                cbRegion->addItem(tr_("256 x 256 dots"), 256);
                cbRegion->addItem(tr_("Whole bitmap"), 4096);
                cbRegion->setCurrentIndex(qBound(0, mRegionBitmap, cbRegion->count() - 1));
                cbRegion->setEnabled(true);
            } else {
                cbRegion->addItem(tr_("1 character"), 1);
                cbRegion->addItem(tr_("2 x 2 characters"), 2);
                cbRegion->addItem(tr_("4 x 4 characters"), 4);
                cbRegion->addItem(tr_("8 x 8 characters"), 8);
                cbRegion->addItem(tr_("16 x 16 characters"), 16);
                cbRegion->setCurrentIndex(qBound(0, mRegionCells, cbRegion->count() - 1));
                cbRegion->setEnabled(true);
            }
        }
        refreshOverview();
    }
    updateActionState();
}

void UIDebugGfxEditor::scanSprites()
{
    u8 *ram = Vdp1DebugGetRam();
    if (!ram) return;
    const bool library = cbLibrary->isChecked();
    std::map<quint64, int> index;
    if (library) {
        for (size_t i = 0; i < mSprites.size(); i++) {
            index[mSprites[i].key] = (int)i;
            mSprites[i].cmdAddr = 0xFFFFFFFF;
            mSprites[i].frameBox[2] = -1;
        }
    } else {
        mSprites.clear();
    }

    mFrameAddrs.assign(16384, 0);
    const int n = Vdp1DebugGetCommandList(mFrameAddrs.data(), (int)mFrameAddrs.size());
    mFrameAddrs.resize(n > 0 ? n : 0);

    for (size_t c = 0; c < mFrameAddrs.size(); c++) {
        SpriteEntry e;
        if (VdpEditSpriteFromCmd(ram, mFrameAddrs[c], &e.spr) != 0) continue;
        int mode = 0;
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&e.spr, &e.w, &e.h, &mode, &addr, &bytes);
        e.tex = QByteArray((const char *)ram + addr, (int)bytes);
        if (mode == 1) {
            const u32 la = VdpEditSpriteLutAddr(&e.spr);
            if (la + 32 <= 0x80000) e.lut = QByteArray((const char *)ram + la, 32);
        }
        // What the sprite looks like, not where it is drawn: the color
        // words of CMDPMOD (mode, SPD, ECD), CMDCOLR, CMDSRCA, CMDSIZE.
        quint64 k = 1469598103934665603ULL;
        const u16 key[4] = { (u16)(e.spr.CMDPMOD & 0x00F8), e.spr.CMDCOLR, e.spr.CMDSRCA, e.spr.CMDSIZE };
        k = fnv(k, key, sizeof(key));
        k = fnv(k, e.tex.constData(), e.tex.size());
        k = fnv(k, e.lut.constData(), e.lut.size());
        e.key = k;
        e.cmdAddr = mFrameAddrs[c];
        std::map<quint64, int>::iterator it = index.find(k);
        if (it != index.end()) {
            SpriteEntry &o = mSprites[it->second];
            if (o.cmdAddr == 0xFFFFFFFF) o.cmdAddr = e.cmdAddr;
            if (!library) o.uses++;
            else if (o.cmdAddr == e.cmdAddr) o.uses++;
            continue;
        }
        if (mSprites.size() >= 8192) continue;
        index[k] = (int)mSprites.size();
        mSprites.push_back(e);
    }

    // frame preview for the sprite layer
    if (cbSource->currentIndex() == SrcSpriteLayer) {
        int w = 0, h = 0;
        mFrameBoxes.assign(mFrameAddrs.size() * 4, 0);
        u32 *fb = mFrameAddrs.empty() ? NULL
            : Vdp1DebugRenderFrame(mFrameAddrs.data(), (int)mFrameAddrs.size(), &w, &h, mFrameBoxes.data());
        if (fb) {
            QImage img((uchar *)fb, w, h, QImage::Format_ARGB32);
            mOverview = img.rgbSwapped().copy();
            free(fb);
        } else {
            mOverview = QImage();
        }
        mOverviewIsMap = false;
    }
}

QImage UIDebugGfxEditor::spriteThumb(const SpriteEntry &e) const
{
    int mode = 0;
    VdpEditSpriteGeometry(&e.spr, NULL, NULL, &mode, NULL, NULL);
    std::vector<u32> codes((size_t)e.w * e.h, 0);
    QByteArray tex = e.tex;
    const int need = (mode <= 1) ? e.w * e.h / 2 : (mode <= 4) ? e.w * e.h : e.w * e.h * 2;
    if (tex.size() < need) tex.append(QByteArray(need - tex.size(), 0));
    VdpEditSpriteDecode(mode, (const u8 *)tex.constData(), e.w, e.h, codes.data());
    return renderSprite(e.spr, e.lut, e.w, e.h, codes, false);
}

void UIDebugGfxEditor::rebuildSpriteList()
{
    quint64 selKey = 0;
    const int row = lwSprites->currentRow();
    if (row >= 0 && row < (int)mShown.size()) selKey = mSprites[mShown[row]].key;

    QSignalBlocker blk(lwSprites);
    lwSprites->clear();
    mShown.clear();
    const QString f = leFilter->text().trimmed();
    int newRow = -1;
    for (size_t i = 0; i < mSprites.size(); i++) {
        const SpriteEntry &e = mSprites[i];
        int mode = 0;
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&e.spr, NULL, NULL, &mode, &addr, &bytes);
        const bool edited = VdpEditIsEdited(VDPEDIT_VDP1, addr, bytes);
        QString text = QString("%1 x %2   %3\n%4%5%6")
            .arg(e.w).arg(e.h).arg(QString::fromLatin1(spriteModeName(mode)))
            .arg(hex(addr, 5))
            .arg(e.cmdAddr != 0xFFFFFFFF ? QString("   cmd %1").arg(hex(e.cmdAddr, 5)) : tr_("   (not in this frame)"))
            .arg(edited ? tr_("   edited") : QString());
        if (!f.isEmpty() && !text.contains(f, Qt::CaseInsensitive)) continue;
        QListWidgetItem *it = new QListWidgetItem(text);
        QImage t = spriteThumb(e);
        QPixmap pm(48, 48);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.fillRect(pm.rect(), checkerBrush(4));
            const QImage s = t.scaled(48, 48, Qt::KeepAspectRatio, t.width() < 48 && t.height() < 48
                                                                   ? Qt::FastTransformation : Qt::SmoothTransformation);
            p.drawImage((48 - s.width()) / 2, (48 - s.height()) / 2, s);
        }
        it->setIcon(QIcon(pm));
        it->setToolTip(tr_("CMDPMOD %1  CMDCOLR %2  CMDSRCA %3  CMDSIZE %4  - used %5 time(s)")
                           .arg(hex(e.spr.CMDPMOD, 4)).arg(hex(e.spr.CMDCOLR, 4))
                           .arg(hex(e.spr.CMDSRCA, 4)).arg(hex(e.spr.CMDSIZE, 4)).arg(e.uses));
        if (edited) { QFont fo = it->font(); fo.setBold(true); it->setFont(fo); }
        lwSprites->addItem(it);
        if (e.key == selKey) newRow = (int)mShown.size();
        mShown.push_back((int)i);
    }
    if (newRow >= 0) lwSprites->setCurrentRow(newRow);
    lFootNote->setText(tr_("%1 sprite(s) listed").arg(mShown.size()));
    updateActionState();
}

void UIDebugGfxEditor::onFilterChanged(const QString &)
{
    rebuildSpriteList();
}

void UIDebugGfxEditor::onSpriteSelected()
{
    const int row = lwSprites->currentRow();
    if (row < 0 || row >= (int)mShown.size()) return;
    const SpriteEntry &e = mSprites[mShown[row]];
    openSprite(e.spr, &e.tex, &e.lut, QString());
}

void UIDebugGfxEditor::refreshOverview()
{
    const int src = cbSource->currentIndex();
    mOverviewScene->clear();
    mOverviewSel = nullptr;

    if (src >= SrcNBG0) {
        VdpEditBg bg;
        const int rc = VdpEditBgGet(kSourceLayer[src], &bg);
        mOverviewBg = bg;
        mOverviewIsMap = true;
        if (rc == -1) {
            lLayerInfo->setText(tr_("VDP2 not ready."));
            mOverview = QImage();
            return;
        }
        QString info = rc == -3 ? tr_("Layer is OFF (BGON). Its map is shown as the registers describe it.") + "\n" : QString();
        if (bg.bitmap)
            info += tr_("Bitmap %1 x %2, %3, at %4").arg(bg.bmW).arg(bg.bmH)
                        .arg(QString::fromLatin1(bgFormatName(bg.cn))).arg(hex(bg.bmBase, 5));
        else
            info += tr_("Cells: %1x%1 dots, %2, pattern name %3 word(s), plane %4x%5 pages, map %6 x %7")
                        .arg(bg.csize).arg(QString::fromLatin1(bgFormatName(bg.cn))).arg(bg.pdsize)
                        .arg(bg.planew).arg(bg.planeh).arg(bg.mapW).arg(bg.mapH);
        if (kSourceLayer[src] == VDPEDIT_RBG0B && Vdp2Regs && (Vdp2Regs->RPMD & 3) == 0)
            info += "\n" + tr_("RPMD = 0: the game only uses rotation parameter A.");
        lLayerInfo->setText(info);
        lFootNote->setText(bg.bitmap ? tr_("Bitmap %1 x %2").arg(bg.bmW).arg(bg.bmH)
                                     : tr_("Map %1 x %2").arg(bg.mapW).arg(bg.mapH));
        int w = 0, h = 0;
        u32 *img = VdpEditBgRender(&bg, &w, &h);
        if (!img) { mOverview = QImage(); return; }
        mOverview = QImage((const uchar *)img, w, h, QImage::Format_ARGB32).copy();
        free(img);
    } else {
        lLayerInfo->setText(tr_("The VDP1 frame: every command assembled at its coordinates. Click a sprite to edit it. "
                                "(No color calculation, mesh or user clipping.)"));
        lFootNote->setText(tr_("%1 command(s) in the frame").arg(mFrameAddrs.size()));
        mOverviewIsMap = false;
    }

    if (mOverview.isNull()) {
        lOverviewInfo->setText(tr_("Nothing to show."));
        return;
    }
    QPen border(QColor(128, 128, 128));
    border.setCosmetic(true);
    mOverviewScene->addRect(QRectF(0, 0, mOverview.width(), mOverview.height()), border, Qt::NoBrush)->setZValue(-1);
    QGraphicsPixmapItem *pix = mOverviewScene->addPixmap(QPixmap::fromImage(mOverview));
    pix->setTransformationMode(Qt::FastTransformation);
    QPen selPen(QColor(255, 0, 255));
    selPen.setCosmetic(true);
    selPen.setWidth(2);
    mOverviewSel = mOverviewScene->addRect(QRectF(), selPen, Qt::NoBrush);
    mOverviewSel->setZValue(10);
    mOverviewSel->setVisible(false);
    mOverviewScene->setSceneRect(QRectF(0, 0, mOverview.width(), mOverview.height()));
    // new picture size (other layer, other map size): fit it again;
    // otherwise keep the user's zoom and scroll position
    if (mOverview.size() != mOverviewFitSize) fitOverview();
    updateOverviewSelection();
}

void UIDebugGfxEditor::fitOverview()
{
    mOverviewAutoFit = true;
    gvOverview->resetTransform();
    if (mOverview.isNull()) return;
    mOverviewFitSize = mOverview.size();
    const QSize vp = gvOverview->viewport()->size();
    double s = std::min((vp.width() - 8.0) / mOverview.width(), (vp.height() - 8.0) / mOverview.height());
    if (s <= 0) s = 1.0;
    if (s >= 1.0) s = std::floor(s);
    gvOverview->scale(s, s);
    gvOverview->centerOn(mOverview.width() / 2.0, mOverview.height() / 2.0);
}

void UIDebugGfxEditor::updateOverviewSelection()
{
    if (!mOverviewSel) return;
    const int src = cbSource->currentIndex();
    if (src >= SrcNBG0 && (mItem.kind == Item::Cells || mItem.kind == Item::Bitmap) &&
        mItem.bg.layer == kSourceLayer[src]) {
        mOverviewSel->setRect(QRectF(mItem.originX, mItem.originY, mItem.w, mItem.h));
        mOverviewSel->setVisible(true);
        return;
    }
    if (src == SrcSpriteLayer && mItem.kind == Item::Sprite) {
        for (size_t i = 0; i < mFrameAddrs.size() && 4 * i + 3 < mFrameBoxes.size(); i++) {
            VdpEditSprite s;
            if (VdpEditSpriteFromCmd(NULL, mFrameAddrs[i], &s) != 0) continue;
            if (s.CMDSRCA == mItem.spr.CMDSRCA && s.CMDSIZE == mItem.spr.CMDSIZE && s.CMDCOLR == mItem.spr.CMDCOLR) {
                const int *b = &mFrameBoxes[4 * i];
                if (b[2] < b[0]) continue;
                mOverviewSel->setRect(QRectF(b[0], b[1], b[2] - b[0] + 1, b[3] - b[1] + 1));
                mOverviewSel->setVisible(true);
                return;
            }
        }
    }
    mOverviewSel->setVisible(false);
}

void UIDebugGfxEditor::onRegionChanged(int idx)
{
    if (cbRegion->count() > 0 && cbRegion->itemData(0).toInt() == 16) mRegionBitmap = idx;
    else if (cbRegion->count() > 1) mRegionCells = idx;
    if ((mItem.kind == Item::Cells || mItem.kind == Item::Bitmap) && cbSource->currentIndex() >= SrcNBG0) {
        const int x = mItem.originX, y = mItem.originY;
        if (mItem.kind == Item::Cells) openCells(mItem.bg, x, y);
        else openBitmap(mItem.bg, x, y);
    }
}

bool UIDebugGfxEditor::eventFilter(QObject *o, QEvent *e)
{
    // Events reach the filters while buildUi() is still creating widgets
    // (style changes): nothing to do until the dialog is complete.
    if (!mUiReady) return QDialog::eventFilter(o, e);
    if (o == mCanvasArea->viewport() && e->type() == QEvent::Wheel) {
        QWheelEvent *we = static_cast<QWheelEvent *>(e);
        if (we->modifiers() & Qt::ControlModifier) {
            onZoom(we->angleDelta().y() > 0 ? 1 : -1);
            return true;
        }
        return false;
    }
    if (o != gvOverview->viewport()) return QDialog::eventFilter(o, e);

    switch (e->type()) {
    case QEvent::Resize:
        if (!mOverview.isNull() && mOverview.size() == mOverviewFitSize) {
            // keep "fitted" while the window is resized, unless the user zoomed
            if (mOverviewAutoFit) QTimer::singleShot(0, this, [this]() { fitOverview(); });
        }
        break;
    case QEvent::Paint: {
        QPainter p(gvOverview->viewport());
        p.fillRect(static_cast<QPaintEvent *>(e)->rect(), checkerBrush(8));
        break;
    }
    case QEvent::Wheel: {
        QWheelEvent *we = static_cast<QWheelEvent *>(e);
        if (we->modifiers() & Qt::ControlModifier) {
            const double f = (we->angleDelta().y() > 0) ? 1.25 : 1.0 / 1.25;
            gvOverview->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
            gvOverview->scale(f, f);
            mOverviewAutoFit = false;
            return true;
        }
        break;
    }
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress: {
        QMouseEvent *me = static_cast<QMouseEvent *>(e);
        const QPointF sp = gvOverview->mapToScene(me->pos());
        const int x = (int)std::floor(sp.x()), y = (int)std::floor(sp.y());
        const int src = cbSource->currentIndex();
        if (mOverview.isNull() || x < 0 || y < 0 || x >= mOverview.width() || y >= mOverview.height()) {
            lOverviewInfo->setText(QString());
            break;
        }
        if (src == SrcSpriteLayer) {
            int found = -1;
            for (int i = (int)mFrameBoxes.size() / 4 - 1; i >= 0; i--) {
                const int *b = &mFrameBoxes[4 * i];
                if (b[2] >= b[0] && x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) {
                    VdpEditSprite s;
                    if (i < (int)mFrameAddrs.size() && VdpEditSpriteFromCmd(NULL, mFrameAddrs[i], &s) == 0) { found = i; break; }
                }
            }
            if (found < 0) { lOverviewInfo->setText(QString("(%1, %2)").arg(x).arg(y)); break; }
            if (e->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                VdpEditSprite s;
                VdpEditSpriteFromCmd(NULL, mFrameAddrs[found], &s);
                openSprite(s, NULL, NULL, tr_("Sprite of command #%1 at %2").arg(found).arg(hex(mFrameAddrs[found], 5)));
            } else {
                lOverviewInfo->setText(tr_("(%1, %2)  command #%3 at %4").arg(x).arg(y).arg(found).arg(hex(mFrameAddrs[found], 5)));
            }
        } else if (src >= SrcNBG0) {
            const VdpEditBg &bg = mOverviewBg;
            if (e->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                if (bg.bitmap) openBitmap(bg, x, y);
                else openCells(bg, x, y);
            } else if (bg.bitmap) {
                lOverviewInfo->setText(tr_("Dot (%1, %2) at %3").arg(x).arg(y).arg(hex(VdpEditBitmapAddr(&bg, x, y), 5)));
            } else {
                VdpEditCell c;
                if (VdpEditBgCellAt(&bg, x, y, &c) == 0)
                    lOverviewInfo->setText(tr_("Dot (%1, %2)  cell (%3, %4)\npattern name %5  character %6  palette %7%8")
                        .arg(x).arg(y).arg(x / bg.csize).arg(y / bg.csize)
                        .arg(hex(c.pnAddr, 5)).arg(hex(c.charAddr, 5)).arg(c.palBase >> 4)
                        .arg(c.flip ? tr_("  flip %1%2").arg((c.flip & 1) ? "H" : "").arg((c.flip & 2) ? "V" : "") : QString()));
            }
        }
        break;
    }
    case QEvent::Leave:
        lOverviewInfo->setText(QString());
        break;
    default:
        break;
    }
    return QDialog::eventFilter(o, e);
}

// ===========================================================================
// Opening an item
// ===========================================================================

void UIDebugGfxEditor::closeItem()
{
    mItem = Item();
    memset(&mItem.spr, 0, sizeof(mItem.spr));
    memset(&mItem.bg, 0, sizeof(mItem.bg));
    mDerived.clear();
    mDirty = false;
    mCanvas->clearPicture();
    lItemTitle->setText(tr_("Nothing open"));
    refreshPalette();
    refreshItemInfo();
}

// Asks before dropping changes that were not applied.
static bool confirmDiscard(QWidget *w, bool dirty, bool autoApply)
{
    if (!dirty || autoApply) return true;
    return QMessageBox::question(w, QtYabause::translate("Graphics Editor"),
        QtYabause::translate("The picture has changes that were not applied to the game. Discard them?"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard;
}

bool UIDebugGfxEditor::openSprite(const VdpEditSprite &spr, const QByteArray *tex, const QByteArray *lut,
                                  const QString &title)
{
    if (!confirmDiscard(this, mDirty, cbAutoApply->isChecked())) return false;
    if (mDirty) mDirty = false;

    Item it;
    it.kind = Item::Sprite;
    it.spr = spr;
    memset(&it.bg, 0, sizeof(it.bg));
    int mode = 0;
    u32 addr = 0, bytes = 0;
    VdpEditSpriteGeometry(&spr, &it.w, &it.h, &mode, &addr, &bytes);
    it.mode = mode;

    Block b;
    b.space = VDPEDIT_VDP1;
    b.addr = addr;
    b.kind = 0;
    b.w = it.w; b.h = it.h;
    if (tex && tex->size() == (int)bytes) {
        b.applied = *tex;
    } else {
        u8 *ram = Vdp1DebugGetRam();
        b.applied = ram ? QByteArray((const char *)ram + addr, (int)bytes) : QByteArray((int)bytes, 0);
    }
    b.orig = b.applied;
    {
        const int need = (mode <= 1) ? it.w * it.h / 2 : (mode <= 4) ? it.w * it.h : it.w * it.h * 2;
        QByteArray full = b.applied;
        if (full.size() < need) full.append(QByteArray(need - full.size(), 0));   // wraps past the VRAM end
        b.codes.assign((size_t)it.w * it.h, 0);
        VdpEditSpriteDecode(mode, (const u8 *)full.constData(), it.w, it.h, b.codes.data());
    }
    it.blocks.push_back(b);
    if (mode == 1) {
        if (lut && lut->size() == 32) it.lut = *lut;
        else {
            u8 *ram = Vdp1DebugGetRam();
            const u32 la = VdpEditSpriteLutAddr(&spr);
            if (ram && la + 32 <= 0x80000) it.lut = QByteArray((const char *)ram + la, 32);
        }
        it.lutApplied = it.lut;
    }
    it.pixBlock.assign((size_t)it.w * it.h, 0);
    it.pixOff.resize((size_t)it.w * it.h);
    for (size_t i = 0; i < it.pixOff.size(); i++) it.pixOff[i] = (int)i;
    it.title = title.isEmpty() ? tr_("Sprite %1 x %2 at %3 (%4)").arg(it.w).arg(it.h).arg(hex(addr, 5))
                                    .arg(QString::fromLatin1(spriteModeName(mode)))
                               : title + QString("  -  %1 x %2, %3").arg(it.w).arg(it.h).arg(QString::fromLatin1(spriteModeName(mode)));
    mItem = it;
    finishOpen();
    return true;
}

bool UIDebugGfxEditor::openCells(const VdpEditBg &bg, int x, int y)
{
    if (!confirmDiscard(this, mDirty, cbAutoApply->isChecked())) return false;
    mDirty = false;
    const int r = qMax(1, cbRegion->currentData().toInt());
    const int span = r * bg.csize;
    const int x0 = (x / span) * span, y0 = (y / span) * span;
    const int w = qMin(span, bg.mapW - x0), h = qMin(span, bg.mapH - y0);
    if (w <= 0 || h <= 0) return false;

    Item it;
    it.kind = Item::Cells;
    it.bg = bg;
    memset(&it.spr, 0, sizeof(it.spr));
    it.originX = x0; it.originY = y0;
    it.w = w; it.h = h;
    it.cellsX = w / bg.csize;
    const int cellsY = h / bg.csize;
    it.pixBlock.assign((size_t)w * h, 0);
    it.pixOff.assign((size_t)w * h, 0);
    std::map<u32, int> byChar;
    const int cs = bg.csize;
    for (int cy = 0; cy < cellsY; cy++)
        for (int cx = 0; cx < it.cellsX; cx++) {
            VdpEditCell c;
            if (VdpEditBgCellAt(&bg, x0 + cx * cs, y0 + cy * cs, &c) != 0) { it.cellPal.push_back(0); continue; }
            it.cellPal.push_back(c.palBase);
            int bi;
            std::map<u32, int>::iterator f = byChar.find(c.charAddr);
            if (f == byChar.end()) {
                Block b;
                b.space = VDPEDIT_VDP2;
                b.addr = c.charAddr;
                b.kind = 1;
                b.w = b.h = cs;
                b.applied = QByteArray((int)c.charBytes, 0);
                VdpEditReadVdp2(c.charAddr, (u8 *)b.applied.data(), c.charBytes);
                b.orig = b.applied;
                b.codes.assign((size_t)cs * cs, 0);
                VdpEditCharDecode(&bg, (const u8 *)b.applied.constData(), b.codes.data());
                bi = (int)it.blocks.size();
                it.blocks.push_back(b);
                byChar[c.charAddr] = bi;
            } else {
                bi = f->second;
            }
            for (int j = 0; j < cs; j++)
                for (int i = 0; i < cs; i++) {
                    const int ix = (c.flip & 1) ? cs - 1 - i : i;
                    const int iy = (c.flip & 2) ? cs - 1 - j : j;
                    const size_t p = (size_t)(cy * cs + j) * w + cx * cs + i;
                    it.pixBlock[p] = bi;
                    it.pixOff[p] = iy * cs + ix;
                }
        }
    static const char *names[VDPEDIT_LAYERS] = { "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG0 (B)", "RBG1" };
    it.title = tr_("%1 cells (%2, %3) - %4 x %5 dots, %6 character(s)")
                   .arg(QString::fromLatin1(names[bg.layer])).arg(x0 / cs).arg(y0 / cs).arg(w).arg(h).arg(it.blocks.size());
    mItem = it;
    finishOpen();
    return true;
}

bool UIDebugGfxEditor::openBitmap(const VdpEditBg &bg, int x, int y)
{
    if (!confirmDiscard(this, mDirty, cbAutoApply->isChecked())) return false;
    mDirty = false;
    const int span = qMax(16, cbRegion->currentData().toInt());
    int x0 = 0, y0 = 0, w = bg.bmW, h = bg.bmH;
    if (span < bg.bmW || span < bg.bmH) {
        x0 = (x / span) * span; y0 = (y / span) * span;
        w = qMin(span, bg.bmW - x0); h = qMin(span, bg.bmH - y0);
    }
    if (w <= 0 || h <= 0) return false;
    Item it;
    it.kind = Item::Bitmap;
    it.bg = bg;
    memset(&it.spr, 0, sizeof(it.spr));
    it.originX = x0; it.originY = y0;
    it.w = w; it.h = h;
    it.pixBlock.assign((size_t)w * h, 0);
    it.pixOff.assign((size_t)w * h, 0);
    for (int j = 0; j < h; j++) {
        Block b;
        b.space = VDPEDIT_VDP2;
        b.addr = VdpEditBitmapAddr(&bg, x0, y0 + j);
        b.kind = 2;
        b.w = w; b.h = 1;
        const u32 n = VdpEditBitmapRowBytes(&bg, w);
        b.applied = QByteArray((int)n, 0);
        VdpEditReadVdp2(b.addr, (u8 *)b.applied.data(), n);
        b.orig = b.applied;
        b.codes.assign((size_t)w, 0);
        VdpEditBitmapDecode(&bg, (const u8 *)b.applied.constData(), w, b.codes.data());
        it.blocks.push_back(b);
        for (int i = 0; i < w; i++) {
            it.pixBlock[(size_t)j * w + i] = j;
            it.pixOff[(size_t)j * w + i] = i;
        }
    }
    static const char *names[VDPEDIT_LAYERS] = { "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG0 (B)", "RBG1" };
    it.title = tr_("%1 bitmap (%2, %3) - %4 x %5 dots")
                   .arg(QString::fromLatin1(names[bg.layer])).arg(x0).arg(y0).arg(w).arg(h);
    mItem = it;
    finishOpen();
    return true;
}

void UIDebugGfxEditor::finishOpen()
{
    lItemTitle->setText(mItem.title);
    deriveCanvasCodes(mDerived);
    mDirty = false;
    // fit the zoom to the area
    const QSize area = mCanvasArea->viewport()->size();
    int z = qMin((area.width() - 8) / qMax(1, mItem.w), (area.height() - 8) / qMax(1, mItem.h));
    z = qBound(1, z, 32);
    mCanvas->setZoom(z);
    lZoom->setText(QString("%1x").arg(z));
    mCanvas->setCellGrid(cbCellGrid->isChecked() ? (mItem.kind == Item::Cells ? mItem.bg.csize : 8) : 0);
    mCanvas->setTransparentCode(transparentCode());
    mCanvas->setPicture(mItem.w, mItem.h, mDerived);

    // colors: keep codes that still make sense
    const int n = paletteSize();
    if (itemIsRgb()) {
        setPrimary(rgbToCode(qRgb(255, 255, 255)));
        setSecondary(transparentCode());
    } else {
        if ((int)mPrimaryCode >= n) setPrimary(n > 1 ? 1 : 0);
        if ((int)mSecondaryCode >= n) setSecondary(0);
    }
    refreshPalette();
    refreshItemInfo();
    updateOverviewSelection();
    lApplyStatus->setText(QString());
    lHover->setText(tr_("Left button: primary color, right button: secondary. Ctrl+wheel zooms."));
    updateActionState();
}

void UIDebugGfxEditor::deriveCanvasCodes(std::vector<u32> &out) const
{
    out.assign(mItem.pixBlock.size(), 0);
    for (size_t p = 0; p < out.size(); p++) {
        const Block &b = mItem.blocks[mItem.pixBlock[p]];
        const int o = mItem.pixOff[p];
        out[p] = (o >= 0 && o < (int)b.codes.size()) ? b.codes[o] : 0;
    }
}

// Copies the dots the user changed into the blocks, then rebuilds the canvas
// from the blocks: a character used twice in the area changes in both places.
void UIDebugGfxEditor::storeCanvasCodes()
{
    const std::vector<u32> &cur = mCanvas->codes();
    if (cur.size() != mDerived.size()) return;
    bool any = false;
    for (size_t p = 0; p < cur.size(); p++) {
        if (cur[p] == mDerived[p]) continue;
        Block &b = mItem.blocks[mItem.pixBlock[p]];
        b.codes[mItem.pixOff[p]] = cur[p];
        any = true;
    }
    if (!any) {
        // undo/redo to an older state: re-derive the blocks from the canvas
        for (size_t p = 0; p < cur.size(); p++)
            mItem.blocks[mItem.pixBlock[p]].codes[mItem.pixOff[p]] = cur[p];
    }
    deriveCanvasCodes(mDerived);
    if (mDerived != cur) mCanvas->replaceCodes(mDerived, false);
}

void UIDebugGfxEditor::encodeBlock(const Block &b, QByteArray &out) const
{
    out = b.applied;
    switch (b.kind) {
    case 0: {
        const int need = (mItem.mode <= 1) ? b.w * b.h / 2 : (mItem.mode <= 4) ? b.w * b.h : b.w * b.h * 2;
        QByteArray full = out;
        if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
        VdpEditSpriteEncode(mItem.mode, b.codes.data(), b.w, b.h, (u8 *)full.data());
        out = full.left(b.applied.size());
        break;
    }
    case 1:
        VdpEditCharEncode(&mItem.bg, b.codes.data(), (u8 *)out.data());
        break;
    default:
        VdpEditBitmapEncode(&mItem.bg, b.codes.data(), b.w, (u8 *)out.data());
        break;
    }
}

int UIDebugGfxEditor::applyItem()
{
    if (mItem.kind == Item::None || !emuReady()) return -1;
    int now = 0, later = 0, failed = 0;
    for (size_t i = 0; i < mItem.blocks.size(); i++) {
        Block &b = mItem.blocks[i];
        QByteArray bytes;
        encodeBlock(b, bytes);
        if (bytes == b.applied) continue;
        const int rc = VdpEditWrite(b.space, b.addr, (const u8 *)b.applied.constData(),
                                    (const u8 *)bytes.constData(), (u32)bytes.size());
        if (rc < 0) { failed++; continue; }
        if (rc == 0) later++; else now++;
        b.applied = bytes;
    }
    mDirty = false;
    QString s;
    if (now + later + failed == 0) s = tr_("Nothing changed.");
    else {
        s = tr_("%1 block(s) written to VRAM").arg(now);
        if (later) s += ", " + tr_("%1 waiting for the game to load the original graphic again").arg(later);
        if (failed) s += ", " + tr_("%1 failed").arg(failed);
        s += ".";
        YabauseThread *t = thread();
        if (t && t->emulationPaused())
            s += " " + tr_("Run or step a frame to see it in the game.");
    }
    lApplyStatus->setText(s);
    refreshItemInfo();
    updateChips();
    updateActionState();
    if (cbSource->currentIndex() == SrcSprites) rebuildSpriteList();
    return now;
}

void UIDebugGfxEditor::onCanvasEdited()
{
    if (mItem.kind == Item::None) return;
    storeCanvasCodes();
    mDirty = true;
    if (cbAutoApply->isChecked() && emuReady()) applyItem();
    else lApplyStatus->setText(tr_("Not applied yet (Ctrl+Enter)."));
}

void UIDebugGfxEditor::onApplyClicked()
{
    if (mItem.kind == Item::None) return;
    storeCanvasCodes();
    applyItem();
}

void UIDebugGfxEditor::onRevertItemClicked()
{
    if (mItem.kind == Item::None || !emuReady()) return;
    int n = 0;
    for (size_t i = 0; i < mItem.blocks.size(); i++)
        n += VdpEditRevertRange(mItem.blocks[i].space, mItem.blocks[i].addr, (u32)mItem.blocks[i].applied.size());
    if (mItem.kind == Item::Sprite && mItem.mode == 1 && mItem.lut.size() == 32)
        n += VdpEditRevertRange(VDPEDIT_VDP1, VdpEditSpriteLutAddr(&mItem.spr), 32);
    lApplyStatus->setText(tr_("%1 edit(s) reverted.").arg(n));
    reloadItemFromMemory();
    if (cbSource->currentIndex() == SrcSprites) refreshSource(true);
    else refreshOverview();
}

// Reads the blocks again (after a revert). A sprite of the library that is
// not in VRAM any more gets its original bytes back from the edit list: here
// we simply take what the memory or the list entry holds.
void UIDebugGfxEditor::reloadItemFromMemory()
{
    if (mItem.kind == Item::None) return;
    for (size_t i = 0; i < mItem.blocks.size(); i++) {
        Block &b = mItem.blocks[i];
        const u32 n = (u32)b.applied.size();
        QByteArray now(n, 0);
        if (b.space == VDPEDIT_VDP1) {
            u8 *ram = Vdp1DebugGetRam();
            if (ram) memcpy(now.data(), ram + b.addr, n);
        } else {
            VdpEditReadVdp2(b.addr, (u8 *)now.data(), n);
        }
        // the original of an item opened from the library may not be in VRAM
        // right now: fall back to the bytes it was opened with
        if (b.space == VDPEDIT_VDP1 && now != b.orig && !VdpEditIsEdited(VDPEDIT_VDP1, b.addr, n)) now = b.orig;
        b.applied = now;
        b.orig = now;
        if (b.kind == 0) {
            const int need = (mItem.mode <= 1) ? b.w * b.h / 2 : (mItem.mode <= 4) ? b.w * b.h : b.w * b.h * 2;
            QByteArray full = now;
            if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
            VdpEditSpriteDecode(mItem.mode, (const u8 *)full.constData(), b.w, b.h, b.codes.data());
        } else if (b.kind == 1) {
            VdpEditCharDecode(&mItem.bg, (const u8 *)now.constData(), b.codes.data());
        } else {
            VdpEditBitmapDecode(&mItem.bg, (const u8 *)now.constData(), b.w, b.codes.data());
        }
    }
    if (mItem.kind == Item::Sprite && mItem.mode == 1) {
        u8 *ram = Vdp1DebugGetRam();
        const u32 la = VdpEditSpriteLutAddr(&mItem.spr);
        if (ram && la + 32 <= 0x80000) mItem.lut = mItem.lutApplied = QByteArray((const char *)ram + la, 32);
    }
    deriveCanvasCodes(mDerived);
    mCanvas->setPicture(mItem.w, mItem.h, mDerived);
    mDirty = false;
    refreshPalette();
    refreshItemInfo();
    updateChips();
    updateActionState();
}

void UIDebugGfxEditor::onRevertAllClicked()
{
    if (QMessageBox::question(this, windowTitle(), tr_("Put every original graphic and color back and forget all %1 edit(s)?")
                                  .arg(VdpEditCount(-1)), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    VdpEditRevertAll();
    if (mItem.kind != Item::None) reloadItemFromMemory();
    refreshSource(true);
    updateChips();
    updateActionState();
}

// ===========================================================================
// Colors
// ===========================================================================

bool UIDebugGfxEditor::itemIsRgb() const
{
    if (mItem.kind == Item::Sprite) return mItem.mode == 5;
    if (mItem.kind == Item::Cells || mItem.kind == Item::Bitmap) return mItem.bg.cn >= 3;
    return false;
}

u32 UIDebugGfxEditor::transparentCode() const
{
    // 0 in every format: palette code 0, or an RGB word / long with MSB = 0
    return 0;
}

u32 UIDebugGfxEditor::rgbToCode(QRgb c) const
{
    if (mItem.kind == Item::Sprite) return VdpEditArgbToRgb555((u32)c);
    if (mItem.bg.cn == 3) return VdpEditArgbToRgb555((u32)c);
    return 0x80000000u | ((u32)qBlue(c) << 16) | ((u32)qGreen(c) << 8) | (u32)qRed(c);
}

int UIDebugGfxEditor::paletteSize() const
{
    if (mItem.kind == Item::Sprite) return VdpEditSpritePaletteSize(&mItem.spr);
    if (mItem.kind == Item::Cells || mItem.kind == Item::Bitmap) return VdpEditBgPaletteSize(mItem.bg.cn);
    return 0;
}

QRgb UIDebugGfxEditor::paletteColor(u32 code) const
{
    if (mItem.kind == Item::Sprite) {
        if (mItem.mode == 5) return VdpEditSpriteDataArgb(code);
        const u8 *l = mItem.lut.size() >= 32 ? (const u8 *)mItem.lut.constData() : NULL;
        return VdpEditSpriteDataArgb(VdpEditSpriteCodeData(&mItem.spr, l, code));
    }
    if (mItem.kind == Item::Cells || mItem.kind == Item::Bitmap) {
        const u32 pal = (mItem.kind == Item::Bitmap) ? mItem.bg.bmPal
            : (mItem.activeCell < (int)mItem.cellPal.size() ? mItem.cellPal[mItem.activeCell] : 0);
        if (mItem.bg.cn >= 3) {
            VdpEditBg b = mItem.bg;
            b.tpon = 1;
            return VdpEditBgCodeArgb(&b, pal, code);
        }
        return VdpEditCramArgb((u32)VdpEditBgCodeCramIndex(&mItem.bg, pal, code));
    }
    return 0;
}

int UIDebugGfxEditor::paletteCram(u32 code, bool *lutRgb) const
{
    if (lutRgb) *lutRgb = false;
    if (mItem.kind == Item::Sprite) {
        if (mItem.mode == 5) return -1;
        const u8 *l = mItem.lut.size() >= 32 ? (const u8 *)mItem.lut.constData() : NULL;
        const int idx = VdpEditSpriteDataCram(VdpEditSpriteCodeData(&mItem.spr, l, code));
        if (idx < 0 && mItem.mode == 1 && lutRgb) *lutRgb = true;
        return idx;
    }
    if (mItem.kind == Item::Cells || mItem.kind == Item::Bitmap) {
        const u32 pal = (mItem.kind == Item::Bitmap) ? mItem.bg.bmPal
            : (mItem.activeCell < (int)mItem.cellPal.size() ? mItem.cellPal[mItem.activeCell] : 0);
        return VdpEditBgCodeCramIndex(&mItem.bg, pal, code);
    }
    return -1;
}

void UIDebugGfxEditor::refreshPalette()
{
    QVector<QRgb> colors;
    QVector<u32> codes;
    QVector<QString> tips;
    QString info;
    int columns = 16;

    if (mItem.kind == Item::None) {
        mPalette->setEntries(colors, codes, tips, columns);
        lPaletteInfo->setText(tr_("Open a sprite or a part of a background."));
        updateSwatches();
        return;
    }

    if (itemIsRgb()) {
        // direct colors: the colors used in the picture, transparent first
        std::map<u32, int> used;
        const std::vector<u32> &c = mCanvas->codes();
        for (size_t i = 0; i < c.size() && used.size() < 256; i++) used[c[i]]++;
        codes << transparentCode();
        for (std::map<u32, int>::const_iterator it = used.begin(); it != used.end(); ++it)
            if (it->first != transparentCode()) codes << it->first;
        if (!codes.contains(mPrimaryCode)) codes << mPrimaryCode;
        for (int i = 0; i < codes.size(); i++) {
            const QRgb col = (i == 0) ? qRgba(0, 0, 0, 0) : paletteColor(codes[i]);
            colors << col;
            tips << (i == 0 ? tr_("Transparent (MSB = 0)") : tr_("RGB %1, %2, %3  (code %4)")
                         .arg(qRed(col) >> 3).arg(qGreen(col) >> 3).arg(qBlue(col) >> 3)
                         .arg(hex(codes[i], mItem.kind == Item::Sprite || mItem.bg.cn == 3 ? 4 : 8)));
        }
        columns = 8;
        info = tr_("Direct color format: \"Any color...\" picks a new color. The swatches are the colors used in the picture.");
    } else {
        const int n = paletteSize();
        columns = (n <= 64) ? 8 : (n <= 256) ? 16 : 32;
        bool anyLutRgb = false;
        for (int k = 0; k < n; k++) {
            bool lutRgb = false;
            const int cram = paletteCram((u32)k, &lutRgb);
            anyLutRgb |= lutRgb;
            QRgb col = paletteColor((u32)k);
            bool transparent = false;
            if (mItem.kind == Item::Sprite) {
                const int kind = VdpEditSpriteCodeKind(&mItem.spr, (u32)k);
                if (kind == 1) transparent = true;
                if (kind == 2) col = qRgba(255, 0, 255, 150);
                QString t = tr_("Code %1").arg(k);
                if (kind == 1) t += " - " + tr_("transparent");
                if (kind == 2) t += " - " + tr_("end code: ends the line after the second one (do not paint with it)");
                if (cram >= 0) t += "\n" + tr_("Color RAM %1 = RGB %2, %3, %4").arg(hex((u32)cram, 3))
                                            .arg(qRed(col) >> 3).arg(qGreen(col) >> 3).arg(qBlue(col) >> 3);
                else if (lutRgb) t += "\n" + tr_("RGB color stored in the lookup table");
                tips << t;
            } else {
                transparent = (k == 0 && !mItem.bg.tpon);
                QString t = tr_("Code %1").arg(k);
                if (transparent) t += " - " + tr_("transparent");
                t += "\n" + tr_("Color RAM %1 = RGB %2, %3, %4").arg(hex((u32)cram, 3))
                                .arg(qRed(col) >> 3).arg(qGreen(col) >> 3).arg(qBlue(col) >> 3);
                tips << t;
            }
            colors << (transparent ? (col & 0x00FFFFFF) | 0x50000000u : col);
            codes << (u32)k;
        }
        if (mItem.kind == Item::Sprite) {
            if (mItem.mode == 1)
                info = tr_("Lookup table at %1 in VDP1 VRAM.").arg(hex(VdpEditSpriteLutAddr(&mItem.spr), 5));
            else
                info = tr_("Color bank %1 (CMDCOLR), sprite type %2, color RAM offset %3.")
                           .arg(hex(mItem.spr.CMDCOLR, 4)).arg(Vdp2Regs ? (Vdp2Regs->SPCTL & 0xF) : 0)
                           .arg(Vdp2Regs ? hex((u32)((Vdp2Regs->CRAOFB >> 4) & 7) << 8, 3) : QString("-"));
            if (anyLutRgb) info += " " + tr_("Some entries are RGB colors stored in the table.");
        } else {
            const u32 pal = (mItem.kind == Item::Bitmap) ? mItem.bg.bmPal
                : (mItem.activeCell < (int)mItem.cellPal.size() ? mItem.cellPal[mItem.activeCell] : 0);
            info = tr_("Palette %1 (color RAM from %2).").arg(pal >> 4)
                       .arg(hex((u32)VdpEditBgCodeCramIndex(&mItem.bg, pal, 0), 3));
            if (mItem.kind == Item::Cells) {
                bool several = false;
                for (size_t i = 1; i < mItem.cellPal.size(); i++) if (mItem.cellPal[i] != mItem.cellPal[0]) several = true;
                if (several) info += " " + tr_("The area uses several palettes: the one shown is the palette of the last cell you clicked or hovered.");
            }
        }
        info += " " + tr_("Double-click a color to change it (shared by everything that uses it).");
    }
    mPalCodes = codes;
    mPalette->setEntries(colors, codes, tips, columns);
    mPalette->setSelection(mPrimaryCode, mSecondaryCode);
    lPaletteInfo->setText(info);
    updateSwatches();
}

void UIDebugGfxEditor::updateSwatches()
{
    auto css = [this](u32 code) {
        if (mItem.kind == Item::None) return QString("background: transparent;");
        const QRgb c = paletteColor(code);
        const bool transp = (code == transparentCode()) &&
            (mItem.kind == Item::Sprite ? VdpEditSpriteCodeKind(&mItem.spr, code) == 1 : !mItem.bg.tpon);
        if (transp) return QString("background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ccc, stop:0.49 #ccc, stop:0.51 #999, stop:1 #999);");
        return QString("background: %1;").arg(QColor(c).name());
    };
    lPrimary->setStyleSheet(css(mPrimaryCode) + "border: 2px solid palette(highlight); border-radius: 4px;");
    lSecondary->setStyleSheet(css(mSecondaryCode) + "border: 1px solid palette(mid); border-radius: 4px;");
    lPrimary->setText(QString());
    lSecondary->setText(QString());
    mPalette->setSelection(mPrimaryCode, mSecondaryCode);
}

void UIDebugGfxEditor::setPrimary(u32 c)
{
    mPrimaryCode = c;
    mCanvas->setCodes(mPrimaryCode, mSecondaryCode);
    updateSwatches();
}

void UIDebugGfxEditor::setSecondary(u32 c)
{
    mSecondaryCode = c;
    mCanvas->setCodes(mPrimaryCode, mSecondaryCode);
    updateSwatches();
}

void UIDebugGfxEditor::onPaletteChosen(u32 code, bool primary)
{
    if (primary) setPrimary(code); else setSecondary(code);
}

void UIDebugGfxEditor::onPicked(u32 code, bool primary)
{
    if (primary) setPrimary(code); else setSecondary(code);
    if (itemIsRgb()) refreshPalette();
}

void UIDebugGfxEditor::onCustomColor()
{
    if (!itemIsRgb()) return;
    const QColor c = QColorDialog::getColor(QColor(paletteColor(mPrimaryCode)), this, tr_("Primary color"));
    if (!c.isValid()) return;
    setPrimary(rgbToCode(c.rgb()));
    refreshPalette();
}

void UIDebugGfxEditor::onPaletteEdit(int entry)
{
    if (mItem.kind == Item::None || entry < 0 || entry >= mPalCodes.size() || !emuReady()) return;
    const u32 code = mPalCodes[entry];
    if (itemIsRgb()) {
        // direct colors: replace every dot of this color in the picture
        const QColor c = QColorDialog::getColor(QColor(paletteColor(code)), this, tr_("Replace color"));
        if (!c.isValid()) return;
        const u32 nc = rgbToCode(c.rgb());
        std::vector<u32> codes = mCanvas->codes();
        for (size_t i = 0; i < codes.size(); i++) if (codes[i] == code) codes[i] = nc;
        mCanvas->replaceCodes(codes, true);
        onCanvasEdited();
        refreshPalette();
        return;
    }
    bool lutRgb = false;
    const int cram = paletteCram(code, &lutRgb);
    const QColor cur(paletteColor(code));
    const QColor c = QColorDialog::getColor(cur, this, tr_("Color of code %1").arg(code));
    if (!c.isValid()) return;
    if (lutRgb && mItem.lut.size() == 32) {
        QByteArray nl = mItem.lut;
        const u16 v = VdpEditArgbToRgb555(c.rgb());
        nl[(int)code * 2] = (char)(v >> 8);
        nl[(int)code * 2 + 1] = (char)v;
        const int rc = VdpEditWrite(VDPEDIT_VDP1, VdpEditSpriteLutAddr(&mItem.spr), (const u8 *)mItem.lutApplied.constData(),
                                    (const u8 *)nl.constData(), 32);
        if (rc >= 0) { mItem.lut = mItem.lutApplied = nl; }
    } else if (cram >= 0) {
        if (VdpEditSetCramColor((u32)cram, c.red(), c.green(), c.blue()) != 0)
            QMessageBox::warning(this, windowTitle(), tr_("Could not change the color."));
    } else {
        QMessageBox::information(this, windowTitle(), tr_("This entry is not read from color RAM."));
        return;
    }
    refreshPalette();
    mCanvas->rerender();
    refreshItemInfo();
    updateChips();
    lApplyStatus->setText(tr_("Color changed. It is shared by every sprite and background that uses it."));
}

// ===========================================================================
// Rendering
// ===========================================================================

QImage UIDebugGfxEditor::renderCodes(const std::vector<u32> &codes) const
{
    if (mItem.kind == Item::None || mItem.w <= 0) return QImage();
    if (mItem.kind == Item::Sprite)
        return renderSprite(mItem.spr, mItem.lut, mItem.w, mItem.h, codes, true);
    QImage img(mItem.w, mItem.h, QImage::Format_ARGB32);
    for (int y = 0; y < mItem.h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < mItem.w; x++) {
            const int dot = y * mItem.w + x;
            line[x] = codeColor(dot, codes[(size_t)dot]);
        }
    }
    return img;
}

QRgb UIDebugGfxEditor::codeColor(int dot, u32 code) const
{
    if (mItem.kind == Item::Bitmap) return VdpEditBgCodeArgb(&mItem.bg, mItem.bg.bmPal, code);
    if (mItem.kind == Item::Cells) {
        const int cs = mItem.bg.csize;
        const int x = dot % mItem.w, y = dot / mItem.w;
        const int cell = (y / cs) * mItem.cellsX + x / cs;
        const u32 pal = cell < (int)mItem.cellPal.size() ? mItem.cellPal[cell] : 0;
        return VdpEditBgCodeArgb(&mItem.bg, pal, code);
    }
    return 0;
}

u32 UIDebugGfxEditor::nearestCode(int dot, QRgb c) const
{
    if (itemIsRgb()) return qAlpha(c) < 128 ? transparentCode() : rgbToCode(c);
    const int n = paletteSize();
    u32 pal = 0;
    if (mItem.kind == Item::Cells) {
        const int cs = mItem.bg.csize, x = dot % mItem.w, y = dot / mItem.w;
        const int cell = (y / cs) * mItem.cellsX + x / cs;
        pal = cell < (int)mItem.cellPal.size() ? mItem.cellPal[cell] : 0;
    } else if (mItem.kind == Item::Bitmap) {
        pal = mItem.bg.bmPal;
    }
    bool hasTransparent = true;
    int first = 0;
    if (mItem.kind == Item::Sprite) {
        hasTransparent = VdpEditSpriteCodeKind(&mItem.spr, 0) == 1;
    } else {
        hasTransparent = !mItem.bg.tpon;
    }
    if (qAlpha(c) < 128 && hasTransparent) return 0;
    if (hasTransparent) first = 1;
    long bestD = 0x7FFFFFFF;
    u32 best = (u32)first;
    for (int k = first; k < n; k++) {
        QRgb pc;
        if (mItem.kind == Item::Sprite) {
            if (VdpEditSpriteCodeKind(&mItem.spr, (u32)k) == 2) continue;   // never the end code
            pc = paletteColor((u32)k);
        } else {
            pc = VdpEditCramArgb((u32)VdpEditBgCodeCramIndex(&mItem.bg, pal, (u32)k));
        }
        const long dr = qRed(pc) - qRed(c), dg = qGreen(pc) - qGreen(c), db = qBlue(pc) - qBlue(c);
        const long d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = (u32)k; if (d == 0) break; }
    }
    return best;
}

// ===========================================================================
// Hover / info
// ===========================================================================

void UIDebugGfxEditor::onCanvasHover(int x, int y)
{
    if (mItem.kind == Item::None || x < 0) {
        if (mItem.kind != Item::None)
            lHover->setText(tr_("Left button: primary color, right button: secondary. Ctrl+wheel zooms."));
        return;
    }
    const int dot = y * mItem.w + x;
    const u32 code = mCanvas->codes()[(size_t)dot];
    const QRgb c = mCanvas->image().pixel(x, y);
    QString t = tr_("Dot (%1, %2)").arg(x).arg(y);
    const bool wide = itemIsRgb();
    t += "   " + tr_("code %1").arg(hex(code, wide ? (mItem.kind == Item::Sprite || mItem.bg.cn == 3 ? 4 : 8) : 2));
    if (mItem.kind == Item::Sprite) {
        const int kind = VdpEditSpriteCodeKind(&mItem.spr, code);
        if (kind == 1) t += "  " + tr_("(transparent)");
        else if (kind == 2) t += "  " + tr_("(end code)");
        else if (qAlpha(c) < 255) t += "  " + tr_("(after the second end code of the line: not drawn)");
        if (kind == 0) {
            const int cram = paletteCram(code, NULL);
            if (cram >= 0) t += "  " + tr_("color RAM %1").arg(hex((u32)cram, 3));
        }
    } else {
        const int cs = mItem.bg.csize;
        if (mItem.kind == Item::Cells) {
            const int cell = (y / cs) * mItem.cellsX + x / cs;
            const Block &b = mItem.blocks[mItem.pixBlock[(size_t)dot]];
            t += "  " + tr_("character %1").arg(hex(b.addr, 5));
            if (cell != mItem.activeCell && cell < (int)mItem.cellPal.size() &&
                mItem.cellPal[cell] != mItem.cellPal[mItem.activeCell]) {
                mItem.activeCell = cell;
                refreshPalette();
            } else {
                mItem.activeCell = cell;
            }
        }
        const int cram = paletteCram(code, NULL);
        if (cram >= 0) t += "  " + tr_("color RAM %1").arg(hex((u32)cram, 3));
    }
    if (qAlpha(c) > 0) t += "  " + tr_("RGB %1, %2, %3").arg(qRed(c) >> 3).arg(qGreen(c) >> 3).arg(qBlue(c) >> 3);
    lHover->setText(t);
}

void UIDebugGfxEditor::refreshItemInfo()
{
    twInfo->clear();
    if (mItem.kind == Item::None) return;
    QTreeWidgetItem *g = makeGroup(twInfo, tr_("Item"));
    int changed = 0, edited = 0;
    for (size_t i = 0; i < mItem.blocks.size(); i++) {
        const Block &b = mItem.blocks[i];
        if (b.applied != b.orig) changed++;
        if (VdpEditIsEdited(b.space, b.addr, (u32)b.applied.size())) edited++;
    }
    if (mItem.kind == Item::Sprite) {
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&mItem.spr, NULL, NULL, NULL, &addr, &bytes);
        makeRow(g, tr_("Size"), QString("%1 x %2").arg(mItem.w).arg(mItem.h));
        makeRow(g, tr_("Color mode"), QString::fromLatin1(spriteModeName(mItem.mode)));
        makeRow(g, tr_("Texture"), QString("%1 (%2 bytes)").arg(hex(addr, 5)).arg(bytes));
        makeRow(g, "CMDPMOD", hex(mItem.spr.CMDPMOD, 4));
        makeRow(g, "CMDCOLR", hex(mItem.spr.CMDCOLR, 4));
        makeRow(g, "SPD", (mItem.spr.CMDPMOD & 0x40) ? tr_("1 (code 0 opaque)") : tr_("0 (code 0 transparent)"));
        makeRow(g, "ECD", (mItem.spr.CMDPMOD & 0x80) ? tr_("1 (end codes off)") : tr_("0 (end codes on)"));
    } else {
        static const char *names[VDPEDIT_LAYERS] = { "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG0 (B)", "RBG1" };
        makeRow(g, tr_("Layer"), QString::fromLatin1(names[mItem.bg.layer]));
        makeRow(g, tr_("Area"), QString("(%1, %2) %3 x %4").arg(mItem.originX).arg(mItem.originY).arg(mItem.w).arg(mItem.h));
        makeRow(g, tr_("Color format"), QString::fromLatin1(bgFormatName(mItem.bg.cn)));
        makeRow(g, tr_("Transparent code"), mItem.bg.tpon ? tr_("off (TPON)") : tr_("0"));
        if (mItem.kind == Item::Cells) {
            makeRow(g, tr_("Character size"), QString("%1 x %1").arg(mItem.bg.csize));
            makeRow(g, tr_("Characters"), QString::number(mItem.blocks.size()));
            makeRow(g, tr_("Pattern name"), tr_("%1 word(s)").arg(mItem.bg.pdsize));
        } else {
            makeRow(g, tr_("Bitmap"), QString("%1 x %2 at %3").arg(mItem.bg.bmW).arg(mItem.bg.bmH).arg(hex(mItem.bg.bmBase, 5)));
        }
        makeRow(g, tr_("Color RAM offset"), hex(mItem.bg.coff, 3));
    }
    QTreeWidgetItem *e = makeGroup(twInfo, tr_("Edit"));
    makeRow(e, tr_("Blocks"), QString::number(mItem.blocks.size()));
    makeRow(e, tr_("Changed in this session"), QString::number(changed));
    makeRow(e, tr_("With an active edit"), QString::number(edited));
    makeRow(e, tr_("Edits in total"), QString::number(VdpEditCount(-1)));
}

// ===========================================================================
// Zoom
// ===========================================================================

void UIDebugGfxEditor::onZoom(int delta)
{
    if (!mCanvas->hasPicture()) return;
    static const int steps[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
    const int n = sizeof(steps) / sizeof(steps[0]);
    int i = 0;
    while (i < n - 1 && steps[i] < mCanvas->zoom()) i++;
    i = qBound(0, i + delta, n - 1);
    mCanvas->setZoom(steps[i]);
    lZoom->setText(QString("%1x").arg(steps[i]));
}

// ===========================================================================
// PNG import / export
// ===========================================================================

void UIDebugGfxEditor::onExportPng()
{
    if (mItem.kind == Item::None) return;
    const QString f = CommonDialogs::getSaveFileName(QString("edit_%1x%2.png").arg(mItem.w).arg(mItem.h),
                                                     tr_("Export PNG"), "PNG (*.png)");
    if (f.isEmpty()) return;
    QImage img;
    if (mItem.kind == Item::Sprite) img = renderSprite(mItem.spr, mItem.lut, mItem.w, mItem.h, mCanvas->codes(), false);
    else img = mCanvas->image();
    if (!img.save(f)) CommonDialogs::error(tr_("An error occured while writing file."));
}

void UIDebugGfxEditor::onImportPng()
{
    if (mItem.kind == Item::None) return;
    const QString f = QFileDialog::getOpenFileName(this, tr_("Import PNG"), QString(), "Images (*.png *.bmp *.gif)");
    if (f.isEmpty()) return;
    QImage img(f);
    if (img.isNull()) { CommonDialogs::error(tr_("An error occured while reading file.")); return; }
    if (img.width() != mItem.w || img.height() != mItem.h) {
        QMessageBox::warning(this, windowTitle(), tr_("The picture is %1 x %2, the item is %3 x %4.")
                                 .arg(img.width()).arg(img.height()).arg(mItem.w).arg(mItem.h));
        return;
    }
    img = img.convertToFormat(QImage::Format_ARGB32);
    std::vector<u32> codes(mCanvas->codes());
    int approx = 0;
    for (int y = 0; y < mItem.h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < mItem.w; x++) {
            const int dot = y * mItem.w + x;
            const u32 c = nearestCode(dot, line[x]);
            if (!itemIsRgb() && qAlpha(line[x]) >= 128) {
                const QRgb got = paletteColor(c);
                if ((got & 0xF8F8F8) != (line[x] & 0xF8F8F8)) approx++;
            }
            codes[(size_t)dot] = c;
        }
    }
    mCanvas->replaceCodes(codes, true);
    onCanvasEdited();
    if (approx > 0)
        lApplyStatus->setText(lApplyStatus->text() + " " +
            tr_("%1 dot(s) used colors that are not in the palette and got the nearest one.").arg(approx));
}

// ---- whole sprite list to a folder, and back ----

void UIDebugGfxEditor::onExportAllSprites()
{
    if (mSprites.empty()) return;
    const QString dir = CommonDialogs::getExistingDirectory(QString(), tr_("Select output folder for sprites"));
    if (dir.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int written = exportSprites(dir);
    QApplication::restoreOverrideCursor();
    if (written < 0) {
        CommonDialogs::error(tr_("An error occured while writing file."));
        return;
    }
    QMessageBox::information(this, windowTitle(), tr_("%1 sprite(s) exported to:\n%2\n\n"
        "Edit the PNG files (keep their size and colors of the palette), then use \"Import sprite folder...\".").arg(written).arg(dir));
}

// One PNG per sprite of the list, its original bytes (.raw: texture, then
// the LUT in color mode 1) and index.csv. Returns the number of sprites
// written, -1 if index.csv cannot be created.
int UIDebugGfxEditor::exportSprites(const QString &dir)
{
    QDir().mkpath(dir);
    QFile csv(QDir(dir).filePath("index.csv"));
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return -1;
    QTextStream ts(&csv);
    ts << "file,vram_addr,width,height,color_mode,cmdpmod,cmdcolr,cmdsrca,cmdsize,raw_file\n";
    int written = 0;
    for (size_t i = 0; i < mSprites.size(); i++) {
        const SpriteEntry &e = mSprites[i];
        int mode = 0;
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&e.spr, NULL, NULL, &mode, &addr, &bytes);
        const QString base = QString("spr_%1_%2_%3x%4").arg(i, 4, 10, QChar('0')).arg(addr, 5, 16, QChar('0')).toUpper()
                                 .arg(e.w).arg(e.h);
        if (!spriteThumb(e).save(QDir(dir).filePath(base + ".png"))) continue;
        // original bytes (texture + LUT): the import replaces them, they must
        // not depend on what the VRAM holds at import time
        QFile raw(QDir(dir).filePath(base + ".raw"));
        if (raw.open(QIODevice::WriteOnly)) { raw.write(e.tex); raw.write(e.lut); raw.close(); }
        ts << base << ".png," << hex(addr, 5) << "," << e.w << "," << e.h << "," << mode << ","
           << hex(e.spr.CMDPMOD, 4) << "," << hex(e.spr.CMDCOLR, 4) << "," << hex(e.spr.CMDSRCA, 4) << ","
           << hex(e.spr.CMDSIZE, 4) << "," << base << ".raw\n";
        written++;
    }
    ts.flush();
    csv.close();
    return written;
}

void UIDebugGfxEditor::onImportSpriteFolder()
{
    if (!emuReady()) return;
    const QString dir = CommonDialogs::getExistingDirectory(QString(), tr_("Select the sprite folder"));
    if (dir.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString msg = importSpriteFolder(dir);
    QApplication::restoreOverrideCursor();
    QMessageBox::information(this, windowTitle(), msg);
}

// Re-imports the PNG files of an exportSprites() folder that were modified
// after index.csv was written. Returns the report shown to the user.
QString UIDebugGfxEditor::importSpriteFolder(const QString &dir)
{
    QFile csv(QDir(dir).filePath("index.csv"));
    if (!csv.open(QIODevice::ReadOnly | QIODevice::Text))
        return tr_("index.csv not found in this folder.");
    const QDateTime csvTime = QFileInfo(csv).lastModified();
    QTextStream in(&csv);
    const QString header = in.readLine();
    if (!header.contains("raw_file"))
        return tr_("This index.csv was not written by this editor (no raw_file column).");
    int imported = 0, unchanged = 0, failed = 0, approx = 0, pending = 0;
    QStringList errors;
    const Item saved = mItem;
    const std::vector<u32> savedDerived = mDerived;
    while (!in.atEnd()) {
        const QStringList c = in.readLine().split(',');
        if (c.size() < 10) continue;
        const QFileInfo png(QDir(dir).filePath(c[0]));
        if (!png.exists() || png.lastModified() <= csvTime) { unchanged++; continue; }
        VdpEditSprite s;
        bool ok = true, o2;
        s.CMDCTRL = 0;
        s.CMDPMOD = (u16)c[5].toUInt(&o2, 16); ok &= o2;
        s.CMDCOLR = (u16)c[6].toUInt(&o2, 16); ok &= o2;
        s.CMDSRCA = (u16)c[7].toUInt(&o2, 16); ok &= o2;
        s.CMDSIZE = (u16)c[8].toUInt(&o2, 16); ok &= o2;
        QFile raw(QDir(dir).filePath(c[9].trimmed()));
        QImage img(png.filePath());
        if (!ok || img.isNull() || !raw.open(QIODevice::ReadOnly)) { failed++; errors << c[0]; continue; }
        const QByteArray rb = raw.readAll();
        int w = 0, h = 0, mode = 0;
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&s, &w, &h, &mode, &addr, &bytes);
        if (img.width() != w || img.height() != h || rb.size() < (int)bytes) {
            failed++; errors << tr_("%1 (size differs)").arg(c[0]); continue;
        }
        // reuse the item machinery for the color matching
        mItem = Item();
        mItem.kind = Item::Sprite;
        mItem.spr = s;
        mItem.mode = mode;
        mItem.w = w; mItem.h = h;
        if (mode == 1 && rb.size() >= (int)bytes + 32) mItem.lut = rb.mid((int)bytes, 32);
        img = img.convertToFormat(QImage::Format_ARGB32);
        std::vector<u32> codes((size_t)w * h);
        for (int y = 0; y < h; y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < w; x++) {
                codes[(size_t)y * w + x] = nearestCode(y * w + x, line[x]);
                if (!itemIsRgb() && qAlpha(line[x]) >= 128 &&
                    (paletteColor(codes[(size_t)y * w + x]) & 0xF8F8F8) != (line[x] & 0xF8F8F8)) approx++;
            }
        }
        const QByteArray orig = rb.left((int)bytes);
        const int need = (mode <= 1) ? w * h / 2 : (mode <= 4) ? w * h : w * h * 2;
        QByteArray full = orig;
        if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
        // keep the dots the picture does not change (end codes, dots after
        // the second end code): only the visible dots come from the PNG
        std::vector<u32> old((size_t)w * h);
        VdpEditSpriteDecode(mode, (const u8 *)full.constData(), w, h, old.data());
        const QImage before = renderSprite(s, mItem.lut, w, h, old, false);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t i = (size_t)y * w + x;
                if (before.pixel(x, y) == img.pixel(x, y)) codes[i] = old[i];
            }
        VdpEditSpriteEncode(mode, codes.data(), w, h, (u8 *)full.data());
        const QByteArray repl = full.left((int)bytes);
        if (repl == orig) { unchanged++; continue; }
        const int rc = VdpEditWrite(VDPEDIT_VDP1, addr, (const u8 *)orig.constData(), (const u8 *)repl.constData(), bytes);
        if (rc < 0) { failed++; errors << c[0]; }
        else { imported++; if (rc == 0) pending++; }
    }
    mItem = saved;
    mDerived = savedDerived;
    QString msg = tr_("%1 sprite(s) imported (%2 waiting for the game to load them), %3 unchanged, %4 failed.")
                      .arg(imported).arg(pending).arg(unchanged).arg(failed);
    if (approx) msg += "\n" + tr_("%1 dot(s) got the nearest palette color.").arg(approx);
    if (!errors.isEmpty()) msg += "\n\n" + errors.mid(0, 10).join("\n");
    refreshSource(true);
    updateChips();
    updateActionState();
    return msg;
}

void UIDebugGfxEditor::onEditsDialog()
{
    UIEditsExport dlg(this);
    dlg.exec();
    if (mItem.kind != Item::None) reloadItemFromMemory();
    refreshSource(true);
    updateChips();
    updateActionState();
}

// ===========================================================================
// Entry points
// ===========================================================================

void UIDebugGfxEditor::openSpriteCommand(u32 cmdAddr)
{
    {
        QSignalBlocker b(cbSource);
        cbSource->setCurrentIndex(SrcSprites);
    }
    refreshSource(false);
    updateChips();
    for (size_t r = 0; r < mShown.size(); r++)
        if (mSprites[mShown[r]].cmdAddr == cmdAddr) { lwSprites->setCurrentRow((int)r); return; }
    VdpEditSprite s;
    if (VdpEditSpriteFromCmd(NULL, cmdAddr, &s) == 0)
        openSprite(s, NULL, NULL, tr_("Sprite of the command at %1").arg(hex(cmdAddr, 5)));
}

void UIDebugGfxEditor::openLayer(int layer)
{
    // layer < 0: the VDP2 sprite layer, i.e. the VDP1 frame
    int src = SrcSpriteLayer;
    for (int i = SrcNBG0; i < SrcCount; i++) if (kSourceLayer[i] == layer) src = i;
    {
        QSignalBlocker b(cbSource);
        cbSource->setCurrentIndex(src);
    }
    refreshSource(false);
    updateChips();
}
