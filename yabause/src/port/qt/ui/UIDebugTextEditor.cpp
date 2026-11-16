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

/*  UIDebugTextEditor -- see UIDebugTextEditor.h. */

#include "UIDebugTextEditor.h"
#include "UIDebugGfxEditor.h"
#include <QAction>
#include "UIEditsExport.h"
#include "../QtYabause.h"
#include "../Settings.h"
#include "../VolatileSettings.h"
#include "CommonDialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCodec>
#include <QTimer>
#include <QTime>
#include <QMap>
#include <QStyle>
#include <QGridLayout>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QListWidget>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <unordered_map>
#include <algorithm>
#include <map>

extern "C" {
#include "memory.h"
}

using TextTools::CharTable;
using TextTools::TblEntry;

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QString hexOff(quint64 v) { return QString("0x%1").arg(v, 8, 16, QLatin1Char('0')).toUpper().replace("0X", "0x"); }

QColor dimColor(const QWidget *w)
{
    const QColor a = w->palette().color(QPalette::WindowText), b = w->palette().color(QPalette::Window);
    return QColor((a.red() * 6 + b.red() * 4) / 10, (a.green() * 6 + b.green() * 4) / 10, (a.blue() * 6 + b.blue() * 4) / 10);
}

// work RAM byte of an SH-2 address (cache-through mirror 0x2xxxxxxx too)
u8 *wramOf(quint32 addr, u32 *off)
{
    const quint32 a = addr & 0x0FFFFFFF;
    if (a >= 0x06000000 && a < 0x06100000 && HighWram) { *off = a - 0x06000000; return HighWram; }
    if (a >= 0x00200000 && a < 0x00300000 && LowWram) { *off = a - 0x00200000; return LowWram; }
    return nullptr;
}

// one line in a list cell
QString oneLine(const QString &s)
{
    QString t = s;
    t.replace(QChar('\n'), QString::fromUtf8("\xE2\x8F\x8E"));
    return t;
}

std::string toStd(const QString &s) { const QByteArray b = s.toUtf8(); return std::string(b.constData(), (size_t)b.size()); }
QString fromStd(const std::string &s) { return QString::fromUtf8(s.data(), (int)s.size()); }

QString kindName(int k) { return k == TextTools::End ? QString("end") : k == TextTools::NewLine ? QString("newline") : QString(); }
int kindFromName(const QString &s)
{
    const QString t = s.trimmed().toLower();
    if (t.startsWith("e") || t == "/") return TextTools::End;
    if (t.startsWith("n") || t == "*") return TextTools::NewLine;
    return TextTools::Normal;
}
QString showText(const std::string &t)
{
    QString s = fromStd(t);
    s.replace("\\", "\\\\");
    s.replace("\n", "\\n");
    return s;
}
std::string readText(const QString &s)
{
    QString o;
    for (int i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'n') { o += '\n'; i++; continue; }
            if (s[i + 1] == '\\') { o += '\\'; i++; continue; }
        }
        o += s[i];
    }
    return toStd(o);
}

// CSV with ';', fields in quotes when needed
QString csvField(const QString &s)
{
    if (s.contains(';') || s.contains('"') || s.contains('\n') || s.contains('\r')) {
        QString t = s;
        t.replace("\"", "\"\"");
        return "\"" + t + "\"";
    }
    return s;
}

QList<QStringList> parseCsv(const QString &text)
{
    // separator of the first line: ';' (French spreadsheets) or ','
    const int nl = text.indexOf('\n');
    const QString head = text.left(nl < 0 ? text.size() : nl);
    const QChar sep = head.count(';') >= head.count(',') ? QChar(';') : QChar(',');
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, any = false;
    for (int i = 0; i < text.size(); i++) {
        const QChar c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; i++; }
                else quoted = false;
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"') { quoted = true; any = true; }
        else if (c == sep) { row << field; field.clear(); any = true; }
        else if (c == '\r') {}
        else if (c == '\n') { row << field; field.clear(); rows << row; row.clear(); any = false; }
        else { field += c; any = true; }
    }
    if (any || !field.isEmpty() || !row.isEmpty()) { row << field; rows << row; }
    return rows;
}

enum { ColOn = 0, ColId, ColFile, ColOffset, ColPtrs, ColOrig, ColTrans, ColBytes };

} // namespace

// ===========================================================================
// Construction
// ===========================================================================

UIDebugTextEditor::UIDebugTextEditor(QWidget *parent)
    : QDialog(parent), mImgOk(false), mPlanOk(false), mRelBlock(-1), mNextId(1), mFilling(false), mCurrent(-1), mProgress(nullptr),
      lChipImage(nullptr), lChipStrings(nullptr), lChipDone(nullptr), lChipLong(nullptr), leImage(nullptr),
      twFiles(nullptr), cbCodec(nullptr), lePackOffset(nullptr), tabs(nullptr), leQuery(nullptr), cbMode(nullptr),
      sbMinChars(nullptr), cbRequireEnd(nullptr), twStrings(nullptr), teOriginal(nullptr), teTranslation(nullptr),
      lBytes(nullptr), twRel(nullptr), pbAddRel(nullptr), leHexOffset(nullptr), teHex(nullptr),
      lPtrContainer(nullptr), cbPtrOn(nullptr), cbPtrWidth(nullptr), cbPtrEndian(nullptr), cbPtrWhere(nullptr),
      lePtrBase(nullptr), leFree(nullptr), cbReuse(nullptr), lwBases(nullptr), lPtrInfo(nullptr),
      cbPreset(nullptr), twTable(nullptr), lePadding(nullptr), leTest(nullptr),
      lTestOut(nullptr), lTableInfo(nullptr), cbLive(nullptr), lStatus(nullptr), mLiveTimer(nullptr)
{
    setWindowTitle(tr_("Text Editor"));
    resize(1360, 820);
    setSizeGripEnabled(true);
    mTable = TextTools::asciiTable();
    buildUi();
    fillTableWidget();

    QString img = QtYabause::volatileSettings()->value("General/CdRomISO").toString();
    if (Settings *s = QtYabause::settings()) {
        const QString last = s->value("TextEditor/Image").toString();
        if (img.isEmpty() || !QFileInfo(img).exists()) img = last;
    }
    if (!img.isEmpty() && QFileInfo(img).exists()) openImage(img);
    fillPtrPanel();
    updateChips();
    QtYabause::retranslateWidget(this);
}

static QFrame *panel(QWidget *parent)
{
    QFrame *f = new QFrame(parent);
    f->setObjectName("panel");
    return f;
}

static QLabel *section(const QString &t, QWidget *parent)
{
    QLabel *l = new QLabel(t, parent);
    l->setProperty("section", true);
    return l;
}

static QPushButton *button(const QString &t, QWidget *parent, const QString &tip = QString())
{
    QPushButton *b = new QPushButton(t, parent);
    b->setProperty("flat2", true);
    if (!tip.isEmpty()) b->setToolTip(tip);
    return b;
}

void UIDebugTextEditor::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(12);

    QHBoxLayout *hdr = new QHBoxLayout();
    QLabel *title = new QLabel(tr_("Text Editor"), this);
    QFont f = title->font(); f.setPointSize(f.pointSize() + 6); f.setBold(true); title->setFont(f);
    hdr->addWidget(title);
    hdr->addStretch(1);
    lChipImage = new QLabel(this);   lChipImage->setObjectName("chip");
    lChipStrings = new QLabel(this); lChipStrings->setObjectName("chip");
    lChipDone = new QLabel(this);    lChipDone->setObjectName("chip");
    lChipLong = new QLabel(this);    lChipLong->setObjectName("chip");
    hdr->addWidget(lChipImage);
    hdr->addWidget(lChipStrings);
    hdr->addWidget(lChipDone);
    hdr->addWidget(lChipLong);
    root->addLayout(hdr);

    QSplitter *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(12);
    root->addWidget(split, 1);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // ---- left: disc ----
    QFrame *left = panel(this);
    left->setMinimumWidth(300);
    QVBoxLayout *vl = new QVBoxLayout(left);
    vl->setContentsMargins(14, 12, 14, 12);
    vl->setSpacing(8);
    vl->addWidget(section(tr_("Disc image"), left));
    QHBoxLayout *il = new QHBoxLayout();
    leImage = new QLineEdit(left);
    leImage->setReadOnly(true);
    QPushButton *browse = button(tr_("Open..."), left, tr_("Disc image to read and to patch (.cue, .iso, .bin)"));
    il->addWidget(leImage, 1);
    il->addWidget(browse);
    vl->addLayout(il);
    QLabel *lf = new QLabel(tr_("Search in:"), left);
    vl->addWidget(lf);
    twFiles = new QTreeWidget(left);
    twFiles->setColumnCount(3);
    twFiles->setHeaderLabels(QStringList() << tr_("File") << tr_("LBA") << tr_("Size"));
    twFiles->setRootIsDecorated(false);
    twFiles->setAlternatingRowColors(true);
    twFiles->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    twFiles->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    twFiles->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    twFiles->header()->setStretchLastSection(false);
    twFiles->setToolTip(tr_("The searches and the string dump look in the selected file, unpacked block, or in the whole disc"));
    vl->addWidget(twFiles, 1);
    vl->addWidget(section(tr_("Compressed data"), left));
    QHBoxLayout *cl = new QHBoxLayout();
    cbCodec = new QComboBox(left);
    for (int c = 0; c < TextTools::CodecCount; c++) cbCodec->addItem(QString::fromLatin1(TextTools::codecName(c)));
    cbCodec->setCurrentIndex(TextTools::CodecPrs);
    lePackOffset = new QLineEdit(left);
    lePackOffset->setPlaceholderText(tr_("offset (hex)"));
    lePackOffset->setToolTip(tr_("Offset of the packed data in the selected file (0 = start of the file)"));
    lePackOffset->setMaximumWidth(110);
    cl->addWidget(cbCodec, 1);
    cl->addWidget(lePackOffset);
    vl->addLayout(cl);
    QHBoxLayout *cb2 = new QHBoxLayout();
    QPushButton *unpack = button(tr_("Unpack"), left, tr_("Unpacks the data at this offset of the selected file; the block appears under the file"));
    QPushButton *findPrs = button(tr_("Find PRS"), left, tr_("Looks for every PRS block of the selected file"));
    cb2->addWidget(unpack);
    cb2->addWidget(findPrs);
    vl->addLayout(cb2);
    QLabel *chelp = new QLabel(tr_("Blocks are searched like files and packed again when the image is written."), left);
    chelp->setToolTip(tr_("The packed data written again must fit in the room of the old one (plus the 00 padding after it)."));
    chelp->setWordWrap(true);
    chelp->setProperty("dim", true);
    vl->addWidget(chelp);
    vl->addWidget(section(tr_("Text drawn as graphics"), left));
    QPushButton *gfxText = button(tr_("Open in the graphics editor..."), left,
        tr_("Title screens often draw their words with tiles of a VDP2 layer (NBG0-3) or with VDP1 sprites, e.g. "
            "\"PRESS START BUTTON\". Opens the graphics editor in screen pick mode: click the word in the game, "
            "then use its \"Text...\" tool to write the translation with a font."));
    vl->addWidget(gfxText);
    connect(gfxText, &QPushButton::clicked, this, &UIDebugTextEditor::onGfxText);
    split->addWidget(left);

    // ---- center: strings / relative search / hex ----
    QFrame *center = panel(this);
    QVBoxLayout *vc = new QVBoxLayout(center);
    vc->setContentsMargins(14, 12, 14, 12);
    vc->setSpacing(8);
    QHBoxLayout *sb = new QHBoxLayout();
    sb->setSpacing(6);
    leQuery = new QLineEdit(center);
    leQuery->setPlaceholderText(tr_("Text to find (e.g. a word seen in the game)"));
    cbMode = new QComboBox(center);
    cbMode->addItem(tr_("Text (with the table)"));
    cbMode->addItem(tr_("Relative search (unknown table)"));
    cbMode->addItem(tr_("Hex bytes"));
    cbMode->setToolTip(tr_("Relative search: finds a word whose codes keep the distances between its letters "
                           "(A-Z, a-z, 0-9), whatever the table. A space or ? matches any code."));
    QPushButton *search = button(tr_("Search"), center);
    search->setDefault(false);
    search->setAutoDefault(false);
    sb->addWidget(leQuery, 1);
    sb->addWidget(cbMode);
    sb->addWidget(search);
    vc->addLayout(sb);
    QHBoxLayout *db = new QHBoxLayout();
    db->setSpacing(6);
    QPushButton *dump = button(tr_("Dump strings"), center, tr_("Lists every string that the table can read in the selected file (or the whole disc)"));
    sbMinChars = new QSpinBox(center);
    sbMinChars->setRange(1, 64);
    sbMinChars->setValue(4);
    sbMinChars->setPrefix(tr_("at least "));
    sbMinChars->setSuffix(tr_(" characters"));
    cbRequireEnd = new QCheckBox(tr_("ending with an end code"), center);
    cbRequireEnd->setChecked(true);
    QPushButton *addAt = button(tr_("Add at offset..."), center, tr_("Adds the string that starts at a given offset of the disc"));
    QPushButton *remove = button(tr_("Remove"), center, tr_("Removes the selected strings from the list"));
    db->addWidget(dump);
    db->addWidget(sbMinChars);
    db->addWidget(cbRequireEnd);
    db->addStretch(1);
    db->addWidget(addAt);
    db->addWidget(remove);
    vc->addLayout(db);

    tabs = new QTabWidget(center);
    tabs->setDocumentMode(true);
    // strings
    QWidget *ts = new QWidget(tabs);
    QVBoxLayout *vs = new QVBoxLayout(ts);
    vs->setContentsMargins(0, 6, 0, 0);
    QSplitter *sv = new QSplitter(Qt::Vertical, ts);
    sv->setChildrenCollapsible(false);
    twStrings = new QTreeWidget(sv);
    twStrings->setColumnCount(8);
    twStrings->setHeaderLabels(QStringList() << tr_("On") << tr_("ID") << tr_("File") << tr_("Offset") << tr_("Ptrs")
                                             << tr_("Original") << tr_("Translation") << tr_("Bytes"));
    twStrings->setRootIsDecorated(false);
    twStrings->setAlternatingRowColors(true);
    twStrings->setUniformRowHeights(true);
    twStrings->setSelectionMode(QAbstractItemView::ExtendedSelection);
    twStrings->header()->setStretchLastSection(false);
    for (int c = 0; c < 8; c++)
        twStrings->header()->setSectionResizeMode(c, (c == ColOrig || c == ColTrans) ? QHeaderView::Stretch
                                                     : c == ColFile ? QHeaderView::Interactive : QHeaderView::ResizeToContents);
    twStrings->header()->resizeSection(ColFile, 140);
    QWidget *ed = new QWidget(sv);
    QGridLayout *eg = new QGridLayout(ed);
    eg->setContentsMargins(0, 6, 0, 0);
    eg->setHorizontalSpacing(10);
    teOriginal = new QPlainTextEdit(ed);
    teOriginal->setReadOnly(true);
    teTranslation = new QPlainTextEdit(ed);
    teTranslation->setPlaceholderText(tr_("Translation (Enter = new line code of the table, <$XX> = raw byte)"));
    teOriginal->setFrameShape(QFrame::StyledPanel);
    teTranslation->setFrameShape(QFrame::StyledPanel);
    lBytes = new QLabel(ed);
    lBytes->setWordWrap(true);
    lBytes->setTextFormat(Qt::RichText);
    connect(lBytes, &QLabel::linkActivated, this, [this](const QString &link) {
        const QStringList p = link.split(':');
        if (p.size() != 3 || p[0] != "hex") return;
        const int block = p[1].toInt();
        const quint64 off = p[2].toULongLong();
        leHexOffset->setText(QString::number(off, 16).toUpper());
        showHex(block, off);
        tabs->setCurrentIndex(2);
    });
    eg->addWidget(new QLabel(tr_("Original"), ed), 0, 0);
    eg->addWidget(new QLabel(tr_("Translation"), ed), 0, 1);
    eg->addWidget(teOriginal, 1, 0);
    eg->addWidget(teTranslation, 1, 1);
    eg->addWidget(lBytes, 2, 0, 1, 2);
    sv->addWidget(twStrings);
    sv->addWidget(ed);
    sv->setSizes(QList<int>() << 420 << 180);
    vs->addWidget(sv);
    tabs->addTab(ts, tr_("Strings"));
    // relative search
    QWidget *tr = new QWidget(tabs);
    QVBoxLayout *vr = new QVBoxLayout(tr);
    vr->setContentsMargins(0, 6, 0, 0);
    QLabel *rhelp = new QLabel(tr_("Results of the relative search. Pick the one that reads right, then add its letters "
                                   "to the character table."), tr);
    rhelp->setWordWrap(true);
    rhelp->setProperty("dim", true);
    vr->addWidget(rhelp);
    twRel = new QTreeWidget(tr);
    twRel->setColumnCount(5);
    twRel->setHeaderLabels(QStringList() << tr_("Offset") << tr_("File") << tr_("Codes") << tr_("First letter") << tr_("Preview with these codes"));
    twRel->setRootIsDecorated(false);
    twRel->setAlternatingRowColors(true);
    twRel->header()->setStretchLastSection(true);
    vr->addWidget(twRel, 1);
    QHBoxLayout *rb = new QHBoxLayout();
    rb->addStretch(1);
    pbAddRel = button(tr_("Add these letters to the table"), tr);
    pbAddRel->setEnabled(false);
    rb->addWidget(pbAddRel);
    vr->addLayout(rb);
    tabs->addTab(tr, tr_("Relative search"));
    // hex view
    QWidget *th = new QWidget(tabs);
    QVBoxLayout *vh = new QVBoxLayout(th);
    vh->setContentsMargins(0, 6, 0, 0);
    QHBoxLayout *hb = new QHBoxLayout();
    leHexOffset = new QLineEdit(th);
    leHexOffset->setPlaceholderText(tr_("Offset (hex)"));
    leHexOffset->setMaximumWidth(180);
    QPushButton *go = button(tr_("Go"), th);
    QPushButton *addHere = button(tr_("Add the string here"), th);
    hb->addWidget(leHexOffset);
    hb->addWidget(go);
    hb->addWidget(addHere);
    hb->addStretch(1);
    vh->addLayout(hb);
    teHex = new QPlainTextEdit(th);
    teHex->setReadOnly(true);
    teHex->setFont(mono);
    teHex->setLineWrapMode(QPlainTextEdit::NoWrap);
    vh->addWidget(teHex, 1);
    tabs->addTab(th, tr_("Hex view"));
    // pointers
    QWidget *tp = new QWidget(tabs);
    QVBoxLayout *vp = new QVBoxLayout(tp);
    vp->setContentsMargins(0, 6, 0, 0);
    vp->setSpacing(8);
    lPtrContainer = new QLabel(tp);
    lPtrContainer->setProperty("section", true);
    lPtrContainer->setWordWrap(true);
    vp->addWidget(lPtrContainer);
    QLabel *phelp = new QLabel(tr_("A translation longer than its original is moved to free space of the same file / block, "
                                   "and the words that point to it are rewritten. 1. dump the strings of the file, 2. guess the "
                                   "base (32-bit: RAM address where the game loads the file; 16-bit: start of a table), "
                                   "3. find the pointers, 4. give the free space."), tp);
    phelp->setWordWrap(true);
    phelp->setProperty("dim", true);
    vp->addWidget(phelp);
    QGridLayout *pgr = new QGridLayout();
    cbPtrOn = new QCheckBox(tr_("Move the texts that do not fit, with these pointers"), tp);
    pgr->addWidget(cbPtrOn, 0, 0, 1, 4);
    pgr->addWidget(new QLabel(tr_("Words"), tp), 1, 0);
    cbPtrWidth = new QComboBox(tp);
    cbPtrWidth->addItem(tr_("32 bits"));
    cbPtrWidth->addItem(tr_("16 bits"));
    pgr->addWidget(cbPtrWidth, 1, 1);
    cbPtrEndian = new QComboBox(tp);
    cbPtrEndian->addItem(tr_("big endian (SH-2)"));
    cbPtrEndian->addItem(tr_("little endian"));
    pgr->addWidget(cbPtrEndian, 1, 2);
    pgr->addWidget(new QLabel(tr_("Base"), tp), 2, 0);
    lePtrBase = new QLineEdit(tp);
    lePtrBase->setPlaceholderText(tr_("hex, e.g. 6040000 or -40"));
    lePtrBase->setToolTip(tr_("Pointer value = base + offset of the text in its file / block"));
    pgr->addWidget(lePtrBase, 2, 1);
    QPushButton *guess = button(tr_("Guess the base"), tp, tr_("Looks for the base that makes the most words point to the strings found"));
    pgr->addWidget(guess, 2, 2);
    QPushButton *fromRam = button(tr_("Base from RAM"), tp, tr_("While the game runs: finds where it has loaded this file in the work RAM (the base of 32-bit pointers)"));
    pgr->addWidget(fromRam, 2, 3);
    pgr->addWidget(new QLabel(tr_("Pointers"), tp), 3, 0);
    cbPtrWhere = new QComboBox(tp);
    cbPtrWhere->setToolTip(tr_("The pointer table may be in the program (e.g. 1ST_READ.BIN) rather than in the file of the texts"));
    pgr->addWidget(cbPtrWhere, 3, 1, 1, 2);
    QPushButton *findPtr = button(tr_("Find the pointers"), tp, tr_("Gives each string of this file / block the words that point to it"));
    pgr->addWidget(findPtr, 3, 3);
    pgr->addWidget(new QLabel(tr_("Free space"), tp), 4, 0);
    leFree = new QLineEdit(tp);
    leFree->setPlaceholderText(tr_("offsets in the file / block, e.g. 1F00-1FFF, 3000-30FF"));
    pgr->addWidget(leFree, 4, 1, 1, 2);
    QPushButton *findFree = button(tr_("Find free space"), tp, tr_("Runs of 32 bytes of 00 or FF at least"));
    pgr->addWidget(findFree, 4, 3);
    cbReuse = new QCheckBox(tr_("Reuse the room of the moved texts"), tp);
    cbReuse->setToolTip(tr_("Only if nothing but these pointers reads the old texts"));
    pgr->addWidget(cbReuse, 5, 0, 1, 4);
    pgr->setColumnStretch(1, 1);
    vp->addLayout(pgr);
    lwBases = new QListWidget(tp);
    lwBases->setMaximumHeight(140);
    vp->addWidget(lwBases);
    QHBoxLayout *ub = new QHBoxLayout();
    ub->addStretch(1);
    QPushButton *useBase = button(tr_("Use this base"), tp);
    ub->addWidget(useBase);
    vp->addLayout(ub);
    lPtrInfo = new QLabel(tp);
    lPtrInfo->setWordWrap(true);
    vp->addWidget(lPtrInfo);
    vp->addStretch(1);
    tabs->addTab(tp, tr_("Pointers"));

    vc->addWidget(tabs, 1);
    split->addWidget(center);

    // ---- right: character table ----
    QFrame *right = panel(this);
    QVBoxLayout *vt = new QVBoxLayout(right);
    vt->setContentsMargins(14, 12, 14, 12);
    vt->setSpacing(8);
    vt->addWidget(section(tr_("Character table"), right));
    cbPreset = new QComboBox(right);
    cbPreset->addItem(tr_("ASCII"));
    cbPreset->addItem(tr_("Shift-JIS (Japanese)"));
    cbPreset->addItem(tr_("Empty (build it with the relative search)"));
    cbPreset->addItem(tr_("Custom table"));
    cbPreset->setToolTip(tr_("Starts the table from a ready-made one (the current table is replaced)"));
    vt->addWidget(cbPreset);
    QHBoxLayout *tl = new QHBoxLayout();
    QPushButton *loadT = button(tr_("Load .tbl..."), right);
    QPushButton *saveT = button(tr_("Save .tbl..."), right);
    tl->addWidget(loadT);
    tl->addWidget(saveT);
    vt->addLayout(tl);
    twTable = new QTableWidget(right);
    twTable->setColumnCount(3);
    twTable->setHorizontalHeaderLabels(QStringList() << tr_("Hex") << tr_("Text") << tr_("Kind"));
    twTable->verticalHeader()->setVisible(false);
    twTable->verticalHeader()->setDefaultSectionSize(22);
    twTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    twTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    twTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    twTable->setAlternatingRowColors(true);
    twTable->setToolTip(tr_("Kind: empty = character, \"end\" = end of string, \"newline\" = new line. \\n in a text is a new line."));
    vt->addWidget(twTable, 1);
    QHBoxLayout *te = new QHBoxLayout();
    QPushButton *addE = button(tr_("Add"), right);
    QPushButton *delE = button(tr_("Delete"), right);
    te->addWidget(addE);
    te->addWidget(delE);
    te->addStretch(1);
    vt->addLayout(te);
    lTableInfo = new QLabel(right);
    lTableInfo->setProperty("dim", true);
    lTableInfo->setWordWrap(true);
    vt->addWidget(lTableInfo);
    QGridLayout *pg = new QGridLayout();
    pg->addWidget(new QLabel(tr_("Padding"), right), 0, 0);
    lePadding = new QLineEdit(right);
    lePadding->setPlaceholderText(tr_("end code"));
    lePadding->setToolTip(tr_("Bytes (hex) written after a shorter translation and its end code, up to the room of the original. "
                              "Empty: the end code is repeated."));
    pg->addWidget(lePadding, 0, 1);
    pg->addWidget(new QLabel(tr_("Test"), right), 1, 0);
    leTest = new QLineEdit(right);
    leTest->setPlaceholderText(tr_("Type a text to see its bytes"));
    pg->addWidget(leTest, 1, 1);
    vt->addLayout(pg);
    lTestOut = new QLabel(right);
    lTestOut->setWordWrap(true);
    lTestOut->setFont(mono);
    lTestOut->setTextInteractionFlags(Qt::TextSelectableByMouse);
    vt->addWidget(lTestOut);
    split->addWidget(right);
    split->setSizes(QList<int>() << 310 << 740 << 310);

    // ---- footer ----
    QHBoxLayout *ft = new QHBoxLayout();
    ft->setSpacing(6);
    cbLive = new QCheckBox(tr_("Live in game"), this);
    cbLive->setToolTip(tr_("Writes the translations into the work RAM each time the game loads the original text "
                           "(also the texts of unpacked blocks; moved texts with 32-bit pointers to the work RAM)"));
    QPushButton *saveP = button(tr_("Save project..."), this);
    QPushButton *loadP = button(tr_("Open project..."), this);
    QPushButton *csvOut = button(tr_("Export CSV..."), this, tr_("ID, file, offset, original and translation, for a translator or a spreadsheet"));
    QPushButton *csvIn = button(tr_("Import CSV..."), this, tr_("Reads the translations back, by ID"));
    QPushButton *img = button(tr_("Write patched image..."), this);
    QPushButton *bps = button(tr_("BPS..."), this);
    QPushButton *ips = button(tr_("IPS..."), this);
    QPushButton *closeBtn = button(tr_("Close"), this);
    ft->addWidget(cbLive);
    ft->addSpacing(8);
    ft->addWidget(saveP);
    ft->addWidget(loadP);
    ft->addWidget(csvOut);
    ft->addWidget(csvIn);
    ft->addSpacing(8);
    ft->addWidget(img);
    ft->addWidget(bps);
    ft->addWidget(ips);
    ft->addStretch(1);
    lStatus = new QLabel(this);
    lStatus->setProperty("dim", true);
    ft->addWidget(lStatus);
    ft->addWidget(closeBtn);
    root->addLayout(ft);

    setStyleSheet(QString::fromLatin1("QLabel[dim=\"true\"] { color: %1; }").arg(dimColor(this).name()) + QString::fromLatin1(
        "QFrame#panel { background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }"
        "QLabel#chip { border: 1px solid palette(mid); border-radius: 10px; padding: 2px 10px; font-weight: 600; }"
        "QLabel#chip[alert=\"true\"] { border-color: #d03030; color: #d03030; }"
        "QLabel[section=\"true\"] { font-weight: 700; }"
        "QPushButton[flat2=\"true\"] { border: 1px solid palette(mid); border-radius: 8px; padding: 5px 12px; background: transparent; }"
        "QPushButton[flat2=\"true\"]:hover { border-color: palette(highlight); }"
        "QPushButton:disabled { color: palette(mid); }"
        "QLineEdit { border: 1px solid palette(mid); border-radius: 6px; padding: 4px 8px; background: palette(base); }"
        "QTreeWidget, QTableWidget { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar::tab { padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { border-bottom: 2px solid palette(highlight); color: palette(highlight); }"));

    connect(browse, SIGNAL(clicked()), this, SLOT(onBrowseImage()));
    connect(search, SIGNAL(clicked()), this, SLOT(onSearch()));
    connect(leQuery, SIGNAL(returnPressed()), this, SLOT(onSearch()));
    connect(dump, SIGNAL(clicked()), this, SLOT(onDump()));
    connect(addAt, SIGNAL(clicked()), this, SLOT(onAddAtOffset()));
    connect(remove, SIGNAL(clicked()), this, SLOT(onRemoveStrings()));
    connect(twStrings, SIGNAL(itemSelectionChanged()), this, SLOT(onStringSelected()));
    connect(twStrings, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this, SLOT(onStringItemChanged(QTreeWidgetItem *, int)));
    connect(teTranslation, SIGNAL(textChanged()), this, SLOT(onTranslationEdited()));
    connect(twRel, SIGNAL(itemSelectionChanged()), this, SLOT(onRelSelected()));
    connect(pbAddRel, SIGNAL(clicked()), this, SLOT(onAddRelative()));
    connect(go, SIGNAL(clicked()), this, SLOT(onHexGo()));
    connect(leHexOffset, SIGNAL(returnPressed()), this, SLOT(onHexGo()));
    connect(addHere, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const quint64 off = leHexOffset->text().trimmed().replace("0x", "", Qt::CaseInsensitive).toULongLong(&ok, 16);
        if (!ok) return;
        const int i = addStringAt(off);
        if (i >= 0) { tabs->setCurrentIndex(0); twStrings->setCurrentItem(twStrings->topLevelItem(i)); }
    });
    connect(cbPreset, SIGNAL(activated(int)), this, SLOT(onPreset(int)));
    connect(loadT, SIGNAL(clicked()), this, SLOT(onLoadTable()));
    connect(saveT, SIGNAL(clicked()), this, SLOT(onSaveTable()));
    connect(twTable, SIGNAL(cellChanged(int, int)), this, SLOT(onTableCellChanged(int, int)));
    connect(addE, SIGNAL(clicked()), this, SLOT(onAddEntry()));
    connect(delE, SIGNAL(clicked()), this, SLOT(onDeleteEntries()));
    connect(leTest, SIGNAL(textChanged(QString)), this, SLOT(onTestChanged()));
    connect(lePadding, &QLineEdit::textChanged, this, [this]() { refreshAll(); });
    connect(twFiles, SIGNAL(currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)), this, SLOT(onScopeChanged()));
    connect(unpack, SIGNAL(clicked()), this, SLOT(onUnpack()));
    connect(findPrs, SIGNAL(clicked()), this, SLOT(onFindPrs()));
    connect(cbPtrOn, SIGNAL(toggled(bool)), this, SLOT(onPtrSpecEdited()));
    connect(cbPtrWidth, SIGNAL(activated(int)), this, SLOT(onPtrSpecEdited()));
    connect(cbPtrEndian, SIGNAL(activated(int)), this, SLOT(onPtrSpecEdited()));
    connect(cbPtrWhere, SIGNAL(activated(int)), this, SLOT(onPtrSpecEdited()));
    connect(lePtrBase, SIGNAL(editingFinished()), this, SLOT(onPtrSpecEdited()));
    connect(leFree, SIGNAL(editingFinished()), this, SLOT(onPtrSpecEdited()));
    connect(cbReuse, SIGNAL(toggled(bool)), this, SLOT(onPtrSpecEdited()));
    connect(guess, SIGNAL(clicked()), this, SLOT(onGuessBase()));
    connect(fromRam, SIGNAL(clicked()), this, SLOT(onBaseFromRam()));
    connect(useBase, SIGNAL(clicked()), this, SLOT(onUseBase()));
    connect(lwBases, SIGNAL(itemDoubleClicked(QListWidgetItem *)), this, SLOT(onUseBase()));
    connect(findPtr, SIGNAL(clicked()), this, SLOT(onFindPointers()));
    connect(findFree, SIGNAL(clicked()), this, SLOT(onFindFree()));

    connect(cbLive, SIGNAL(toggled(bool)), this, SLOT(onLiveToggled(bool)));
    connect(saveP, SIGNAL(clicked()), this, SLOT(onSaveProject()));
    connect(loadP, SIGNAL(clicked()), this, SLOT(onLoadProject()));
    connect(csvOut, SIGNAL(clicked()), this, SLOT(onExportCsv()));
    connect(csvIn, SIGNAL(clicked()), this, SLOT(onImportCsv()));
    connect(img, SIGNAL(clicked()), this, SLOT(onWriteImage()));
    connect(bps, SIGNAL(clicked()), this, SLOT(onWriteBps()));
    connect(ips, SIGNAL(clicked()), this, SLOT(onWriteIps()));
    connect(closeBtn, SIGNAL(clicked()), this, SLOT(accept()));

    mLiveTimer = new QTimer(this);
    mLiveTimer->setInterval(500);
    connect(mLiveTimer, SIGNAL(timeout()), this, SLOT(onLiveTick()));
}

// ===========================================================================
// Disc, files and unpacked blocks
// ===========================================================================

bool UIDebugTextEditor::openImage(const QString &path, QString *errOut)
{
    std::string err;
    EditsPatcher::Image img;
    if (!EditsPatcher::openImage(path.toLocal8Bit().constData(), img, err) || img.tracks.empty()) {
        if (errOut) *errOut = err.empty() ? tr_("No data track in this image.") : QString::fromLocal8Bit(err.c_str());
        return false;
    }
    mImg = img;
    mImgOk = true;
    mFiles.clear();
    mFileCache.clear();
    mBlocks.clear();
    if (!TextTools::listIsoFiles(mImg, 0, mFiles, err))
        lStatus->setText(tr_("No file system found (%1): searches cover the whole track.").arg(QString::fromLocal8Bit(err.c_str())));
    leImage->setText(path);
    if (Settings *s = QtYabause::settings()) s->setValue("TextEditor/Image", path);
    fillFiles();
    invalidatePlan();
    updateChips();
    return true;
}

static const int kBlockRole = 100000;

void UIDebugTextEditor::fillFiles()
{
    QSignalBlocker b(twFiles);
    twFiles->clear();
    QTreeWidgetItem *all = new QTreeWidgetItem(twFiles);
    all->setText(0, tr_("(whole disc)"));
    all->setText(2, QString("%1 MB").arg((double)EditsPatcher::userDataSize(mImg, 0) / 1048576.0, 0, 'f', 1));
    all->setData(0, Qt::UserRole, -1);
    QVector<QTreeWidgetItem *> items;
    for (size_t i = 0; i < mFiles.size(); i++) {
        QTreeWidgetItem *it = new QTreeWidgetItem(twFiles);
        it->setText(0, fromStd(mFiles[i].path));
        it->setText(1, QString::number(mFiles[i].lba));
        it->setText(2, QString::number(mFiles[i].size));
        it->setData(0, Qt::UserRole, (int)i);
        items << it;
    }
    for (int bi = 0; bi < mBlocks.size(); bi++) {
        const Block &bl = mBlocks[bi];
        QTreeWidgetItem *parent = (bl.file >= 0 && bl.file < items.size()) ? items[bl.file] : all;
        QTreeWidgetItem *it = new QTreeWidgetItem(parent);
        it->setText(0, QString("@%1 %2").arg(QString::number(bl.packOffset - (bl.file >= 0 ? (quint64)mFiles[bl.file].lba * 2048 : 0), 16).toUpper())
                           .arg(QString::fromLatin1(TextTools::codecName(bl.codec))));
        it->setText(2, QString("%1>%2").arg(bl.packSize).arg(bl.data.size()));
        it->setToolTip(0, tr_("Unpacked block: %1 bytes packed at disc offset %2, %3 bytes unpacked; packed again it may take %4 bytes")
                              .arg(bl.packSize).arg(hexOff(bl.packOffset)).arg(bl.data.size()).arg(qMax(bl.room, bl.packSize)));
        it->setData(0, Qt::UserRole, kBlockRole + bi);
        parent->setExpanded(true);
    }
    twFiles->setRootIsDecorated(!mBlocks.isEmpty());
    twFiles->setCurrentItem(all);
}

void UIDebugTextEditor::selectScope(int fileIndex)
{
    QTreeWidgetItemIterator it(twFiles);
    for (; *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toInt() == fileIndex) { twFiles->setCurrentItem(*it); return; }
}

void UIDebugTextEditor::selectBlock(int b) { selectScope(kBlockRole + b); }

bool UIDebugTextEditor::scope(int &block, quint64 &start, quint64 &length, QString &name) const
{
    block = -1;
    start = 0;
    length = 0;
    name = tr_("the whole disc");
    QTreeWidgetItem *it = twFiles->currentItem();
    const int idx = it ? it->data(0, Qt::UserRole).toInt() : -1;
    if (idx >= kBlockRole && idx - kBlockRole < mBlocks.size()) {
        block = idx - kBlockRole;
        length = (quint64)mBlocks[block].data.size();
        name = blockName(block);
        return mImgOk;
    }
    if (idx >= 0 && idx < (int)mFiles.size()) {
        start = (quint64)mFiles[idx].lba * 2048;
        length = mFiles[idx].size;
        name = fromStd(mFiles[idx].path);
        if (length == 0) return false;
    }
    return mImgOk;
}

QString UIDebugTextEditor::scopeKey() const
{
    QTreeWidgetItem *it = twFiles->currentItem();
    const int idx = it ? it->data(0, Qt::UserRole).toInt() : -1;
    if (idx >= kBlockRole) return QString("b:%1").arg(idx - kBlockRole);
    if (idx >= 0) return QString("f:%1").arg(idx);
    return QString("d");
}

QString UIDebugTextEditor::ptrKey() const
{
    if (mCurrent >= 0 && mCurrent < mStrings.size()) {
        const QString k = containerKey(mStrings[mCurrent]);
        if (k != "d") return k;
    }
    return scopeKey();
}

QString UIDebugTextEditor::fileName(quint64 offset) const
{
    const TextTools::IsoFile *f = TextTools::fileAt(mFiles, offset);
    return f ? fromStd(f->path) : QString();
}

QString UIDebugTextEditor::blockName(int b) const
{
    if (b < 0 || b >= mBlocks.size()) return QString();
    const Block &bl = mBlocks[b];
    const QString file = bl.file >= 0 && bl.file < (int)mFiles.size() ? fromStd(mFiles[bl.file].path) : tr_("disc");
    const quint64 at = bl.packOffset - (bl.file >= 0 ? (quint64)mFiles[bl.file].lba * 2048 : 0);
    static const char *shortNames[TextTools::CodecCount] = { "LZSS", "LZSS 00", "LZSS+size", "PRS" };
    return QString("%1 @%2 %3").arg(file).arg(QString::number(at, 16).toUpper())
        .arg(QString::fromLatin1(bl.codec >= 0 && bl.codec < TextTools::CodecCount ? shortNames[bl.codec] : "?"));
}

QString UIDebugTextEditor::containerKey(const Str &s) const
{
    if (s.block >= 0) return QString("b:%1").arg(s.block);
    const TextTools::IsoFile *f = TextTools::fileAt(mFiles, s.offset);
    if (f) return QString("f:%1").arg((int)(f - &mFiles[0]));
    return QString("d");
}

quint64 UIDebugTextEditor::containerOffset(const Str &s) const
{
    if (s.block >= 0) return s.offset;
    const TextTools::IsoFile *f = TextTools::fileAt(mFiles, s.offset);
    return f ? s.offset - (quint64)f->lba * 2048 : s.offset;
}

QString UIDebugTextEditor::containerName(const QString &key) const
{
    if (key.startsWith("b:")) return blockName(key.mid(2).toInt());
    if (key.startsWith("f:")) {
        const int i = key.mid(2).toInt();
        return i >= 0 && i < (int)mFiles.size() ? fromStd(mFiles[i].path) : QString();
    }
    return tr_("the whole disc");
}

bool UIDebugTextEditor::fileData(int i, QByteArray &out) const
{
    if (i < 0 || i >= (int)mFiles.size()) return false;
    if (mFileCache.contains(i)) { out = mFileCache.value(i); return true; }
    const quint64 size = qMin<quint64>(mFiles[i].size, 64u << 20);
    std::vector<uint8_t> b;
    std::string err;
    if (!EditsPatcher::readUserData(mImg, 0, (quint64)mFiles[i].lba * 2048, (size_t)size, b, err)) return false;
    out = QByteArray((const char *)b.data(), (int)b.size());
    mFileCache.insert(i, out);
    return true;
}

bool UIDebugTextEditor::containerData(const QString &key, QByteArray &out, quint64 *discStart, QString *err) const
{
    if (discStart) *discStart = 0;
    if (key.startsWith("b:")) {
        const int b = key.mid(2).toInt();
        if (b < 0 || b >= mBlocks.size()) return false;
        out = mBlocks[b].data;
        return true;
    }
    if (key.startsWith("f:")) {
        const int i = key.mid(2).toInt();
        if (!fileData(i, out)) { if (err) *err = tr_("The file cannot be read."); return false; }
        if (discStart) *discStart = (quint64)mFiles[i].lba * 2048;
        return true;
    }
    if (err) *err = tr_("Choose a file or an unpacked block (pointers and free space belong to a file).");
    return false;
}

void UIDebugTextEditor::onBrowseImage()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Choose the disc image"), QFileInfo(leImage->text()).absolutePath(),
                                                   "Disc images (*.cue *.iso *.bin *.img);;All files (*)");
    if (f.isEmpty()) return;
    QString err;
    if (!openImage(f, &err)) QMessageBox::warning(this, windowTitle(), err);
}

void UIDebugTextEditor::onScopeChanged()
{
    fillPtrPanel();
}

// ---- compressed data ----

// Room for the packed data written again: its old size, plus the 00 bytes
// that follow it in the same file (padding). Not for LZSS without a size
// header: the game may read the packed data up to a size of its own.
quint32 UIDebugTextEditor::blockRoom(const Block &bl) const
{
    if (bl.codec == TextTools::CodecLzss || bl.codec == TextTools::CodecLzssZero || bl.file < 0) return bl.packSize;
    const quint64 fileEnd = (quint64)mFiles[bl.file].lba * 2048 + mFiles[bl.file].size;
    const quint64 from = bl.packOffset + bl.packSize;
    if (from >= fileEnd) return bl.packSize;
    std::vector<uint8_t> b;
    std::string err;
    if (!EditsPatcher::readUserData(mImg, 0, from, (size_t)qMin<quint64>(fileEnd - from, 65536), b, err)) return bl.packSize;
    size_t k = 0;
    while (k < b.size() && b[k] == 0) k++;
    return bl.packSize + (quint32)k;
}

int UIDebugTextEditor::unpackAt(int fileIndex, quint64 offsetInFile, int codec, QString *msg)
{
    if (!mImgOk) { if (msg) *msg = tr_("Open a disc image first."); return -1; }
    quint64 start = 0, avail = EditsPatcher::userDataSize(mImg, 0);
    if (fileIndex >= 0 && fileIndex < (int)mFiles.size()) {
        start = (quint64)mFiles[fileIndex].lba * 2048;
        avail = mFiles[fileIndex].size;
    }
    if (offsetInFile >= avail) { if (msg) *msg = tr_("The offset is past the end of the file."); return -1; }
    const quint64 n = qMin<quint64>(avail - offsetInFile, 16u << 20);
    std::vector<uint8_t> packed, out;
    std::string err;
    if (!EditsPatcher::readUserData(mImg, 0, start + offsetInFile, (size_t)n, packed, err)) { if (msg) *msg = fromStd(err); return -1; }
    size_t used = 0;
    if (!TextTools::decompress(codec, packed.data(), packed.size(), out, &used, 16u << 20, err) || out.size() < 16 || used == 0) {
        if (msg) *msg = tr_("No %1 data at this offset (%2).").arg(QString::fromLatin1(TextTools::codecName(codec)))
                            .arg(err.empty() ? tr_("too little data") : fromStd(err));
        return -1;
    }
    for (int i = 0; i < mBlocks.size(); i++)
        if (mBlocks[i].packOffset == start + offsetInFile && mBlocks[i].codec == codec) { if (msg) *msg = tr_("Already unpacked."); return i; }
    Block bl;
    bl.file = fileIndex;
    bl.packOffset = start + offsetInFile;
    bl.packSize = (quint32)used;
    bl.codec = codec;
    bl.data = QByteArray((const char *)out.data(), (int)out.size());
    bl.room = blockRoom(bl);
    mBlocks.push_back(bl);
    fillFiles();
    selectBlock(mBlocks.size() - 1);
    if (msg) *msg = tr_("%1: %2 bytes unpacked from %3 bytes.").arg(blockName(mBlocks.size() - 1)).arg(out.size()).arg(used);
    return mBlocks.size() - 1;
}

int UIDebugTextEditor::findPrsBlocks(int fileIndex, QString *msg)
{
    QByteArray d;
    if (!fileData(fileIndex, d)) { if (msg) *msg = tr_("Choose a file first."); return -1; }
    startProgress(tr_("Looking for PRS data..."));
    const std::vector<TextTools::PackedBlock> found =
        TextTools::findPrsBlocks((const uint8_t *)d.constData(), (size_t)d.size(), 256, 500, [this](double f) { return progress(f); });
    endProgress();
    const quint64 start = (quint64)mFiles[fileIndex].lba * 2048;
    int added = 0;
    for (size_t k = 0; k < found.size(); k++) {
        bool have = false;
        for (int i = 0; i < mBlocks.size() && !have; i++) have = mBlocks[i].packOffset == start + found[k].offset;
        if (have) continue;
        std::vector<uint8_t> out;
        std::string err;
        size_t used = 0;
        TextTools::decompress(TextTools::CodecPrs, (const uint8_t *)d.constData() + found[k].offset, (size_t)(d.size() - found[k].offset),
                              out, &used, 16u << 20, err);
        Block bl;
        bl.file = fileIndex;
        bl.packOffset = start + found[k].offset;
        bl.packSize = (quint32)used;
        bl.codec = TextTools::CodecPrs;
        bl.data = QByteArray((const char *)out.data(), (int)out.size());
        bl.room = blockRoom(bl);
        mBlocks.push_back(bl);
        added++;
    }
    fillFiles();
    if (added) selectBlock(mBlocks.size() - added);
    if (msg) *msg = tr_("%1 PRS block(s) found in %2, %3 new.").arg(found.size()).arg(fromStd(mFiles[fileIndex].path)).arg(added);
    return (int)found.size();
}

void UIDebugTextEditor::onUnpack()
{
    QTreeWidgetItem *it = twFiles->currentItem();
    int idx = it ? it->data(0, Qt::UserRole).toInt() : -1;
    if (idx >= kBlockRole) idx = mBlocks[idx - kBlockRole].file;
    bool ok = false;
    const quint64 off = lePackOffset->text().trimmed().isEmpty() ? 0
                      : lePackOffset->text().trimmed().replace("0x", "", Qt::CaseInsensitive).toULongLong(&ok, 16);
    if (!lePackOffset->text().trimmed().isEmpty() && !ok) { lStatus->setText(tr_("Type the offset in hex.")); return; }
    QString msg;
    unpackAt(idx, off, cbCodec->currentIndex(), &msg);
    lStatus->setText(msg);
}

void UIDebugTextEditor::onFindPrs()
{
    QTreeWidgetItem *it = twFiles->currentItem();
    int idx = it ? it->data(0, Qt::UserRole).toInt() : -1;
    if (idx >= kBlockRole) idx = mBlocks[idx - kBlockRole].file;
    QString msg;
    findPrsBlocks(idx, &msg);
    lStatus->setText(msg);
}

// ===========================================================================
// Progress
// ===========================================================================

void UIDebugTextEditor::startProgress(const QString &label)
{
    endProgress();
    mProgress = new QProgressDialog(label, tr_("Cancel"), 0, 1000, this);
    mProgress->setWindowModality(Qt::WindowModal);
    mProgress->setMinimumDuration(400);
}

bool UIDebugTextEditor::progress(double f)
{
    if (!mProgress) return true;
    mProgress->setValue((int)(f * 1000));
    QApplication::processEvents();
    return !mProgress->wasCanceled();
}

void UIDebugTextEditor::endProgress()
{
    if (!mProgress) return;
    mProgress->setValue(1000);
    delete mProgress;
    mProgress = nullptr;
}

// ===========================================================================
// Strings
// ===========================================================================

QString UIDebugTextEditor::nextId()
{
    return QString("T%1").arg(mNextId++, 4, 10, QLatin1Char('0'));
}

int UIDebugTextEditor::indexOf(int block, quint64 offset) const
{
    for (int i = 0; i < mStrings.size(); i++)
        if (mStrings[i].block == block && mStrings[i].offset == offset) return i;
    return -1;
}

// A hit may be in the middle of a string: go back over the codes of the
// table, up to the end code of the previous string (512 bytes at most).
quint64 UIDebugTextEditor::stringStart(const TextTools::Source &src, quint64 hit) const
{
    const int w = mTable.codeWidth();
    const quint64 back = qMin<quint64>(hit, 512);
    std::vector<uint8_t> b;
    std::string err;
    const size_t n = (size_t)qMin<quint64>(back + 8, src.size() - (hit - back));
    if (back == 0 || !src.read(hit - back, n, b, err)) return hit;
    size_t pos = (size_t)back;
    while (pos >= (size_t)w) {
        const size_t cand = pos - (size_t)w;
        const TblEntry *e = mTable.match(&b[cand], b.size() - cand);
        if (!e || e->kind == TextTools::End || e->bytes.size() != (size_t)w) break;
        pos = cand;
    }
    return hit - back + pos;
}

int UIDebugTextEditor::addStringAt(quint64 offset)
{
    int block = -1;
    quint64 st, len;
    QString name;
    scope(block, st, len, name);
    const int before = mStrings.size();
    const int i = appendString(block, offset);
    if (mStrings.size() != before) { invalidatePlan(); fillStrings(); updateChips(); }
    return i;
}

int UIDebugTextEditor::appendString(int block, quint64 offset)
{
    if (!mImgOk) return -1;
    const int have = indexOf(block, offset);
    if (have >= 0) return have;
    const TextTools::Source src = block >= 0 ? TextTools::Source((const uint8_t *)mBlocks[block].data.constData(), (size_t)mBlocks[block].data.size())
                                             : TextTools::Source(mImg, 0);
    TextTools::Found f;
    std::string err;
    if (!TextTools::readString(src, offset, mTable, 2048, f, err) || f.length == 0) {
        lStatus->setText(tr_("No text the table can read at %1.").arg(hexOff(offset)));
        return -1;
    }
    std::vector<uint8_t> raw;
    src.read(offset, f.length + f.termLength, raw, err);
    Str s;
    s.id = nextId();
    s.block = block;
    s.offset = offset;
    s.length = f.length;
    s.termLength = f.termLength;
    s.orig = QByteArray((const char *)raw.data(), (int)raw.size());
    s.text = fromStd(f.text);
    mStrings.push_back(s);
    return mStrings.size() - 1;
}

int UIDebugTextEditor::searchText(const QString &query, int mode, QString *msg)
{
    int block;
    quint64 start, length;
    QString where;
    if (!scope(block, start, length, where)) { if (msg) *msg = tr_("Open a disc image first."); return -1; }
    const TextTools::Source src = block >= 0 ? TextTools::Source((const uint8_t *)mBlocks[block].data.constData(), (size_t)mBlocks[block].data.size())
                                             : TextTools::Source(mImg, 0);
    std::string err;
    startProgress(tr_("Searching %1...").arg(where));
    TextTools::Progress prog = [this](double f) { return progress(f); };
    if (mode == 1) {
        mRelQuery = query;
        mRelBlock = block;
        mRelHits.clear();
        const bool ok = TextTools::relativeSearch(src, start, length, toStd(query), mRelHits, 500, prog, err);
        endProgress();
        twRel->clear();
        if (!ok) { if (msg) *msg = fromStd(err); return -1; }
        for (size_t i = 0; i < mRelHits.size(); i++) {
            const TextTools::RelHit &h = mRelHits[i];
            QTreeWidgetItem *it = new QTreeWidgetItem(twRel);
            it->setText(0, hexOff(h.offset));
            it->setText(1, block >= 0 ? blockName(block) : fileName(h.offset));
            it->setText(2, h.width == 1 ? tr_("8 bit") : h.width == 2 ? tr_("16 bit (big endian)") : tr_("16 bit (little endian)"));
            QString first;
            for (int k = 0; k < query.size() && k < (int)h.values.size(); k++)
                if (query[k].isLetterOrNumber()) {
                    first = QString("%1 = %2").arg(query[k]).arg(h.values[(size_t)k], h.width == 1 ? 2 : 4, 16, QLatin1Char('0')).toUpper();
                    break;
                }
            it->setText(3, first);
            // preview: the bytes around the hit read with the deduced letters
            CharTable t;
            t.setEntries(TextTools::relativeProposal(toStd(mRelQuery), h));
            std::vector<uint8_t> b;
            src.read(h.offset, (size_t)qMin<quint64>(48, src.size() - h.offset), b, err);
            QString prev;
            const int step = h.width < 0 ? 2 : h.width;
            for (size_t p = 0; p + step <= b.size(); ) {
                const TblEntry *e = t.match(&b[p], b.size() - p);
                if (e) { prev += fromStd(e->text); p += e->bytes.size(); }
                else { prev += QChar(0xB7); p += step; }
            }
            it->setText(4, prev);
        }
        tabs->setCurrentIndex(1);
        if (msg) *msg = tr_("%1 place(s) found in %2.").arg(mRelHits.size()).arg(where);
        return (int)mRelHits.size();
    }
    std::vector<uint8_t> pat;
    if (mode == 2) {
        if (!TextTools::parseHexBytes(toStd(query), pat)) { endProgress(); if (msg) *msg = tr_("Type hex bytes, e.g. 82 A0 82 A2."); return -1; }
    } else if (!mTable.encode(toStd(query), pat, err)) {
        endProgress();
        if (msg) *msg = fromStd(err) + " " + tr_("Complete the table, or try the relative search.");
        return -1;
    }
    std::vector<uint64_t> hits;
    const bool ok = TextTools::searchBytes(src, start, length, pat, hits, 2000, prog, err);
    endProgress();
    if (!ok) { if (msg) *msg = fromStd(err); return -1; }
    int added = 0, first = -1;
    for (size_t i = 0; i < hits.size(); i++) {
        const quint64 s0 = mode == 0 ? stringStart(src, hits[i]) : hits[i];
        const int before = mStrings.size();
        const int idx = appendString(block, s0);
        if (idx >= 0 && first < 0) first = idx;
        if (mStrings.size() > before) added++;
    }
    invalidatePlan();
    fillStrings();
    updateChips();
    tabs->setCurrentIndex(0);
    if (first >= 0) twStrings->setCurrentItem(twStrings->topLevelItem(first));
    if (msg) *msg = tr_("%1 occurrence(s) in %2, %3 new string(s) in the list.").arg(hits.size()).arg(where).arg(added);
    return (int)hits.size();
}

void UIDebugTextEditor::onSearch()
{
    const QString q = leQuery->text();
    if (q.isEmpty()) return;
    QString msg;
    searchText(q, cbMode->currentIndex(), &msg);
    lStatus->setText(msg);
}

int UIDebugTextEditor::dumpStrings(int minChars, bool requireEnd, QString *msg)
{
    int block;
    quint64 start, length;
    QString where;
    if (!scope(block, start, length, where)) { if (msg) *msg = tr_("Open a disc image first."); return -1; }
    const TextTools::Source src = block >= 0 ? TextTools::Source((const uint8_t *)mBlocks[block].data.constData(), (size_t)mBlocks[block].data.size())
                                             : TextTools::Source(mImg, 0);
    std::vector<TextTools::Found> found;
    std::string err;
    startProgress(tr_("Reading the strings of %1...").arg(where));
    const bool ok = TextTools::dumpStrings(src, start, length, mTable, minChars, requireEnd, found, 20000,
                                           [this](double f) { return progress(f); }, err);
    endProgress();
    if (!ok) { if (msg) *msg = fromStd(err); return -1; }
    int added = 0;
    for (size_t i = 0; i < found.size(); i++) {
        if (indexOf(block, found[i].offset) >= 0) continue;
        std::vector<uint8_t> raw;
        src.read(found[i].offset, found[i].length + found[i].termLength, raw, err);
        Str s;
        s.id = nextId();
        s.block = block;
        s.offset = found[i].offset;
        s.length = found[i].length;
        s.termLength = found[i].termLength;
        s.orig = QByteArray((const char *)raw.data(), (int)raw.size());
        s.text = fromStd(found[i].text);
        mStrings.push_back(s);
        added++;
    }
    std::stable_sort(mStrings.begin(), mStrings.end(), [](const Str &a, const Str &b) {
        return a.block != b.block ? a.block < b.block : a.offset < b.offset;
    });
    invalidatePlan();
    fillStrings();
    updateChips();
    tabs->setCurrentIndex(0);
    if (msg) *msg = tr_("%1 string(s) in %2, %3 new in the list.").arg(found.size()).arg(where).arg(added)
                    + (found.size() >= 20000 ? " " + tr_("(stopped at 20000: choose a file)") : QString());
    return (int)found.size();
}

void UIDebugTextEditor::onDump()
{
    QString msg;
    dumpStrings(sbMinChars->value(), cbRequireEnd->isChecked(), &msg);
    lStatus->setText(msg);
}

void UIDebugTextEditor::onAddAtOffset()
{
    bool ok = false;
    const QString t = QInputDialog::getText(this, tr_("Add a string"),
                                            tr_("Offset (hex): in the disc (LBA x 0x800 + byte), or in the unpacked block selected on the left:"),
                                            QLineEdit::Normal, leHexOffset->text(), &ok);
    if (!ok) return;
    const quint64 off = QString(t).trimmed().replace("0x", "", Qt::CaseInsensitive).toULongLong(&ok, 16);
    if (!ok) return;
    const int i = addStringAt(off);
    if (i >= 0) { tabs->setCurrentIndex(0); twStrings->setCurrentItem(twStrings->topLevelItem(i)); }
}

void UIDebugTextEditor::onRemoveStrings()
{
    QList<int> rows;
    foreach (QTreeWidgetItem *it, twStrings->selectedItems()) rows << twStrings->indexOfTopLevelItem(it);
    std::sort(rows.begin(), rows.end());
    for (int k = rows.size() - 1; k >= 0; k--) if (rows[k] >= 0 && rows[k] < mStrings.size()) mStrings.remove(rows[k]);
    mCurrent = -1;
    invalidatePlan();
    fillStrings();
    updateEditor();
    updateChips();
}

bool UIDebugTextEditor::replacement(const Str &s, std::vector<uint8_t> &out, QString *why, int *overflow) const
{
    if (overflow) *overflow = 0;
    if (s.translation.isEmpty()) { if (why) *why = tr_("no translation"); return false; }
    const std::vector<uint8_t> term(s.orig.constData() + s.length, s.orig.constData() + s.orig.size());
    std::vector<uint8_t> pad;
    if (!lePadding->text().trimmed().isEmpty() && !TextTools::parseHexBytes(toStd(lePadding->text()), pad)) {
        if (why) *why = tr_("the padding is not hex");
        return false;
    }
    std::string err;
    if (!TextTools::buildReplacement(mTable, toStd(s.translation), s.length, term, pad, out, overflow, err)) {
        if (why) *why = fromStd(err);
        return false;
    }
    return true;
}

// ===========================================================================
// Plan: where each translation goes
// ===========================================================================

void UIDebugTextEditor::invalidatePlan() { mPlanOk = false; }

static std::vector<std::pair<quint64, quint64> > parseRanges(const QString &text)
{
    // "1F00-1FFF, 3000-30FF" (hex, end included)
    std::vector<std::pair<quint64, quint64> > out;
    QStringList parts = text.split(QRegularExpression("[,;\\s]+"));
    parts.removeAll(QString());
    for (int i = 0; i < parts.size(); i++) {
        const QStringList ab = parts[i].split('-');
        bool ok1 = false, ok2 = false;
        if (ab.size() != 2) continue;
        const quint64 a = QString(ab[0]).replace("0x", "", Qt::CaseInsensitive).toULongLong(&ok1, 16);
        const quint64 b = QString(ab[1]).replace("0x", "", Qt::CaseInsensitive).toULongLong(&ok2, 16);
        if (ok1 && ok2 && b >= a) out.push_back(std::make_pair(a, b + 1));
    }
    return out;
}

void UIDebugTextEditor::computePlan()
{
    if (mPlanOk && mPlan.size() == (size_t)mStrings.size()) return;
    mPlan.assign((size_t)mStrings.size(), PlanItem());
    QMap<QString, QVector<int> > byKey;
    for (int i = 0; i < mStrings.size(); i++) byKey[containerKey(mStrings[i])] << i;
    const int align = mTable.codeWidth();
    for (QMap<QString, QVector<int> >::const_iterator k = byKey.begin(); k != byKey.end(); ++k) {
        const PtrSpec sp = mSpecs.value(k.key());
        std::vector<std::pair<quint64, quint64> > freeList = parseRanges(sp.freeSpace);
        QVector<int> idx = k.value();
        std::stable_sort(idx.begin(), idx.end(), [this](int a, int b) { return mStrings[a].offset < mStrings[b].offset; });
        for (int n = 0; n < idx.size(); n++) {
            const int i = idx[n];
            const Str &s = mStrings[i];
            PlanItem &it = mPlan[(size_t)i];
            if (!s.enabled || s.translation.isEmpty()) continue;
            QString why;
            int over = 0;
            if (replacement(s, it.bytes, &why, &over)) { it.place = PlaceInPlace; continue; }
            it.place = PlaceRefused;
            it.why = why;
            if (over <= 0) continue;
            if (!sp.on) { it.why += " " + tr_("Set the pointers of this file (Pointers tab) to move it."); continue; }
            if (s.ptrs.isEmpty()) { it.why += " " + tr_("No pointer to this text was found."); continue; }
            std::vector<uint8_t> enc;
            std::string err;
            mTable.encode(toStd(s.translation), enc, err);
            enc.insert(enc.end(), s.orig.constData() + s.length, s.orig.constData() + s.orig.size());
            bool placed = false;
            for (size_t r = 0; r < freeList.size() && !placed; r++) {
                quint64 a = freeList[r].first;
                if (align > 1) a = (a + (quint64)align - 1) / (quint64)align * (quint64)align;
                if (a + enc.size() > freeList[r].second) continue;
                const qint64 value = sp.base + (qint64)a;
                if (value < 0 || (sp.width == 2 && value > 0xFFFF) || value > 0xFFFFFFFFLL) continue;
                freeList[r].first = a + enc.size();
                it.place = PlaceMoved;
                it.newOffset = a;
                it.bytes = enc;
                it.why.clear();
                placed = true;
            }
            if (!placed) { it.why += " " + tr_("Not enough free space left (%1 bytes needed).").arg(enc.size()); continue; }
            if (sp.reuse) {
                const quint64 c = containerOffset(s);
                freeList.push_back(std::make_pair(c, c + s.length + s.termLength));
            }
        }
    }
    mPlanOk = true;
}

const UIDebugTextEditor::PlanItem &UIDebugTextEditor::plan(int i)
{
    computePlan();
    return mPlan[(size_t)i];
}

// ===========================================================================
// String list and editor
// ===========================================================================

void UIDebugTextEditor::refreshAll()
{
    invalidatePlan();
    for (int i = 0; i < mStrings.size(); i++) updateStringRow(i);
    updateEditor();
    updateChips();
}

void UIDebugTextEditor::fillStrings()
{
    QSignalBlocker b(twStrings);
    mFilling = true;
    twStrings->clear();
    QList<QTreeWidgetItem *> items;
    for (int i = 0; i < mStrings.size(); i++) {
        QTreeWidgetItem *it = new QTreeWidgetItem();
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        items << it;
    }
    twStrings->addTopLevelItems(items);
    for (int i = 0; i < mStrings.size(); i++) updateStringRow(i);
    mFilling = false;
    if (mCurrent >= 0 && mCurrent < mStrings.size()) twStrings->setCurrentItem(twStrings->topLevelItem(mCurrent));
}

void UIDebugTextEditor::updateStringRow(int i)
{
    QTreeWidgetItem *it = twStrings->topLevelItem(i);
    if (!it || i >= mStrings.size()) return;
    const Str &s = mStrings[i];
    const PlanItem &p = plan(i);
    QSignalBlocker b(twStrings);
    it->setCheckState(ColOn, s.enabled ? Qt::Checked : Qt::Unchecked);
    it->setText(ColId, s.id);
    it->setText(ColFile, s.block >= 0 ? blockName(s.block) : fileName(s.offset));
    it->setText(ColOffset, s.block >= 0 ? "+" + hexOff(s.offset) : hexOff(s.offset));
    it->setText(ColPtrs, s.ptrs.isEmpty() ? QString() : QString::number(s.ptrs.size()));
    it->setText(ColOrig, oneLine(s.text));
    it->setText(ColTrans, oneLine(s.translation));
    std::vector<uint8_t> enc;
    std::string err;
    const bool encOk = !s.translation.isEmpty() && mTable.encode(toStd(s.translation), enc, err);
    QString bytes = s.translation.isEmpty() ? QString("%1").arg(s.length)
                  : encOk ? QString("%1 / %2").arg(enc.size()).arg(s.length) : QString("? / %1").arg(s.length);
    if (p.place == PlaceMoved) bytes += " -> " + QString::number(p.newOffset, 16).toUpper();
    it->setText(ColBytes, bytes);
    QColor c;
    if (p.place == PlaceInPlace) c = QColor(0x20, 0x90, 0x40);
    else if (p.place == PlaceMoved) c = QColor(0x20, 0x70, 0xD0);
    else if (p.place == PlaceRefused) c = QColor(0xD0, 0x30, 0x30);
    for (int col = ColTrans; col <= ColBytes; col++) it->setForeground(col, c.isValid() ? QBrush(c) : QBrush());
    it->setToolTip(ColBytes, p.place == PlaceNone ? tr_("Room: %1 byte(s), end code %2 byte(s)").arg(s.length).arg(s.termLength)
                           : p.place == PlaceInPlace ? tr_("Fits in place")
                           : p.place == PlaceMoved ? tr_("Moved to offset %1 of %2, %3 pointer(s) rewritten")
                                                         .arg(hexOff(p.newOffset)).arg(containerName(containerKey(s))).arg(s.ptrs.size())
                           : p.why);
}

void UIDebugTextEditor::onStringItemChanged(QTreeWidgetItem *item, int column)
{
    if (mFilling || column != ColOn) return;
    const int i = twStrings->indexOfTopLevelItem(item);
    if (i < 0 || i >= mStrings.size()) return;
    mStrings[i].enabled = item->checkState(ColOn) == Qt::Checked;
    refreshAll();
}

void UIDebugTextEditor::onStringSelected()
{
    QTreeWidgetItem *it = twStrings->currentItem();
    mCurrent = it ? twStrings->indexOfTopLevelItem(it) : -1;
    updateEditor();
    fillPtrPanel();
    if (mCurrent >= 0 && mCurrent < mStrings.size()) {
        leHexOffset->setText(QString::number(mStrings[mCurrent].offset, 16).toUpper());
        if (tabs->currentIndex() == 2) showHex(mStrings[mCurrent].block, mStrings[mCurrent].offset);
    }
}

void UIDebugTextEditor::updateEditor()
{
    const bool ok = mCurrent >= 0 && mCurrent < mStrings.size();
    {
        QSignalBlocker b(teTranslation);
        teOriginal->setPlainText(ok ? mStrings[mCurrent].text : QString());
        if (ok && teTranslation->toPlainText() != mStrings[mCurrent].translation)
            teTranslation->setPlainText(mStrings[mCurrent].translation);
        if (!ok) teTranslation->clear();
    }
    teTranslation->setEnabled(ok);
    if (!ok) { lBytes->clear(); return; }
    const Str &s = mStrings[mCurrent];
    QString text = tr_("%1 at %2 %3 - room: %4 byte(s) + end code %5 byte(s) [%6] - %7 pointer(s)")
                       .arg(s.id).arg(s.block >= 0 ? blockName(s.block) : fileName(s.offset)).arg(hexOff(s.offset))
                       .arg(s.length).arg(s.termLength)
                       .arg(fromStd(TextTools::hexBytes(std::vector<uint8_t>(s.orig.constData() + s.length, s.orig.constData() + s.orig.size()))))
                       .arg(s.ptrs.size());
    if (!s.translation.isEmpty()) {
        const PlanItem &p = plan(mCurrent);
        if (p.place == PlaceInPlace)
            text += "\n" + tr_("Translation: fits in place. Bytes written: %1").arg(fromStd(TextTools::hexBytes(p.bytes)));
        else if (p.place == PlaceMoved) {
            text += "\n" + tr_("Translation: moved to %1 (offset in %2), %3 pointer(s) rewritten. Bytes: %4")
                               .arg(hexOff(p.newOffset)).arg(containerName(containerKey(s))).arg(s.ptrs.size())
                               .arg(fromStd(TextTools::hexBytes(p.bytes)));
            quint64 discStart = 0;
            QByteArray dummy;
            if (s.block < 0 && containerData(containerKey(s), dummy, &discStart, 0))
                text += " - " + tr_("disc offset %1").arg(hexOff(discStart + p.newOffset));
            const QString href = QString("hex:%1:%2").arg(s.block).arg(s.block < 0 ? discStart + p.newOffset : p.newOffset);
            lBytes->setText(text.toHtmlEscaped().replace("\n", "<br>") +
                            QString(" <a href=\"%1\">%2</a>").arg(href).arg(tr_("Show the new place in the Hex view").toHtmlEscaped()));
            return;
        } else {
            text += "\n" + tr_("Translation: %1").arg(p.why);
        }
    }
    lBytes->setText(text.toHtmlEscaped().replace("\n", "<br>"));
}

void UIDebugTextEditor::setTranslation(int i, const QString &t)
{
    if (i < 0 || i >= mStrings.size()) return;
    mStrings[i].translation = t;
    // a moved text changes the free space left for the next ones
    invalidatePlan();
    const QString key = containerKey(mStrings[i]);
    if (mSpecs.value(key).on) {
        for (int k = 0; k < mStrings.size(); k++) if (containerKey(mStrings[k]) == key) updateStringRow(k);
    } else {
        updateStringRow(i);
    }
    if (i == mCurrent) updateEditor();
    updateChips();
}

void UIDebugTextEditor::onTranslationEdited()
{
    if (mCurrent < 0 || mCurrent >= mStrings.size()) return;
    setTranslation(mCurrent, teTranslation->toPlainText());
}

void UIDebugTextEditor::updateChips()
{
    lChipImage->setText(mImgOk ? QFileInfo(leImage->text()).fileName() : tr_("No disc image"));
    int done = 0, moved = 0, bad = 0;
    for (int i = 0; i < mStrings.size(); i++) {
        const PlanItem &p = plan(i);
        if (p.place == PlaceInPlace) done++;
        else if (p.place == PlaceMoved) { done++; moved++; }
        else if (p.place == PlaceRefused) bad++;
    }
    lChipStrings->setText(tr_("%1 string(s)").arg(mStrings.size()));
    lChipDone->setText(moved ? tr_("%1 translated, %2 moved").arg(done).arg(moved) : tr_("%1 translated").arg(done));
    lChipLong->setText(bad ? tr_("%1 to fix").arg(bad) : tr_("Nothing to fix"));
    lChipLong->setProperty("alert", bad > 0);
    lChipLong->style()->unpolish(lChipLong);
    lChipLong->style()->polish(lChipLong);
}

// ===========================================================================
// Relative search results
// ===========================================================================

void UIDebugTextEditor::onRelSelected()
{
    pbAddRel->setEnabled(twRel->currentItem() != nullptr);
}

int UIDebugTextEditor::applyRelativeHit(int row)
{
    if (row < 0 || row >= (int)mRelHits.size()) return 0;
    const std::vector<TblEntry> e = TextTools::relativeProposal(toStd(mRelQuery), mRelHits[(size_t)row]);
    for (size_t i = 0; i < e.size(); i++) mTable.set(e[i].bytes, e[i].text, TextTools::Normal);
    { QSignalBlocker bp(cbPreset); cbPreset->setCurrentIndex(3); }
    fillTableWidget();
    refreshAll();
    return (int)e.size();
}

void UIDebugTextEditor::onAddRelative()
{
    QTreeWidgetItem *it = twRel->currentItem();
    if (!it) return;
    const int n = applyRelativeHit(twRel->indexOfTopLevelItem(it));
    lStatus->setText(tr_("%1 letter(s) added to the table. Add the space, the punctuation and the end code by hand "
                         "(Hex view), then dump the strings.").arg(n));
}

// ===========================================================================
// Pointers and free space
// ===========================================================================

void UIDebugTextEditor::fillPtrPanel()
{
    if (!cbPtrOn) return;
    const QString key = ptrKey();
    const bool usable = key != "d" && mImgOk;
    const PtrSpec sp = mSpecs.value(key);
    QSignalBlocker b1(cbPtrOn), b2(cbPtrWidth), b3(cbPtrEndian), b4(lePtrBase), b5(cbPtrWhere), b6(leFree), b7(cbReuse);
    lPtrContainer->setText(usable ? tr_("Texts of %1").arg(containerName(key))
                                  : tr_("Select a string of the list (or a file / unpacked block on the left): pointers and free space belong to its file."));
    cbPtrWhere->clear();
    cbPtrWhere->addItem(tr_("in the same file / block"), -2);
    for (size_t i = 0; i < mFiles.size(); i++) cbPtrWhere->addItem(fromStd(mFiles[i].path), (int)i);
    cbPtrOn->setChecked(sp.on);
    cbPtrWidth->setCurrentIndex(sp.width == 2 ? 1 : 0);
    cbPtrEndian->setCurrentIndex(sp.bigEndian ? 0 : 1);
    lePtrBase->setText(sp.base < 0 ? "-" + QString::number(-sp.base, 16).toUpper() : QString::number(sp.base, 16).toUpper());
    const int w = cbPtrWhere->findData(sp.where);
    cbPtrWhere->setCurrentIndex(w < 0 ? 0 : w);
    leFree->setText(sp.freeSpace);
    cbReuse->setChecked(sp.reuse);
    foreach (QWidget *wd, QList<QWidget *>() << cbPtrOn << cbPtrWidth << cbPtrEndian << lePtrBase << cbPtrWhere << leFree << cbReuse)
        wd->setEnabled(usable);
    int n = 0, withPtr = 0;
    for (int i = 0; i < mStrings.size(); i++)
        if (containerKey(mStrings[i]) == key) { n++; if (!mStrings[i].ptrs.isEmpty()) withPtr++; }
    lPtrInfo->setText(usable ? tr_("%1 string(s) of this file / block in the list, %2 with pointers.").arg(n).arg(withPtr) : QString());
}

void UIDebugTextEditor::onPtrSpecEdited()
{
    const QString key = ptrKey();
    if (key == "d") return;
    PtrSpec &sp = mSpecs[key];
    sp.on = cbPtrOn->isChecked();
    sp.width = cbPtrWidth->currentIndex() == 1 ? 2 : 4;
    sp.bigEndian = cbPtrEndian->currentIndex() == 0;
    QString t = lePtrBase->text().trimmed().replace("0x", "", Qt::CaseInsensitive);
    const bool neg = t.startsWith('-');
    if (neg) t.remove(0, 1);
    bool ok = false;
    const qint64 v = (qint64)t.toULongLong(&ok, 16);
    if (ok) sp.base = neg ? -v : v;
    sp.where = cbPtrWhere->currentData().toInt();
    sp.freeSpace = leFree->text();
    sp.reuse = cbReuse->isChecked();
    refreshAll();
}

std::vector<std::pair<int64_t, int> > UIDebugTextEditor::guessBase(const QString &key, QString *msg)
{
    std::vector<std::pair<int64_t, int> > out;
    const PtrSpec sp = mSpecs.value(key);
    std::vector<uint64_t> offs;
    for (int i = 0; i < mStrings.size(); i++)
        if (containerKey(mStrings[i]) == key) offs.push_back(containerOffset(mStrings[i]));
    if (offs.size() < 2) { if (msg) *msg = tr_("Find at least 2 strings of this file / block first (dump its strings)."); return out; }
    QByteArray ptrData;
    QString err;
    const bool ok = sp.where >= 0 ? fileData(sp.where, ptrData) : containerData(key, ptrData, 0, &err);
    if (!ok) { if (msg) *msg = err.isEmpty() ? tr_("The file of the pointers cannot be read.") : err; return out; }
    std::sort(offs.begin(), offs.end());
    out = TextTools::guessPointerBase((const uint8_t *)ptrData.constData(), (size_t)ptrData.size(), offs, sp.width, sp.bigEndian, 8);
    if (msg) *msg = out.empty() ? tr_("No base found: try the other width, byte order, or the file that holds the pointers.")
                                : tr_("Best base: %1, it reaches %2 of %3 strings.")
                                      .arg(out[0].first < 0 ? "-" + QString::number(-out[0].first, 16).toUpper() : QString::number(out[0].first, 16).toUpper())
                                      .arg(out[0].second).arg(offs.size())
                                  + (out[0].second * 2 < (int)offs.size()
                                     ? " " + tr_("Few strings: this base may be chance. Run the game to where the texts show and use \"Base from RAM\" "
                                                 "(or the strings found are not all real texts, or the game reads them by index).")
                                     : QString());
    return out;
}

// Where the game has loaded this file / block: pieces of it (the strings
// found, else every 4 KB) are looked for in the work RAM while the game runs.
std::vector<std::pair<int64_t, int> > UIDebugTextEditor::baseFromRam(const QString &key, QString *msg)
{
    std::vector<std::pair<int64_t, int> > out;
    if (!HighWram || !LowWram) { if (msg) *msg = tr_("Start the game and go where the texts are shown first."); return out; }
    QByteArray d;
    QString err;
    if (!containerData(key, d, 0, &err)) { if (msg) *msg = err; return out; }
    const int probeLen = 24;
    std::vector<quint64> probes;
    for (int i = 0; i < mStrings.size() && probes.size() < 64; i++)
        if (containerKey(mStrings[i]) == key && mStrings[i].length + mStrings[i].termLength >= 8) probes.push_back(containerOffset(mStrings[i]));
    for (quint64 o = 0; probes.size() < 64 && o + probeLen <= (quint64)d.size(); o += 4096) {
        // a piece with some variety (not a fill)
        int distinct = 0;
        bool seen[256] = { false };
        for (int k = 0; k < probeLen; k++) if (!seen[(uint8_t)d[(int)(o + k)]]) { seen[(uint8_t)d[(int)(o + k)]] = true; distinct++; }
        if (distinct >= 8) probes.push_back(o);
    }
    if (probes.empty()) { if (msg) *msg = tr_("Nothing to look for in this file."); return out; }
    std::map<int64_t, int> votes;
    u8 *mems[2] = { HighWram, LowWram };
    const quint32 bases[2] = { 0x06000000, 0x00200000 };
    std::vector<uint8_t> lin(0x100000);
    for (int m = 0; m < 2; m++) {
        for (u32 a = 0; a < 0x100000; a++) lin[a] = T2ReadByte(mems[m], a);
        for (size_t k = 0; k < probes.size(); k++) {
            const quint64 o = probes[k];
            const int len = (int)qMin<quint64>(probeLen, (quint64)d.size() - o);
            const uint8_t *pat = (const uint8_t *)d.constData() + o;
            std::vector<uint8_t>::const_iterator it = lin.begin();
            int hits = 0;
            while (hits < 8) {
                it = std::search(it, lin.cend(), pat, pat + len);
                if (it == lin.cend()) break;
                votes[(int64_t)bases[m] + (int64_t)(it - lin.cbegin()) - (int64_t)o]++;
                ++it;
                hits++;
            }
        }
    }
    for (std::map<int64_t, int>::const_iterator it = votes.begin(); it != votes.end(); ++it) out.push_back(std::make_pair(it->first, it->second));
    std::sort(out.begin(), out.end(), [](const std::pair<int64_t, int> &a, const std::pair<int64_t, int> &b) { return a.second > b.second; });
    if (out.size() > 8) out.resize(8);
    if (msg) *msg = out.empty() ? tr_("This file is not in the work RAM now (not loaded yet, compressed, or loaded elsewhere).")
                                : tr_("Loaded at %1 (%2 of %3 pieces found there). Use this base, then Find the pointers.")
                                      .arg(QString::number(out[0].first, 16).toUpper()).arg(out[0].second).arg(probes.size());
    return out;
}

void UIDebugTextEditor::onBaseFromRam()
{
    onPtrSpecEdited();
    QString msg;
    const std::vector<std::pair<int64_t, int> > r = baseFromRam(ptrKey(), &msg);
    lwBases->clear();
    for (size_t i = 0; i < r.size(); i++) {
        const QString b = QString::number(r[i].first, 16).toUpper();
        QListWidgetItem *it = new QListWidgetItem(tr_("%1  -  loaded here (%2 piece(s) found)").arg(b).arg(r[i].second), lwBases);
        it->setData(Qt::UserRole, b);
    }
    if (lwBases->count()) lwBases->setCurrentRow(0);
    lPtrInfo->setText(msg);
}

void UIDebugTextEditor::onGuessBase()
{
    onPtrSpecEdited();
    QString msg;
    const std::vector<std::pair<int64_t, int> > r = guessBase(ptrKey(), &msg);
    lwBases->clear();
    for (size_t i = 0; i < r.size(); i++) {
        const QString b = r[i].first < 0 ? "-" + QString::number(-r[i].first, 16).toUpper() : QString::number(r[i].first, 16).toUpper();
        QListWidgetItem *it = new QListWidgetItem(tr_("%1  -  %2 string(s)").arg(b).arg(r[i].second), lwBases);
        it->setData(Qt::UserRole, b);
    }
    if (lwBases->count()) lwBases->setCurrentRow(0);
    lPtrInfo->setText(msg);
}

void UIDebugTextEditor::onUseBase()
{
    QListWidgetItem *it = lwBases->currentItem();
    if (!it) return;
    lePtrBase->setText(it->data(Qt::UserRole).toString());
    cbPtrOn->setChecked(true);
    onPtrSpecEdited();
    onFindPointers();
}

int UIDebugTextEditor::findPointers(const QString &key, QString *msg)
{
    const PtrSpec sp = mSpecs.value(key);
    QByteArray ptrData;
    QString err;
    const bool ok = sp.where >= 0 ? fileData(sp.where, ptrData) : containerData(key, ptrData, 0, &err);
    if (!ok) { if (msg) *msg = err.isEmpty() ? tr_("The file of the pointers cannot be read.") : err; return -1; }
    // where the positions are: a disc offset (raw file) or an offset in the block
    const bool inBlock = sp.where < 0 && key.startsWith("b:");
    quint64 ptrDiscStart = 0;
    if (sp.where >= 0) ptrDiscStart = (quint64)mFiles[sp.where].lba * 2048;
    else if (key.startsWith("f:")) ptrDiscStart = (quint64)mFiles[key.mid(2).toInt()].lba * 2048;
    TextTools::PtrFormat f;
    f.width = sp.width;
    f.bigEndian = sp.bigEndian;
    f.base = sp.base;
    int n = 0, with = 0, words = 0;
    for (int i = 0; i < mStrings.size(); i++) {
        Str &s = mStrings[i];
        if (containerKey(s) != key) continue;
        n++;
        const std::vector<uint64_t> pos = TextTools::findPointers((const uint8_t *)ptrData.constData(), (size_t)ptrData.size(), f,
                                                                   containerOffset(s));
        s.ptrs.clear();
        s.ptrsInBlock = inBlock;
        for (size_t k = 0; k < pos.size(); k++) s.ptrs << (inBlock ? pos[k] : ptrDiscStart + pos[k]);
        if (!pos.empty()) with++;
        words += (int)pos.size();
    }
    fillStrings();
    refreshAll();
    fillPtrPanel();
    if (msg) *msg = tr_("%1 of %2 string(s) have pointers (%3 word(s)).").arg(with).arg(n).arg(words);
    return with;
}

void UIDebugTextEditor::onFindPointers()
{
    onPtrSpecEdited();
    QString msg;
    findPointers(ptrKey(), &msg);
    lPtrInfo->setText(msg);
    lStatus->setText(msg);
}

QString UIDebugTextEditor::findFreeSpace(const QString &key, int minLen, QString *note)
{
    QByteArray d;
    if (note) note->clear();
    if (!containerData(key, d, 0, 0)) return QString();
    std::vector<std::pair<uint64_t, uint64_t> > r =
        TextTools::findFreeSpace((const uint8_t *)d.constData(), (size_t)d.size(), (size_t)minLen);
    // Game running and the file loaded at the base: a run whose RAM bytes are
    // not what the file holds any more is written by the game (variables,
    // buffers). Only the parts still equal to the file are kept.
    const PtrSpec sp = mSpecs.value(key);
    u32 o0 = 0;
    u8 *mem = (sp.width == 4 && HighWram) ? wramOf((quint32)sp.base, &o0) : nullptr;
    bool loaded = false;
    if (mem) {
        int same = 0, tried = 0;
        for (int i = 0; i < mStrings.size() && tried < 16; i++) {
            if (containerKey(mStrings[i]) != key) continue;
            const quint64 c = containerOffset(mStrings[i]);
            u32 off;
            u8 *m = wramOf((quint32)(sp.base + (qint64)c), &off);
            if (!m || off + (u32)mStrings[i].orig.size() > 0x100000) continue;
            tried++;
            bool eq = true;
            for (int k = 0; k < mStrings[i].orig.size() && eq; k++) eq = T2ReadByte(m, off + (u32)k) == (u8)mStrings[i].orig[k];
            if (eq) same++;
        }
        loaded = tried > 0 && same * 2 >= tried;
    }
    quint64 dropped = 0;
    if (loaded) {
        std::vector<std::pair<uint64_t, uint64_t> > kept;
        for (size_t i = 0; i < r.size(); i++) {
            uint64_t run = r[i].first;
            for (uint64_t p = r[i].first; p <= r[i].second; p++) {
                bool ok = p < r[i].second;
                if (ok) {
                    u32 off;
                    u8 *m = wramOf((quint32)(sp.base + (qint64)p), &off);
                    ok = m && T2ReadByte(m, off) == (u8)d[(int)p];
                }
                if (!ok) {
                    if (p - run >= (uint64_t)minLen) kept.push_back(std::make_pair(run, p));
                    else dropped += p - run;
                    if (p < r[i].second) dropped++;
                    run = p + 1;
                }
            }
        }
        r = kept;
    }
    QStringList parts;
    for (size_t i = 0; i < r.size() && i < 64; i++)
        parts << QString("%1-%2").arg(QString::number(r[i].first, 16).toUpper()).arg(QString::number(r[i].second - 1, 16).toUpper());
    if (note) {
        if (loaded) *note = tr_("Checked in the work RAM: %1 byte(s) of 00 / FF that the game has changed are left out. "
                                "Play a little further and search again to check more.").arg(dropped);
        else *note = tr_("Not checked in RAM (game not running, or the file is not loaded at this base): check the runs in the Hex view.");
    }
    return parts.join(", ");
}

void UIDebugTextEditor::onFindFree()
{
    onPtrSpecEdited();
    QString note;
    const QString t = findFreeSpace(ptrKey(), 32, &note);
    if (t.isEmpty()) { lPtrInfo->setText(tr_("No run of 32 bytes of 00 or FF found: type the free space by hand.") + " " + note); return; }
    leFree->setText(t);
    onPtrSpecEdited();
    lPtrInfo->setText(tr_("Runs of 00 / FF found (offsets in the file). They must not be data the game uses.") + " " + note);
}

// ===========================================================================
// Character table
// ===========================================================================

TextTools::CharTable UIDebugTextEditor::shiftJisTable()
{
    static CharTable cached;
    if (!cached.empty()) return cached;
    std::vector<TblEntry> v;
    QTextCodec *codec = QTextCodec::codecForName("Shift-JIS");
    for (int c = 0x20; c < 0x7F; c++) {
        TblEntry e;
        e.bytes.push_back((uint8_t)c);
        e.text = std::string(1, (char)c);
        v.push_back(e);
    }
    if (codec) {
        for (int c = 0xA1; c <= 0xDF; c++) {   // half-width katakana
            const char b = (char)c;
            QTextCodec::ConverterState st(QTextCodec::ConvertInvalidToNull);
            const QString s = codec->toUnicode(&b, 1, &st);
            if (st.invalidChars || s.size() != 1 || s[0].isNull()) continue;
            TblEntry e;
            e.bytes.push_back((uint8_t)c);
            e.text = toStd(s);
            v.push_back(e);
        }
        for (int lead = 0x81; lead <= 0xEF; lead++) {
            if (lead > 0x9F && lead < 0xE0) continue;
            for (int trail = 0x40; trail <= 0xFC; trail++) {
                if (trail == 0x7F) continue;
                const char b[2] = { (char)lead, (char)trail };
                QTextCodec::ConverterState st(QTextCodec::ConvertInvalidToNull);
                const QString s = codec->toUnicode(b, 2, &st);
                if (st.invalidChars || s.size() != 1 || s[0].isNull() || s[0] == QChar::ReplacementCharacter) continue;
                TblEntry e;
                e.bytes.push_back((uint8_t)lead);
                e.bytes.push_back((uint8_t)trail);
                e.text = toStd(s);
                v.push_back(e);
            }
        }
    }
    TblEntry end;
    end.bytes.push_back(0);
    end.kind = TextTools::End;
    v.push_back(end);
    cached.setEntries(v);
    return cached;
}

void UIDebugTextEditor::setTable(const TextTools::CharTable &t)
{
    mTable = t;
    fillTableWidget();
    refreshAll();
}

void UIDebugTextEditor::fillTableWidget()
{
    QSignalBlocker b(twTable);
    mFilling = true;
    const std::vector<TblEntry> &e = mTable.entries();
    twTable->setRowCount((int)e.size());
    for (size_t i = 0; i < e.size(); i++) {
        twTable->setItem((int)i, 0, new QTableWidgetItem(fromStd(TextTools::hexBytes(e[i].bytes, ""))));
        twTable->setItem((int)i, 1, new QTableWidgetItem(showText(e[i].text)));
        twTable->setItem((int)i, 2, new QTableWidgetItem(kindName(e[i].kind)));
    }
    mFilling = false;
    lTableInfo->setText(tr_("%1 entries, codes of %2 byte(s), end code %3")
                            .arg(e.size()).arg(mTable.codeWidth())
                            .arg(mTable.endBytes().empty() ? tr_("none") : fromStd(TextTools::hexBytes(mTable.endBytes(), ""))));
    onTestChanged();
}

void UIDebugTextEditor::onTableCellChanged(int, int)
{
    if (mFilling) return;
    std::vector<TblEntry> v;
    int bad = 0;
    for (int r = 0; r < twTable->rowCount(); r++) {
        QTableWidgetItem *h = twTable->item(r, 0), *t = twTable->item(r, 1), *k = twTable->item(r, 2);
        TblEntry e;
        if (!h || !TextTools::parseHexBytes(toStd(h->text()), e.bytes)) { if (h && !h->text().trimmed().isEmpty()) bad++; continue; }
        e.text = t ? readText(t->text()) : std::string();
        e.kind = k ? kindFromName(k->text()) : TextTools::Normal;
        if (e.kind == TextTools::NewLine && e.text.empty()) e.text = "\n";
        v.push_back(e);
    }
    mTable.setEntries(v);
    { QSignalBlocker bp(cbPreset); cbPreset->setCurrentIndex(3); }
    lTableInfo->setText(tr_("%1 entries, codes of %2 byte(s), end code %3")
                            .arg(v.size()).arg(mTable.codeWidth())
                            .arg(mTable.endBytes().empty() ? tr_("none") : fromStd(TextTools::hexBytes(mTable.endBytes(), "")))
                        + (bad ? " - " + tr_("%1 row(s) with a bad hex code are ignored").arg(bad) : QString()));
    refreshAll();
    onTestChanged();
}

void UIDebugTextEditor::onAddEntry()
{
    const int r = twTable->rowCount();
    {
        QSignalBlocker b(twTable);
        twTable->insertRow(r);
        twTable->setItem(r, 0, new QTableWidgetItem(QString()));
        twTable->setItem(r, 1, new QTableWidgetItem(QString()));
        twTable->setItem(r, 2, new QTableWidgetItem(QString()));
    }
    twTable->setCurrentCell(r, 0);
    twTable->editItem(twTable->item(r, 0));
}

void UIDebugTextEditor::onDeleteEntries()
{
    QList<int> rows;
    foreach (QTableWidgetItem *it, twTable->selectedItems()) if (!rows.contains(it->row())) rows << it->row();
    std::sort(rows.begin(), rows.end());
    {
        QSignalBlocker b(twTable);
        for (int k = rows.size() - 1; k >= 0; k--) twTable->removeRow(rows[k]);
    }
    onTableCellChanged(0, 0);
}

void UIDebugTextEditor::onTestChanged()
{
    if (!leTest || leTest->text().isEmpty()) { if (lTestOut) lTestOut->clear(); return; }
    std::vector<uint8_t> b;
    std::string err;
    if (mTable.encode(readText(leTest->text()), b, err)) lTestOut->setText(fromStd(TextTools::hexBytes(b)));
    else lTestOut->setText(fromStd(err));
}

void UIDebugTextEditor::onPreset(int index)
{
    if (index == 3) return;
    if (!mTable.empty() && QMessageBox::question(this, windowTitle(), tr_("Replace the current table?"),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    if (index == 0) setTable(TextTools::asciiTable());
    else if (index == 1) setTable(shiftJisTable());
    else if (index == 2) setTable(CharTable());
    QApplication::restoreOverrideCursor();
}

bool UIDebugTextEditor::loadTableFile(const QString &path, QString *errOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (errOut) *errOut = tr_("%1 cannot be read.").arg(path); return false; }
    const QByteArray raw = f.readAll();
    // UTF-8 when valid, Shift-JIS otherwise (tables of Japanese games)
    QTextCodec::ConverterState st;
    QString text = QTextCodec::codecForName("UTF-8")->toUnicode(raw.constData(), raw.size(), &st);
    if (st.invalidChars > 0) {
        if (QTextCodec *sj = QTextCodec::codecForName("Shift-JIS")) text = sj->toUnicode(raw);
    }
    CharTable t;
    std::string err;
    int bad = 0;
    if (!t.parse(toStd(text), err, &bad)) { if (errOut) *errOut = fromStd(err); return false; }
    setTable(t);
    { QSignalBlocker bp(cbPreset); cbPreset->setCurrentIndex(3); }
    lStatus->setText(tr_("%1: %2 entries").arg(QFileInfo(path).fileName()).arg(t.size())
                     + (bad ? ", " + tr_("%1 line(s) not understood").arg(bad) : QString()));
    return true;
}

bool UIDebugTextEditor::saveTableFile(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const std::string t = mTable.toTbl();
    return f.write(t.data(), (qint64)t.size()) == (qint64)t.size();
}

void UIDebugTextEditor::onLoadTable()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Load a character table"), QString(), "Tables (*.tbl *.txt);;All files (*)");
    if (f.isEmpty()) return;
    QString err;
    if (!loadTableFile(f, &err)) QMessageBox::warning(this, windowTitle(), err);
}

void UIDebugTextEditor::onSaveTable()
{
    const QString f = CommonDialogs::getSaveFileName(QString(), tr_("Save the character table"), "Tables (*.tbl)");
    if (f.isEmpty()) return;
    if (!saveTableFile(f)) QMessageBox::warning(this, windowTitle(), tr_("An error occured while writing file."));
}

// ===========================================================================
// Hex view
// ===========================================================================

void UIDebugTextEditor::onHexGo()
{
    bool ok = false;
    const quint64 off = leHexOffset->text().trimmed().replace("0x", "", Qt::CaseInsensitive).toULongLong(&ok, 16);
    int block;
    quint64 st, len;
    QString name;
    scope(block, st, len, name);
    if (ok) showHex(block, off);
    tabs->setCurrentIndex(2);
}

void UIDebugTextEditor::showHex(int block, quint64 offset)
{
    if (!mImgOk) return;
    const TextTools::Source src = block >= 0 ? TextTools::Source((const uint8_t *)mBlocks[block].data.constData(), (size_t)mBlocks[block].data.size())
                                             : TextTools::Source(mImg, 0);
    const quint64 total = src.size();
    if (offset >= total) { teHex->setPlainText(tr_("Past the end of the data.")); return; }
    const quint64 start = (offset & ~(quint64)15) >= 0x40 ? (offset & ~(quint64)15) - 0x40 : 0;
    const size_t n = (size_t)qMin<quint64>(0x200, total - start);
    std::vector<uint8_t> b;
    std::string err;
    if (!src.read(start, n, b, err)) { teHex->setPlainText(fromStd(err)); return; }
    std::vector<QString> cell(n, QString("."));
    for (size_t p = 0; p < n; ) {
        const TblEntry *e = mTable.match(&b[p], n - p);
        if (!e) { p++; continue; }
        QString t = e->kind == TextTools::End ? QString::fromUtf8("\xC2\xB6") : e->kind == TextTools::NewLine ? QString::fromUtf8("\xE2\x8F\x8E")
                                                                                                             : fromStd(e->text);
        if (t.isEmpty() || t.size() > 1) t = t.isEmpty() ? QString(".") : QString::fromUtf8("\xE2\x80\xA6");
        if (t[0].isSpace()) t = " ";
        cell[p] = t;
        for (size_t k = 1; k < e->bytes.size() && p + k < n; k++) cell[p + k] = QString();
        p += e->bytes.size();
    }
    QString out = block >= 0 ? tr_("Unpacked block %1").arg(blockName(block)) + "\n" : QString();
    for (size_t row = 0; row < n; row += 16) {
        const quint64 a = start + row;
        out += (offset >= a && offset < a + 16) ? ">" : " ";
        out += QString("%1  ").arg(a, 8, 16, QLatin1Char('0')).toUpper();
        QString text;
        for (size_t k = 0; k < 16; k++) {
            if (row + k < n) out += QString("%1 ").arg(b[row + k], 2, 16, QLatin1Char('0')).toUpper();
            else out += "   ";
            if (k == 7) out += " ";
            if (row + k < n) text += cell[row + k];
        }
        const QString file = block >= 0 ? QString() : fileName(a);
        out += " " + text + (file.isEmpty() ? QString() : "   " + file) + "\n";
    }
    teHex->setPlainText(out);
}

// ===========================================================================
// Live in game: the original text is looked for in the work RAM (High
// 0x06000000 and Low 0x00200000, 1 MB each) and replaced by the translation.
// A moved text (32-bit big endian pointers whose base is in the work RAM) is
// written at base + its new offset, and the words holding its old address
// are rewritten.
// ===========================================================================


int UIDebugTextEditor::writeLive()
{
    if (!HighWram || !LowWram) return 0;
    computePlan();
    struct Pat { QByteArray orig; std::vector<uint8_t> repl; bool moved; qint64 base; quint64 coff, newOff; };
    std::vector<Pat> pats;
    std::unordered_map<uint32_t, std::vector<int> > byKey;
    for (int i = 0; i < mStrings.size(); i++) {
        const Str &s = mStrings[i];
        const PlanItem &p = mPlan[(size_t)i];
        if (s.orig.size() < 4 || (p.place != PlaceInPlace && p.place != PlaceMoved)) continue;
        Pat pt;
        pt.orig = s.orig;
        pt.repl = p.bytes;
        pt.moved = p.place == PlaceMoved;
        pt.coff = containerOffset(s);
        pt.newOff = p.newOffset;
        pt.base = 0;
        if (pt.moved) {
            const PtrSpec sp = mSpecs.value(containerKey(s));
            u32 o;
            if (sp.width != 4 || !sp.bigEndian || !wramOf((quint32)sp.base, &o)) continue;   // not a RAM address
            pt.base = sp.base;
        } else if (memcmp(pt.repl.data(), s.orig.constData(), pt.repl.size()) == 0) {
            continue;
        }
        const uint8_t *o = (const uint8_t *)s.orig.constData();
        byKey[(uint32_t)o[0] << 24 | o[1] << 16 | o[2] << 8 | o[3]].push_back((int)pats.size());
        pats.push_back(pt);
    }
    if (pats.empty()) return 0;
    int written = 0;
    u8 *mems[2] = { HighWram, LowWram };
    const quint32 bases[2] = { 0x06000000, 0x00200000 };
    std::vector<uint8_t> lin(0x100000);
    std::vector<std::pair<quint32, quint32> > retarget;   // old address -> new address (moved texts)
    for (int m = 0; m < 2; m++) {
        u8 *mem = mems[m];
        for (u32 a = 0; a < 0x100000; a++) lin[a] = T2ReadByte(mem, a);
        for (size_t p = 0; p + 4 <= lin.size(); p++) {
            const uint32_t key = (uint32_t)lin[p] << 24 | lin[p + 1] << 16 | lin[p + 2] << 8 | lin[p + 3];
            std::unordered_map<uint32_t, std::vector<int> >::const_iterator it = byKey.find(key);
            if (it == byKey.end()) continue;
            for (size_t k = 0; k < it->second.size(); k++) {
                const Pat &pt = pats[(size_t)it->second[k]];
                const size_t len = (size_t)pt.orig.size();
                if (p + len > lin.size() || memcmp(&lin[p], pt.orig.constData(), len) != 0) continue;
                const quint32 at = bases[m] + (quint32)p;
                if (!pt.moved) {
                    for (size_t j = 0; j < len; j++) { lin[p + j] = pt.repl[j]; T2WriteByte(mem, (u32)(p + j), pt.repl[j]); }
                    written++;
                    p += len - 1;
                    break;
                }
                // the container is loaded at base: the text must be at base + its offset
                if ((at & 0x0FFFFFFF) != (quint32)((pt.base + (qint64)pt.coff) & 0x0FFFFFFF)) continue;
                const quint32 dst = (quint32)(pt.base + (qint64)pt.newOff);
                u32 doff;
                u8 *dm = wramOf(dst, &doff);
                if (!dm || doff + pt.repl.size() > 0x100000) continue;
                bool changed = false;
                for (size_t j = 0; j < pt.repl.size(); j++)
                    if (T2ReadByte(dm, doff + (u32)j) != pt.repl[j]) { T2WriteByte(dm, doff + (u32)j, pt.repl[j]); changed = true; }
                if (dm == mem) for (size_t j = 0; j < pt.repl.size(); j++) lin[doff + j] = pt.repl[j];
                retarget.push_back(std::make_pair(at & 0x0FFFFFFF, dst & 0x0FFFFFFF));
                if (changed) written++;
                break;
            }
        }
    }
    // pointers of the moved texts (aligned 32-bit words holding the old address)
    if (!retarget.empty()) {
        std::unordered_map<uint32_t, uint32_t> map;
        for (size_t i = 0; i < retarget.size(); i++) map[retarget[i].first] = retarget[i].second;
        for (int m = 0; m < 2; m++) {
            u8 *mem = mems[m];
            for (u32 a = 0; a + 4 <= 0x100000; a += 4) {
                const uint32_t v = (uint32_t)T2ReadByte(mem, a) << 24 | T2ReadByte(mem, a + 1) << 16 | T2ReadByte(mem, a + 2) << 8 | T2ReadByte(mem, a + 3);
                std::unordered_map<uint32_t, uint32_t>::const_iterator it = map.find(v & 0x0FFFFFFF);
                if (it == map.end() || (v & 0xF0000000) > 0x20000000) continue;
                const uint32_t nv = (v & 0xF0000000) | it->second;
                T2WriteByte(mem, a, (u8)(nv >> 24)); T2WriteByte(mem, a + 1, (u8)(nv >> 16));
                T2WriteByte(mem, a + 2, (u8)(nv >> 8)); T2WriteByte(mem, a + 3, (u8)nv);
                written++;
            }
        }
    }
    return written;
}

void UIDebugTextEditor::onLiveToggled(bool on)
{
    if (on) { mLiveTimer->start(); onLiveTick(); }
    else { mLiveTimer->stop(); lStatus->setText(tr_("Live in game stopped.")); }
}

void UIDebugTextEditor::onLiveTick()
{
    if (!HighWram) { lStatus->setText(tr_("Live in game: no game running.")); return; }
    const int n = writeLive();
    if (n) lStatus->setText(tr_("%1  Live: %2 write(s) into the work RAM.").arg(QTime::currentTime().toString("HH:mm:ss")).arg(n));
}

// ===========================================================================
// Project, CSV
// ===========================================================================

bool UIDebugTextEditor::saveProject(const QString &path) const
{
    QJsonObject root;
    root["type"] = "kronos-text";
    root["version"] = 2;
    root["image"] = leImage->text();
    root["product"] = UIEditsExport::currentProduct();
    root["table"] = fromStd(mTable.toTbl());
    root["padding"] = lePadding->text();
    QJsonArray blocks;
    for (int b = 0; b < mBlocks.size(); b++) {
        QJsonObject o;
        o["file"] = mBlocks[b].file >= 0 ? fromStd(mFiles[mBlocks[b].file].path) : QString();
        o["packOffset"] = QString::number(mBlocks[b].packOffset, 16).toUpper();
        o["packSize"] = (int)mBlocks[b].packSize;
        o["size"] = mBlocks[b].data.size();
        o["codec"] = mBlocks[b].codec;
        blocks.append(o);
    }
    root["blocks"] = blocks;
    QJsonObject specs;
    for (QMap<QString, PtrSpec>::const_iterator it = mSpecs.begin(); it != mSpecs.end(); ++it) {
        QString key = it.key();
        if (key.startsWith("f:")) {
            const int i = key.mid(2).toInt();
            if (i < 0 || i >= (int)mFiles.size()) continue;
            key = "f:" + fromStd(mFiles[i].path);
        }
        QJsonObject o;
        o["on"] = it->on;
        o["width"] = it->width;
        o["bigEndian"] = it->bigEndian;
        o["base"] = QString::number(it->base);
        o["where"] = it->where >= 0 && it->where < (int)mFiles.size() ? fromStd(mFiles[it->where].path) : QString();
        o["free"] = it->freeSpace;
        o["reuse"] = it->reuse;
        specs[key] = o;
    }
    root["pointers"] = specs;
    QJsonArray a;
    for (int i = 0; i < mStrings.size(); i++) {
        const Str &s = mStrings[i];
        QJsonObject o;
        o["id"] = s.id;
        o["block"] = s.block;
        o["offset"] = QString::number(s.offset, 16).toUpper();
        o["length"] = (int)s.length;
        o["term"] = (int)s.termLength;
        o["orig"] = QString::fromLatin1(s.orig.toHex().toUpper());
        o["text"] = s.text;
        o["translation"] = s.translation;
        o["enabled"] = s.enabled;
        QJsonArray p;
        for (int k = 0; k < s.ptrs.size(); k++) p.append(QString::number(s.ptrs[k], 16).toUpper());
        o["ptrs"] = p;
        o["ptrsInBlock"] = s.ptrsInBlock;
        a.append(o);
    }
    root["strings"] = a;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray j = QJsonDocument(root).toJson();
    return f.write(j) == j.size();
}

bool UIDebugTextEditor::loadProject(const QString &path, QString *errOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (errOut) *errOut = tr_("%1 cannot be read.").arg(path); return false; }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root["type"].toString() != "kronos-text") { if (errOut) *errOut = tr_("%1 is not a project of the text editor.").arg(path); return false; }
    QStringList notes;
    const QString img = root["image"].toString();
    if (!img.isEmpty() && QFileInfo(img).exists() && img != leImage->text()) openImage(img);
    const QString prod = root["product"].toString(), cur = UIEditsExport::currentProduct();
    if (!prod.isEmpty() && !cur.isEmpty() && prod != cur)
        notes << tr_("This project was made for %1, the disc in the drive is %2.").arg(prod).arg(cur);
    CharTable t;
    std::string err;
    if (t.parse(toStd(root["table"].toString()), err)) { mTable = t; QSignalBlocker bp(cbPreset); cbPreset->setCurrentIndex(3); }
    { QSignalBlocker bp(lePadding); lePadding->setText(root["padding"].toString()); }
    auto fileIndex = [this](const QString &p) -> int {
        for (size_t i = 0; i < mFiles.size(); i++) if (fromStd(mFiles[i].path) == p) return (int)i;
        return -1;
    };
    // blocks: unpacked again from the image
    mBlocks.clear();
    QVector<int> blockMap;
    const QJsonArray ba = root["blocks"].toArray();
    for (int i = 0; i < ba.size(); i++) {
        const QJsonObject o = ba[i].toObject();
        Block bl;
        bl.file = fileIndex(o["file"].toString());
        bl.packOffset = o["packOffset"].toString().toULongLong(0, 16);
        bl.codec = o["codec"].toInt();
        std::vector<uint8_t> packed, out;
        size_t used = 0;
        const quint64 avail = mImgOk ? EditsPatcher::userDataSize(mImg, 0) - qMin<quint64>(bl.packOffset, EditsPatcher::userDataSize(mImg, 0)) : 0;
        const size_t n = (size_t)qMin<quint64>(avail, qMax<quint64>((quint64)o["packSize"].toInt(), 16u) + 16);
        if (!mImgOk || !EditsPatcher::readUserData(mImg, 0, bl.packOffset, n, packed, err) ||
            !TextTools::decompress(bl.codec, packed.data(), qMin<size_t>(packed.size(), (size_t)o["packSize"].toInt()), out, &used, 16u << 20, err) ||
            (int)out.size() != o["size"].toInt()) {
            notes << tr_("A compressed block at %1 is not on this image any more: its strings are left out.").arg(hexOff(bl.packOffset));
            blockMap << -1;
            continue;
        }
        bl.packSize = (quint32)used;
        bl.data = QByteArray((const char *)out.data(), (int)out.size());
        bl.room = blockRoom(bl);
        blockMap << mBlocks.size();
        mBlocks.push_back(bl);
    }
    mSpecs.clear();
    const QJsonObject specs = root["pointers"].toObject();
    for (QJsonObject::const_iterator it = specs.begin(); it != specs.end(); ++it) {
        QString key = it.key();
        if (key.startsWith("f:")) { const int fi = fileIndex(key.mid(2)); if (fi < 0) continue; key = QString("f:%1").arg(fi); }
        else if (key.startsWith("b:")) { const int bi = key.mid(2).toInt(); if (bi < 0 || bi >= blockMap.size() || blockMap[bi] < 0) continue; key = QString("b:%1").arg(blockMap[bi]); }
        const QJsonObject o = it.value().toObject();
        PtrSpec sp;
        sp.on = o["on"].toBool();
        sp.width = o["width"].toInt(4) == 2 ? 2 : 4;
        sp.bigEndian = o["bigEndian"].toBool(true);
        sp.base = o["base"].toString().toLongLong();
        sp.where = o["where"].toString().isEmpty() ? -2 : fileIndex(o["where"].toString());
        if (sp.where == -1) sp.where = -2;
        sp.freeSpace = o["free"].toString();
        sp.reuse = o["reuse"].toBool();
        mSpecs[key] = sp;
    }
    mStrings.clear();
    mNextId = 1;
    int changed = 0;
    const QJsonArray a = root["strings"].toArray();
    for (int i = 0; i < a.size(); i++) {
        const QJsonObject o = a[i].toObject();
        Str s;
        s.id = o["id"].toString();
        const int b = o["block"].toInt(-1);
        if (b >= 0) { if (b >= blockMap.size() || blockMap[b] < 0) continue; s.block = blockMap[b]; }
        s.offset = o["offset"].toString().toULongLong(0, 16);
        s.length = (quint32)o["length"].toInt();
        s.termLength = (quint32)o["term"].toInt();
        s.orig = QByteArray::fromHex(o["orig"].toString().toLatin1());
        s.text = o["text"].toString();
        s.translation = o["translation"].toString();
        s.enabled = o["enabled"].toBool(true);
        const QJsonArray p = o["ptrs"].toArray();
        for (int k = 0; k < p.size(); k++) s.ptrs << p[k].toString().toULongLong(0, 16);
        s.ptrsInBlock = o["ptrsInBlock"].toBool();
        // compatibility: the disc (or the block) must still hold the original text
        if (mImgOk) {
            QByteArray cur2;
            if (s.block >= 0) cur2 = mBlocks[s.block].data.mid((int)s.offset, s.orig.size());
            else {
                std::vector<uint8_t> v;
                std::string e2;
                if (EditsPatcher::readUserData(mImg, 0, s.offset, (size_t)s.orig.size(), v, e2)) cur2 = QByteArray((const char *)v.data(), (int)v.size());
            }
            if (cur2 != s.orig) changed++;
        }
        const int n = s.id.mid(1).toInt();
        if (n >= mNextId) mNextId = n + 1;
        mStrings.push_back(s);
    }
    mCurrent = -1;
    fillFiles();
    fillTableWidget();
    invalidatePlan();
    fillStrings();
    updateEditor();
    updateChips();
    fillPtrPanel();
    if (changed) notes << tr_("%1 string(s) are not on this disc image (other image or other version): they will not be written.").arg(changed);
    if (errOut) *errOut = notes.join("\n");
    return true;
}

bool UIDebugTextEditor::exportCsv(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QString out = QString::fromUtf8("\xEF\xBB\xBF") + "id;file;offset;room;original;translation\n";
    for (int i = 0; i < mStrings.size(); i++) {
        const Str &s = mStrings[i];
        out += csvField(s.id) + ";" + csvField(s.block >= 0 ? blockName(s.block) : fileName(s.offset)) + ";" + hexOff(s.offset) + ";" +
               QString::number(s.length) + ";" + csvField(s.text) + ";" + csvField(s.translation) + "\n";
    }
    const QByteArray b = out.toUtf8();
    return f.write(b) == b.size();
}

int UIDebugTextEditor::importCsv(const QString &path, QString *errOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (errOut) *errOut = tr_("%1 cannot be read.").arg(path); return -1; }
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    const QList<QStringList> rows = parseCsv(text);
    if (rows.isEmpty()) { if (errOut) *errOut = tr_("Empty file."); return -1; }
    const QStringList head = rows[0];
    const int cId = head.indexOf("id"), cTr = head.indexOf("translation");
    if (cId < 0 || cTr < 0) { if (errOut) *errOut = tr_("The first line must name the columns \"id\" and \"translation\"."); return -1; }
    QMap<QString, int> byId;
    for (int i = 0; i < mStrings.size(); i++) byId[mStrings[i].id] = i;
    int n = 0, unknown = 0;
    for (int r = 1; r < rows.size(); r++) {
        const QStringList &row = rows[r];
        if (row.size() <= qMax(cId, cTr)) continue;
        if (!byId.contains(row[cId])) { if (!row[cId].isEmpty()) unknown++; continue; }
        const int i = byId[row[cId]];
        if (mStrings[i].translation != row[cTr]) { mStrings[i].translation = row[cTr]; n++; }
    }
    invalidatePlan();
    fillStrings();
    updateEditor();
    updateChips();
    if (errOut) *errOut = unknown ? tr_("%1 line(s) with an unknown ID.").arg(unknown) : QString();
    return n;
}

void UIDebugTextEditor::onSaveProject()
{
    const QString f = CommonDialogs::getSaveFileName(QString(), tr_("Save the translation project"), "Kronos text project (*.ktext)");
    if (f.isEmpty()) return;
    if (!saveProject(f)) QMessageBox::warning(this, windowTitle(), tr_("An error occured while writing file."));
    else lStatus->setText(tr_("Project saved: %1").arg(QFileInfo(f).fileName()));
}

void UIDebugTextEditor::onLoadProject()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Open a translation project"), QString(), "Kronos text project (*.ktext)");
    if (f.isEmpty()) return;
    QString msg;
    if (!loadProject(f, &msg)) { QMessageBox::warning(this, windowTitle(), msg); return; }
    if (!msg.isEmpty()) QMessageBox::warning(this, windowTitle(), msg);
    lStatus->setText(tr_("Project opened: %1 string(s)").arg(mStrings.size()));
}

void UIDebugTextEditor::onExportCsv()
{
    const QString f = CommonDialogs::getSaveFileName(QString(), tr_("Export the strings"), "CSV (*.csv)");
    if (f.isEmpty()) return;
    if (!exportCsv(f)) QMessageBox::warning(this, windowTitle(), tr_("An error occured while writing file."));
    else lStatus->setText(tr_("%1 string(s) exported.").arg(mStrings.size()));
}

void UIDebugTextEditor::onImportCsv()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Import the translations"), QString(), "CSV (*.csv)");
    if (f.isEmpty()) return;
    QString msg;
    const int n = importCsv(f, &msg);
    if (n < 0) { QMessageBox::warning(this, windowTitle(), msg); return; }
    lStatus->setText(tr_("%1 translation(s) read.").arg(n) + (msg.isEmpty() ? QString() : " " + msg));
}

// ===========================================================================
// Disc output: texts in place, moved texts and their pointers, unpacked
// blocks packed again (they must not grow).
// ===========================================================================

bool UIDebugTextEditor::buildDisc(EditsPatcher::Build &build, int *refused, QString *msg)
{
    if (refused) *refused = 0;
    if (!mImgOk) { if (msg) *msg = tr_("Open a disc image first."); return false; }
    computePlan();
    std::vector<EditsPatcher::DiskEdit> edits;
    QStringList problems;
    QMap<int, QByteArray> blocks;                 // modified copies of the unpacked blocks
    QMap<int, QStringList> blockStrings;          // ids written in each block
    std::string err;
    auto discEdit = [&](quint64 off, const std::vector<uint8_t> &bytes, const QString &label) -> bool {
        EditsPatcher::DiskEdit e;
        e.track = 0;
        e.offset = off;
        if (!EditsPatcher::readUserData(mImg, 0, off, bytes.size(), e.orig, err)) { problems << label + ": " + fromStd(err); return false; }
        e.repl = bytes;
        e.label = toStd(label);
        if (e.orig != e.repl) edits.push_back(e);
        return true;
    };
    auto blockCopy = [&](int b) -> QByteArray & {
        if (!blocks.contains(b)) blocks[b] = mBlocks[b].data;
        return blocks[b];
    };
    int written = 0;
    for (int i = 0; i < mStrings.size(); i++) {
        const Str &s = mStrings[i];
        const PlanItem &p = mPlan[(size_t)i];
        if (p.place == PlaceNone) continue;
        if (p.place == PlaceRefused) { if (refused) (*refused)++; problems << QString("%1: %2").arg(s.id).arg(p.why); continue; }
        if (p.place == PlaceInPlace) {
            if (s.block < 0) { if (discEdit(s.offset, p.bytes, s.id)) written++; }
            else {
                QByteArray &d = blockCopy(s.block);
                memcpy(d.data() + s.offset, p.bytes.data(), p.bytes.size());
                blockStrings[s.block] << s.id;
                written++;
            }
            continue;
        }
        // moved: the text in the free space, then its pointers
        const QString key = containerKey(s);
        const PtrSpec sp = mSpecs.value(key);
        if (s.block >= 0) {
            QByteArray &d = blockCopy(s.block);
            if (p.newOffset + p.bytes.size() > (quint64)d.size()) { problems << s.id + ": " + tr_("free space past the end of the block"); continue; }
            memcpy(d.data() + p.newOffset, p.bytes.data(), p.bytes.size());
            blockStrings[s.block] << s.id;
        } else {
            quint64 discStart = 0;
            QByteArray dummy;
            containerData(key, dummy, &discStart, 0);
            if (!discEdit(discStart + p.newOffset, p.bytes, s.id)) continue;
        }
        TextTools::PtrFormat f;
        f.width = sp.width;
        f.bigEndian = sp.bigEndian;
        f.base = sp.base;
        const uint32_t oldValue = (uint32_t)(sp.base + (qint64)containerOffset(s)), newValue = (uint32_t)(sp.base + (qint64)p.newOffset);
        for (int k = 0; k < s.ptrs.size(); k++) {
            std::vector<uint8_t> w((size_t)sp.width);
            if (s.ptrsInBlock) {
                QByteArray &d = blockCopy(s.block);
                if (s.ptrs[k] + sp.width > (quint64)d.size()) continue;
                const uint32_t cur = TextTools::readPtr((const uint8_t *)d.constData() + s.ptrs[k], f);
                if (!TextTools::ptrMatches(cur, f, oldValue)) {
                    problems << s.id + ": " + tr_("pointer at %1 changed").arg(hexOff(s.ptrs[k]));
                    continue;
                }
                TextTools::writePtr((uint8_t *)d.data() + s.ptrs[k], f, TextTools::ptrRetarget(cur, f, newValue));
            } else {
                std::vector<uint8_t> cur;
                if (!EditsPatcher::readUserData(mImg, 0, s.ptrs[k], (size_t)sp.width, cur, err) ||
                    !TextTools::ptrMatches(TextTools::readPtr(cur.data(), f), f, oldValue)) {
                    problems << s.id + ": " + tr_("pointer at %1 is not on this image").arg(hexOff(s.ptrs[k]));
                    continue;
                }
                TextTools::writePtr(w.data(), f, TextTools::ptrRetarget(TextTools::readPtr(cur.data(), f), f, newValue));
                discEdit(s.ptrs[k], w, s.id + " ptr");
            }
        }
        written++;
    }
    // unpacked blocks: packed again, must fit in the room of the old packed data
    for (QMap<int, QByteArray>::const_iterator it = blocks.begin(); it != blocks.end(); ++it) {
        const Block &bl = mBlocks[it.key()];
        if (it.value() == bl.data) continue;
        const std::vector<uint8_t> data(it.value().constData(), it.value().constData() + it.value().size());
        std::vector<uint8_t> packed = TextTools::compress(bl.codec, data);
        const quint32 room = qMax(bl.room, bl.packSize);
        if (packed.size() > room) {
            problems << tr_("%1: packed again it takes %2 bytes, %3 more than the room it has (%4); its %5 text(s) are not written.")
                            .arg(blockName(it.key())).arg(packed.size()).arg(packed.size() - room).arg(room)
                            .arg(blockStrings[it.key()].size());
            written -= blockStrings[it.key()].size();
            if (refused) *refused += blockStrings[it.key()].size();
            continue;
        }
        // check that it unpacks to the same data
        std::vector<uint8_t> back;
        std::string e2;
        TextTools::decompress(bl.codec, packed.data(), packed.size(), back, 0, (size_t)data.size(), e2);
        back.resize(qMin(back.size(), data.size()));
        if (back != data) { problems << blockName(it.key()) + ": " + tr_("packing check failed, not written"); continue; }
        if (packed.size() < bl.packSize) packed.resize(bl.packSize, 0);
        discEdit(bl.packOffset, packed, blockName(it.key()));
    }
    if (edits.empty()) {
        if (msg) *msg = tr_("No translation ready to write.") + (problems.isEmpty() ? QString() : "\n\n" + problems.mid(0, 30).join("\n"));
        return false;
    }
    if (!EditsPatcher::buildDiskPatches(mImg, edits, build, err)) { if (msg) *msg = fromStd(err); return false; }
    QStringList log;
    for (size_t i = 0; i < build.status.size(); i++)
        if (build.status[i].matches == 0 && !build.status[i].note.empty() && build.status[i].note != "unchanged")
            log << fromStd(edits[i].label) + ": " + fromStd(build.status[i].note);
    if (msg) {
        *msg = tr_("%1 translation(s) written (%2 change(s) on the disc).").arg(qMax(0, written)).arg(build.applied);
        if (!problems.isEmpty()) *msg += "\n" + tr_("Not written (to fix):") + "\n" + problems.mid(0, 20).join("\n") + (problems.size() > 20 ? "\n..." : QString());
        if (!log.isEmpty()) *msg += "\n" + log.mid(0, 20).join("\n");
    }
    return build.applied > 0;
}

bool UIDebugTextEditor::writePatchedImage(const QString &dir, QString *msg)
{
    EditsPatcher::Build build;
    int refused = 0;
    if (!buildDisc(build, &refused, msg)) return false;
    std::string err;
    if (!EditsPatcher::writePatchedImage(mImg, build, dir.toLocal8Bit().constData(), err,
                                         [this](const char *, double f) { return progress(f); })) {
        if (msg) *msg = fromStd(err);
        return false;
    }
    return true;
}

void UIDebugTextEditor::onWriteImage()
{
    const QString dir = CommonDialogs::getExistingDirectory(QFileInfo(leImage->text()).absolutePath(),
                                                            tr_("Select an empty output folder (not the folder of the original image)"));
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    startProgress(tr_("Copying the image..."));
    QString msg;
    const bool ok = writePatchedImage(dir, &msg);
    endProgress();
    if (!ok) { QMessageBox::warning(this, windowTitle(), msg); return; }
    QMessageBox::information(this, windowTitle(), tr_("Patched image written to:\n%1").arg(dir) + "\n\n" + msg);
}

bool UIDebugTextEditor::writePatches(int kind)
{
    EditsPatcher::Build build;
    int refused = 0;
    QString msg;
    if (!buildDisc(build, &refused, &msg)) { QMessageBox::warning(this, windowTitle(), msg); return false; }
    const QFileInfo im(leImage->text());
    const QString ext = kind == 0 ? "bps" : "ips";
    const QString base = CommonDialogs::getSaveFileName(im.absolutePath() + "/" + im.completeBaseName() + "_text." + ext,
                                                        kind == 0 ? tr_("Save BPS patch") : tr_("Save IPS patch"),
                                                        kind == 0 ? "BPS patch (*.bps)" : "IPS patch (*.ips)");
    if (base.isEmpty()) return false;
    const bool multi = build.changes.size() > 1;
    QStringList written;
    startProgress(tr_("Computing checksums..."));
    for (std::map<std::string, std::vector<EditsPatcher::Change> >::const_iterator it = build.changes.begin(); it != build.changes.end(); ++it) {
        QString out = base;
        if (multi) {
            const QFileInfo bi(base), fi(QString::fromLocal8Bit(it->first.c_str()));
            out = bi.absolutePath() + "/" + bi.completeBaseName() + "_" + fi.completeBaseName() + "." + ext;
        }
        std::string err;
        const bool ok = kind == 0 ? EditsPatcher::writeBps(it->first, it->second, out.toLocal8Bit().constData(), err,
                                                           [this](const char *, double f) { return progress(f); })
                                  : EditsPatcher::writeIps(it->second, out.toLocal8Bit().constData(), err);
        if (!ok) { endProgress(); QMessageBox::warning(this, windowTitle(), fromStd(err)); return false; }
        written << out;
    }
    endProgress();
    QMessageBox::information(this, windowTitle(), tr_("Patch written:\n%1").arg(written.join("\n")) + "\n\n" + msg);
    return true;
}

void UIDebugTextEditor::onWriteBps() { writePatches(0); }
void UIDebugTextEditor::onWriteIps() { writePatches(1); }

// Words drawn with graphics (VDP2 cells, VDP1 sprites) are not text bytes: the
// graphics editor (screen pick, then its Text tool) handles them. It is opened
// through the main window action, so there is still a single instance.
void UIDebugTextEditor::onGfxText()
{
    QWidget *main = parentWidget();
    QAction *act = main ? main->findChild<QAction *>(QStringLiteral("aViewDebugGfxEditor")) : 0;
    if (!act) {
        lStatus->setText(tr_("Open the graphics editor from the menu: Debug > Graphics Editor (VDP1/VDP2)."));
        return;
    }
    act->trigger();
    UIDebugGfxEditor *ed = main->findChild<UIDebugGfxEditor *>();
    if (ed) ed->startScreenPick();
    lStatus->setText(tr_("Graphics editor: click the word in the game, then use \"Text...\"."));
}
