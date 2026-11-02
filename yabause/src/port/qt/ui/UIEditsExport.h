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

/*  UIEditsExport -- the edits of the graphics editor, one by one.

    - list with before/after thumbnails: switch each edit on or off, name it,
      undo / redo it, delete it;
    - packs (.kmod): several can be loaded at once; conflicts between them
      (same original bytes changed two ways) are shown; a pack made for
      another disc (product number of the IP.BIN) is refused or flagged;
    - packs of the running game are loaded automatically at reset from the
      mods folder (autoLoadPacks, called by UIYabause);
    - writing the edits into a disc image (patched copy, BPS, IPS), see
      EditsPatcher.h. */

#ifndef UIEDITSEXPORT_H
#define UIEDITSEXPORT_H

#include <QDialog>
#include <QString>
#include <vector>
#include "../EditsPatcher.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class UIEditsExport : public QDialog
{
    Q_OBJECT

public:
    explicit UIEditsExport(QWidget *parent = 0);

    // Product number / version / title of the disc in the drive ("" if none).
    static QString currentProduct();
    static QString currentVersion();
    static QString currentTitle();
    // Folder the packs are loaded from automatically (setting GfxEditor/ModsDir).
    static QString modsDir();
    static bool autoLoadEnabled();
    // Loads every pack of the mods folder made for the disc in the drive and
    // not loaded yet. Returns the number of packs loaded; 'log' gets one line
    // per pack. Called at every reset.
    static int autoLoadPacks(QStringList *log);
    // Loads one pack. 'ask' = the user can be asked (other disc, already
    // loaded); otherwise a pack for another disc is refused. Returns the
    // number of edits loaded, -1 on error / refusal (message in *msg).
    static int loadPackFile(const QString &path, bool ask, QWidget *parent, QString *msg);

private slots:
    void refreshList();
    void onItemChanged(QTreeWidgetItem *item, int column);
    void onSelectionChanged();
    void onUndo();
    void onRedo();
    void onDelete();
    void onToggleSelected();
    void onSavePack();
    void onSaveSelected();
    void onLoadPack();
    void onUnloadPack();
    void onOpenModsDir();
    void onChooseImage();
    void onAnalyze();
    void onExportImage();
    void onExportBps();
    void onExportIps();
    void onTick();

private:
    // edits as pack records ('onlyIds' empty = all enabled, non-empty edits)
    std::vector<EditsPatcher::Record> collectPack(const std::vector<unsigned int> &onlyIds) const;
    // edits ready to be looked for on the disc (colors grouped in palettes)
    std::vector<EditsPatcher::Record> collectForDisc() const;
    bool prepare(EditsPatcher::Image &img, EditsPatcher::Build &build);
    bool savePackTo(const QString &file, const std::vector<unsigned int> &ids);
    std::vector<unsigned int> selectedIds() const;
    QString selectedPack() const;
    void addLog(const QString &line);
    QString imageBaseName() const;
    void updateHeader();

    QLabel *lTitle, *lChipGame, *lChipEdits, *lChipConflicts;
    QTreeWidget *twEdits;
    QPushButton *pbUndo, *pbRedo, *pbDelete, *pbToggle;
    QListWidget *lwPacks;
    QPushButton *pbLoadPack, *pbUnloadPack, *pbSavePack, *pbSaveSelected, *pbOpenMods;
    QCheckBox *cbAutoLoad;
    QLabel *lModsDir;
    QPlainTextEdit *mConflicts;
    QLineEdit *mImage;
    QPlainTextEdit *mLog;
    QTimer *mTimer;
    unsigned mLastGen;
    bool mFilling;
};

#endif
