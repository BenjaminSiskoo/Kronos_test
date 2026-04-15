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
#include "UICheatSearch.h"
#include "UICheatRaw.h"
#include "../CommonDialogs.h"
#include <QIntValidator>
#include <QRegularExpressionValidator>

UICheatSearch::UICheatSearch(QWidget* p, QList<cheatsearch_struct> *search, int searchType) 
    : QDialog(p)
{
    setupUi(this);
    if (p && !p->isFullScreen())
        setWindowFlags(Qt::Sheet);

    this->search = *search;
    this->searchType = searchType;

    if (this->search.isEmpty())
    {
        pbRestart->setText(QtYabause::translate("Start"));
        pbSearch->setEnabled(false);
        pbAddCheat->setEnabled(false);
    }

    getSearchTypes();
    listResults();
    adjustSearchValueQValidator();

    QtYabause::retranslateWidget(this);
}

UICheatSearch::~UICheatSearch()
{
    for (int i = 0; i < search.count(); i++) {
        if (search[i].results) {
            free(search[i].results);
            search[i].results = NULL;
        }
    }
}

QList<cheatsearch_struct> * UICheatSearch::getSearchVariables(int *searchType)
{
    if (searchType)
        *searchType = this->searchType;
    return &this->search;
}

void UICheatSearch::on_leSearchValue_textChanged(const QString &text)
{
    pbSearch->setEnabled(!text.isEmpty());
}

void UICheatSearch::getSearchTypes()
{
    switch (searchType & 0xC)
    {
        case SEARCHEXACT: rbExact->setChecked(true); break;
        case SEARCHLESSTHAN: rbLessThan->setChecked(true); break;
        case SEARCHGREATERTHAN: rbGreaterThan->setChecked(true); break;
    }

    if ((searchType & 0x70) == SEARCHSIGNED) rbSigned->setChecked(true);
    else rbUnsigned->setChecked(true);

    switch (searchType & 0x3)
    {
        case SEARCHBYTE: rb8Bit->setChecked(true); break;
        case SEARCHWORD: rb16Bit->setChecked(true); break;
        case SEARCHLONG: rb32Bit->setChecked(true); break;
    }
}

void UICheatSearch::setSearchTypes()
{
    searchType = 0;
    if (rbExact->isChecked()) searchType |= SEARCHEXACT;
    else if (rbLessThan->isChecked()) searchType |= SEARCHLESSTHAN;
    else searchType |= SEARCHGREATERTHAN;

    if (rbSigned->isChecked()) searchType |= SEARCHSIGNED;
    else searchType |= SEARCHUNSIGNED;

    if (rb8Bit->isChecked()) searchType |= SEARCHBYTE;
    else if (rb16Bit->isChecked()) searchType |= SEARCHWORD;
    else searchType |= SEARCHLONG;
}

void UICheatSearch::listResults()
{
    twSearchResults->setUpdatesEnabled(false);
    twSearchResults->clear();
    pbAddCheat->setEnabled(false);

    for (int j = 0; j < search.count(); j++)
    {
        if (search[j].results)
        {
            for (u32 i = 0; i < search[j].numResults; i++)
            {
                QTreeWidgetItem* it = new QTreeWidgetItem(twSearchResults);
                it->setText(0, QString("%1").arg(search[j].results[i].addr, 8, 16, QChar('0')).toUpper());

                QString valStr;
                u32 addr = search[j].results[i].addr;
                switch (searchType & 0x3)
                {
                    case SEARCHBYTE: valStr = QString::number(DMAMappedMemoryReadByte(addr)); break;
                    case SEARCHWORD: valStr = QString::number(DMAMappedMemoryReadWord(addr)); break;
                    case SEARCHLONG: valStr = QString::number(DMAMappedMemoryReadLong(addr)); break;
                }
                it->setText(1, valStr);
            }
        }
    }
    twSearchResults->setUpdatesEnabled(true);

    QLabel* resLabel = this->findChild<QLabel*>("lResultCount");
    if (resLabel)
        resLabel->setText(QtYabause::translate("%1 result(s) found").arg(twSearchResults->topLevelItemCount()));
}

void UICheatSearch::adjustSearchValueQValidator()
{
    long long min = 0;
    unsigned long long max = 0;

    if (rb8Bit->isChecked()) max = 0xFF;
    else if (rb16Bit->isChecked()) max = 0xFFFF;
    else max = 0xFFFFFFFF;

    if (rbSigned->isChecked()) {
        min = -static_cast<long long>((max >> 1) + 1);
        max >>= 1;
    }

    if (rb32Bit->isChecked()) {
        QString pattern = rbSigned->isChecked() ? "^-?\\d{1,10}$" : "^\\d{1,10}$";
        leSearchValue->setValidator(new QRegularExpressionValidator(QRegularExpression(pattern), leSearchValue));
    } else {
        leSearchValue->setValidator(new QIntValidator(static_cast<int>(min), static_cast<int>(max), leSearchValue));
    }
}

void UICheatSearch::on_twSearchResults_itemSelectionChanged()
{
    pbAddCheat->setEnabled(twSearchResults->selectedItems().count() > 0);
}

void UICheatSearch::on_rbUnsigned_toggled(bool checked) { if (checked) adjustSearchValueQValidator(); }
void UICheatSearch::on_rbSigned_toggled(bool checked) { if (checked) adjustSearchValueQValidator(); }
void UICheatSearch::on_rb8Bit_toggled(bool checked) { if (checked) adjustSearchValueQValidator(); }
void UICheatSearch::on_rb16Bit_toggled(bool checked) { if (checked) adjustSearchValueQValidator(); }
void UICheatSearch::on_rb32Bit_toggled(bool checked) { if (checked) adjustSearchValueQValidator(); }

void UICheatSearch::on_pbRestart_clicked()
{
    if (search.isEmpty())
        pbRestart->setText(QtYabause::translate("Restart"));
    else {
        for (int j = 0; j < search.count(); j++)
            if (search[j].results) free(search[j].results);
        search.clear();
    }

    cheatsearch_struct swram;
    swram.results = NULL; swram.startAddr = 0x06000000; swram.endAddr = 0x06100000;
    swram.numResults = swram.endAddr - swram.startAddr;
    search.append(swram);

    cheatsearch_struct lwram;
    lwram.results = NULL; lwram.startAddr = 0x00200000; lwram.endAddr = 0x00300000;
    lwram.numResults = lwram.endAddr - lwram.startAddr;
    search.append(lwram);

    twSearchResults->clear();
}

void UICheatSearch::on_pbSearch_clicked()
{
    if (LowWram && HighWram) {
        setSearchTypes();
        for (int i = 0; i < search.count(); i++) {
            search[i].results = MappedMemorySearch(search[i].startAddr, search[i].endAddr, searchType,
                leSearchValue->text().toLatin1().data(), search[i].results, &search[i].numResults);
        }
        listResults();
    }
}

void UICheatSearch::on_pbAddCheat_clicked()
{
    QTreeWidgetItem *it = twSearchResults->currentItem();
    if (!it) return;

    UICheatRaw d(this);
    d.leAddress->setText(it->text(0));
    d.leValue->setText(QString("%1").arg(it->text(1).toUInt(), 0, 16).toUpper());
    d.rbByte->setChecked(rb8Bit->isChecked());
    d.rbWord->setChecked(rb16Bit->isChecked());
    d.rbLong->setChecked(rb32Bit->isChecked());

    if (d.exec()) {
        bool ok;
        u32 addr = d.leAddress->text().toUInt(&ok, 16);
        u32 val = d.leValue->text().toUInt(&ok, 16);
        if (CheatAddCode(d.type(), addr, val) == 0) {
            int count;
            CheatGetList(&count);
            CheatChangeDescriptionByIndex(count - 1, d.teDescription->toPlainText().toLatin1().data());
        }
    }
}
