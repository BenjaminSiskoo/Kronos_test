/* Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

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
#ifndef UICHEATSEARCH_H
#define UICHEATSEARCH_H

#include "ui_UICheatSearch.h"
#include "../QtYabause.h"
#include <QList>
#include <QVector>

// One searchable memory region. "results" holds every remaining candidate
// together with the value it had at the last pass (used as the reference for
// the "decreased / increased / by %" comparisons). It is a plain value type, so
// copying the list between UIYabause and the dialog needs no manual memory
// management. "scanned" is false until the first pass has been done.
struct cheatsearch_struct
{
   cheatsearch_struct() : scanned(false), startAddr(0), endAddr(0) {}

   QVector<result_struct> results;
   QVector<qint64> history;   // number of candidates after each pass, for the "narrowing" display
   bool scanned;
   u32 startAddr;
   u32 endAddr;   // exclusive
};

// Layout of the "searchType" integer kept by UIYabause between two openings:
//   bits 0-1  data size (SEARCHBYTE / SEARCHWORD / SEARCHLONG)
//   bits 4-6  SEARCHUNSIGNED / SEARCHSIGNED
//   bits 8-11 comparison mode (UICheatSearch::CompareMode)
//   bit  12   set once the dialog has stored a choice
#define CHEATSEARCH_MODE_SHIFT  8
#define CHEATSEARCH_MODE_MASK   (0xF << CHEATSEARCH_MODE_SHIFT)
#define CHEATSEARCH_INITIALIZED (1 << 12)

class UICheatSearch : public QDialog, public Ui::UICheatSearch
{
    Q_OBJECT

public:
   // Order must match the items of cbCompare in UICheatSearch.ui
   enum CompareMode
   {
      CmpExact = 0,
      CmpLessThan,
      CmpGreaterThan,
      CmpDecreased,
      CmpIncreased,
      CmpChanged,
      CmpUnchanged,
      CmpDecreasedByPercent,
      CmpIncreasedByPercent
   };

   UICheatSearch(QWidget* p, QList<cheatsearch_struct> *search, int searchType);
   virtual ~UICheatSearch();

   QList<cheatsearch_struct> *getSearchVariables(int *searchType);

protected:
   QList<cheatsearch_struct> search;
   int searchType;

   void getSearchTypes();
   void setSearchTypes();
   void listResults();
   void adjustSearchValueQValidator();
   void updateUiState();
   void updateSteps(int step);
   void setupCards();
   void applyStyle();
   bool hasScannedRegions() const;
   int  compareMode() const;
   void setCompareMode(int mode);
   void loadPercentSettings();
   void savePercentSettings() const;

protected slots:
   void on_twSearchResults_itemSelectionChanged();
   void on_twSearchResults_itemDoubleClicked(QTreeWidgetItem *item, int column);
   void on_leSearchValue_textChanged(const QString &text);
   void on_pbRestart_clicked();
   void on_pbSearch_clicked();
   void on_pbAddCheat_clicked();
   void on_rbUnsigned_toggled(bool checked);
   void on_rbSigned_toggled(bool checked);
   void on_rb8Bit_toggled(bool checked);
   void on_rb16Bit_toggled(bool checked);
   void on_rb32Bit_toggled(bool checked);
   void on_tbAdvanced_toggled(bool checked);
   void onCompareChanged();
};

#endif
