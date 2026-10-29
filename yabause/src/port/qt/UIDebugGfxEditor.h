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

/*  Graphics editor: draws directly in the VDP1 sprites and in the VDP2
    backgrounds (NBG0-3, RBG0/RBG1, cells or bitmap) of the running game.

    The picture is edited in the game's own codes (palette indices, or RGB
    words for the direct color formats), so nothing is lost or quantized:
    what is written back to VRAM is exactly what was drawn. Palette colors are
    edited in color RAM (or in the VDP1 lookup table). Every change goes
    through vdpedit.c, which keeps it alive while the game runs and which the
    edit pack / disc patch export (UIEditsExport) reads. */

#ifndef UIDEBUGGFXEDITOR_H
#define UIDEBUGGFXEDITOR_H

#include <QDialog>
#include <QImage>
#include <QVector>
#include <QByteArray>
#include <QPointer>
#include <functional>
#include <vector>

#include "../QtYabause.h"

extern "C" {
#include "vdpedit.h"
}

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class YabauseThread;

// ---------------------------------------------------------------------------
// Pixel canvas: edits a w x h array of codes. The owner turns codes into
// colors (render callback), so the canvas knows nothing about the VDPs.
// ---------------------------------------------------------------------------
class GfxCanvas : public QWidget
{
    Q_OBJECT
public:
    enum Tool { Pencil, Line, Rect, FillRect, Fill, Picker, Eraser };

    explicit GfxCanvas(QWidget *parent = 0);

    void setPicture(int w, int h, const std::vector<u32> &codes);
    void clearPicture();
    const std::vector<u32> &codes() const { return mCodes; }
    void replaceCodes(const std::vector<u32> &codes, bool undoable);
    int picWidth() const { return mW; }
    int picHeight() const { return mH; }
    bool hasPicture() const { return mW > 0 && mH > 0; }

    void setRenderer(std::function<QImage(const std::vector<u32> &)> r) { mRender = r; }
    void rerender();
    QImage image() const { return mImage; }

    void setTool(Tool t) { mTool = t; }
    void setCodes(u32 primary, u32 secondary) { mPrimary = primary; mSecondary = secondary; }
    void setTransparentCode(u32 c) { mTransparent = c; }
    void setBrush(int n) { mBrush = n < 1 ? 1 : n; }
    void setZoom(int z);
    int  zoom() const { return mZoom; }
    void setGrid(bool g) { mGrid = g; update(); }
    void setCellGrid(int n) { mCellGrid = n; update(); }
    void setHighlight(const QRect &r) { mHighlight = r; update(); }

    bool canUndo() const { return !mUndo.empty(); }
    bool canRedo() const { return !mRedo.empty(); }
    void undo();
    void redo();
    void clearHistory() { mUndo.clear(); mRedo.clear(); emit historyChanged(); }

    QSize sizeHint() const override;

signals:
    void edited();                       // after every completed stroke
    void hovered(int x, int y);          // -1, -1 when leaving
    void picked(u32 code, bool primary); // picker tool
    void historyChanged();
    void zoomRequest(int delta, QPoint at);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    QPoint toPic(const QPoint &p) const;
    void plot(int x, int y, u32 c);
    void line(QPoint a, QPoint b, u32 c);
    void rect(QPoint a, QPoint b, u32 c, bool filled);
    void flood(QPoint p, u32 c);
    void beginStroke();
    void endStroke();

    int mW = 0, mH = 0;
    std::vector<u32> mCodes, mStrokeStart;
    std::vector<std::vector<u32> > mUndo, mRedo;
    QImage mImage;
    std::function<QImage(const std::vector<u32> &)> mRender;
    Tool mTool = Pencil;
    u32 mPrimary = 1, mSecondary = 0, mTransparent = 0, mStrokeCode = 0;
    int mBrush = 1, mZoom = 8, mCellGrid = 0;
    bool mGrid = true, mDrawing = false;
    QPoint mAnchor, mLast = QPoint(-1, -1), mHover = QPoint(-1, -1);
    QRect mHighlight;
};

// ---------------------------------------------------------------------------
// Palette swatches. Click = choose (left primary / right secondary),
// double-click = change the color.
// ---------------------------------------------------------------------------
class GfxPalette : public QWidget
{
    Q_OBJECT
public:
    explicit GfxPalette(QWidget *parent = 0);
    void setEntries(const QVector<QRgb> &colors, const QVector<u32> &codes,
                    const QVector<QString> &tips, int columns);
    void setSelection(u32 primary, u32 secondary) { mPrimary = primary; mSecondary = secondary; update(); }
    int  entryCount() const { return mColors.size(); }
    QSize sizeHint() const override;
    int heightForWidth(int w) const override;
    bool hasHeightForWidth() const override { return true; }

signals:
    void chosen(u32 code, bool primary);
    void editRequested(int entry);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    bool event(QEvent *) override;

private:
    int entryAt(const QPoint &p) const;
    int cell() const;
    QVector<QRgb> mColors;
    QVector<u32> mCodes;
    QVector<QString> mTips;
    int mColumns = 16;
    u32 mPrimary = 1, mSecondary = 0;
};

// ---------------------------------------------------------------------------
// The editor dialog
// ---------------------------------------------------------------------------
class UIDebugGfxEditor : public QDialog
{
    Q_OBJECT
public:
    enum Source { SrcSprites = 0, SrcSpriteLayer, SrcNBG0, SrcNBG1, SrcNBG2, SrcNBG3,
                  SrcRBG0, SrcRBG0B, SrcRBG1, SrcCount };

    explicit UIDebugGfxEditor(QWidget *parent = 0);
    ~UIDebugGfxEditor();

    // Opens on the sprite drawn by the command at cmdAddr of the VDP1 debug
    // memory, or on a VDP2 layer.
    void openSpriteCommand(u32 cmdAddr);
    void openLayer(int vdpeditLayer);

    // Sprite list <-> folder of PNG files (+ .raw originals and index.csv),
    // for editing in another program.
    int exportSprites(const QString &dir);
    QString importSpriteFolder(const QString &dir);

public slots:
    void done(int r) override;

protected:
    void closeEvent(QCloseEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private slots:
    void onSourceChanged(int);
    void onSpriteSelected();
    void onFilterChanged(const QString &);
    void onTick();
    void onRunClicked();
    void onStepClicked();
    void onRefreshClicked();
    void onApplyClicked();
    void onRevertItemClicked();
    void onRevertAllClicked();
    void onImportPng();
    void onExportPng();
    void onExportAllSprites();
    void onImportSpriteFolder();
    void onEditsDialog();
    void onCanvasEdited();
    void onCanvasHover(int x, int y);
    void onPicked(u32 code, bool primary);
    void onPaletteChosen(u32 code, bool primary);
    void onPaletteEdit(int entry);
    void onCustomColor();
    void onZoom(int delta);
    void onRegionChanged(int);

private:
    // ---- item being edited ----
    struct Block {
        int space = VDPEDIT_VDP1;
        u32 addr = 0;
        QByteArray orig;      // bytes when the item was opened (true original if unedited)
        QByteArray applied;   // bytes last written / read
        std::vector<u32> codes;
        int kind = 0;         // 0 sprite, 1 character, 2 bitmap row
        int w = 0, h = 0;     // dots of the block (row: w x 1)
    };
    struct Item {
        enum Kind { None, Sprite, Cells, Bitmap } kind = None;
        QString title;
        int w = 0, h = 0;
        // sprite
        VdpEditSprite spr;
        int mode = 0;
        QByteArray lut;           // 32 bytes (LUT mode)
        QByteArray lutApplied;
        // VDP2
        VdpEditBg bg;
        int originX = 0, originY = 0;       // region position in the map
        std::vector<u32> cellPal;           // palette per cell location
        int cellsX = 0;
        std::vector<Block> blocks;
        std::vector<int> pixBlock, pixOff;  // canvas dot -> block / code index
        int activeCell = 0;                 // cell whose palette is shown
    };

    // ---- VDP1 sprite list (this frame or library) ----
    struct SpriteEntry {
        VdpEditSprite spr;
        QByteArray tex, lut;
        quint64 key = 0;
        int w = 0, h = 0, uses = 1;
        u32 cmdAddr = 0xFFFFFFFF;  // in the frame it was last seen
        int frameBox[4] = { 0, -1, 0, -1 };
    };

    void buildUi();
    void applyStyle();
    QWidget *makePanel(const QString &name);
    QToolButton *makeTool(const QString &text, const QString &tip, int id);

    bool emuReady() const;
    YabauseThread *thread() const;
    void updateChips();
    void updateActionState();

    void refreshSource(bool keepSelection);
    void scanSprites();
    void rebuildSpriteList();
    QImage spriteThumb(const SpriteEntry &e) const;
    void refreshOverview();
    void fitOverview();
    void updateOverviewSelection();

    bool openSprite(const VdpEditSprite &spr, const QByteArray *tex, const QByteArray *lut, const QString &title);
    bool openCells(const VdpEditBg &bg, int x, int y);
    bool openBitmap(const VdpEditBg &bg, int x, int y);
    void finishOpen();
    void closeItem();
    void deriveCanvasCodes(std::vector<u32> &out) const;
    void storeCanvasCodes();
    QImage renderCodes(const std::vector<u32> &codes) const;
    QRgb codeColor(int dot, u32 code) const;
    void encodeBlock(const Block &b, QByteArray &out) const;
    int  applyItem();
    void reloadItemFromMemory();

    void refreshPalette();
    void refreshItemInfo();
    u32  transparentCode() const;
    bool itemIsRgb() const;
    u32  rgbToCode(QRgb c) const;
    u32  nearestCode(int dot, QRgb c) const;
    int  paletteSize() const;
    QRgb paletteColor(u32 code) const;
    int  paletteCram(u32 code, bool *lutRgb) const;
    void setPrimary(u32 c);
    void setSecondary(u32 c);
    void updateSwatches();

    // ---- widgets ----
    QLabel *lTitle = nullptr, *lChipSource = nullptr, *lChipEmu = nullptr, *lChipEdits = nullptr;
    QComboBox *cbSource = nullptr;
    QStackedWidget *stLeft = nullptr;
    QLineEdit *leFilter = nullptr;
    QListWidget *lwSprites = nullptr;
    QCheckBox *cbLibrary = nullptr, *cbLive = nullptr;
    QPushButton *pbClearLibrary = nullptr;
    QGraphicsView *gvOverview = nullptr;
    QGraphicsScene *mOverviewScene = nullptr;
    QGraphicsRectItem *mOverviewSel = nullptr;
    QLabel *lOverviewInfo = nullptr;
    QComboBox *cbRegion = nullptr;
    QLabel *lLayerInfo = nullptr;
    QPushButton *pbFitOverview = nullptr;

    GfxCanvas *mCanvas = nullptr;
    QScrollArea *mCanvasArea = nullptr;
    QButtonGroup *mTools = nullptr;
    QSpinBox *sbBrush = nullptr;
    QLabel *lZoom = nullptr, *lHover = nullptr, *lItemTitle = nullptr;
    QCheckBox *cbGrid = nullptr, *cbCellGrid = nullptr;
    QToolButton *tbUndo = nullptr, *tbRedo = nullptr;

    GfxPalette *mPalette = nullptr;
    QScrollArea *mPaletteArea = nullptr;
    QLabel *lPrimary = nullptr, *lSecondary = nullptr, *lPaletteInfo = nullptr;
    QPushButton *pbCustomColor = nullptr;
    QTreeWidget *twInfo = nullptr;
    QPushButton *pbApply = nullptr, *pbRevertItem = nullptr, *pbImportPng = nullptr, *pbExportPng = nullptr;
    QCheckBox *cbAutoApply = nullptr;
    QLabel *lApplyStatus = nullptr;

    QPushButton *pbRun = nullptr, *pbStep = nullptr, *pbRefresh = nullptr, *pbEdits = nullptr, *pbRevertAll = nullptr, *pbExportAll = nullptr, *pbImportFolder = nullptr;
    QLabel *lFootNote = nullptr;

    QTimer *mTimer = nullptr;

    // ---- state ----
    Item mItem;
    std::vector<u32> mDerived;     // canvas codes as derived from the blocks
    bool mDirty = false;           // canvas differs from what was applied
    u32 mPrimaryCode = 1, mSecondaryCode = 0;
    QVector<u32> mPalCodes;        // codes shown in the palette widget

    std::vector<SpriteEntry> mSprites;   // current list (frame or library)
    std::vector<int> mShown;             // list row -> mSprites index
    QImage mOverview;                    // VDP1 frame or VDP2 map
    VdpEditBg mOverviewBg;
    bool mOverviewIsMap = false;
    QSize mOverviewFitSize;
    bool mOverviewAutoFit = true;
    int mRegionCells = 2, mRegionBitmap = 2;   // remembered "edit area" choices
    std::vector<u32> mFrameAddrs;
    std::vector<int> mFrameBoxes;

    bool mUiReady = false;               // buildUi() finished: event filters may run
    bool mResumedEmulation = false;      // we un-paused the game: pause again on close
    u32 mLastGen = 0xFFFFFFFF;
    int mTickCount = 0;
};

#endif // UIDEBUGGFXEDITOR_H
