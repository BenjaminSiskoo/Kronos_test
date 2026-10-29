/*  UIEditsExport -- voir UIEditsExport.h

    Lit la liste d'edits du coeur (vdpedit.c) : sprites VDP1, tuiles / bitmaps
    VDP2 et couleurs de Color RAM, quel que soit le dialogue qui les a faits
    (editeur graphique, debogueurs VDP1 / VDP2). */

#include "UIEditsExport.h"
#include "../QtYabause.h"
#include "../VolatileSettings.h"
#include "CommonDialogs.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <set>

extern "C" {
#include "vdp1.h"
#include "vdp2.h"
#include "vdpedit.h"
}

namespace {


QString hexStr(uint32_t v, int width = 0)
{
    return QString("0x%1").arg((qulonglong)v, width, 16, QLatin1Char('0')).toUpper().replace("0X", "0x");
}

EditsPatcher::Record makeRecord(const EditRecord &r, const QString &label)
{
    EditsPatcher::Record rec;
    rec.type = r.type;
    rec.addr = r.addr;
    rec.orig.assign(r.orig, r.orig + r.len);
    rec.repl.assign(r.repl, r.repl + r.len);
    rec.label = label.toStdString();
    return rec;
}

} // namespace

UIEditsExport::UIEditsExport(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QtYabause::translate("Save edits"));
    resize(720, 520);

    QVBoxLayout *root = new QVBoxLayout(this);

    mSummary = new QLabel(this);
    root->addWidget(mSummary);

    QHBoxLayout *imgRow = new QHBoxLayout();
    imgRow->addWidget(new QLabel(QtYabause::translate("Disc image:"), this));
    mImage = new QLineEdit(this);
    mImage->setText(QtYabause::volatileSettings()->value("General/CdRomISO").toString());
    mImage->setToolTip(QtYabause::translate("Image to patch (.cue, .iso, .bin, .img). Used by the patched image and the BPS / IPS exports."));
    imgRow->addWidget(mImage, 1);
    QPushButton *browse = new QPushButton(QtYabause::translate("Choose..."), this);
    connect(browse, SIGNAL(clicked()), this, SLOT(onChooseImage()));
    imgRow->addWidget(browse);
    root->addLayout(imgRow);

    QLabel *packTitle = new QLabel(QtYabause::translate("<b>Edit pack</b> (always works, even when the game data is compressed)"), this);
    root->addWidget(packTitle);
    QHBoxLayout *packRow = new QHBoxLayout();
    QPushButton *save = new QPushButton(QtYabause::translate("Save edit pack (.kmod)..."), this);
    save->setToolTip(QtYabause::translate("Saves every edit as (original bytes -> new bytes) pairs. Reload it later, on any dump of the same game."));
    connect(save, SIGNAL(clicked()), this, SLOT(onSavePack()));
    packRow->addWidget(save);
    QPushButton *load = new QPushButton(QtYabause::translate("Load edit pack..."), this);
    load->setToolTip(QtYabause::translate("Re-applies a saved pack to the running game. Edits stay active every frame."));
    connect(load, SIGNAL(clicked()), this, SLOT(onLoadPack()));
    packRow->addWidget(load);
    packRow->addStretch(1);
    root->addLayout(packRow);

    QLabel *discTitle = new QLabel(QtYabause::translate("<b>Write the edits into the disc image</b> (only if the data is stored uncompressed)"), this);
    root->addWidget(discTitle);
    QHBoxLayout *discRow = new QHBoxLayout();
    QPushButton *analyze = new QPushButton(QtYabause::translate("Analyze"), this);
    analyze->setToolTip(QtYabause::translate("Looks for each edited block in the image and tells which ones can be patched."));
    connect(analyze, SIGNAL(clicked()), this, SLOT(onAnalyze()));
    discRow->addWidget(analyze);
    QPushButton *image = new QPushButton(QtYabause::translate("Export patched image..."), this);
    image->setToolTip(QtYabause::translate("Copies the image into another folder and patches the copy (EDC/ECC regenerated)."));
    connect(image, SIGNAL(clicked()), this, SLOT(onExportImage()));
    discRow->addWidget(image);
    QPushButton *bps = new QPushButton(QtYabause::translate("Export BPS patch..."), this);
    bps->setToolTip(QtYabause::translate("Small patch for the original image (any size). Recommended way to share."));
    connect(bps, SIGNAL(clicked()), this, SLOT(onExportBps()));
    discRow->addWidget(bps);
    QPushButton *ips = new QPushButton(QtYabause::translate("Export IPS patch..."), this);
    ips->setToolTip(QtYabause::translate("IPS cannot address beyond 16 MiB: only works if every edit sits in the first 16 MiB of the image file."));
    connect(ips, SIGNAL(clicked()), this, SLOT(onExportIps()));
    discRow->addWidget(ips);
    discRow->addStretch(1);
    root->addLayout(discRow);

    mLog = new QPlainTextEdit(this);
    mLog->setReadOnly(true);
    root->addWidget(mLog, 1);

    QPushButton *closeBtn = new QPushButton(QtYabause::translate("Close"), this);
    connect(closeBtn, SIGNAL(clicked()), this, SLOT(accept()));
    QHBoxLayout *bottom = new QHBoxLayout();
    bottom->addStretch(1);
    bottom->addWidget(closeBtn);
    root->addLayout(bottom);

    refreshSummary();
}

void UIEditsExport::addLog(const QString &line)
{
    mLog->appendPlainText(line);
}

QString UIEditsExport::imageBaseName() const
{
    const QFileInfo fi(mImage->text());
    return fi.completeBaseName().isEmpty() ? QString("game") : fi.completeBaseName();
}

void UIEditsExport::refreshSummary()
{
    int spr = 0, tile = 0, col = 0;
    const std::vector<EditsPatcher::Record> v = collectPack();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].type == 0) ++spr; else if (v[i].type == 1) ++tile; else ++col;
    }
    mSummary->setText(QtYabause::translate("Current edits: %1 sprite block(s), %2 VDP2 tile/bitmap block(s), %3 palette color(s).")
                          .arg(spr).arg(tile).arg(col));
}

std::vector<EditsPatcher::Record> UIEditsExport::collectPack() const
{
    std::vector<EditsPatcher::Record> out;
    const int total = VdpEditCount(-1);
    if (total <= 0)
        return out;
    std::vector<EditRecord> raw((size_t)total);
    const int n = VdpEditCollect(&raw[0], total);
    const int bpc = VdpEditCramBytes();
    for (int i = 0; i < n; ++i) {
        QString label;
        if (raw[i].type == VDPEDIT_VDP1)
            label = QString("VDP1 sprite data at VRAM %1 (%2 bytes)").arg(hexStr(raw[i].addr)).arg(raw[i].len);
        else if (raw[i].type == VDPEDIT_VDP2)
            label = QString("VDP2 tile/bitmap data at VRAM %1 (%2 bytes)").arg(hexStr(raw[i].addr)).arg(raw[i].len);
        else
            label = QString("palette color %1").arg(hexStr(raw[i].addr / (u32)bpc));
        out.push_back(makeRecord(raw[i], label));
    }
    EditsFree(&raw[0], n);
    return out;
}

std::vector<EditsPatcher::Record> UIEditsExport::collectForDisc() const
{
    std::vector<EditsPatcher::Record> out;
    std::set<uint32_t> blocks16;
    const std::vector<EditsPatcher::Record> pack = collectPack();
    const uint32_t bpc = (uint32_t)VdpEditCramBytes();
    for (size_t i = 0; i < pack.size(); ++i) {
        if (pack[i].type != VDPEDIT_CRAM) {
            out.push_back(pack[i]);
        } else {
            // addr = adresse octet en CRAM ; un edit peut couvrir plusieurs couleurs
            for (uint32_t b = 0; b < (uint32_t)pack[i].orig.size(); b += bpc)
                blocks16.insert(((pack[i].addr + b) / bpc) >> 4);
        }
    }
    // Une couleur seule (2 octets) ne se retrouve pas sur le disque : on cherche sa palette
    // entiere. D'abord les 256 entrees alignees, sinon les 16 entrees alignees.
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
        QMessageBox::information(this, windowTitle(), QtYabause::translate("There is no edit to export yet."));
        return false;
    }
    const QString path = mImage->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), QtYabause::translate("Choose the disc image first."));
        return false;
    }
    std::string err;
    if (!EditsPatcher::openImage(path.toLocal8Bit().constData(), img, err)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return false;
    }

    QProgressDialog pd(QtYabause::translate("Searching the disc image..."), QtYabause::translate("Cancel"), 0, 100, this);
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
    addLog(QtYabause::translate("%1 of %2 edit(s) found on the disc.").arg(build.applied).arg((int)recs.size()));

    if (build.applied == 0) {
        QMessageBox::warning(this, windowTitle(), QtYabause::translate(
            "None of the edits was found in this image: the game most likely stores its graphics "
            "compressed, or in a different layout than in VRAM.\n\n"
            "Use the edit pack (.kmod) instead, which does not depend on the disc format."));
        return false;
    }
    return true;
}

void UIEditsExport::onChooseImage()
{
    const QString f = QFileDialog::getOpenFileName(this, QtYabause::translate("Choose the disc image"),
        QFileInfo(mImage->text()).absolutePath(), "Disc images (*.cue *.iso *.bin *.img);;All files (*)");
    if (!f.isEmpty())
        mImage->setText(f);
}

void UIEditsExport::onSavePack()
{
    const std::vector<EditsPatcher::Record> recs = collectPack();
    if (recs.empty()) {
        QMessageBox::information(this, windowTitle(), QtYabause::translate("There is no edit to save yet."));
        return;
    }
    const QString f = CommonDialogs::getSaveFileName(
        QFileInfo(mImage->text()).absolutePath() + "/" + imageBaseName() + ".kmod",
        QtYabause::translate("Save edit pack"), "Kronos edit pack (*.kmod)");
    if (f.isEmpty())
        return;
    std::string err;
    if (!EditsPatcher::savePack(f.toLocal8Bit().constData(), recs, imageBaseName().toStdString(), err)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return;
    }
    addLog(QtYabause::translate("Saved %1 edit(s) to %2").arg((int)recs.size()).arg(f));
}

void UIEditsExport::onLoadPack()
{
    const QString f = QFileDialog::getOpenFileName(this, QtYabause::translate("Load edit pack"),
        QFileInfo(mImage->text()).absolutePath(), "Kronos edit pack (*.kmod)");
    if (f.isEmpty())
        return;
    std::vector<EditsPatcher::Record> recs;
    std::string meta, err;
    if (!EditsPatcher::loadPack(f.toLocal8Bit().constData(), recs, meta, err)) {
        QMessageBox::warning(this, windowTitle(), QString::fromLocal8Bit(err.c_str()));
        return;
    }
    int ok = 0, bad = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (recs[i].orig.empty() || recs[i].orig.size() != recs[i].repl.size()) { ++bad; continue; }
        EditRecord r;
        r.type = recs[i].type;
        r.addr = recs[i].addr;
        r.len = (u32)recs[i].orig.size();
        r.orig = &recs[i].orig[0];
        r.repl = &recs[i].repl[0];
        // >= 0 : ecrit tout de suite (1), ou en attente que le jeu charge
        // les donnees d'origine (0), ou deja identique (2)
        const int rc = VdpEditRestore(&r);
        if (rc >= 0) ++ok; else ++bad;
    }
    addLog(QtYabause::translate("Loaded %1 edit(s) from %2 (%3 failed). Pack made for: %4")
               .arg(ok).arg(f).arg(bad).arg(QString::fromStdString(meta)));
    refreshSummary();
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
        QtYabause::translate("Select an empty output folder (not the folder of the original image)"));
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);

    QProgressDialog pd(QtYabause::translate("Copying the image..."), QtYabause::translate("Cancel"), 0, 100, this);
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
    addLog(QtYabause::translate("Patched image written to %1").arg(dir));
    QMessageBox::information(this, windowTitle(),
        QtYabause::translate("Patched image written to:\n%1\n\n%2 edit(s) applied.").arg(dir).arg(build.applied));
}

void UIEditsExport::onExportBps()
{
    EditsPatcher::Image img;
    EditsPatcher::Build build;
    if (!prepare(img, build))
        return;
    const QString base = CommonDialogs::getSaveFileName(
        QFileInfo(mImage->text()).absolutePath() + "/" + imageBaseName() + ".bps",
        QtYabause::translate("Save BPS patch"), "BPS patch (*.bps)");
    if (base.isEmpty())
        return;

    QProgressDialog pd(QtYabause::translate("Computing checksums..."), QtYabause::translate("Cancel"), 0, 100, this);
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
        if (multi) {   // plusieurs fichiers modifies : un patch par fichier
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
    addLog(QtYabause::translate("BPS patch written: %1").arg(written.join(", ")));
    QMessageBox::information(this, windowTitle(),
        QtYabause::translate("BPS patch written:\n%1\n\nApply it to the ORIGINAL image with any BPS patcher "
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
        QtYabause::translate("Save IPS patch"), "IPS patch (*.ips)");
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
    addLog(QtYabause::translate("IPS patch written: %1").arg(written.join(", ")));
    QMessageBox::information(this, windowTitle(), QtYabause::translate("IPS patch written:\n%1").arg(written.join("\n")));
}
