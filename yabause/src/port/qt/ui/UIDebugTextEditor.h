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

/*  UIDebugTextEditor -- text of the game for translation.

    - character table (.tbl): ASCII / Shift-JIS presets, loaded / saved,
      edited by hand or completed by a relative search;
    - search of the strings on the disc image (text through the table,
      relative search, hex bytes), string dump of a file or of the disc;
    - compressed data: LZSS (Okumura) and PRS blocks are unpacked, searched
      like files, and packed again when the image is written;
    - pointers: the base of the pointers of a file / block is guessed from the
      strings found, then each string gets the words that point to it. A
      translation longer than its original is moved to free space (given or
      found in the same file / block) and its pointers are rewritten;
    - table of strings with their translation, byte count, place (in place,
      moved, too long);
    - "Live in game": the translations are written into the work RAM each
      time the game loads the original text there (moved texts too, with
      32-bit pointers);
    - words drawn with graphics (title screens: VDP2 layer tiles, VDP1
      sprites) go to the graphics editor and its Text tool;
    - output: patched copy of the image, BPS or IPS patch (EditsPatcher,
      EDC/ECC regenerated), project file (.ktext), CSV for translators. */

#ifndef UIDEBUGTEXTEDITOR_H
#define UIDEBUGTEXTEDITOR_H

#include <QDialog>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>
#include <vector>
#include "../EditsPatcher.h"
#include "../TextTools.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class UIDebugTextEditor : public QDialog
{
    Q_OBJECT

public:
    explicit UIDebugTextEditor(QWidget *parent = 0);

    struct Str {
        QString id;
        int block;               // -1: on the disc, else index of the unpacked block
        quint64 offset;          // disc user data offset, or offset in the block
        quint32 length;          // text bytes (room for the translation in place)
        quint32 termLength;      // end code bytes after the text
        QByteArray orig;         // length + termLength bytes
        QString text;            // decoded original
        QString translation;
        bool enabled;
        QVector<quint64> ptrs;   // words pointing to the text (disc offsets, or block offsets when ptrsInBlock)
        bool ptrsInBlock;
        Str() : block(-1), offset(0), length(0), termLength(0), enabled(true), ptrsInBlock(false) {}
    };

    // Unpacked compressed data.
    struct Block {
        int file;                // index in the ISO file list, -1 = none
        quint64 packOffset;      // disc offset of the packed data
        quint32 packSize;        // packed bytes
        quint32 room;            // bytes the packed data may take again: packSize + the 00 bytes after it in the file
        int codec;               // TextTools::Codec
        QByteArray data;         // unpacked
        Block() : file(-1), packOffset(0), packSize(0), room(0), codec(0) {}
    };

    // Pointers and free space of one container (a file, a block, the disc).
    struct PtrSpec {
        bool on;
        int width;
        bool bigEndian;
        qint64 base;
        int where;               // -2: in the same file / block, >= 0: in this ISO file
        QString freeSpace;       // "1F00-1FFF, 3000-30FF" (offsets in the container)
        bool reuse;              // the room of a moved text becomes free space
        PtrSpec() : on(false), width(4), bigEndian(true), base(0), where(-2), reuse(false) {}
    };

    // Where each translation goes (in place, moved, refused).
    enum Place { PlaceNone = 0, PlaceInPlace, PlaceMoved, PlaceRefused };
    struct PlanItem {
        int place;
        std::vector<uint8_t> bytes;  // in place: length + termLength bytes; moved: text + end code
        quint64 newOffset;           // moved: offset in the container
        QString why;
        PlanItem() : place(PlaceNone), newOffset(0) {}
    };

    // ---- used by the tests too ----
    bool openImage(const QString &path, QString *err = 0);
    const TextTools::CharTable &table() const { return mTable; }
    void setTable(const TextTools::CharTable &t);
    bool loadTableFile(const QString &path, QString *err = 0);
    bool saveTableFile(const QString &path) const;
    static TextTools::CharTable shiftJisTable();
    int searchText(const QString &query, int mode, QString *msg = 0);   // mode: 0 text, 1 relative, 2 hex
    int dumpStrings(int minChars, bool requireEnd, QString *msg = 0);
    int applyRelativeHit(int row);
    int addStringAt(quint64 offset);
    int stringCount() const { return mStrings.size(); }
    Str &string(int i) { return mStrings[i]; }
    void setTranslation(int i, const QString &t);
    const PlanItem &plan(int i);
    bool replacement(const Str &s, std::vector<uint8_t> &out, QString *why = 0, int *overflow = 0) const;
    bool buildDisc(EditsPatcher::Build &build, int *refused, QString *msg);
    bool writePatchedImage(const QString &dir, QString *msg);
    bool saveProject(const QString &path) const;
    bool loadProject(const QString &path, QString *err = 0);
    bool exportCsv(const QString &path) const;
    int importCsv(const QString &path, QString *err = 0);
    int writeLive();     // translations written into the work RAM now
    void selectScope(int fileIndex);   // -1: whole disc
    void selectBlock(int block);
    // compressed data
    int unpackAt(int fileIndex, quint64 offsetInFile, int codec, QString *msg = 0);
    int findPrsBlocks(int fileIndex, QString *msg = 0);
    int blockCount() const { return mBlocks.size(); }
    const Block &block(int i) const { return mBlocks[i]; }
    // pointers of the container of the current scope
    QString scopeKey() const;
    // file / block of the Pointers tab: the one of the selected string, else the one chosen on the left
    QString ptrKey() const;
    PtrSpec &spec(const QString &key) { return mSpecs[key]; }
    std::vector<std::pair<int64_t, int> > guessBase(const QString &key, QString *msg = 0);
    // base = RAM address where the game has loaded the file / block (game running)
    std::vector<std::pair<int64_t, int> > baseFromRam(const QString &key, QString *msg = 0);
    int findPointers(const QString &key, QString *msg = 0);
    QString findFreeSpace(const QString &key, int minLen, QString *note = 0);

private slots:
    void onBrowseImage();
    void onScopeChanged();
    void onSearch();
    void onDump();
    void onAddAtOffset();
    void onRemoveStrings();
    void onStringSelected();
    void onStringItemChanged(QTreeWidgetItem *item, int column);
    void onTranslationEdited();
    void onAddRelative();
    void onRelSelected();
    void onPreset(int index);
    void onLoadTable();
    void onSaveTable();
    void onTableCellChanged(int row, int column);
    void onAddEntry();
    void onDeleteEntries();
    void onTestChanged();
    void onHexGo();
    void onUnpack();
    void onFindPrs();
    void onPtrSpecEdited();
    void onGuessBase();
    void onBaseFromRam();
    void onUseBase();
    void onFindPointers();
    void onFindFree();
    void onLiveToggled(bool on);
    void onLiveTick();
    void onSaveProject();
    void onLoadProject();
    void onExportCsv();
    void onImportCsv();
    void onWriteImage();
    void onWriteBps();
    void onWriteIps();
    void onGfxText();

private:
    void buildUi();
    void fillFiles();
    void fillTableWidget();
    void fillStrings();
    void fillPtrPanel();
    void updateStringRow(int i);
    void updateChips();
    void updateEditor();
    void refreshAll();
    void showHex(int block, quint64 offset);
    quint64 stringStart(const TextTools::Source &src, quint64 hit) const;
    bool scope(int &block, quint64 &start, quint64 &length, QString &name) const;
    QString fileName(quint64 offset) const;
    QString blockName(int b) const;
    QString containerKey(const Str &s) const;
    quint64 containerOffset(const Str &s) const;
    QString containerName(const QString &key) const;
    bool containerData(const QString &key, QByteArray &out, quint64 *discStart, QString *err) const;
    bool fileData(int fileIndex, QByteArray &out) const;
    quint32 blockRoom(const Block &bl) const;
    QString nextId();
    int indexOf(int block, quint64 offset) const;
    int appendString(int block, quint64 offset);
    void invalidatePlan();
    void computePlan();
    bool progress(double f);
    void startProgress(const QString &label);
    void endProgress();
    bool writePatches(int kind);   // 0 BPS, 1 IPS

    EditsPatcher::Image mImg;
    bool mImgOk;
    std::vector<TextTools::IsoFile> mFiles;
    TextTools::CharTable mTable;
    QVector<Str> mStrings;
    QVector<Block> mBlocks;
    QMap<QString, PtrSpec> mSpecs;
    mutable QHash<int, QByteArray> mFileCache;
    std::vector<PlanItem> mPlan;
    bool mPlanOk;
    std::vector<TextTools::RelHit> mRelHits;
    int mRelBlock;
    QString mRelQuery;
    int mNextId;
    bool mFilling;
    int mCurrent;
    class QProgressDialog *mProgress;

    QLabel *lChipImage, *lChipStrings, *lChipDone, *lChipLong;
    QLineEdit *leImage;
    QTreeWidget *twFiles;
    QComboBox *cbCodec;
    QLineEdit *lePackOffset;
    QTabWidget *tabs;
    QLineEdit *leQuery;
    QComboBox *cbMode;
    QSpinBox *sbMinChars;
    QCheckBox *cbRequireEnd;
    QTreeWidget *twStrings;
    QPlainTextEdit *teOriginal, *teTranslation;
    QLabel *lBytes;
    QTreeWidget *twRel;
    QPushButton *pbAddRel;
    QLineEdit *leHexOffset;
    QPlainTextEdit *teHex;
    // pointers tab
    QLabel *lPtrContainer;
    QCheckBox *cbPtrOn;
    QComboBox *cbPtrWidth, *cbPtrEndian, *cbPtrWhere;
    QLineEdit *lePtrBase, *leFree;
    QCheckBox *cbReuse;
    QListWidget *lwBases;
    QLabel *lPtrInfo;
    QComboBox *cbPreset;
    QTableWidget *twTable;
    QLineEdit *lePadding, *leTest;
    QLabel *lTestOut, *lTableInfo;
    QCheckBox *cbLive;
    QLabel *lStatus;
    QTimer *mLiveTimer;
};

#endif
