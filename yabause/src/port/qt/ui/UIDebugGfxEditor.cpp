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
#include <QDialogButtonBox>
#include <QFileSystemWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QRegularExpression>
#include <QSlider>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

extern "C" {
#include "vdp1.h"
#include "vdp2.h"
#include "vdp2debug.h"
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

// ---- palettes and pictures shared by the item, the folder, atlas and
//      tile-sheet code ----

// Color table of a palette sprite for an indexed PNG: index = texel code.
// Transparent code: alpha 0; end code: opaque magenta (painting with it ends
// the line, it must stand out).
QVector<QRgb> spriteColorTable(const VdpEditSprite &s, const QByteArray &lut)
{
    QVector<QRgb> t;
    const int n = VdpEditSpritePaletteSize(&s);
    const u8 *l = lut.size() >= 32 ? (const u8 *)lut.constData() : NULL;
    for (int k = 0; k < n; k++) {
        const int kind = VdpEditSpriteCodeKind(&s, (u32)k);
        QRgb c = VdpEditSpriteDataArgb(VdpEditSpriteCodeData(&s, l, (u32)k));
        if (kind == 1) c &= 0x00FFFFFFu;
        else if (kind == 2) c = qRgb(255, 0, 255);
        t << c;
    }
    return t;
}

// Same for a VDP2 palette (16 or 256 colors; 2048 does not fit a PNG).
QVector<QRgb> bgColorTable(const VdpEditBg &b, u32 palBase)
{
    QVector<QRgb> t;
    const int n = VdpEditBgPaletteSize(b.cn);
    if (n <= 0 || n > 256) return t;
    for (int k = 0; k < n; k++) {
        QRgb c = VdpEditCramArgb((u32)VdpEditBgCodeCramIndex(&b, palBase, (u32)k));
        if (k == 0 && !b.tpon) c &= 0x00FFFFFFu;
        t << c;
    }
    return t;
}

QImage codesToIndexed(const std::vector<u32> &codes, int w, int h, const QVector<QRgb> &table, int stride = -1,
                      int ox = 0, int oy = 0, QImage *into = NULL)
{
    QImage img;
    if (into) img = *into;
    else { img = QImage(w, h, QImage::Format_Indexed8); img.setColorTable(table); img.fill(0); }
    if (stride < 0) stride = w;
    for (int y = 0; y < h; y++) {
        uchar *line = img.scanLine(oy + y);
        for (int x = 0; x < w; x++) {
            const u32 c = codes[(size_t)y * stride + x];
            line[ox + x] = (uchar)(c < (u32)table.size() ? c : 0);
        }
    }
    if (into) *into = img;
    return img;
}

int colorDist(QRgb a, QRgb b)
{
    const int dr = qRed(a) - qRed(b), dg = qGreen(a) - qGreen(b), db = qBlue(a) - qBlue(b);
    return dr * dr + dg * dg + db * db;
}

// Nearest code of a sprite palette for a color (never the end code).
u32 spriteNearest(const VdpEditSprite &s, const QByteArray &lut, QRgb c, int *approx)
{
    int mode = 0;
    VdpEditSpriteGeometry(&s, NULL, NULL, &mode, NULL, NULL);
    const bool hasTransparent = VdpEditSpriteCodeKind(&s, 0) == 1;
    if (qAlpha(c) < 128 && hasTransparent) return 0;
    if (mode == 5) return VdpEditArgbToRgb555((u32)c);
    const int n = VdpEditSpritePaletteSize(&s);
    const u8 *l = lut.size() >= 32 ? (const u8 *)lut.constData() : NULL;
    int best = hasTransparent ? 1 : 0, bestD = 0x7FFFFFFF;
    for (int k = hasTransparent ? 1 : 0; k < n; k++) {
        if (VdpEditSpriteCodeKind(&s, (u32)k) == 2) continue;
        const int d = colorDist(VdpEditSpriteDataArgb(VdpEditSpriteCodeData(&s, l, (u32)k)), c);
        if (d < bestD) { bestD = d; best = k; if (d == 0) break; }
    }
    if (approx && bestD > 3 * 49) (*approx)++;   // more than the 5-bit rounding
    return (u32)best;
}

u32 bgNearest(const VdpEditBg &b, u32 palBase, QRgb c, int *approx)
{
    if (qAlpha(c) < 128 && !b.tpon) return 0;
    if (b.cn == 3) return VdpEditArgbToRgb555((u32)c);
    if (b.cn == 4) return 0x80000000u | ((u32)qBlue(c) << 16) | ((u32)qGreen(c) << 8) | (u32)qRed(c);
    const int n = VdpEditBgPaletteSize(b.cn);
    int best = b.tpon ? 0 : 1, bestD = 0x7FFFFFFF;
    for (int k = b.tpon ? 0 : 1; k < n; k++) {
        const int d = colorDist(VdpEditCramArgb((u32)VdpEditBgCodeCramIndex(&b, palBase, (u32)k)), c);
        if (d < bestD) { bestD = d; best = k; if (d == 0) break; }
    }
    if (approx && bestD > 3 * 49) (*approx)++;
    return (u32)best;
}

// Texture codes of a sprite from an edited picture (w x h, at (ox, oy) of
// 'pic'). Indexed picture: the index IS the code (an index the palette does
// not have gets the nearest color). Other pictures: nearest palette color,
// and a dot whose color did not change keeps its original code (end codes,
// dots after the second end code of a line).
std::vector<u32> spriteCodesFromPicture(const VdpEditSprite &s, const QByteArray &lut, const QByteArray &origTex,
                                        const QImage &pic, int ox, int oy, int *approx, bool *indexed)
{
    int w = 0, h = 0, mode = 0;
    VdpEditSpriteGeometry(&s, &w, &h, &mode, NULL, NULL);
    const int need = (mode <= 1) ? w * h / 2 : (mode <= 4) ? w * h : w * h * 2;
    QByteArray full = origTex;
    if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
    std::vector<u32> old((size_t)w * h), codes((size_t)w * h);
    VdpEditSpriteDecode(mode, (const u8 *)full.constData(), w, h, old.data());
    const int n = VdpEditSpritePaletteSize(&s);
    if (pic.format() == QImage::Format_Indexed8 && n > 0) {
        const QVector<QRgb> table = pic.colorTable();
        for (int y = 0; y < h; y++) {
            const uchar *line = pic.constScanLine(oy + y);
            for (int x = 0; x < w; x++) {
                const int idx = line[ox + x];
                codes[(size_t)y * w + x] = (idx < n) ? (u32)idx
                    : spriteNearest(s, lut, idx < table.size() ? table[idx] : 0, approx);
            }
        }
        if (indexed) *indexed = true;
        return codes;
    }
    if (indexed) *indexed = false;
    const QImage img = pic.convertToFormat(QImage::Format_ARGB32);
    const QImage before = renderSprite(s, lut, w, h, old, false);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const QRgb p = img.pixel(ox + x, oy + y);
            const size_t i = (size_t)y * w + x;
            codes[i] = (before.pixel(x, y) == p) ? old[i] : spriteNearest(s, lut, p, approx);
        }
    return codes;
}

// ---- colors used by a picture ---------------------------------------------
// Counted on the codes of the picture (opaque codes only: transparent code 0
// with SPD = 0 / TPON = 0 and end codes with ECD = 0 are counted apart).
//
// 8 bpp sprite -> 4 bpp color bank (VDP1 color modes 2/3/4 -> 0): the VDP1
// writes CMDCOLR | dot in the frame buffer, CMDCOLR bits 0-3 ignored in mode 0
// and 0-5 / 0-6 / 0-7 in modes 2 / 3 / 4 (ST-013, CMDCOLR; Technical Bulletin
// #25). When every opaque frame-buffer word of the sprite has the same bits
// 15-4, a 4 bpp sprite with CMDCOLR = those bits and dot = bits 3-0 writes
// exactly the same words, so the VDP2 shows the same thing (palette, priority,
// color calculation, shadow). Two dots cannot be kept: the one that would
// become 0 (transparent when SPD = 0) and the one that would become F (end
// code when ECD = 0).
struct ColorUse {
    int used = 0;                 // distinct opaque codes / RGB colors
    int palette = 0;              // codes of the palette (0 = RGB)
    bool transparent = false, endCodes = false;
    u32 minCode = 0, maxCode = 0;
    std::vector<std::pair<u32, int> > blocks;   // first code of each 16-code block, codes used in it
    int to4 = -1;                 // -1: not a 256/128/64 or 2048 color picture, 0: no, 1: yes
    QString why;
    u16 newColr = 0;              // sprite: CMDCOLR of the 4 bpp version
    u32 first = 0;                // first code of the block
};

ColorUse spriteColorUse(const VdpEditSprite &s, const std::vector<u32> &codes)
{
    ColorUse u;
    const int mode = (s.CMDPMOD >> 3) & 7;
    u.palette = VdpEditSpritePaletteSize(&s);
    const u32 mask = mode <= 1 ? 0xF : mode == 2 ? 0x3F : mode == 3 ? 0x7F : mode == 4 ? 0xFF : 0xFFFF;
    std::set<u32> used;
    for (size_t i = 0; i < codes.size(); i++) {
        const int k = VdpEditSpriteCodeKind(&s, codes[i]);
        if (k == 1) { u.transparent = true; continue; }
        if (k == 2) { u.endCodes = true; continue; }
        used.insert(codes[i] & mask);
    }
    u.used = (int)used.size();
    if (!used.empty()) { u.minCode = *used.begin(); u.maxCode = *used.rbegin(); }
    if (mode < 2 || mode > 4) return u;
    const u32 bank = s.CMDCOLR & ~mask & 0xFFFF;
    std::map<u32, int> blocks;                     // frame-buffer word & 0xFFF0 -> codes
    for (std::set<u32>::const_iterator it = used.begin(); it != used.end(); ++it) blocks[(*it | bank) & 0xFFF0]++;
    for (std::map<u32, int>::const_iterator it = blocks.begin(); it != blocks.end(); ++it)
        u.blocks.push_back(std::make_pair(it->first & mask, it->second));
    u.to4 = 0;
    if (blocks.size() > 1) {
        u.why = tr_("the colors are in %1 blocks of 16").arg(blocks.size());
        return u;
    }
    const u32 bd = blocks.empty() ? (bank & 0xFFF0) : blocks.begin()->first;
    const bool spd = (s.CMDPMOD & 0x40) != 0, ecd = !(s.CMDPMOD & 0x80);
    for (std::set<u32>::const_iterator it = used.begin(); it != used.end(); ++it) {
        const u32 low = (*it | bank) & 0xF;
        if (low == 0 && !spd) { u.why = tr_("code %1 would become 0, the transparent code").arg(hex(*it, 2)); return u; }
        if (low == 0xF && ecd) { u.why = tr_("code %1 would become F, the end code").arg(hex(*it, 2)); return u; }
    }
    u.to4 = 1;
    u.newColr = (u16)bd;
    u.first = bd & mask;
    return u;
}

ColorUse bgColorUse(const VdpEditBg &b, const std::vector<u32> &codes)
{
    ColorUse u;
    u.palette = VdpEditBgPaletteSize(b.cn);
    std::set<u32> used;
    for (size_t i = 0; i < codes.size(); i++) {
        if (VdpEditBgCodeArgb(&b, 0, codes[i]) == 0) { u.transparent = true; continue; }
        used.insert(codes[i]);
    }
    u.used = (int)used.size();
    if (!used.empty()) { u.minCode = *used.begin(); u.maxCode = *used.rbegin(); }
    if (u.palette < 64) return u;
    std::map<u32, int> blocks;
    for (std::set<u32>::const_iterator it = used.begin(); it != used.end(); ++it) blocks[*it & ~0xFu]++;
    for (std::map<u32, int>::const_iterator it = blocks.begin(); it != blocks.end(); ++it)
        u.blocks.push_back(std::make_pair(it->first, it->second));
    u.to4 = 0;
    if (blocks.size() > 1) { u.why = tr_("the colors are in %1 blocks of 16").arg(blocks.size()); return u; }
    if (!blocks.empty() && used.count(blocks.begin()->first) && !b.tpon) {
        u.why = tr_("code %1 would become 0, the transparent code").arg(hex(blocks.begin()->first, 3));
        return u;
    }
    u.to4 = 1;
    u.first = blocks.empty() ? 0 : blocks.begin()->first;
    return u;
}

QString blocksText(const ColorUse &u, int digits)
{
    QStringList parts;
    for (size_t i = 0; i < u.blocks.size() && i < 8; i++)
        parts << QString("%1-%2 (%3)").arg(hex(u.blocks[i].first, digits)).arg(hex(u.blocks[i].first + 15, digits)).arg(u.blocks[i].second);
    if (u.blocks.size() > 8) parts << QString("... +%1").arg(u.blocks.size() - 8);
    return parts.join(", ");
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
    if (colors.size() != mColors.size()) { mMarked.clear(); mLastMark = -1; }
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
    for (QSet<int>::const_iterator it = mMarked.begin(); it != mMarked.end(); ++it) {
        if (*it >= mColors.size()) continue;
        const QRect r((*it % mColumns) * c, (*it / mColumns) * c, c, c);
        QPen pen(palette().color(QPalette::Highlight));
        pen.setWidth(2);
        pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(2, 2, -3, -3));
        p.fillRect(QRect(r.right() - 5, r.top() + 1, 5, 5), palette().color(QPalette::Highlight));
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

QVector<int> GfxPalette::marked() const
{
    QVector<int> v;
    for (QSet<int>::const_iterator it = mMarked.begin(); it != mMarked.end(); ++it)
        if (*it < mColors.size()) v << *it;
    std::sort(v.begin(), v.end());
    return v;
}

void GfxPalette::mousePressEvent(QMouseEvent *e)
{
    const int i = entryAt(e->pos());
    if (i < 0) return;
    if (e->button() == Qt::LeftButton && (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        if ((e->modifiers() & Qt::ShiftModifier) && mLastMark >= 0) {
            for (int k = qMin(mLastMark, i); k <= qMax(mLastMark, i); k++) mMarked.insert(k);
        } else if (mMarked.contains(i)) {
            mMarked.remove(i);
        } else {
            mMarked.insert(i);
        }
        mLastMark = i;
        update();
        emit marksChanged();
        return;
    }
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
    lwSprites->setSelectionMode(QAbstractItemView::ExtendedSelection);
    lwSprites->setToolTip(tr_("Click a sprite to edit it. Ctrl/Shift+click selects several sprites for an atlas."));
    pl->addWidget(lwSprites, 1);
    cbLibrary = new QCheckBox(tr_("Keep sprites seen in earlier frames"), pgList);
    cbLibrary->setToolTip(tr_("Builds a library of every sprite drawn while the game runs (or while you step frames), "
                              "so animation frames and sprites that are not on screen right now can be edited too"));
    pl->addWidget(cbLibrary);
    pbClearLibrary = new QPushButton(tr_("Clear the library"), pgList);
    pl->addWidget(pbClearLibrary);
    QGridLayout *ag = new QGridLayout();
    ag->setSpacing(6);
    pbSamePalette = new QPushButton(tr_("Same palette"), pgList);
    pbSamePalette->setToolTip(tr_("Selects every sprite that uses the palette of the current one: usually the frames of one character"));
    pbExportAtlas = new QPushButton(tr_("Export atlas..."), pgList);
    pbExportAtlas->setToolTip(tr_("Writes the selected sprites as one named sheet (PNG, indexed when they share a palette) "
                                  "with a JSON file describing each frame"));
    pbImportAtlas = new QPushButton(tr_("Import atlas..."), pgList);
    pbImportAtlas->setToolTip(tr_("Reads an edited atlas back by its name (.atlas.json)"));
    pbSelect4bpp = new QPushButton(tr_("Fit 4 bpp"), pgList);
    pbSelect4bpp->setToolTip(tr_("Selects the 64/128/256-color sprites whose colors all fit in one block of 16: "
                                 "they could be stored as 4 bpp sprites (half the VRAM) with the same colors on screen"));
    ag->addWidget(pbSamePalette, 0, 0);
    ag->addWidget(pbSelect4bpp, 0, 1);
    ag->addWidget(pbExportAtlas, 1, 0);
    ag->addWidget(pbImportAtlas, 1, 1);
    pl->addLayout(ag);
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
    QHBoxLayout *tr = new QHBoxLayout();
    pbExportTiles = new QPushButton(tr_("Export tile sheet..."), pgView);
    pbExportTiles->setToolTip(tr_("Writes every character used by the layer once, as it is stored (no flip), "
                                  "in one sheet: a character used in many places is edited once"));
    pbImportTiles = new QPushButton(tr_("Import tile sheet..."), pgView);
    tr->addWidget(pbExportTiles);
    tr->addWidget(pbImportTiles);
    pv->addLayout(tr);
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
    // tools keep their full width (no "Pe...il" when the window is narrow);
    // Undo / Redo live on the second row
    for (int id = GfxCanvas::Pencil; id <= GfxCanvas::Eraser; id++)
        if (QAbstractButton *b = mTools->button(id)) b->setSizePolicy(QSizePolicy::Minimum, b->sizePolicy().verticalPolicy());
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
    zb->addWidget(tbUndo);
    zb->addWidget(tbRedo);
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
    pbRecolor = new QPushButton(tr_("Recolor..."), right);
    pbRecolor->setToolTip(tr_("Shifts the hue, saturation and lightness of the marked colors (Ctrl/Shift+click in the palette; "
                              "none marked = the whole palette), with a live preview"));
    sw->addWidget(pbRecolor);
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
    lColorUse = new QLabel(right);
    lColorUse->setWordWrap(true);
    lColorUse->setTextInteractionFlags(Qt::TextSelectableByMouse);
    vr->addWidget(lColorUse);

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
    for (QPushButton *b : { pbRevertItem, pbImportPng, pbExportPng, pbCustomColor, pbClearLibrary, pbFitOverview,
                            pbRecolor, pbSamePalette, pbSelect4bpp, pbExportAtlas, pbImportAtlas, pbExportTiles, pbImportTiles }) {
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
    pbPick = new QPushButton(tr_("Pick on screen"), this);
    pbPick->setCheckable(true);
    pbPick->setToolTip(tr_("Then click a sprite or a background in the game window: the editor opens what is drawn there "
                           "(right click or Esc cancels)"));
    cbWatch = new QCheckBox(tr_("Watch folder"), this);
    cbWatch->setToolTip(tr_("Hot reload: every PNG saved in the last exported folder (sprites, atlas, tile sheet) "
                            "is imported at once and shows in the game"));
    for (QPushButton *b : { pbStep, pbRefresh, pbEdits, pbRevertAll, pbExportAll, pbImportFolder, pbPick }) {
        b->setProperty("flat2", true);
        b->setMinimumHeight(32);
        b->setCursor(Qt::PointingHandCursor);
    }
    ft->addWidget(pbRun);
    ft->addWidget(pbStep);
    ft->addWidget(pbRefresh);
    ft->addWidget(pbPick);
    ft->addSpacing(12);
    ft->addWidget(pbExportAll);
    ft->addWidget(pbImportFolder);
    ft->addWidget(cbWatch);
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
    connect(pbRecolor, SIGNAL(clicked()), this, SLOT(onRecolor()));
    connect(pbPick, SIGNAL(toggled(bool)), this, SLOT(onPickToggled(bool)));
    connect(cbWatch, SIGNAL(toggled(bool)), this, SLOT(onWatchToggled(bool)));
    connect(pbSamePalette, SIGNAL(clicked()), this, SLOT(onSelectSamePalette()));
    connect(pbSelect4bpp, SIGNAL(clicked()), this, SLOT(onSelect4bppCandidates()));
    connect(pbExportAtlas, SIGNAL(clicked()), this, SLOT(onExportAtlas()));
    connect(pbImportAtlas, SIGNAL(clicked()), this, SLOT(onImportAtlas()));
    connect(pbExportTiles, SIGNAL(clicked()), this, SLOT(onExportTiles()));
    connect(pbImportTiles, SIGNAL(clicked()), this, SLOT(onImportTiles()));
    connect(mPalette, &GfxPalette::marksChanged, this, [this]() {
        const int n = mPalette->marked().size();
        pbRecolor->setText(n ? tr_("Recolor %1...").arg(n) : tr_("Recolor..."));
    });
    mWatchTimer = new QTimer(this);
    mWatchTimer->setSingleShot(true);
    mWatchTimer->setInterval(400);   // editors write a file in several steps
    connect(mWatchTimer, SIGNAL(timeout()), this, SLOT(onWatchEvent()));

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
    // YabauseThread::emulationRunning() is only "!mPause" (YabauseThread.cpp):
    // it is false for a game that is merely paused. Whether a game is loaded
    // is init() >= 0 (YabauseInit succeeded, YabauseDeInit not called yet).
    YabauseThread *t = thread();
    if (t && t->init() < 0) return false;
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
            {
                // rows of the list index mSprites: drop them first, without
                // letting the list call onSpriteSelected() on a stale row
                QSignalBlocker blk(lwSprites);
                lwSprites->clear();
                mShown.clear();
            }
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
    if (isModal() && mResumedEmulation && t && t->init() >= 0 && !t->emulationPaused())
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
        {
            QSignalBlocker blk(lwSprites);
            lwSprites->clear();
            mShown.clear();
        }
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

std::vector<u32> UIDebugGfxEditor::spriteCodes(const SpriteEntry &e) const
{
    int mode = 0;
    VdpEditSpriteGeometry(&e.spr, NULL, NULL, &mode, NULL, NULL);
    std::vector<u32> codes((size_t)e.w * e.h, 0);
    QByteArray tex = e.tex;
    const int need = (mode <= 1) ? e.w * e.h / 2 : (mode <= 4) ? e.w * e.h : e.w * e.h * 2;
    if (tex.size() < need) tex.append(QByteArray(need - tex.size(), 0));
    if (!codes.empty()) VdpEditSpriteDecode(mode, (const u8 *)tex.constData(), e.w, e.h, codes.data());
    return codes;
}

void UIDebugGfxEditor::rebuildSpriteList()
{
    quint64 selKey = 0;
    const int row = lwSprites->currentRow();
    if (row >= 0 && row < (int)mShown.size() && (size_t)mShown[row] < mSprites.size()) selKey = mSprites[mShown[row]].key;

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
        const ColorUse cu = spriteColorUse(e.spr, spriteCodes(e));
        text += "\n" + (cu.palette ? tr_("%1/%2 colors").arg(cu.used).arg(cu.palette) : tr_("%1 colors").arg(cu.used))
              + (cu.to4 == 1 ? tr_("   4 bpp ok") : QString());
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
                           .arg(hex(e.spr.CMDSRCA, 4)).arg(hex(e.spr.CMDSIZE, 4)).arg(e.uses)
                       + "\n" + (cu.palette ? tr_("%1 of %2 colors used").arg(cu.used).arg(cu.palette) : tr_("%1 colors").arg(cu.used))
                       + (cu.to4 == 1 ? "\n" + tr_("Can be a 4 bpp sprite: CMDCOLR %1, codes %2-%3").arg(hex(cu.newColr, 4)).arg(hex(cu.first, 2)).arg(hex(cu.first + 15, 2))
                          : cu.to4 == 0 ? "\n" + tr_("Cannot be a 4 bpp sprite: %1").arg(cu.why) : QString()));
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
    if (row < 0 || row >= (int)mShown.size() || (size_t)mShown[row] >= mSprites.size()) return;
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
                b.palBase = c.palBase;
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
        const QByteArray meta = blockMeta(b);
        const int rc = VdpEditWriteEx(b.space, b.addr, (const u8 *)b.applied.constData(),
                                      (const u8 *)bytes.constData(), (u32)bytes.size(),
                                      meta.isEmpty() ? NULL : meta.constData(), NULL, NULL);
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
    refreshColorInfo();
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
        char meta[VDPEDIT_META_SIZE];
        VdpEditLutMeta(&mItem.spr, meta);
        const int rc = VdpEditWriteEx(VDPEDIT_VDP1, VdpEditSpriteLutAddr(&mItem.spr), (const u8 *)mItem.lutApplied.constData(),
                                      (const u8 *)nl.constData(), 32, meta, NULL, NULL);
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
    mColorGroup = nullptr;
    if (mItem.kind == Item::None) { if (lColorUse) lColorUse->clear(); return; }
    mColorGroup = makeGroup(twInfo, tr_("Colors"));
    refreshColorInfo();
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


// Colors used by the picture being edited (follows every stroke).
void UIDebugGfxEditor::refreshColorInfo()
{
    if (!mColorGroup || mItem.kind == Item::None) return;
    qDeleteAll(mColorGroup->takeChildren());
    const std::vector<u32> codes = mCanvas->codes();
    QTreeWidgetItem *g = mColorGroup;
    if (mItem.kind == Item::Sprite) {
        const ColorUse u = spriteColorUse(mItem.spr, codes);
        const int digits = u.palette > 16 ? 2 : 1;
        makeRow(g, tr_("Colors used"), u.palette ? tr_("%1 of %2").arg(u.used).arg(u.palette) : QString::number(u.used));
        if (u.palette && u.used)
            makeRow(g, tr_("Codes used"), QString("%1 - %2").arg(hex(u.minCode, digits)).arg(hex(u.maxCode, digits)));
        makeRow(g, tr_("Transparent dots"), u.transparent ? tr_("yes") : tr_("no"));
        makeRow(g, tr_("End codes"), u.endCodes ? tr_("yes") : tr_("no"));
        if (u.to4 >= 0) {
            makeRow(g, tr_("Blocks of 16"), u.blocks.empty() ? tr_("none") : blocksText(u, 2));
            if (u.to4 == 1)
                makeRow(g, tr_("As 4 bpp"), tr_("possible: color mode 0, CMDCOLR %1, codes %2-%3 become 0-F (same frame-buffer data)")
                                                .arg(hex(u.newColr, 4)).arg(hex(u.first, 2)).arg(hex(u.first + 15, 2)));
            else
                makeRow(g, tr_("As 4 bpp"), tr_("not possible: %1").arg(u.why));
        }
        QString sum = u.palette ? tr_("%1 of %2 colors used").arg(u.used).arg(u.palette) : tr_("%1 colors used").arg(u.used);
        if (u.to4 == 1) sum += " - " + tr_("4 bpp: yes (CMDCOLR %1, codes %2-%3)").arg(hex(u.newColr, 4)).arg(hex(u.first, 2)).arg(hex(u.first + 15, 2));
        else if (u.to4 == 0) sum += " - " + tr_("4 bpp: no, %1").arg(u.why);
        lColorUse->setText(sum);
    } else {
        const ColorUse u = bgColorUse(mItem.bg, codes);
        const int digits = u.palette > 256 ? 3 : u.palette > 16 ? 2 : 1;
        makeRow(g, tr_("Colors used"), u.palette ? tr_("%1 of %2").arg(u.used).arg(u.palette) : QString::number(u.used));
        if (u.palette && u.used)
            makeRow(g, tr_("Codes used"), QString("%1 - %2").arg(hex(u.minCode, digits)).arg(hex(u.maxCode, digits)));
        makeRow(g, tr_("Transparent dots"), u.transparent ? tr_("yes") : tr_("no"));
        if (u.to4 >= 0) {
            makeRow(g, tr_("Blocks of 16"), u.blocks.empty() ? tr_("none") : blocksText(u, digits));
            makeRow(g, tr_("As 16 colors"), u.to4 == 1 ? tr_("possible: codes %1-%2 = one 16-color palette").arg(hex(u.first, digits)).arg(hex(u.first + 15, digits))
                                                       : tr_("not possible: %1").arg(u.why));
        }
        QString sum = u.palette ? tr_("%1 of %2 colors used").arg(u.used).arg(u.palette) : tr_("%1 colors used").arg(u.used);
        if (u.to4 == 1) sum += " - " + tr_("16 colors: yes (codes %1-%2)").arg(hex(u.first, digits)).arg(hex(u.first + 15, digits));
        else if (u.to4 == 0) sum += " - " + tr_("16 colors: no, %1").arg(u.why);
        lColorUse->setText(sum);
    }
    mColorGroup->setExpanded(true);
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
    // Indexed (index = code, color table = the real palette) whenever one
    // palette of 256 colors or less describes the whole picture.
    QImage img = indexedImage(mCanvas->codes(), mItem.w, mItem.h);
    if (img.isNull()) {
        if (mItem.kind == Item::Sprite) img = renderSprite(mItem.spr, mItem.lut, mItem.w, mItem.h, mCanvas->codes(), false);
        else img = mCanvas->image();
    }
    if (!img.save(f)) CommonDialogs::error(tr_("An error occured while writing file."));
}

// Indexed picture of the item, or a null image when one palette cannot
// describe it (RGB formats, 2048 colors, cells using several palettes).
QImage UIDebugGfxEditor::indexedImage(const std::vector<u32> &codes, int w, int h) const
{
    QVector<QRgb> table;
    if (mItem.kind == Item::Sprite) {
        if (mItem.mode > 4) return QImage();
        table = spriteColorTable(mItem.spr, mItem.lut);
    } else if (mItem.kind == Item::Bitmap) {
        table = bgColorTable(mItem.bg, mItem.bg.bmPal);
    } else if (mItem.kind == Item::Cells) {
        for (size_t i = 1; i < mItem.cellPal.size(); i++)
            if (mItem.cellPal[i] != mItem.cellPal[0]) return QImage();
        table = bgColorTable(mItem.bg, mItem.cellPal.empty() ? 0 : mItem.cellPal[0]);
    }
    if (table.isEmpty()) return QImage();
    return codesToIndexed(codes, w, h, table);
}

// Codes from an indexed picture: index = code; an index past the palette
// gets the nearest color. False if the picture is not indexed.
bool UIDebugGfxEditor::codesFromIndexed(const QImage &img, std::vector<u32> &codes, int *approx) const
{
    if (img.format() != QImage::Format_Indexed8 || itemIsRgb()) return false;
    const int n = paletteSize();
    if (n <= 0 || n > 256) return false;
    const QVector<QRgb> table = img.colorTable();
    codes.assign((size_t)mItem.w * mItem.h, 0);
    for (int y = 0; y < mItem.h; y++) {
        const uchar *line = img.constScanLine(y);
        for (int x = 0; x < mItem.w; x++) {
            const int idx = line[x];
            const int dot = y * mItem.w + x;
            if (idx < n) codes[(size_t)dot] = (u32)idx;
            else {
                codes[(size_t)dot] = nearestCode(dot, idx < table.size() ? table[idx] : 0);
                if (approx) (*approx)++;
            }
        }
    }
    return true;
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
    std::vector<u32> codes(mCanvas->codes());
    int approx = 0;
    const bool indexed = codesFromIndexed(img, codes, &approx);
    if (!indexed) {
        img = img.convertToFormat(QImage::Format_ARGB32);
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
    }
    mCanvas->replaceCodes(codes, true);
    onCanvasEdited();
    if (indexed)
        lApplyStatus->setText(lApplyStatus->text() + " " + tr_("Indexed picture: read by palette index."));
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
        // palette sprites: indexed PNG whose indices are the texel codes, so
        // the artist works with the real palette and cannot leave it
        QImage pic;
        if (mode <= 4) {
            std::vector<u32> codes((size_t)e.w * e.h, 0);
            QByteArray tex = e.tex;
            const int need = (mode <= 1) ? e.w * e.h / 2 : e.w * e.h;
            if (tex.size() < need) tex.append(QByteArray(need - tex.size(), 0));
            VdpEditSpriteDecode(mode, (const u8 *)tex.constData(), e.w, e.h, codes.data());
            pic = codesToIndexed(codes, e.w, e.h, spriteColorTable(e.spr, e.lut));
        } else {
            pic = spriteThumb(e);
        }
        if (!pic.save(QDir(dir).filePath(base + ".png"))) continue;
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
    stampFolder(dir);
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
    while (!in.atEnd()) {
        const QStringList c = in.readLine().split(',');
        if (c.size() < 10) continue;
        const QFileInfo png(QDir(dir).filePath(c[0]));
        // watched folder: changed since the last import; otherwise since the export
        const bool stamped = mStamps.contains(png.absoluteFilePath());
        if (!png.exists() || (stamped ? !fileChangedSinceStamp(png.absoluteFilePath()) : png.lastModified() <= csvTime)) {
            unchanged++;
            continue;
        }
        if (!stamped) mStamps[png.absoluteFilePath()] = png.lastModified();
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
        const QByteArray lut = (mode == 1 && rb.size() >= (int)bytes + 32) ? rb.mid((int)bytes, 32) : QByteArray();
        const QByteArray origTex = rb.left((int)bytes);
        std::vector<u32> codes = spriteCodesFromPicture(s, lut, origTex, img, 0, 0, &approx, NULL);
        const QByteArray orig = rb.left((int)bytes);
        const int need = (mode <= 1) ? w * h / 2 : (mode <= 4) ? w * h : w * h * 2;
        QByteArray full = orig;
        if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
        VdpEditSpriteEncode(mode, codes.data(), w, h, (u8 *)full.data());
        const QByteArray repl = full.left((int)bytes);
        if (repl == orig) { unchanged++; continue; }
        char meta[VDPEDIT_META_SIZE];
        VdpEditSpriteMeta(&s, meta);
        const int rc = VdpEditWriteEx(VDPEDIT_VDP1, addr, (const u8 *)orig.constData(), (const u8 *)repl.constData(), bytes,
                                      meta, NULL, NULL);
        if (rc < 0) { failed++; errors << c[0]; }
        else { imported++; if (rc == 0) pending++; }
    }
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
        if ((size_t)mShown[r] < mSprites.size() && mSprites[mShown[r]].cmdAddr == cmdAddr) { lwSprites->setCurrentRow((int)r); return; }
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

// ===========================================================================
// Descriptors and previews of edits
// ===========================================================================

QByteArray UIDebugGfxEditor::blockMeta(const Block &b) const
{
    char meta[VDPEDIT_META_SIZE];
    meta[0] = 0;
    switch (b.kind) {
    case 0: VdpEditSpriteMeta(&mItem.spr, meta); break;
    case 1: VdpEditCharMeta(&mItem.bg, b.palBase, meta); break;
    default: VdpEditBitmapMeta(&mItem.bg, b.w, meta); break;
    }
    return QByteArray(meta);
}

QImage UIDebugGfxEditor::editPreview(int space, const char *meta, u32 addr, const u8 *bytes, u32 len)
{
    if (!bytes || len == 0 || !Vdp2Regs || !Vdp2ColorRam) return QImage();
    if (space == VDPEDIT_CRAM) {
        const int bpc = VdpEditCramBytes(), n = (int)len / bpc;
        if (n <= 0) return QImage();
        const int cols = qMin(n, 8), rows = (n + cols - 1) / cols;
        QImage img(cols, rows, QImage::Format_ARGB32);
        img.fill(0);
        for (int i = 0; i < n; i++) {
            const u8 *p = bytes + i * bpc;
            const QRgb c = (bpc == 2) ? (QRgb)VdpEditRgb555ToArgb((u16)((p[0] << 8) | p[1]))
                                      : qRgb(p[3], p[2], p[1]);
            img.setPixel(i % cols, i / cols, c);
        }
        return img;
    }
    if (!meta) return QImage();
    VdpEditSprite s;
    if (strncmp(meta, "spr", 3) == 0 && VdpEditSpriteFromMeta(meta, &s) == 0) {
        int w = 0, h = 0, mode = 0;
        VdpEditSpriteGeometry(&s, &w, &h, &mode, NULL, NULL);
        const int need = (mode <= 1) ? w * h / 2 : (mode <= 4) ? w * h : w * h * 2;
        QByteArray full((const char *)bytes, (int)len);
        if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
        std::vector<u32> codes((size_t)w * h);
        VdpEditSpriteDecode(mode, (const u8 *)full.constData(), w, h, codes.data());
        QByteArray lut;
        if (mode == 1) {
            u8 *ram = Vdp1DebugGetRam();
            const u32 la = VdpEditSpriteLutAddr(&s);
            if (ram && la + 32 <= 0x80000) lut = QByteArray((const char *)ram + la, 32);
        }
        return renderSprite(s, lut, w, h, codes, false);
    }
    if (strncmp(meta, "lut", 3) == 0 && VdpEditSpriteFromMeta(meta, &s) == 0 && len >= 32) {
        QImage img(4, 4, QImage::Format_ARGB32);
        for (int k = 0; k < 16; k++)
            img.setPixel(k % 4, k / 4, VdpEditSpriteDataArgb(((u32)bytes[2 * k] << 8) | bytes[2 * k + 1]));
        return img;
    }
    VdpEditBg b;
    u32 pal = 0;
    int w = 0;
    const int kind = VdpEditBgFromMeta(meta, &b, &pal, &w);
    if (kind == 1) {
        const int cs = b.csize;
        if (len < (u32)(b.pw * b.pw) * ((b.cn == 0) ? 0x20u : (b.cn == 1) ? 0x40u : (b.cn == 4) ? 0x100u : 0x80u)) return QImage();
        u32 codes[256];
        VdpEditCharDecode(&b, bytes, codes);
        QImage img(cs, cs, QImage::Format_ARGB32);
        for (int y = 0; y < cs; y++)
            for (int x = 0; x < cs; x++)
                img.setPixel(x, y, VdpEditBgCodeArgb(&b, pal, codes[y * cs + x]));
        return img;
    }
    if (kind == 2 && w > 0) {
        std::vector<u32> codes((size_t)w);
        if (VdpEditBitmapRowBytes(&b, w) > len) return QImage();
        VdpEditBitmapDecode(&b, bytes, w, codes.data());
        const int rows = qMax(1, w / 16);
        QImage img(w, rows, QImage::Format_ARGB32);
        for (int x = 0; x < w; x++) {
            const QRgb c = VdpEditBgCodeArgb(&b, pal, codes[(size_t)x]);
            for (int y = 0; y < rows; y++) img.setPixel(x, y, c);
        }
        return img;
    }
    Q_UNUSED(addr);
    return QImage();
}

// ===========================================================================
// Recolor: hue / saturation / lightness of a group of palette colors
// ===========================================================================

void UIDebugGfxEditor::onRecolor()
{
    if (mItem.kind == Item::None || !emuReady() || mPalCodes.isEmpty()) return;
    QVector<int> entries = mPalette->marked();
    if (entries.isEmpty()) for (int i = 0; i < mPalCodes.size(); i++) entries << i;

    struct Target { int cram; int lut; QColor orig; u32 code; };
    QVector<Target> targets;
    const bool rgb = itemIsRgb();
    for (int k = 0; k < entries.size(); k++) {
        const u32 code = mPalCodes[entries[k]];
        Target t;
        t.cram = -1; t.lut = -1; t.code = code;
        t.orig = QColor(paletteColor(code));
        if (rgb) {
            if (code == transparentCode()) continue;
        } else {
            if (mItem.kind == Item::Sprite && VdpEditSpriteCodeKind(&mItem.spr, code) == 2) continue;   // end code
            bool lutRgb = false;
            t.cram = paletteCram(code, &lutRgb);
            if (t.cram < 0 && lutRgb) t.lut = (int)code;
            if (t.cram < 0 && t.lut < 0) continue;
        }
        targets << t;
    }
    if (targets.isEmpty()) return;

    const std::vector<u32> baseCodes = mCanvas->codes();
    const QByteArray baseLut = mItem.lut;
    // hue shift (degrees), saturation and lightness shifts (percent of full scale)
    auto apply = [this, targets, rgb, baseCodes, baseLut](int dh, int ds, int dl, bool undoable) {
        auto shift = [dh, ds, dl](const QColor &c0) {
            const QColor c = c0.toHsl();
            int h = c.hslHue();
            if (h >= 0) h = ((h + dh) % 360 + 360) % 360;
            else h = 0;
            const int sat = qBound(0, c.hslSaturation() + ds * 255 / 100, 255);
            const int l = qBound(0, c.lightness() + dl * 255 / 100, 255);
            return QColor::fromHsl(h, sat, l).toRgb();
        };
        if (rgb) {
            std::vector<u32> codes = baseCodes;
            std::map<u32, u32> remap;
            for (int i = 0; i < targets.size(); i++) remap[targets[i].code] = rgbToCode(shift(targets[i].orig).rgb());
            for (size_t i = 0; i < codes.size(); i++) {
                std::map<u32, u32>::const_iterator it = remap.find(codes[i]);
                if (it != remap.end()) codes[i] = it->second;
            }
            mCanvas->replaceCodes(baseCodes, false);
            mCanvas->replaceCodes(codes, undoable);
            if (undoable) onCanvasEdited();
            return;
        }
        QByteArray lut = baseLut;
        bool lutChanged = false;
        for (int i = 0; i < targets.size(); i++) {
            const QColor c = shift(targets[i].orig);
            if (targets[i].cram >= 0) {
                VdpEditSetCramColor((u32)targets[i].cram, c.red(), c.green(), c.blue());
            } else if (targets[i].lut >= 0 && lut.size() == 32) {
                const u16 v = VdpEditArgbToRgb555(c.rgb());
                lut[targets[i].lut * 2] = (char)(v >> 8);
                lut[targets[i].lut * 2 + 1] = (char)v;
                lutChanged = true;
            }
        }
        if (lutChanged || mItem.lut != lut) {
            char meta[VDPEDIT_META_SIZE];
            VdpEditLutMeta(&mItem.spr, meta);
            if (VdpEditWriteEx(VDPEDIT_VDP1, VdpEditSpriteLutAddr(&mItem.spr), (const u8 *)mItem.lutApplied.constData(),
                               (const u8 *)lut.constData(), 32, meta, NULL, NULL) >= 0)
                mItem.lut = mItem.lutApplied = lut;
        }
        refreshPalette();
        mCanvas->rerender();
    };

    QDialog dlg(this);
    dlg.setWindowTitle(tr_("Recolor"));
    QVBoxLayout *v = new QVBoxLayout(&dlg);
    QLabel *info = new QLabel(tr_("%1 color(s). The game shows the result at once while it runs.").arg(targets.size()), &dlg);
    info->setWordWrap(true);
    v->addWidget(info);
    QGridLayout *g = new QGridLayout();
    QSlider *sh = new QSlider(Qt::Horizontal, &dlg), *ss = new QSlider(Qt::Horizontal, &dlg), *sl = new QSlider(Qt::Horizontal, &dlg);
    sh->setRange(-180, 180); ss->setRange(-100, 100); sl->setRange(-100, 100);
    QLabel *vh = new QLabel("0", &dlg), *vs = new QLabel("0", &dlg), *vl = new QLabel("0", &dlg);
    g->addWidget(new QLabel(tr_("Hue"), &dlg), 0, 0);        g->addWidget(sh, 0, 1); g->addWidget(vh, 0, 2);
    g->addWidget(new QLabel(tr_("Saturation"), &dlg), 1, 0); g->addWidget(ss, 1, 1); g->addWidget(vs, 1, 2);
    g->addWidget(new QLabel(tr_("Lightness"), &dlg), 2, 0);  g->addWidget(sl, 2, 1); g->addWidget(vl, 2, 2);
    v->addLayout(g);
    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    connect(bb, SIGNAL(accepted()), &dlg, SLOT(accept()));
    connect(bb, SIGNAL(rejected()), &dlg, SLOT(reject()));
    auto preview = [&]() {
        vh->setText(QString::number(sh->value()));
        vs->setText(QString::number(ss->value()));
        vl->setText(QString::number(sl->value()));
        apply(sh->value(), ss->value(), sl->value(), false);
    };
    connect(sh, &QSlider::valueChanged, &dlg, preview);
    connect(ss, &QSlider::valueChanged, &dlg, preview);
    connect(sl, &QSlider::valueChanged, &dlg, preview);
    dlg.resize(420, dlg.sizeHint().height());
    if (dlg.exec() == QDialog::Accepted) {
        apply(sh->value(), ss->value(), sl->value(), true);
        lApplyStatus->setText(rgb ? tr_("Colors of the picture shifted.")
                                  : tr_("%1 palette color(s) changed. They are shared by everything that uses them.").arg(targets.size()));
    } else {
        apply(0, 0, 0, false);   // back to the colors the dialog started from
    }
    refreshItemInfo();
    updateChips();
}

// ===========================================================================
// Selection in the game screen
// ===========================================================================

void UIDebugGfxEditor::onPickToggled(bool on)
{
    UIYabause *w = QtYabause::mainWindow(false);
    if (!w) { if (on) pbPick->setChecked(false); return; }
    if (on) {
        QPointer<UIDebugGfxEditor> self(this);
        w->setScreenPick([self](int x, int y, const QPoint &g) { if (self) self->pickAt(x, y, g); });
        lApplyStatus->setText(tr_("Click a sprite or a background in the game window (right click: cancel)."));
    } else {
        w->setScreenPick(nullptr);
    }
}

void UIDebugGfxEditor::pickAt(int x, int y, const QPoint &globalPos)
{
    {
        QSignalBlocker b(pbPick);
        pbPick->setChecked(false);
    }
    if (UIYabause *w = QtYabause::mainWindow(false)) w->setScreenPick(nullptr);
    raise();
    activateWindow();
    if (x < 0 || !emuReady()) return;

    struct Cand { QString text; int kind; int layer; u32 cmd; int sx, sy, prio; };
    std::vector<Cand> cands;

    // VDP1: commands whose drawn area holds the dot, last drawn first. The
    // frame buffer is half as wide as a high resolution VDP2 screen unless
    // the VDP1 itself is in high resolution (TVMR), and half as high in
    // double density unless DIE is set (VDP1 manual ST-013, TVMR / FBCR).
    {
        int fx = x, fy = y;
        if ((Vdp2Regs->TVMD & 0x2) && Vdp1Regs && (Vdp1Regs->TVMR & 0x7) != 1) fx /= 2;
        if (((Vdp2Regs->TVMD >> 6) & 3) == 3 && Vdp1Regs && !(Vdp1Regs->FBCR & 0x8)) fy /= 2;
        std::vector<u32> addrs(16384);
        const int n = Vdp1DebugGetCommandList(addrs.data(), (int)addrs.size());
        addrs.resize(n > 0 ? n : 0);
        std::vector<int> boxes(addrs.size() * 4, 0);
        int fw = 0, fh = 0;
        u32 *fb = addrs.empty() ? NULL : Vdp1DebugRenderFrame(addrs.data(), (int)addrs.size(), &fw, &fh, boxes.data());
        free(fb);
        int found = 0;
        for (int i = (int)addrs.size() - 1; i >= 0 && found < 4; i--) {
            const int *b = &boxes[4 * i];
            if (b[2] < b[0] || fx < b[0] || fx > b[2] || fy < b[1] || fy > b[3]) continue;
            VdpEditSprite s;
            if (VdpEditSpriteFromCmd(NULL, addrs[i], &s) != 0) continue;
            int w = 0, h = 0, mode = 0;
            u32 a = 0;
            VdpEditSpriteGeometry(&s, &w, &h, &mode, &a, NULL);
            Cand c;
            c.text = tr_("Sprite %1 x %2 (command #%3 at %4, texture %5)").arg(w).arg(h).arg(i).arg(hex(addrs[i], 5)).arg(hex(a, 5));
            c.kind = 0; c.layer = -1; c.cmd = addrs[i]; c.sx = c.sy = 0; c.prio = 8;
            cands.push_back(c);
            found++;
        }
    }
    // VDP2 NBG0-3: decoded from the line registers by Vdp2DebugPixelSource
    // (vdp2debug.c): scroll, line scroll and zoom are taken into account.
    bool rbgOn = false;
    for (int n = 0; n < 4; n++) {
        VdpEditBg bg;
        if (VdpEditBgGet(n, &bg) != 0) continue;
        const u16 pri = (n < 2) ? Vdp2Regs->PRINA : Vdp2Regs->PRINB;
        const int prio = (pri >> ((n & 1) * 8)) & 7;
        if (prio == 0) continue;
        vdp2debugpixel_struct d;
        if (!Vdp2DebugPixelSource((u32)n, x, y, 0, &d) || !d.valid || d.transparent) continue;
        Cand c;
        static const char *names[4] = { "NBG0", "NBG1", "NBG2", "NBG3" };
        if (d.isbitmap)
            c.text = tr_("%1 bitmap dot (%2, %3), priority %4").arg(QString::fromLatin1(names[n])).arg(d.srcx).arg(d.srcy).arg(prio);
        else
            c.text = tr_("%1 cell (%2, %3), character %4, priority %5").arg(QString::fromLatin1(names[n]))
                         .arg(d.srcx / bg.csize).arg(d.srcy / bg.csize).arg(hex((u32)d.charnum * 0x20, 5)).arg(prio);
        c.kind = 1; c.layer = n; c.cmd = 0; c.sx = d.srcx; c.sy = d.srcy; c.prio = prio;
        cands.push_back(c);
    }
    for (int n = VDPEDIT_RBG0; n < VDPEDIT_LAYERS; n++) {
        VdpEditBg bg;
        if (VdpEditBgGet(n, &bg) == 0) rbgOn = true;
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.prio > b.prio; });

    if (cands.empty()) {
        lApplyStatus->setText(tr_("Nothing editable at (%1, %2).").arg(x).arg(y) +
                              (rbgOn ? " " + tr_("Rotating backgrounds (RBG0/RBG1) cannot be picked on screen: use their map.") : QString()));
        return;
    }
    int chosen = 0;
    if (cands.size() > 1) {
        QMenu menu(this);
        menu.addSection(tr_("At (%1, %2)").arg(x).arg(y));
        for (size_t i = 0; i < cands.size(); i++) menu.addAction(cands[i].text)->setData((int)i);
        if (rbgOn) menu.addAction(tr_("(RBG0/RBG1 cannot be picked on screen)"))->setEnabled(false);
        QAction *a = menu.exec(globalPos);
        if (!a) return;
        chosen = a->data().toInt();
    }
    const Cand &c = cands[(size_t)chosen];
    if (c.kind == 0) {
        openSpriteCommand(c.cmd);
    } else {
        openLayer(c.layer);
        if (mOverviewIsMap) {
            if (mOverviewBg.bitmap) openBitmap(mOverviewBg, c.sx, c.sy);
            else openCells(mOverviewBg, c.sx, c.sy);
        }
    }
    lApplyStatus->setText(tr_("Picked: %1").arg(c.text));
}

// ===========================================================================
// Folder watch (hot reload)
// ===========================================================================

void UIDebugGfxEditor::stampFolder(const QString &dir)
{
    mWatchDir = QDir(dir).absolutePath();
    QDir d(mWatchDir);
    const QFileInfoList files = d.entryInfoList(QStringList() << "*.png" << "*.json", QDir::Files);
    for (int i = 0; i < files.size(); i++) mStamps[files[i].absoluteFilePath()] = files[i].lastModified();
    if (cbWatch) {
        cbWatch->setEnabled(true);
        cbWatch->setToolTip(tr_("Hot reload of %1: every PNG saved there is imported at once").arg(QDir::toNativeSeparators(mWatchDir)));
        if (cbWatch->isChecked()) watchFolder(mWatchDir);
    }
}

bool UIDebugGfxEditor::fileChangedSinceStamp(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return false;
    const QDateTime t = fi.lastModified();
    if (mStamps.contains(path) && mStamps.value(path) == t) return false;
    mStamps[path] = t;
    return true;
}

void UIDebugGfxEditor::watchFolder(const QString &dir)
{
    if (!mWatcher) {
        mWatcher = new QFileSystemWatcher(this);
        connect(mWatcher, SIGNAL(directoryChanged(QString)), mWatchTimer, SLOT(start()));
        connect(mWatcher, SIGNAL(fileChanged(QString)), mWatchTimer, SLOT(start()));
    }
    if (!mWatcher->files().isEmpty()) mWatcher->removePaths(mWatcher->files());
    if (!mWatcher->directories().isEmpty()) mWatcher->removePaths(mWatcher->directories());
    mWatcher->addPath(dir);
    // image editors often save to a temporary file and rename it: the watch
    // of the folder catches that, the watch of the files catches in-place saves
    const QStringList pngs = QDir(dir).entryList(QStringList() << "*.png", QDir::Files);
    QStringList paths;
    for (int i = 0; i < pngs.size(); i++) paths << QDir(dir).filePath(pngs[i]);
    if (!paths.isEmpty()) mWatcher->addPaths(paths);
}

void UIDebugGfxEditor::onWatchToggled(bool on)
{
    if (!on) {
        delete mWatcher;
        mWatcher = nullptr;
        return;
    }
    if (mWatchDir.isEmpty()) {
        const QString d = CommonDialogs::getExistingDirectory(QString(), tr_("Folder to watch (exported sprites, atlas or tile sheet)"));
        if (d.isEmpty()) { QSignalBlocker b(cbWatch); cbWatch->setChecked(false); return; }
        stampFolder(d);
    }
    watchFolder(mWatchDir);
    lApplyStatus->setText(tr_("Watching %1: save a PNG there to see it in the game.").arg(QDir::toNativeSeparators(mWatchDir)));
}

void UIDebugGfxEditor::onWatchEvent()
{
    if (mWatchDir.isEmpty() || !emuReady()) return;
    const QString msg = importChangedInFolder(mWatchDir);
    if (mWatcher) watchFolder(mWatchDir);   // renamed files must be watched again
    if (msg.isEmpty()) return;
    lApplyStatus->setText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + msg);
    if (mItem.kind != Item::None && !mDirty) reloadItemFromMemory();
}

QString UIDebugGfxEditor::importChangedInFolder(const QString &dir)
{
    QStringList out;
    QDir d(dir);
    // sprites of index.csv: importSpriteFolder() compares each PNG with its stamp
    if (d.exists("index.csv")) {
        bool any = false;
        const QStringList pngs = d.entryList(QStringList() << "spr_*.png", QDir::Files);
        for (int i = 0; i < pngs.size() && !any; i++) {
            const QString p = d.absoluteFilePath(pngs[i]);
            const QFileInfo fi(p);
            any = !mStamps.contains(p) || mStamps.value(p) != fi.lastModified();
        }
        if (any) out << importSpriteFolder(dir).section('\n', 0, 0);
    }
    const QStringList atlases = d.entryList(QStringList() << "*.atlas.json", QDir::Files);
    for (int i = 0; i < atlases.size(); i++) {
        const QString png = d.absoluteFilePath(QString(atlases[i]).replace(".atlas.json", ".atlas.png"));
        if (fileChangedSinceStamp(png)) out << importAtlas(d.absoluteFilePath(atlases[i])).section('\n', 0, 0);
    }
    const QStringList sheets = d.entryList(QStringList() << "*.tiles.json", QDir::Files);
    for (int i = 0; i < sheets.size(); i++) {
        const QString png = d.absoluteFilePath(QString(sheets[i]).replace(".tiles.json", ".tiles.png"));
        if (fileChangedSinceStamp(png)) out << importTileSheet(d.absoluteFilePath(sheets[i])).section('\n', 0, 0);
    }
    return out.join("  |  ");
}

// ===========================================================================
// Animation atlas
// ===========================================================================

void UIDebugGfxEditor::onSelectSamePalette()
{
    const int row = lwSprites->currentRow();
    if (row < 0 || row >= (int)mShown.size() || (size_t)mShown[row] >= mSprites.size()) return;
    const SpriteEntry &ref = mSprites[mShown[row]];
    const QVector<QRgb> refTable = spriteColorTable(ref.spr, ref.lut);
    QSignalBlocker b(lwSprites);
    int n = 0;
    for (size_t r = 0; r < mShown.size() && r < (size_t)lwSprites->count(); r++) {
        if ((size_t)mShown[r] >= mSprites.size()) continue;
        const SpriteEntry &e = mSprites[mShown[r]];
        const bool same = ((e.spr.CMDPMOD ^ ref.spr.CMDPMOD) & 0x38) == 0 && spriteColorTable(e.spr, e.lut) == refTable;
        lwSprites->item((int)r)->setSelected(same);
        if (same) n++;
    }
    lApplyStatus->setText(tr_("%1 sprite(s) use this palette.").arg(n));
    updateActionState();
}

void UIDebugGfxEditor::onSelect4bppCandidates()
{
    QSignalBlocker b(lwSprites);
    lwSprites->clearSelection();
    int n = 0, wide = 0, saved = 0;
    for (size_t r = 0; r < mShown.size() && r < (size_t)lwSprites->count(); r++) {
        if ((size_t)mShown[r] >= mSprites.size()) continue;
        const SpriteEntry &e = mSprites[mShown[r]];
        const int mode = (e.spr.CMDPMOD >> 3) & 7;
        if (mode < 2 || mode > 4) continue;
        wide++;
        if (spriteColorUse(e.spr, spriteCodes(e)).to4 != 1) continue;
        lwSprites->item((int)r)->setSelected(true);
        n++;
        saved += e.w * e.h / 2;
    }
    lApplyStatus->setText(tr_("%1 of %2 sprite(s) in 64/128/256 colors could be 4 bpp sprites (%3 bytes of VDP1 VRAM less).")
                              .arg(n).arg(wide).arg(saved));
    updateActionState();
}

void UIDebugGfxEditor::onExportAtlas()
{
    const QList<QListWidgetItem *> sel = lwSprites->selectedItems();
    if (sel.isEmpty()) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, tr_("Export atlas"), tr_("Name of the atlas (for example the character):"),
                                         QLineEdit::Normal, QString("character"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    name.replace(QRegularExpression(QString::fromLatin1("[\\\\/:*?\"<>|. ]")), "_");
    const QString dir = CommonDialogs::getExistingDirectory(mWatchDir, tr_("Folder of the atlas"));
    if (dir.isEmpty()) return;
    const int n = exportAtlas(dir, name);
    if (n < 0) CommonDialogs::error(tr_("An error occured while writing file."));
    else lApplyStatus->setText(tr_("Atlas %1: %2 frame(s) written to %3. Edit %1.atlas.png, then \"Import atlas...\" or watch the folder.")
                                   .arg(name).arg(n).arg(QDir::toNativeSeparators(dir)));
}

int UIDebugGfxEditor::exportAtlas(const QString &dir, const QString &name)
{
    std::vector<int> frames;
    for (int r = 0; r < lwSprites->count() && r < (int)mShown.size(); r++)
        if (lwSprites->item(r)->isSelected() && (size_t)mShown[r] < mSprites.size()) frames.push_back(mShown[r]);
    if (frames.empty()) return -1;

    // one palette for every frame -> indexed sheet
    bool indexed = true;
    const QVector<QRgb> table = spriteColorTable(mSprites[frames[0]].spr, mSprites[frames[0]].lut);
    for (size_t i = 0; i < frames.size() && indexed; i++) {
        const SpriteEntry &e = mSprites[frames[i]];
        if (((e.spr.CMDPMOD >> 3) & 7) > 4 || spriteColorTable(e.spr, e.lut) != table) indexed = false;
    }

    // rows of frames, 2-dot gaps, at most 1024 dots wide
    const int gap = 2;
    int maxW = 0;
    for (size_t i = 0; i < frames.size(); i++) maxW = qMax(maxW, mSprites[frames[i]].w);
    const int limit = qMax(1024, maxW);
    std::vector<QPoint> pos(frames.size());
    int x = 0, y = 0, rowH = 0, W = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        const SpriteEntry &e = mSprites[frames[i]];
        if (x > 0 && x + e.w > limit) { x = 0; y += rowH + gap; rowH = 0; }
        pos[i] = QPoint(x, y);
        x += e.w + gap;
        rowH = qMax(rowH, e.h);
        W = qMax(W, x - gap);
    }
    const int H = y + rowH;

    QImage sheet;
    if (indexed) {
        sheet = QImage(W, H, QImage::Format_Indexed8);
        sheet.setColorTable(table);
        sheet.fill(0);
    } else {
        sheet = QImage(W, H, QImage::Format_ARGB32);
        sheet.fill(0);
    }
    QByteArray raw;
    QJsonArray jframes;
    for (size_t i = 0; i < frames.size(); i++) {
        const SpriteEntry &e = mSprites[frames[i]];
        int mode = 0;
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&e.spr, NULL, NULL, &mode, &addr, &bytes);
        if (indexed) {
            std::vector<u32> codes((size_t)e.w * e.h, 0);
            QByteArray tex = e.tex;
            const int need = (mode <= 1) ? e.w * e.h / 2 : e.w * e.h;
            if (tex.size() < need) tex.append(QByteArray(need - tex.size(), 0));
            VdpEditSpriteDecode(mode, (const u8 *)tex.constData(), e.w, e.h, codes.data());
            codesToIndexed(codes, e.w, e.h, table, -1, pos[i].x(), pos[i].y(), &sheet);
        } else {
            QPainter p(&sheet);
            p.drawImage(pos[i], spriteThumb(e));
        }
        QJsonObject f;
        f["name"] = QString("%1_%2").arg(name).arg(i, 3, 10, QChar('0'));
        f["x"] = pos[i].x(); f["y"] = pos[i].y(); f["w"] = e.w; f["h"] = e.h;
        f["cmdpmod"] = hex(e.spr.CMDPMOD, 4); f["cmdcolr"] = hex(e.spr.CMDCOLR, 4);
        f["cmdsrca"] = hex(e.spr.CMDSRCA, 4); f["cmdsize"] = hex(e.spr.CMDSIZE, 4);
        f["vram"] = hex(addr, 5);
        f["raw"] = raw.size(); f["rawSize"] = e.tex.size();
        raw.append(e.tex);
        if (e.lut.size() == 32) { f["lut"] = raw.size(); raw.append(e.lut); }
        else f["lut"] = -1;
        jframes.append(f);
    }
    QJsonObject root;
    root["type"] = "kronos-atlas";
    root["version"] = 1;
    root["name"] = name;
    root["png"] = name + ".atlas.png";
    root["raw"] = name + ".atlas.raw";
    root["indexed"] = indexed;
    root["product"] = UIEditsExport::currentProduct();
    root["frames"] = jframes;
    QDir().mkpath(dir);
    QFile jf(QDir(dir).filePath(name + ".atlas.json")), rf(QDir(dir).filePath(name + ".atlas.raw"));
    if (!sheet.save(QDir(dir).filePath(name + ".atlas.png"))) return -1;
    if (!rf.open(QIODevice::WriteOnly) || rf.write(raw) != raw.size()) return -1;
    rf.close();
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return -1;
    jf.write(QJsonDocument(root).toJson());
    jf.close();
    stampFolder(dir);
    return (int)frames.size();
}

void UIDebugGfxEditor::onImportAtlas()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Import atlas"), mWatchDir, "Kronos atlas (*.atlas.json)");
    if (f.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString msg = importAtlas(f);
    QApplication::restoreOverrideCursor();
    QMessageBox::information(this, windowTitle(), msg);
    if (mItem.kind != Item::None && !mDirty) reloadItemFromMemory();
}

QString UIDebugGfxEditor::importAtlas(const QString &jsonFile)
{
    QFile jf(jsonFile);
    if (!jf.open(QIODevice::ReadOnly)) return tr_("%1: cannot be read.").arg(jsonFile);
    const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
    if (root["type"].toString() != "kronos-atlas") return tr_("%1 is not an atlas of this editor.").arg(jsonFile);
    const QDir d = QFileInfo(jsonFile).absoluteDir();
    const QString name = root["name"].toString();
    const QImage sheet(d.filePath(root["png"].toString()));
    QFile rf(d.filePath(root["raw"].toString()));
    if (sheet.isNull() || !rf.open(QIODevice::ReadOnly)) return tr_("Atlas %1: picture or .raw missing.").arg(name);
    const QByteArray raw = rf.readAll();
    mStamps[d.absoluteFilePath(root["png"].toString())] = QFileInfo(d.filePath(root["png"].toString())).lastModified();
    const QImage pic = (sheet.format() == QImage::Format_Indexed8) ? sheet : sheet.convertToFormat(QImage::Format_ARGB32);
    int imported = 0, unchanged = 0, failed = 0, pending = 0, approx = 0;
    const QJsonArray frames = root["frames"].toArray();
    for (int i = 0; i < frames.size(); i++) {
        const QJsonObject f = frames[i].toObject();
        VdpEditSprite s;
        memset(&s, 0, sizeof(s));
        bool o1, o2, o3, o4;
        s.CMDPMOD = (u16)f["cmdpmod"].toString().toUInt(&o1, 16);
        s.CMDCOLR = (u16)f["cmdcolr"].toString().toUInt(&o2, 16);
        s.CMDSRCA = (u16)f["cmdsrca"].toString().toUInt(&o3, 16);
        s.CMDSIZE = (u16)f["cmdsize"].toString().toUInt(&o4, 16);
        int w = 0, h = 0, mode = 0;
        u32 addr = 0, bytes = 0;
        VdpEditSpriteGeometry(&s, &w, &h, &mode, &addr, &bytes);
        const int x = f["x"].toInt(), y = f["y"].toInt(), off = f["raw"].toInt(), lutOff = f["lut"].toInt(-1);
        if (!(o1 && o2 && o3 && o4) || x + w > pic.width() || y + h > pic.height() || off + (int)bytes > raw.size()) { failed++; continue; }
        const QByteArray orig = raw.mid(off, (int)bytes);
        const QByteArray lut = (lutOff >= 0 && lutOff + 32 <= raw.size()) ? raw.mid(lutOff, 32) : QByteArray();
        std::vector<u32> codes = spriteCodesFromPicture(s, lut, orig, pic, x, y, &approx, NULL);
        const int need = (mode <= 1) ? w * h / 2 : (mode <= 4) ? w * h : w * h * 2;
        QByteArray full = orig;
        if (full.size() < need) full.append(QByteArray(need - full.size(), 0));
        VdpEditSpriteEncode(mode, codes.data(), w, h, (u8 *)full.data());
        const QByteArray repl = full.left((int)bytes);
        if (repl == orig) { unchanged++; continue; }
        char meta[VDPEDIT_META_SIZE];
        VdpEditSpriteMeta(&s, meta);
        u32 id = 0;
        const int rc = VdpEditWriteEx(VDPEDIT_VDP1, addr, (const u8 *)orig.constData(), (const u8 *)repl.constData(),
                                      bytes, meta, NULL, &id);
        if (rc < 0) { failed++; continue; }
        if (id) VdpEditSetName(id, f["name"].toString().toUtf8().constData());
        imported++;
        if (rc == 0) pending++;
    }
    QString msg = tr_("Atlas %1: %2 frame(s) imported (%3 waiting for the game to load them), %4 unchanged, %5 failed.")
                      .arg(name).arg(imported).arg(pending).arg(unchanged).arg(failed);
    if (approx) msg += "\n" + tr_("%1 dot(s) got the nearest palette color.").arg(approx);
    if (cbSource->currentIndex() <= SrcSpriteLayer) refreshSource(true);
    updateChips();
    updateActionState();
    return msg;
}

// ===========================================================================
// VDP2 tile sheet
// ===========================================================================

void UIDebugGfxEditor::onExportTiles()
{
    const int src = cbSource->currentIndex();
    if (src < SrcNBG0) return;
    const QString dir = CommonDialogs::getExistingDirectory(mWatchDir, tr_("Folder of the tile sheet"));
    if (dir.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int n = exportTileSheet(dir, kSourceLayer[src]);
    QApplication::restoreOverrideCursor();
    if (n < 0) CommonDialogs::error(tr_("The layer is off, in bitmap mode, or the files could not be written."));
    else lApplyStatus->setText(tr_("%1 character(s) written to %2.").arg(n).arg(QDir::toNativeSeparators(dir)));
}

int UIDebugGfxEditor::exportTileSheet(const QString &dir, int layer)
{
    static const char *names[VDPEDIT_LAYERS] = { "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG0B", "RBG1" };
    VdpEditBg bg;
    if (VdpEditBgGet(layer, &bg) != 0 || bg.bitmap) return -1;
    struct Tile { u32 addr, pal; int uses; };
    std::vector<Tile> tiles;
    std::map<u32, int> byAddr;
    for (int y = 0; y < bg.mapH; y += bg.csize)
        for (int x = 0; x < bg.mapW; x += bg.csize) {
            VdpEditCell c;
            if (VdpEditBgCellAt(&bg, x, y, &c) != 0) continue;
            std::map<u32, int>::iterator it = byAddr.find(c.charAddr);
            if (it != byAddr.end()) { tiles[it->second].uses++; continue; }
            if (tiles.size() >= 4096) continue;
            byAddr[c.charAddr] = (int)tiles.size();
            Tile t; t.addr = c.charAddr; t.pal = c.palBase; t.uses = 1;
            tiles.push_back(t);
        }
    if (tiles.empty()) return -1;
    std::sort(tiles.begin(), tiles.end(), [](const Tile &a, const Tile &b) { return a.addr < b.addr; });

    bool indexed = bg.cn <= 1;
    for (size_t i = 1; i < tiles.size() && indexed; i++) if (tiles[i].pal != tiles[0].pal) indexed = false;
    const int cs = bg.csize, cols = 16, rows = ((int)tiles.size() + cols - 1) / cols;
    const u32 charBytes = (u32)(bg.pw * bg.pw) * ((bg.cn == 0) ? 0x20u : (bg.cn == 1) ? 0x40u : (bg.cn == 4) ? 0x100u : 0x80u);
    QImage sheet;
    QVector<QRgb> table;
    if (indexed) {
        table = bgColorTable(bg, tiles[0].pal);
        sheet = QImage(cols * cs, rows * cs, QImage::Format_Indexed8);
        sheet.setColorTable(table);
        sheet.fill(0);
    } else {
        sheet = QImage(cols * cs, rows * cs, QImage::Format_ARGB32);
        sheet.fill(0);
    }
    QByteArray raw;
    QJsonArray jt;
    for (size_t i = 0; i < tiles.size(); i++) {
        QByteArray b((int)charBytes, 0);
        VdpEditReadVdp2(tiles[i].addr, (u8 *)b.data(), charBytes);
        std::vector<u32> codes((size_t)cs * cs);
        VdpEditCharDecode(&bg, (const u8 *)b.constData(), codes.data());
        const int tx = ((int)i % cols) * cs, ty = ((int)i / cols) * cs;
        if (indexed) {
            codesToIndexed(codes, cs, cs, table, -1, tx, ty, &sheet);
        } else {
            for (int y = 0; y < cs; y++)
                for (int x = 0; x < cs; x++)
                    sheet.setPixel(tx + x, ty + y, VdpEditBgCodeArgb(&bg, tiles[i].pal, codes[(size_t)y * cs + x]));
        }
        QJsonObject t;
        t["addr"] = hex(tiles[i].addr, 5);
        t["pal"] = hex(tiles[i].pal, 2);
        t["x"] = tx; t["y"] = ty;
        t["raw"] = raw.size();
        t["uses"] = tiles[i].uses;
        raw.append(b);
        jt.append(t);
    }
    const QString base = QString::fromLatin1(names[layer]);
    QJsonObject root;
    root["type"] = "kronos-tiles";
    root["version"] = 1;
    root["layer"] = layer;
    root["name"] = base;
    root["cn"] = bg.cn; root["pw"] = bg.pw; root["coff"] = hex(bg.coff, 3); root["tpon"] = bg.tpon;
    root["charBytes"] = (int)charBytes;
    root["indexed"] = indexed;
    root["png"] = base + ".tiles.png";
    root["raw"] = base + ".tiles.raw";
    root["product"] = UIEditsExport::currentProduct();
    root["tiles"] = jt;
    QDir().mkpath(dir);
    QFile jf(QDir(dir).filePath(base + ".tiles.json")), rf(QDir(dir).filePath(base + ".tiles.raw"));
    if (!sheet.save(QDir(dir).filePath(base + ".tiles.png"))) return -1;
    if (!rf.open(QIODevice::WriteOnly) || rf.write(raw) != raw.size()) return -1;
    rf.close();
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return -1;
    jf.write(QJsonDocument(root).toJson());
    jf.close();
    stampFolder(dir);
    return (int)tiles.size();
}

void UIDebugGfxEditor::onImportTiles()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Import tile sheet"), mWatchDir, "Kronos tile sheet (*.tiles.json)");
    if (f.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString msg = importTileSheet(f);
    QApplication::restoreOverrideCursor();
    QMessageBox::information(this, windowTitle(), msg);
    if (mItem.kind != Item::None && !mDirty) reloadItemFromMemory();
    refreshOverview();
}

QString UIDebugGfxEditor::importTileSheet(const QString &jsonFile)
{
    QFile jf(jsonFile);
    if (!jf.open(QIODevice::ReadOnly)) return tr_("%1: cannot be read.").arg(jsonFile);
    const QJsonObject root = QJsonDocument::fromJson(jf.readAll()).object();
    if (root["type"].toString() != "kronos-tiles") return tr_("%1 is not a tile sheet of this editor.").arg(jsonFile);
    const QDir d = QFileInfo(jsonFile).absoluteDir();
    const QString name = root["name"].toString();
    const QImage sheet(d.filePath(root["png"].toString()));
    QFile rf(d.filePath(root["raw"].toString()));
    if (sheet.isNull() || !rf.open(QIODevice::ReadOnly)) return tr_("Tile sheet %1: picture or .raw missing.").arg(name);
    const QByteArray raw = rf.readAll();
    mStamps[d.absoluteFilePath(root["png"].toString())] = QFileInfo(d.filePath(root["png"].toString())).lastModified();

    VdpEditBg bg;
    memset(&bg, 0, sizeof(bg));
    bg.layer = root["layer"].toInt();
    bg.cn = qBound(0, root["cn"].toInt(), 4);
    bg.pw = root["pw"].toInt() == 2 ? 2 : 1;
    bg.csize = 8 * bg.pw;
    bg.coff = root["coff"].toString().toUInt(NULL, 16);
    bg.tpon = root["tpon"].toInt();
    bg.enabled = 1;
    const int cs = bg.csize, charBytes = root["charBytes"].toInt();
    const int n = VdpEditBgPaletteSize(bg.cn);
    const bool isIndexed = sheet.format() == QImage::Format_Indexed8 && n > 0 && n <= 256;
    const QImage pic = isIndexed ? sheet : sheet.convertToFormat(QImage::Format_ARGB32);
    const QVector<QRgb> table = pic.colorTable();
    int imported = 0, unchanged = 0, failed = 0, pending = 0, approx = 0;
    const QJsonArray tiles = root["tiles"].toArray();
    for (int i = 0; i < tiles.size(); i++) {
        const QJsonObject t = tiles[i].toObject();
        const u32 addr = t["addr"].toString().toUInt(NULL, 16), pal = t["pal"].toString().toUInt(NULL, 16);
        const int tx = t["x"].toInt(), ty = t["y"].toInt(), off = t["raw"].toInt();
        if (tx + cs > pic.width() || ty + cs > pic.height() || off + charBytes > raw.size() || charBytes <= 0) { failed++; continue; }
        const QByteArray orig = raw.mid(off, charBytes);
        std::vector<u32> old((size_t)cs * cs), codes((size_t)cs * cs);
        VdpEditCharDecode(&bg, (const u8 *)orig.constData(), old.data());
        for (int y = 0; y < cs; y++)
            for (int x = 0; x < cs; x++) {
                const size_t k = (size_t)y * cs + x;
                if (isIndexed) {
                    const int idx = pic.constScanLine(ty + y)[tx + x];
                    codes[k] = (idx < n) ? (u32)idx : bgNearest(bg, pal, idx < table.size() ? table[idx] : 0, &approx);
                } else {
                    const QRgb p = pic.pixel(tx + x, ty + y);
                    codes[k] = ((QRgb)VdpEditBgCodeArgb(&bg, pal, old[k]) == p) ? old[k] : bgNearest(bg, pal, p, &approx);
                }
            }
        QByteArray repl = orig;
        VdpEditCharEncode(&bg, codes.data(), (u8 *)repl.data());
        if (repl == orig) { unchanged++; continue; }
        char meta[VDPEDIT_META_SIZE];
        VdpEditCharMeta(&bg, pal, meta);
        const int rc = VdpEditWriteEx(VDPEDIT_VDP2, addr, (const u8 *)orig.constData(), (const u8 *)repl.constData(),
                                      (u32)charBytes, meta, NULL, NULL);
        if (rc < 0) { failed++; continue; }
        imported++;
        if (rc == 0) pending++;
    }
    QString msg = tr_("Tile sheet %1: %2 character(s) imported (%3 waiting for the game to load them), %4 unchanged, %5 failed.")
                      .arg(name).arg(imported).arg(pending).arg(unchanged).arg(failed);
    if (approx) msg += "\n" + tr_("%1 dot(s) got the nearest palette color.").arg(approx);
    updateChips();
    updateActionState();
    return msg;
}
