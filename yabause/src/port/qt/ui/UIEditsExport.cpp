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

/*  UIEditsExport -- see UIEditsExport.h

    Reads the edit list of the core (vdpedit.c): VDP1 sprites, VDP2
    characters / bitmap rows and color RAM, whatever dialog made them. */

#include "UIEditsExport.h"
#include "UIDebugGfxEditor.h"
#include "../QtYabause.h"
#include "../Settings.h"
#include "../VolatileSettings.h"
#include "CommonDialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QStyle>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <map>
#include <set>

extern "C" {
#include "vdp1.h"
#include "vdp2.h"
#include "vdpedit.h"
#include "cs2.h"
}

namespace {

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QString hexStr(uint32_t v, int width = 0)
{
    return QString("0x%1").arg((qulonglong)v, width, 16, QLatin1Char('0')).toUpper().replace("0X", "0x");
}

QColor dimColor(const QWidget *w)
{
    const QColor a = w->palette().color(QPalette::WindowText), b = w->palette().color(QPalette::Window);
    return QColor((a.red() * 6 + b.red() * 4) / 10, (a.green() * 6 + b.green() * 4) / 10, (a.blue() * 6 + b.blue() * 4) / 10);
}

QString trimmed(const char *s, int n)
{
    return QString::fromLatin1(s, (int)strnlen(s, (size_t)n)).trimmed();
}

QString spaceName(int space)
{
    switch (space) {
    case VDPEDIT_VDP1: return QString("VDP1");
    case VDPEDIT_VDP2: return QString("VDP2");
    default:           return QString("CRAM");
    }
}

// What an edit is, in words, from its descriptor.
QString kindText(const VdpEditInfo &e)
{
    const QString m = e.meta ? QString::fromLatin1(e.meta) : QString();
    if (m.startsWith("spr")) {
        VdpEditSprite s;
        int w = 0, h = 0;
        if (VdpEditSpriteFromMeta(e.meta, &s) == 0) VdpEditSpriteGeometry(&s, &w, &h, NULL, NULL, NULL);
        return tr_("Sprite %1 x %2").arg(w).arg(h);
    }
    if (m.startsWith("lut")) return tr_("Sprite lookup table");
    if (m.startsWith("chr") || m.startsWith("bmp")) {
        static const char *names[VDPEDIT_LAYERS] = { "NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG0 (B)", "RBG1" };
        VdpEditBg b;
        const int k = VdpEditBgFromMeta(e.meta, &b, NULL, NULL);
        const QString layer = (b.layer >= 0 && b.layer < VDPEDIT_LAYERS) ? QString::fromLatin1(names[b.layer]) : QString("VDP2");
        return k == 1 ? tr_("%1 character").arg(layer) : tr_("%1 bitmap row").arg(layer);
    }
    if (e.space == VDPEDIT_CRAM) {
        const int n = (int)e.len / VdpEditCramBytes();
        return n == 1 ? tr_("Color %1").arg(hexStr(e.addr / (u32)VdpEditCramBytes(), 3))
                      : tr_("%1 colors from %2").arg(n).arg(hexStr(e.addr / (u32)VdpEditCramBytes(), 3));
    }
    return tr_("%1 data").arg(spaceName(e.space));
}

QIcon thumbIcon(const VdpEditInfo &e)
{
    const int H = 40, W = 96;
    QPixmap pm(W, H);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const QImage before = UIDebugGfxEditor::editPreview(e.space, e.meta, e.addr, e.orig, e.len);
    const QImage after = UIDebugGfxEditor::editPreview(e.space, e.meta, e.addr, e.repl, e.len);
    auto draw = [&](const QImage &img, int x) {
        QPixmap tile(8, 8);
        tile.fill(QColor(0xCC, 0xCC, 0xCC));
        { QPainter tp(&tile); tp.fillRect(0, 0, 4, 4, QColor(0x99, 0x99, 0x99)); tp.fillRect(4, 4, 4, 4, QColor(0x99, 0x99, 0x99)); }
        const QRect box(x, 0, 40, 40);
        p.fillRect(box, QBrush(tile));
        if (img.isNull()) { p.drawText(box, Qt::AlignCenter, "?"); return; }
        const QImage s = img.scaled(40, 40, Qt::KeepAspectRatio,
                                    (img.width() < 40 && img.height() < 40) ? Qt::FastTransformation : Qt::SmoothTransformation);
        p.drawImage(x + (40 - s.width()) / 2, (40 - s.height()) / 2, s);
    };
    draw(before, 0);
    draw(after, 56);
    p.setPen(QColor(128, 128, 128));
    p.drawText(QRect(40, 0, 16, 40), Qt::AlignCenter, QString::fromUtf8("\xE2\x86\x92"));
    p.end();
    return QIcon(pm);
}

EditsPatcher::Record makeRecord(const EditRecord &r, const QString &label)
{
    EditsPatcher::Record rec;
    rec.type = r.type;
    rec.addr = r.addr;
    rec.orig.assign(r.orig, r.orig + r.len);
    rec.repl.assign(r.repl, r.repl + r.len);
    rec.label = label.toStdString();
    rec.enabled = r.enabled != 0;
    rec.name = r.name ? std::string(r.name) : std::string();
    rec.meta = r.meta ? std::string(r.meta) : std::string();
    return rec;
}

// Names of the packs of the edits currently loaded ("" = this session).
std::set<QString> loadedPacks()
{
    std::set<QString> out;
    const int n = VdpEditCount(-1);
    for (int i = 0; i < n; i++) {
        VdpEditInfo e;
        if (VdpEditGetInfo(i, &e) == 0) out.insert(e.pack ? QString::fromUtf8(e.pack) : QString());
    }
    return out;
}

enum { ColName = 0, ColKind, ColAddr, ColPack, ColState };

} // namespace

// ===========================================================================
// Game identity, mods folder, automatic loading
// ===========================================================================

QString UIEditsExport::currentProduct() { return cdip ? trimmed(cdip->itemnum, 10) : QString(); }
QString UIEditsExport::currentVersion() { return cdip ? trimmed(cdip->version, 6) : QString(); }
QString UIEditsExport::currentTitle()   { return cdip ? trimmed(cdip->gamename, 112) : QString(); }

QString UIEditsExport::modsDir()
{
    Settings *s = QtYabause::settings();
    const QString d = s ? s->value("GfxEditor/ModsDir", getDataDirPath() + "/mods").toString()
                        : getDataDirPath() + "/mods";
    return d;
}

bool UIEditsExport::autoLoadEnabled()
{
    Settings *s = QtYabause::settings();
    return s ? s->value("GfxEditor/AutoLoadPacks", true).toBool() : true;
}

int UIEditsExport::loadPackFile(const QString &path, bool ask, QWidget *parent, QString *msg)
{
    std::vector<EditsPatcher::Record> recs;
    std::string meta, err;
    const QString packName = QFileInfo(path).fileName();
    if (!EditsPatcher::loadPack(path.toLocal8Bit().constData(), recs, meta, err)) {
        if (msg) *msg = QString::fromLocal8Bit(err.c_str());
        return -1;
    }
    EditsPatcher::PackInfo info = EditsPatcher::packInfoFromString(meta);
    const QString product = QString::fromStdString(info["product"]);
    const QString cur = currentProduct();
    if (!product.isEmpty() && !cur.isEmpty() && product != cur) {
        const QString text = tr_("%1 was made for %2 (%3), the disc in the drive is %4 (%5).")
            .arg(packName).arg(QString::fromStdString(info["title"])).arg(product).arg(currentTitle()).arg(cur);
        if (!ask) { if (msg) *msg = text; return -1; }
        if (QMessageBox::warning(parent, tr_("Load edit pack"), text + "\n\n" + tr_("Load it anyway?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            if (msg) *msg = tr_("%1: not loaded (other disc).").arg(packName);
            return -1;
        }
    }
    if (loadedPacks().count(packName)) {
        if (!ask) { if (msg) *msg = tr_("%1: already loaded.").arg(packName); return 0; }
        if (QMessageBox::question(parent, tr_("Load edit pack"), tr_("%1 is already loaded. Load it again (its edits are replaced)?").arg(packName),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return 0;
        VdpEditRemovePack(packName.toUtf8().constData());
    }
    const QByteArray packUtf8 = packName.toUtf8();
    int ok = 0, bad = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (recs[i].orig.empty() || recs[i].orig.size() != recs[i].repl.size()) { ++bad; continue; }
        EditRecord r;
        memset(&r, 0, sizeof(r));
        r.type = recs[i].type;
        r.addr = recs[i].addr;
        r.len = (u32)recs[i].orig.size();
        r.orig = &recs[i].orig[0];
        r.repl = &recs[i].repl[0];
        r.enabled = recs[i].enabled ? 1 : 0;
        r.name = recs[i].name.empty() ? NULL : const_cast<char *>(recs[i].name.c_str());
        r.meta = recs[i].meta.empty() ? NULL : const_cast<char *>(recs[i].meta.c_str());
        r.pack = const_cast<char *>(packUtf8.constData());
        // >= 0: written now (1), waiting for the game to load the original
        // data (0), or already identical (2)
        if (VdpEditRestore(&r) >= 0) ++ok; else ++bad;
    }
    if (msg) {
        *msg = tr_("%1: %2 edit(s) loaded").arg(packName).arg(ok);
        if (bad) *msg += ", " + tr_("%1 refused").arg(bad);
        if (product.isEmpty()) *msg += " " + tr_("(old pack without the disc number: not checked)");
        const int c = VdpEditConflicts(NULL, 0);
        if (c) *msg += " - " + tr_("%1 conflict(s) with other edits").arg(c);
    }
    return ok;
}

int UIEditsExport::autoLoadPacks(QStringList *log)
{
    if (!autoLoadEnabled() || !Vdp1Ram || !Vdp2Ram) return 0;
    const QString cur = currentProduct();
    if (cur.isEmpty()) return 0;
    QDir dir(modsDir());
    if (!dir.exists()) return 0;
    const std::set<QString> loaded = loadedPacks();
    int n = 0;
    const QStringList files = dir.entryList(QStringList() << "*.kmod", QDir::Files, QDir::Name);
    for (int i = 0; i < files.size(); i++) {
        if (loaded.count(files[i])) continue;
        // read the header only through loadPack: packs are small
        std::vector<EditsPatcher::Record> recs;
        std::string meta, err;
        if (!EditsPatcher::loadPack(dir.filePath(files[i]).toLocal8Bit().constData(), recs, meta, err)) continue;
        EditsPatcher::PackInfo info = EditsPatcher::packInfoFromString(meta);
        if (QString::fromStdString(info["product"]) != cur) continue;
        QString msg;
        if (loadPackFile(dir.filePath(files[i]), false, NULL, &msg) >= 0) {
            n++;
            if (log) *log << msg;
        }
    }
    return n;
}

// ===========================================================================
// Dialog
// ===========================================================================

UIEditsExport::UIEditsExport(QWidget *parent)
    : QDialog(parent), mTimer(NULL), mLastGen(0xFFFFFFFF), mFilling(false)
{
    setWindowTitle(tr_("Edits"));
    resize(1180, 720);
    setSizeGripEnabled(true);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(12);

    QHBoxLayout *hdr = new QHBoxLayout();
    lTitle = new QLabel(tr_("Edits"), this);
    QFont f = lTitle->font(); f.setPointSize(f.pointSize() + 6); f.setBold(true); lTitle->setFont(f);
    hdr->addWidget(lTitle);
    hdr->addStretch(1);
    lChipGame = new QLabel(this);      lChipGame->setObjectName("chip");
    lChipEdits = new QLabel(this);     lChipEdits->setObjectName("chip");
    lChipConflicts = new QLabel(this); lChipConflicts->setObjectName("chip");
    hdr->addWidget(lChipGame);
    hdr->addWidget(lChipEdits);
    hdr->addWidget(lChipConflicts);
    root->addLayout(hdr);

    QSplitter *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(12);
    root->addWidget(split, 1);

    // ---- left: the edits ----
    QFrame *left = new QFrame(this);
    left->setObjectName("panel");
    QVBoxLayout *vl = new QVBoxLayout(left);
    vl->setContentsMargins(14, 12, 14, 12);
    vl->setSpacing(8);
    QLabel *le = new QLabel(tr_("Edits"), left); le->setProperty("section", true);
    vl->addWidget(le);
    twEdits = new QTreeWidget(left);
    twEdits->setColumnCount(5);
    twEdits->setHeaderLabels(QStringList() << tr_("Name") << tr_("What") << tr_("Address")
                                           << tr_("Pack") << tr_("State"));
    twEdits->setRootIsDecorated(false);
    twEdits->setAlternatingRowColors(true);
    twEdits->setIconSize(QSize(96, 40));
    twEdits->setSelectionMode(QAbstractItemView::ExtendedSelection);
    twEdits->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    twEdits->setFrameShape(QFrame::NoFrame);
    twEdits->header()->setStretchLastSection(false);
    twEdits->header()->setMinimumSectionSize(60);
    for (int c = 0; c < 5; c++)
        twEdits->header()->setSectionResizeMode(c, c == ColName ? QHeaderView::Interactive
                                                   : c == 1 ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    twEdits->header()->resizeSection(ColName, 210);
    twEdits->setToolTip(tr_("Tick to switch an edit on or off, double-click the name to rename it. Left: original, right: edit (colors of the current palette)."));
    vl->addWidget(twEdits, 1);
    QHBoxLayout *eb = new QHBoxLayout();
    pbUndo = new QPushButton(tr_("Undo"), left);
    pbRedo = new QPushButton(tr_("Redo"), left);
    pbToggle = new QPushButton(tr_("On / off"), left);
    pbDelete = new QPushButton(tr_("Delete"), left);
    pbUndo->setToolTip(tr_("Previous version of the selected edit (down to the original)"));
    pbRedo->setToolTip(tr_("Next version of the selected edit"));
    pbDelete->setToolTip(tr_("Puts the original back and removes the selected edits"));
    for (QPushButton *b : { pbUndo, pbRedo, pbToggle, pbDelete }) { b->setProperty("flat2", true); eb->addWidget(b); }
    eb->addStretch(1);
    vl->addLayout(eb);
    split->addWidget(left);

    // ---- right: packs, conflicts, disc ----
    QFrame *right = new QFrame(this);
    right->setObjectName("panel");
    right->setMinimumWidth(360);
    QVBoxLayout *vr = new QVBoxLayout(right);
    vr->setContentsMargins(14, 12, 14, 12);
    vr->setSpacing(8);
    QTabWidget *tabs = new QTabWidget(right);
    tabs->setDocumentMode(true);
    vr->addWidget(tabs, 1);

    QWidget *tPacks = new QWidget(tabs);
    QVBoxLayout *vp = new QVBoxLayout(tPacks);
    vp->setContentsMargins(0, 8, 0, 0);
    lwPacks = new QListWidget(tPacks);
    lwPacks->setToolTip(tr_("Loaded packs. \"This session\" = edits made since the game started, not saved yet."));
    vp->addWidget(lwPacks, 1);
    QGridLayout *pg = new QGridLayout();
    pbLoadPack = new QPushButton(tr_("Load pack..."), tPacks);
    pbUnloadPack = new QPushButton(tr_("Unload"), tPacks);
    pbSavePack = new QPushButton(tr_("Save as pack..."), tPacks);
    pbSaveSelected = new QPushButton(tr_("Save selected..."), tPacks);
    pbSavePack->setToolTip(tr_("Saves every edit that is on (all packs) into one .kmod, with the disc number of this game"));
    pbSaveSelected->setToolTip(tr_("Saves only the edits selected in the list"));
    pbLoadPack->setToolTip(tr_("Adds a pack to the edits already loaded. A pack made for another disc is flagged."));
    pg->addWidget(pbLoadPack, 0, 0);
    pg->addWidget(pbUnloadPack, 0, 1);
    pg->addWidget(pbSavePack, 1, 0);
    pg->addWidget(pbSaveSelected, 1, 1);
    for (QPushButton *b : { pbLoadPack, pbUnloadPack, pbSavePack, pbSaveSelected }) b->setProperty("flat2", true);
    vp->addLayout(pg);
    cbAutoLoad = new QCheckBox(tr_("Load the packs of this game automatically"), tPacks);
    cbAutoLoad->setChecked(autoLoadEnabled());
    cbAutoLoad->setToolTip(tr_("At each reset, every .kmod of the mods folder made for the disc in the drive "
                               "(same product number) is loaded."));
    vp->addWidget(cbAutoLoad);
    lModsDir = new QLabel(tPacks);
    lModsDir->setWordWrap(true);
    lModsDir->setProperty("dim", true);
    vp->addWidget(lModsDir);
    pbOpenMods = new QPushButton(tr_("Mods folder..."), tPacks);
    pbOpenMods->setProperty("flat2", true);
    pbOpenMods->setToolTip(tr_("Choose the folder the packs are loaded from (the pack files are saved there by default)"));
    vp->addWidget(pbOpenMods);
    QLabel *lc = new QLabel(tr_("Conflicts"), tPacks); lc->setProperty("section", true);
    vp->addWidget(lc);
    mConflicts = new QPlainTextEdit(tPacks);
    mConflicts->setReadOnly(true);
    mConflicts->setMaximumHeight(120);
    vp->addWidget(mConflicts);
    tabs->addTab(tPacks, tr_("Packs"));

    QWidget *tDisc = new QWidget(tabs);
    QVBoxLayout *vd = new QVBoxLayout(tDisc);
    vd->setContentsMargins(0, 8, 0, 0);
    QLabel *dl = new QLabel(tr_("Write the edits into the disc image (only if the data is stored uncompressed). "
                                "The pack always works, even when the game data is compressed."), tDisc);
    dl->setWordWrap(true);
    dl->setProperty("dim", true);
    vd->addWidget(dl);
    QHBoxLayout *imgRow = new QHBoxLayout();
    mImage = new QLineEdit(tDisc);
    mImage->setText(QtYabause::volatileSettings()->value("General/CdRomISO").toString());
    mImage->setToolTip(tr_("Image to patch (.cue, .iso, .bin, .img)"));
    imgRow->addWidget(mImage, 1);
    QPushButton *browse = new QPushButton(tr_("Choose..."), tDisc);
    browse->setProperty("flat2", true);
    imgRow->addWidget(browse);
    vd->addLayout(imgRow);
    QGridLayout *dg = new QGridLayout();
    QPushButton *analyze = new QPushButton(tr_("Analyze"), tDisc);
    QPushButton *image = new QPushButton(tr_("Export patched image..."), tDisc);
    QPushButton *bps = new QPushButton(tr_("Export BPS patch..."), tDisc);
    QPushButton *ips = new QPushButton(tr_("Export IPS patch..."), tDisc);
    analyze->setToolTip(tr_("Looks for each edited block in the image and tells which ones can be patched."));
    image->setToolTip(tr_("Copies the image into another folder and patches the copy (EDC/ECC regenerated)."));
    bps->setToolTip(tr_("Small patch for the original image (any size). Recommended way to share."));
    ips->setToolTip(tr_("IPS cannot address beyond 16 MiB: only works if every edit sits in the first 16 MiB of the image file."));
    dg->addWidget(analyze, 0, 0); dg->addWidget(image, 0, 1);
    dg->addWidget(bps, 1, 0);     dg->addWidget(ips, 1, 1);
    for (QPushButton *b : { analyze, image, bps, ips }) b->setProperty("flat2", true);
    vd->addLayout(dg);
    mLog = new QPlainTextEdit(tDisc);
    mLog->setReadOnly(true);
    vd->addWidget(mLog, 1);
    tabs->addTab(tDisc, tr_("Disc image"));
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setSizes(QList<int>() << 820 << 340);

    QHBoxLayout *ft = new QHBoxLayout();
    QLabel *note = new QLabel(tr_("Only the edits that are on are saved and written to the disc."), this);
    note->setProperty("dim", true);
    ft->addWidget(note);
    ft->addStretch(1);
    QPushButton *closeBtn = new QPushButton(tr_("Close"), this);
    closeBtn->setProperty("flat2", true);
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
        "QTreeWidget, QListWidget, QPlainTextEdit { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar::tab { padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { border-bottom: 2px solid palette(highlight); color: palette(highlight); }"));

    connect(twEdits, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this, SLOT(onItemChanged(QTreeWidgetItem *, int)));
    connect(twEdits, SIGNAL(itemSelectionChanged()), this, SLOT(onSelectionChanged()));
    connect(pbUndo, SIGNAL(clicked()), this, SLOT(onUndo()));
    connect(pbRedo, SIGNAL(clicked()), this, SLOT(onRedo()));
    connect(pbDelete, SIGNAL(clicked()), this, SLOT(onDelete()));
    connect(pbToggle, SIGNAL(clicked()), this, SLOT(onToggleSelected()));
    connect(pbLoadPack, SIGNAL(clicked()), this, SLOT(onLoadPack()));
    connect(pbUnloadPack, SIGNAL(clicked()), this, SLOT(onUnloadPack()));
    connect(pbSavePack, SIGNAL(clicked()), this, SLOT(onSavePack()));
    connect(pbSaveSelected, SIGNAL(clicked()), this, SLOT(onSaveSelected()));
    connect(pbOpenMods, SIGNAL(clicked()), this, SLOT(onOpenModsDir()));
    connect(lwPacks, &QListWidget::itemSelectionChanged, this, [this]() { pbUnloadPack->setEnabled(lwPacks->currentRow() >= 0); });
    connect(cbAutoLoad, &QCheckBox::toggled, this, [](bool on) {
        if (Settings *s = QtYabause::settings()) s->setValue("GfxEditor/AutoLoadPacks", on);
    });
    connect(browse, SIGNAL(clicked()), this, SLOT(onChooseImage()));
    connect(analyze, SIGNAL(clicked()), this, SLOT(onAnalyze()));
    connect(image, SIGNAL(clicked()), this, SLOT(onExportImage()));
    connect(bps, SIGNAL(clicked()), this, SLOT(onExportBps()));
    connect(ips, SIGNAL(clicked()), this, SLOT(onExportIps()));
    connect(closeBtn, SIGNAL(clicked()), this, SLOT(accept()));

    // the list follows the editor (and the game) while this window is open
    mTimer = new QTimer(this);
    connect(mTimer, SIGNAL(timeout()), this, SLOT(onTick()));
    mTimer->start(500);

    refreshList();
    QtYabause::retranslateWidget(this);
}

void UIEditsExport::onTick()
{
    if (mLastGen != VdpEditGeneration()) refreshList();
    else {
        // the "active / waiting" state follows the game: update that column only
        const int n = VdpEditCount(-1);
        for (int i = 0; i < n && i < twEdits->topLevelItemCount(); i++) {
            VdpEditInfo e;
            if (VdpEditGetInfo(i, &e) != 0) continue;
            QTreeWidgetItem *it = twEdits->topLevelItem(i);
            const QString st = !e.enabled ? tr_("off") : e.noop ? tr_("at original") : e.active ? tr_("in VRAM") : tr_("waiting");
            if (it->text(ColState) != st) { QSignalBlocker b(twEdits); it->setText(ColState, st); }
        }
    }
}

void UIEditsExport::updateHeader()
{
    const QString prod = currentProduct();
    lChipGame->setText(prod.isEmpty() ? tr_("No disc") : QString("%1  %2").arg(prod, currentTitle().left(28)));
    const int n = VdpEditCount(-1);
    lChipEdits->setText(n == 1 ? tr_("1 edit") : tr_("%1 edits").arg(n));
    const int c = VdpEditConflicts(NULL, 0);
    lChipConflicts->setText(c ? tr_("%1 conflict(s)").arg(c) : tr_("No conflict"));
    lChipConflicts->setProperty("alert", c > 0);
    lChipConflicts->style()->unpolish(lChipConflicts);
    lChipConflicts->style()->polish(lChipConflicts);
    lModsDir->setText(tr_("Mods folder: %1").arg(QDir::toNativeSeparators(modsDir())));
}

void UIEditsExport::refreshList()
{
    mLastGen = VdpEditGeneration();
    mFilling = true;
    std::set<u32> selected;
    for (u32 id : selectedIds()) selected.insert(id);

    // conflicts: id -> other ids
    std::vector<u32> pairs(512);
    const int nc = VdpEditConflicts(pairs.data(), 256);
    std::map<u32, QStringList> conflictOf;
    QStringList conflictLines;

    {
        QSignalBlocker b(twEdits);
        twEdits->clear();
        const int n = VdpEditCount(-1);
        std::map<u32, QString> label;
        for (int i = 0; i < n; i++) {
            VdpEditInfo e;
            if (VdpEditGetInfo(i, &e) != 0) continue;
            QTreeWidgetItem *it = new QTreeWidgetItem(twEdits);
            const QString name = e.name ? QString::fromUtf8(e.name) : QString();
            it->setText(ColName, name.isEmpty() ? tr_("(unnamed)") : name);
            if (name.isEmpty()) it->setForeground(ColName, dimColor(this));
            it->setText(ColKind, kindText(e));
            it->setText(ColAddr, QString("%1 %2 (%3 B)").arg(spaceName(e.space)).arg(hexStr(e.addr, 5)).arg(e.len));
            it->setText(ColPack, e.pack ? QString::fromUtf8(e.pack) : tr_("this session"));
            it->setText(ColState, !e.enabled ? tr_("off") : e.noop ? tr_("at original") : e.active ? tr_("in VRAM") : tr_("waiting"));
            it->setIcon(ColName, thumbIcon(e));
            it->setData(ColName, Qt::UserRole, e.id);
            it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
            it->setCheckState(ColName, e.enabled ? Qt::Checked : Qt::Unchecked);
            it->setToolTip(ColState, tr_("in VRAM: the game shows the edit now; waiting: the original data is not in memory "
                                         "right now, the edit is written when the game loads it"));
            if (selected.count(e.id)) it->setSelected(true);
            label[e.id] = QString("%1 [%2]").arg(name.isEmpty() ? kindText(e) : name)
                                            .arg(e.pack ? QString::fromUtf8(e.pack) : tr_("this session"));
        }
        for (int k = 0; k < nc && k < 256; k++) {
            const u32 a = pairs[2 * k], b2 = pairs[2 * k + 1];
            conflictLines << QString("%1  <->  %2").arg(label[a]).arg(label[b2]);
            conflictOf[a] << label[b2];
            conflictOf[b2] << label[a];
        }
        for (int i = 0; i < twEdits->topLevelItemCount(); i++) {
            QTreeWidgetItem *it = twEdits->topLevelItem(i);
            const u32 id = it->data(ColName, Qt::UserRole).toUInt();
            if (!conflictOf.count(id)) continue;
            for (int c = 0; c < 5; c++) it->setForeground(c, QColor(0xD0, 0x30, 0x30));
            it->setToolTip(ColName, tr_("Conflicts with: %1").arg(conflictOf[id].join(", ")));
        }
    }
    mConflicts->setPlainText(nc ? tr_("These edits change the same original data in different ways; "
                                      "the one listed first wins. Switch one off.") + "\n" + conflictLines.join("\n")
                                : tr_("No conflict."));

    // packs
    {
        const QString cur = lwPacks->currentItem() ? lwPacks->currentItem()->data(Qt::UserRole).toString() : QString("\x01");
        QSignalBlocker b(lwPacks);
        lwPacks->clear();
        std::map<QString, int> count;
        const int n = VdpEditCount(-1);
        for (int i = 0; i < n; i++) {
            VdpEditInfo e;
            if (VdpEditGetInfo(i, &e) == 0) count[e.pack ? QString::fromUtf8(e.pack) : QString()]++;
        }
        for (std::map<QString, int>::const_iterator it = count.begin(); it != count.end(); ++it) {
            QListWidgetItem *li = new QListWidgetItem(QString("%1  (%2)").arg(it->first.isEmpty() ? tr_("This session") : it->first).arg(it->second));
            li->setData(Qt::UserRole, it->first);
            lwPacks->addItem(li);
            if (it->first == cur) lwPacks->setCurrentItem(li);
        }
        pbUnloadPack->setEnabled(lwPacks->currentRow() >= 0);
    }
    updateHeader();
    onSelectionChanged();
    mFilling = false;
}

std::vector<unsigned int> UIEditsExport::selectedIds() const
{
    std::vector<unsigned int> out;
    const QList<QTreeWidgetItem *> sel = twEdits->selectedItems();
    for (int i = 0; i < sel.size(); i++) out.push_back(sel[i]->data(ColName, Qt::UserRole).toUInt());
    return out;
}

QString UIEditsExport::selectedPack() const
{
    return lwPacks->currentItem() ? lwPacks->currentItem()->data(Qt::UserRole).toString() : QString();
}

void UIEditsExport::onSelectionChanged()
{
    const std::vector<u32> ids = selectedIds();
    bool undo = false, redo = false;
    for (u32 id : ids) {
        VdpEditInfo e;
        if (VdpEditGetInfo(VdpEditIndexOf(id), &e) != 0) continue;
        undo |= e.canUndo != 0;
        redo |= e.canRedo != 0;
    }
    pbUndo->setEnabled(undo);
    pbRedo->setEnabled(redo);
    pbDelete->setEnabled(!ids.empty());
    pbToggle->setEnabled(!ids.empty());
    pbSaveSelected->setEnabled(!ids.empty());
}

void UIEditsExport::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (mFilling || !item || column != ColName) return;
    const u32 id = item->data(ColName, Qt::UserRole).toUInt();
    VdpEditInfo e;
    if (VdpEditGetInfo(VdpEditIndexOf(id), &e) != 0) return;
    const bool on = item->checkState(ColName) == Qt::Checked;
    if (on != (e.enabled != 0)) VdpEditSetEnabled(id, on ? 1 : 0);
    const QString name = item->text(ColName).trimmed();
    const QString old = e.name ? QString::fromUtf8(e.name) : QString();
    if (name != old && name != tr_("(unnamed)")) VdpEditSetName(id, name.toUtf8().constData());
    QTimer::singleShot(0, this, SLOT(refreshList()));
}

void UIEditsExport::onUndo()
{
    for (u32 id : selectedIds()) VdpEditUndo(id);
    refreshList();
}

void UIEditsExport::onRedo()
{
    for (u32 id : selectedIds()) VdpEditRedo(id);
    refreshList();
}

void UIEditsExport::onToggleSelected()
{
    for (u32 id : selectedIds()) {
        VdpEditInfo e;
        if (VdpEditGetInfo(VdpEditIndexOf(id), &e) == 0) VdpEditSetEnabled(id, e.enabled ? 0 : 1);
    }
    refreshList();
}

void UIEditsExport::onDelete()
{
    const std::vector<u32> ids = selectedIds();
    if (ids.empty()) return;
    if (QMessageBox::question(this, windowTitle(), tr_("Put the original back and remove %1 edit(s)?").arg(ids.size()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    for (u32 id : ids) VdpEditRemove(id);
    refreshList();
}

// ===========================================================================
// Packs
// ===========================================================================

std::vector<EditsPatcher::Record> UIEditsExport::collectPack(const std::vector<unsigned int> &onlyIds) const
{
    std::vector<EditsPatcher::Record> out;
    const int total = VdpEditCount(-1);
    if (total <= 0)
        return out;
    std::set<u32> only(onlyIds.begin(), onlyIds.end());
    std::vector<EditRecord> raw((size_t)total);
    const int n = VdpEditCollect(&raw[0], total);
    const int bpc = VdpEditCramBytes();
    for (int i = 0; i < n; ++i) {
        if (!only.empty() && !only.count(raw[i].id)) continue;
        if (only.empty() && !raw[i].enabled) continue;
        if (memcmp(raw[i].orig, raw[i].repl, raw[i].len) == 0) continue;   // undone to the original
        QString label;
        if (raw[i].name) label = QString::fromUtf8(raw[i].name) + " - ";
        if (raw[i].type == VDPEDIT_VDP1)
            label += QString("VDP1 data at VRAM %1 (%2 bytes)").arg(hexStr(raw[i].addr)).arg(raw[i].len);
        else if (raw[i].type == VDPEDIT_VDP2)
            label += QString("VDP2 tile/bitmap data at VRAM %1 (%2 bytes)").arg(hexStr(raw[i].addr)).arg(raw[i].len);
        else
            label += QString("palette color %1").arg(hexStr(raw[i].addr / (u32)bpc));
        out.push_back(makeRecord(raw[i], label));
    }
    EditsFree(&raw[0], n);
    return out;
}

bool UIEditsExport::savePackTo(const QString &file, const std::vector<unsigned int> &ids)
{
    const std::vector<EditsPatcher::Record> recs = collectPack(ids);
    if (recs.empty()) {
        QMessageBox::information(this, windowTitle(), tr_("There is no edit to save."));
        return false;
    }
    EditsPatcher::PackInfo info;
    info["product"] = currentProduct().toStdString();
    info["version"] = currentVersion().toStdString();
    info["title"] = currentTitle().toStdString();
    info["saved"] = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
    info["tool"] = "Kronos graphics editor";
    std::string err;
    if (!EditsPatcher::savePack(file.toLocal8Bit().constData(), recs, EditsPatcher::packInfoToString(info), err)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return false;
    }
    // the edits of this session now belong to that pack
    const QByteArray pack = QFileInfo(file).fileName().toUtf8();
    std::set<u32> only(ids.begin(), ids.end());
    const int n = VdpEditCount(-1);
    std::vector<u32> toTag;
    for (int i = 0; i < n; i++) {
        VdpEditInfo e;
        if (VdpEditGetInfo(i, &e) != 0 || e.pack) continue;
        if (!only.empty() && !only.count(e.id)) continue;
        if (only.empty() && !e.enabled) continue;
        toTag.push_back(e.id);
    }
    for (u32 id : toTag) VdpEditSetPack(id, pack.constData());
    addLog(tr_("Saved %1 edit(s) to %2").arg((int)recs.size()).arg(file));
    refreshList();
    return true;
}

static QString suggestedPackName()
{
    QString base = UIEditsExport::currentProduct();
    const QString title = UIEditsExport::currentTitle();
    if (!title.isEmpty()) base += (base.isEmpty() ? "" : " - ") + title;
    if (base.isEmpty()) base = "edits";
    base.replace(QRegularExpression(QString::fromLatin1("[\\\\/:*?\"<>|]")), "_");
    return base + ".kmod";
}

void UIEditsExport::onSavePack()
{
    QDir().mkpath(modsDir());
    const QString f = CommonDialogs::getSaveFileName(QDir(modsDir()).filePath(suggestedPackName()),
                                                     tr_("Save edit pack"), "Kronos edit pack (*.kmod)");
    if (!f.isEmpty()) savePackTo(f, std::vector<unsigned int>());
}

void UIEditsExport::onSaveSelected()
{
    const std::vector<unsigned int> ids = selectedIds();
    if (ids.empty()) return;
    QDir().mkpath(modsDir());
    const QString f = CommonDialogs::getSaveFileName(QDir(modsDir()).filePath(suggestedPackName()),
                                                     tr_("Save the selected edits"), "Kronos edit pack (*.kmod)");
    if (!f.isEmpty()) savePackTo(f, ids);
}

void UIEditsExport::onLoadPack()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Load edit pack"), modsDir(), "Kronos edit pack (*.kmod)");
    if (f.isEmpty())
        return;
    QString msg;
    const int n = loadPackFile(f, true, this, &msg);
    if (!msg.isEmpty()) addLog(msg);
    if (n < 0 && !msg.isEmpty()) QMessageBox::warning(this, windowTitle(), msg);
    refreshList();
}

void UIEditsExport::onUnloadPack()
{
    if (lwPacks->currentRow() < 0) return;
    const QString pack = selectedPack();
    const QString shown = pack.isEmpty() ? tr_("the edits of this session") : pack;
    if (QMessageBox::question(this, windowTitle(), tr_("Remove %1 (the originals are put back)?").arg(shown),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const int n = VdpEditRemovePack(pack.isEmpty() ? NULL : pack.toUtf8().constData());
    addLog(tr_("%1 edit(s) removed (%2)").arg(n).arg(shown));
    refreshList();
}

void UIEditsExport::onOpenModsDir()
{
    const QString d = CommonDialogs::getExistingDirectory(modsDir(), tr_("Mods folder (packs loaded automatically)"));
    if (d.isEmpty()) return;
    if (Settings *s = QtYabause::settings()) s->setValue("GfxEditor/ModsDir", d);
    updateHeader();
}

// ===========================================================================
// Disc image
// ===========================================================================

void UIEditsExport::addLog(const QString &line)
{
    mLog->appendPlainText(line);
}

QString UIEditsExport::imageBaseName() const
{
    const QFileInfo fi(mImage->text());
    return fi.completeBaseName().isEmpty() ? QString("game") : fi.completeBaseName();
}

std::vector<EditsPatcher::Record> UIEditsExport::collectForDisc() const
{
    std::vector<EditsPatcher::Record> out;
    std::set<uint32_t> blocks16;
    const std::vector<EditsPatcher::Record> pack = collectPack(std::vector<unsigned int>());
    const uint32_t bpc = (uint32_t)VdpEditCramBytes();
    for (size_t i = 0; i < pack.size(); ++i) {
        if (pack[i].type != VDPEDIT_CRAM) {
            out.push_back(pack[i]);
        } else {
            // addr = byte address in color RAM; one edit may cover several colors
            for (uint32_t b = 0; b < (uint32_t)pack[i].orig.size(); b += bpc)
                blocks16.insert(((pack[i].addr + b) / bpc) >> 4);
        }
    }
    // A color alone (2 bytes) cannot be found on the disc: look for its whole
    // palette instead, first the aligned 256 colors, then the aligned 16.
    for (std::set<uint32_t>::const_iterator it = blocks16.begin(); it != blocks16.end(); ++it) {
        const uint32_t b16 = *it << 4, b256 = b16 & ~0xFFu;
        EditsPatcher::Record r;
        r.type = 2;
        r.addr = b16;
        r.label = QString("palette colors %1-%2").arg(hexStr(b16)).arg(hexStr(b16 + 15)).toStdString();
        std::vector<uint8_t> o256(256 * bpc), p256(256 * bpc), o16(16 * bpc), p16(16 * bpc);
        if (VdpEditCramBlock(b256, 256, &o256[0], &p256[0]) != 0 || VdpEditCramBlock(b16, 16, &o16[0], &p16[0]) != 0)
            continue;
        r.orig = o256; r.repl = p256;
        r.origAlt = o16; r.replAlt = p16;
        out.push_back(r);
    }
    return out;
}

bool UIEditsExport::prepare(EditsPatcher::Image &img, EditsPatcher::Build &build)
{
    const std::vector<EditsPatcher::Record> recs = collectForDisc();
    if (recs.empty()) {
        QMessageBox::information(this, windowTitle(), tr_("There is no edit to export yet."));
        return false;
    }
    const QString path = mImage->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr_("Choose the disc image first."));
        return false;
    }
    std::string err;
    if (!EditsPatcher::openImage(path.toLocal8Bit().constData(), img, err)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return false;
    }

    QProgressDialog pd(tr_("Searching the disc image..."), tr_("Cancel"), 0, 100, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(300);
    EditsPatcher::Progress prog = [&pd](const char *what, double f) -> bool {
        pd.setLabelText(QString::fromUtf8(what));
        pd.setValue((int)(f * 100));
        QApplication::processEvents();
        return !pd.wasCanceled();
    };
    if (!EditsPatcher::buildPatches(img, recs, build, err, prog)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return false;
    }
    pd.setValue(100);

    addLog(QString("--- %1 ---").arg(path));
    for (size_t i = 0; i < build.log.size(); ++i)
        addLog(QString::fromStdString(build.log[i]));
    addLog(tr_("%1 of %2 edit(s) found on the disc.").arg(build.applied).arg((int)recs.size()));

    if (build.applied == 0) {
        QMessageBox::warning(this, windowTitle(), tr_(
            "None of the edits was found in this image: the game most likely stores its graphics "
            "compressed, or in a different layout than in VRAM.\n\n"
            "Use the edit pack (.kmod) instead, which does not depend on the disc format."));
        return false;
    }
    return true;
}

void UIEditsExport::onChooseImage()
{
    const QString f = QFileDialog::getOpenFileName(this, tr_("Choose the disc image"),
        QFileInfo(mImage->text()).absolutePath(), "Disc images (*.cue *.iso *.bin *.img);;All files (*)");
    if (!f.isEmpty())
        mImage->setText(f);
}

void UIEditsExport::onAnalyze()
{
    EditsPatcher::Image img;
    EditsPatcher::Build build;
    prepare(img, build);
}

void UIEditsExport::onExportImage()
{
    EditsPatcher::Image img;
    EditsPatcher::Build build;
    if (!prepare(img, build))
        return;
    const QString dir = CommonDialogs::getExistingDirectory(
        QFileInfo(mImage->text()).absolutePath(),
        tr_("Select an empty output folder (not the folder of the original image)"));
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);

    QProgressDialog pd(tr_("Copying the image..."), tr_("Cancel"), 0, 100, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(300);
    EditsPatcher::Progress prog = [&pd](const char *what, double f) -> bool {
        pd.setLabelText(QString::fromUtf8(what));
        pd.setValue((int)(f * 100));
        QApplication::processEvents();
        return !pd.wasCanceled();
    };
    std::string err;
    if (!EditsPatcher::writePatchedImage(img, build, dir.toLocal8Bit().constData(), err, prog)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return;
    }
    pd.setValue(100);
    addLog(tr_("Patched image written to %1").arg(dir));
    QMessageBox::information(this, windowTitle(),
        tr_("Patched image written to:\n%1\n\n%2 edit(s) applied.").arg(dir).arg(build.applied));
}

void UIEditsExport::onExportBps()
{
    EditsPatcher::Image img;
    EditsPatcher::Build build;
    if (!prepare(img, build))
        return;
    const QString base = CommonDialogs::getSaveFileName(
        QFileInfo(mImage->text()).absolutePath() + "/" + imageBaseName() + ".bps",
        tr_("Save BPS patch"), "BPS patch (*.bps)");
    if (base.isEmpty())
        return;

    QProgressDialog pd(tr_("Computing checksums..."), tr_("Cancel"), 0, 100, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(300);
    EditsPatcher::Progress prog = [&pd](const char *what, double f) -> bool {
        pd.setLabelText(QString::fromUtf8(what));
        pd.setValue((int)(f * 100));
        QApplication::processEvents();
        return !pd.wasCanceled();
    };
    const bool multi = build.changes.size() > 1;
    QStringList written;
    for (std::map<std::string, std::vector<EditsPatcher::Change> >::const_iterator it = build.changes.begin();
         it != build.changes.end(); ++it) {
        QString out = base;
        if (multi) {   // several files changed: one patch per file
            const QFileInfo bi(base), fi(QString::fromLocal8Bit(it->first.c_str()));
            out = bi.absolutePath() + "/" + bi.completeBaseName() + "_" + fi.completeBaseName() + ".bps";
        }
        std::string err;
        if (!EditsPatcher::writeBps(it->first, it->second, out.toLocal8Bit().constData(), err, prog)) {
            QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
            return;
        }
        written << out;
    }
    addLog(tr_("BPS patch written: %1").arg(written.join(", ")));
    QMessageBox::information(this, windowTitle(),
        tr_("BPS patch written:\n%1\n\nApply it to the ORIGINAL image with any BPS patcher "
            "(Flips, RomPatcher.js, MultiPatch...).").arg(written.join("\n")));
}

void UIEditsExport::onExportIps()
{
    EditsPatcher::Image img;
    EditsPatcher::Build build;
    if (!prepare(img, build))
        return;
    const QString base = CommonDialogs::getSaveFileName(
        QFileInfo(mImage->text()).absolutePath() + "/" + imageBaseName() + ".ips",
        tr_("Save IPS patch"), "IPS patch (*.ips)");
    if (base.isEmpty())
        return;
    const bool multi = build.changes.size() > 1;
    QStringList written;
    for (std::map<std::string, std::vector<EditsPatcher::Change> >::const_iterator it = build.changes.begin();
         it != build.changes.end(); ++it) {
        QString out = base;
        if (multi) {
            const QFileInfo bi(base), fi(QString::fromLocal8Bit(it->first.c_str()));
            out = bi.absolutePath() + "/" + bi.completeBaseName() + "_" + fi.completeBaseName() + ".ips";
        }
        std::string err;
        if (!EditsPatcher::writeIps(it->second, out.toLocal8Bit().constData(), err)) {
            QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
            return;
        }
        written << out;
    }
    addLog(tr_("IPS patch written: %1").arg(written.join(", ")));
    QMessageBox::information(this, windowTitle(), tr_("IPS patch written:\n%1").arg(written.join("\n")));
}
