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
#include "../Settings.h"
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QVBoxLayout>
#include <QtGlobal>
#include <cmath>

namespace {

// Showing millions of rows in a QTreeWidget would freeze the dialog, so only
// the first candidates are listed. The search itself always uses all of them.
const int MaxDisplayedResults = 1000;

// Below this many candidates the next logical step is to add a cheat
const qint64 FewCandidates = 20;

const u32 HighWramStart = 0x06000000;
const u32 HighWramEnd   = 0x06100000;
const u32 LowWramStart  = 0x00200000;
const u32 LowWramEnd    = 0x00300000;

bool needsValue(int mode)    { return mode <= UICheatSearch::CmpGreaterThan; }
bool isPercentMode(int mode) { return mode == UICheatSearch::CmpDecreasedByPercent || mode == UICheatSearch::CmpIncreasedByPercent; }
bool isRelativeMode(int mode){ return mode >= UICheatSearch::CmpDecreased; }

// 0 = known value, 1 = went up/down, 2 = percentage
int categoryOf(int mode)
{
   if (mode <= UICheatSearch::CmpGreaterThan) return 0;
   if (mode <= UICheatSearch::CmpUnchanged) return 1;
   return 2;
}

u32 readRaw(u32 addr, int size)
{
   switch (size)
   {
      case SEARCHBYTE: return DMAMappedMemoryReadByte(addr);
      case SEARCHWORD: return DMAMappedMemoryReadWord(addr);
      default:         return DMAMappedMemoryReadLong(addr);
   }
}

qint64 toNumber(u32 raw, int size, bool isSigned)
{
   switch (size)
   {
      case SEARCHBYTE: return isSigned ? (qint64)(qint8)raw  : (qint64)(quint8)raw;
      case SEARCHWORD: return isSigned ? (qint64)(qint16)raw : (qint64)(quint16)raw;
      default:         return isSigned ? (qint64)(qint32)raw : (qint64)(quint32)raw;
   }
}

void valueRange(int size, bool isSigned, qint64 *minVal, qint64 *maxVal)
{
   const int bits = 8 << size;
   if (isSigned)
   {
      *minVal = -((qint64)1 << (bits - 1));
      *maxVal = ((qint64)1 << (bits - 1)) - 1;
   }
   else
   {
      *minVal = 0;
      *maxVal = ((qint64)1 << bits) - 1;
   }
}

struct ScanParams
{
   int mode;
   int size;
   bool isSigned;
   qint64 value;      // value modes
   double percent;    // percentage modes
   double tolerance;  // percentage modes, in percentage points
};

// "prev" is the value stored at the previous pass, "cur" the one read now.
bool matches(const ScanParams &p, qint64 cur, qint64 prev)
{
   switch (p.mode)
   {
      case UICheatSearch::CmpExact:       return cur == p.value;
      case UICheatSearch::CmpLessThan:    return cur < p.value;
      case UICheatSearch::CmpGreaterThan: return cur > p.value;
      case UICheatSearch::CmpDecreased:   return cur < prev;
      case UICheatSearch::CmpIncreased:   return cur > prev;
      case UICheatSearch::CmpChanged:     return cur != prev;
      case UICheatSearch::CmpUnchanged:   return cur == prev;
      case UICheatSearch::CmpDecreasedByPercent:
      case UICheatSearch::CmpIncreasedByPercent:
      {
         // A ratio against zero is meaningless, and the direction must be
         // right so that a 0 % drift never matches a "decreased by 1 %" search.
         if (prev == 0)
            return false;
         const bool decrease = (p.mode == UICheatSearch::CmpDecreasedByPercent);
         if (decrease ? (cur >= prev) : (cur <= prev))
            return false;
         const double change = std::fabs((double)(cur - prev)) * 100.0 / std::fabs((double)prev);
         return std::fabs(change - p.percent) <= p.tolerance;
      }
   }
   return false;
}

// One pass over a region. The first pass reads the whole region (a plain
// snapshot for the relative modes), the following ones only the candidates
// that survived. Every candidate keeps the value read now, which becomes the
// reference of the next pass.
void runPass(cheatsearch_struct &r, const ScanParams &p)
{
   QVector<result_struct> out;
   const u32 step = 1u << p.size;

   if (!r.scanned)
   {
      const bool snapshot = isRelativeMode(p.mode);
      out.reserve(snapshot ? (int)((r.endAddr - r.startAddr) / step) : 4096);

      for (u32 addr = r.startAddr; addr + step <= r.endAddr; addr += step)
      {
         const u32 raw = readRaw(addr, p.size);
         if (snapshot || matches(p, toNumber(raw, p.size, p.isSigned), 0))
         {
            result_struct e;
            e.addr = addr;
            e.val = raw;
            out.append(e);
         }
      }
   }
   else
   {
      out.reserve(r.results.size());

      for (int i = 0; i < r.results.size(); i++)
      {
         const u32 raw = readRaw(r.results[i].addr, p.size);
         if (matches(p, toNumber(raw, p.size, p.isSigned), toNumber(r.results[i].val, p.size, p.isSigned)))
         {
            result_struct e;
            e.addr = r.results[i].addr;
            e.val = raw;
            out.append(e);
         }
      }
   }

   r.results = out;
   r.scanned = true;
   r.history.append(out.size());
}

QString tr_(const char *s) { return QtYabause::translate(QString::fromUtf8(s)); }

QColor mixColors(const QColor &a, const QColor &b, double t)
{
   return QColor((int)(a.red() * t + b.red() * (1.0 - t)),
                 (int)(a.green() * t + b.green() * (1.0 - t)),
                 (int)(a.blue() * t + b.blue() * (1.0 - t)));
}

// Secondary text color that follows the current theme, light or dark
QColor dimColor(const QWidget *w)
{
   return mixColors(w->palette().color(QPalette::WindowText), w->palette().color(QPalette::Window), 0.6);
}

void setDim(QWidget *w)
{
   w->setProperty("dim", true);   // colored by the "QLabel[dim]" rule of the dialog stylesheet
}

// A "card" is a checkable button holding a bold title and a dimmed description
void fillCard(QPushButton *b, const QString &title, const QString &sub)
{
   QVBoxLayout *l = new QVBoxLayout(b);
   l->setContentsMargins(14, 8, 14, 8);
   l->setSpacing(2);

   QLabel *t = new QLabel(title, b);
   QFont f = t->font();
   f.setBold(true);
   t->setFont(f);

   QLabel *s = new QLabel(sub, b);
   s->setWordWrap(true);
   setDim(s);

   t->setAttribute(Qt::WA_TransparentForMouseEvents);
   s->setAttribute(Qt::WA_TransparentForMouseEvents);
   l->addWidget(t);
   l->addWidget(s);
}

} // namespace

UICheatSearch::UICheatSearch(QWidget* p, QList<cheatsearch_struct> *search, int searchType) 
    : QDialog(p)
{
    setupUi(this);
    if (p && !p->isFullScreen())
        setWindowFlags(Qt::Sheet);

    this->search = *search;
    this->searchType = searchType;

    setupCards();
    applyStyle();

    twSearchResults->header()->setSectionResizeMode(QHeaderView::Stretch);
    for (int c = 1; c <= 3; c++)
        twSearchResults->headerItem()->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    wAdvanced->setVisible(false);

    getSearchTypes();
    loadPercentSettings();

    // Regions already scanned in a previous session decide which boxes are checked
    if (!this->search.isEmpty())
    {
        bool high = false, low = false;
        for (int i = 0; i < this->search.count(); i++)
        {
            if (this->search[i].startAddr == HighWramStart) high = true;
            if (this->search[i].startAddr == LowWramStart) low = true;
        }
        cbHighWram->setChecked(high);
        cbLowWram->setChecked(low);
    }

    // Everything that changes what the options panel shows
    QList<QPushButton*> buttons = findChildren<QPushButton*>();
    for (int i = 0; i < buttons.count(); i++)
        if (buttons[i]->isCheckable())
            connect(buttons[i], SIGNAL(toggled(bool)), this, SLOT(onCompareChanged()));
    connect(dsbPercent, SIGNAL(valueChanged(double)), this, SLOT(onCompareChanged()));
    connect(cbHighWram, SIGNAL(toggled(bool)), this, SLOT(onCompareChanged()));
    connect(cbLowWram, SIGNAL(toggled(bool)), this, SLOT(onCompareChanged()));
    connect(leSearchValue, SIGNAL(returnPressed()), pbSearch, SLOT(click()));

    listResults();
    adjustSearchValueQValidator();
    updateUiState();

    if (categoryOf(compareMode()) == 0)
        leSearchValue->setFocus();

    QtYabause::retranslateWidget(this);
}

UICheatSearch::~UICheatSearch()
{
    savePercentSettings();
}

QList<cheatsearch_struct> * UICheatSearch::getSearchVariables(int *searchType)
{
    if (searchType)
        *searchType = this->searchType;
    return &this->search;
}

void UICheatSearch::setupCards()
{
    fillCard(pbCardValue, tr_("An exact number"), tr_("Lives, ammo, score, money..."));
    fillCard(pbCardRelative, tr_("It went up or down"), tr_("You don't know the real value"));
    fillCard(pbCardPercent, tr_("A bar lost or gained a percentage"), tr_("Life bar, energy gauge..."));

    setDim(lSelectHint);
    setDim(lHistory);
    setDim(lPaused);
    setDim(lAdvSummary);
    setDim(lPercentPreview);
    setDim(lHint);
    setDim(lRelTitle);

    QFont f = lTitle->font();
    f.setPointSize(f.pointSize() + 6);
    f.setBold(true);
    lTitle->setFont(f);

    f = lFind->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 1);
    lFind->setFont(f);
    lResultsTitle->setFont(f);
}

void UICheatSearch::applyStyle()
{
    // Only palette() colors are used, so the dialog follows light and dark themes
    setStyleSheet(QString::fromLatin1("QLabel[dim=\"true\"] { color: %1; }").arg(dimColor(this).name()) + QString::fromLatin1(
        "QFrame#panelLeft, QFrame#panelRight {"
        "  background: palette(base); border: 1px solid palette(mid); border-radius: 10px; }"

        "QLabel#lResultCount { border: 1px solid palette(mid); border-radius: 10px;"
        "  padding: 2px 10px; font-weight: 600; }"

        "QPushButton[card=\"true\"] { text-align: left; background: transparent;"
        "  border: 2px solid palette(mid); border-radius: 8px; }"
        "QPushButton[card=\"true\"]:hover { border-color: palette(highlight); }"
        "QPushButton[card=\"true\"]:checked { border-color: palette(highlight); background: palette(alternate-base); }"

        "QPushButton[segment=\"true\"] { border: 1px solid palette(mid); border-radius: 6px;"
        "  padding: 6px 10px; background: transparent; }"
        "QPushButton[segment=\"true\"]:hover { border-color: palette(highlight); }"
        "QPushButton[segment=\"true\"]:checked { background: palette(highlight);"
        "  color: palette(highlighted-text); border-color: palette(highlight); }"

        "QPushButton#pbSearch { background: palette(highlight); color: palette(highlighted-text);"
        "  border: none; border-radius: 8px; padding: 8px 18px; font-weight: 600; }"
        "QPushButton#pbSearch:disabled { background: palette(midlight); color: palette(mid); }"

        "QPushButton#pbRestart, QPushButton#pbAddCheat { border: 1px solid palette(mid);"
        "  border-radius: 8px; padding: 6px 14px; background: transparent; }"
        "QPushButton#pbRestart:hover, QPushButton#pbAddCheat:hover { border-color: palette(highlight); }"
        "QPushButton#pbRestart:disabled, QPushButton#pbAddCheat:disabled { color: palette(mid); }"

        "QLineEdit { border: 1px solid palette(mid); border-radius: 6px; padding: 6px 8px;"
        "  background: palette(base); }"
        "QLineEdit:focus { border-color: palette(highlight); }"

        "QTreeWidget { background: transparent; alternate-background-color: palette(alternate-base); border: none; }"
        "QTreeWidget::item { padding: 3px 4px; }"
        "QHeaderView::section { background: transparent; border: none;"
        "  border-bottom: 1px solid palette(mid); padding: 6px 8px; font-weight: 600; }"

        "QToolButton#tbAdvanced { border: none; font-weight: 600; }"
        "QGroupBox { border: 1px solid palette(mid); border-radius: 8px; margin-top: 12px; padding-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"));
}

void UICheatSearch::loadPercentSettings()
{
    QSettings *s = QtYabause::settings();
    if (!s)
        return;
    dsbPercent->setValue(s->value("CheatSearch/Percent", dsbPercent->value()).toDouble());
    dsbTolerance->setValue(s->value("CheatSearch/Tolerance", dsbTolerance->value()).toDouble());
}

void UICheatSearch::savePercentSettings() const
{
    QSettings *s = QtYabause::settings();
    if (!s)
        return;
    s->setValue("CheatSearch/Percent", dsbPercent->value());
    s->setValue("CheatSearch/Tolerance", dsbTolerance->value());
}

bool UICheatSearch::hasScannedRegions() const
{
    for (int i = 0; i < search.count(); i++)
        if (search[i].scanned)
            return true;
    return false;
}

int UICheatSearch::compareMode() const
{
    if (pbCardValue->isChecked())
    {
        if (pbLess->isChecked()) return CmpLessThan;
        if (pbGreater->isChecked()) return CmpGreaterThan;
        return CmpExact;
    }
    if (pbCardRelative->isChecked())
    {
        if (pbInc->isChecked()) return CmpIncreased;
        if (pbChg->isChecked()) return CmpChanged;
        if (pbSame->isChecked()) return CmpUnchanged;
        return CmpDecreased;
    }
    return pbGained->isChecked() ? CmpIncreasedByPercent : CmpDecreasedByPercent;
}

void UICheatSearch::setCompareMode(int mode)
{
    switch (categoryOf(mode))
    {
        case 0:
            pbCardValue->setChecked(true);
            (mode == CmpLessThan ? pbLess : (mode == CmpGreaterThan ? pbGreater : pbEq))->setChecked(true);
            break;
        case 1:
            pbCardRelative->setChecked(true);
            (mode == CmpIncreased ? pbInc : (mode == CmpChanged ? pbChg : (mode == CmpUnchanged ? pbSame : pbDec)))->setChecked(true);
            break;
        default:
            pbCardPercent->setChecked(true);
            (mode == CmpIncreasedByPercent ? pbGained : pbLost)->setChecked(true);
            break;
    }
}

void UICheatSearch::getSearchTypes()
{
    int mode = CmpExact;
    int size = SEARCHWORD;   // 16-bit is the most common size for counters and life bars
    bool isSigned = false;

    if (searchType & CHEATSEARCH_INITIALIZED)
    {
        mode = (searchType & CHEATSEARCH_MODE_MASK) >> CHEATSEARCH_MODE_SHIFT;
        size = searchType & 0x3;
        isSigned = (searchType & 0x70) == SEARCHSIGNED;
    }

    setCompareMode(mode <= CmpIncreasedByPercent ? mode : CmpExact);

    rbSigned->setChecked(isSigned);
    rbUnsigned->setChecked(!isSigned);

    switch (size)
    {
        case SEARCHBYTE: rb8Bit->setChecked(true); break;
        case SEARCHLONG: rb32Bit->setChecked(true); break;
        default: rb16Bit->setChecked(true); break;
    }
}

void UICheatSearch::setSearchTypes()
{
    searchType = CHEATSEARCH_INITIALIZED;
    searchType |= (compareMode() << CHEATSEARCH_MODE_SHIFT) & CHEATSEARCH_MODE_MASK;

    searchType |= rbSigned->isChecked() ? SEARCHSIGNED : SEARCHUNSIGNED;

    if (rb8Bit->isChecked()) searchType |= SEARCHBYTE;
    else if (rb16Bit->isChecked()) searchType |= SEARCHWORD;
    else searchType |= SEARCHLONG;
}

void UICheatSearch::listResults()
{
    const int size = rb8Bit->isChecked() ? SEARCHBYTE : (rb16Bit->isChecked() ? SEARCHWORD : SEARCHLONG);
    const bool isSigned = rbSigned->isChecked();
    const QColor down(209, 52, 56), up(46, 160, 67);
    const QString downArrow = QString::fromUtf8("\xE2\x96\xBC "), upArrow = QString::fromUtf8("\xE2\x96\xB2 ");
    const QLocale loc;

    twSearchResults->setUpdatesEnabled(false);
    twSearchResults->clear();

    qint64 total = 0;
    int shown = 0;
    int passes = -1;

    for (int j = 0; j < search.count(); j++)
    {
        if (!search[j].scanned)
            continue;

        total += search[j].results.size();
        passes = (passes < 0) ? search[j].history.size() : qMin(passes, search[j].history.size());

        for (int i = 0; i < search[j].results.size() && shown < MaxDisplayedResults; i++, shown++)
        {
            const result_struct &res = search[j].results[i];
            const u32 curRaw = readRaw(res.addr, size);
            const qint64 cur = toNumber(curRaw, size, isSigned);
            const qint64 prev = toNumber(res.val, size, isSigned);

            QTreeWidgetItem* it = new QTreeWidgetItem(twSearchResults);
            it->setText(0, QString("0x%1").arg(res.addr, 8, 16, QChar('0')).toUpper().replace("0X", "0x"));
            it->setText(1, loc.toString(cur));
            it->setText(2, loc.toString(prev));
            it->setData(1, Qt::UserRole, (uint)curRaw);   // raw value, used by "Add Cheat"
            it->setToolTip(1, QString("0x%1").arg(curRaw, 2 << size, 16, QChar('0')).toUpper().replace("0X", "0x"));

            QString change;
            if (cur == prev)
                change = QString::fromUtf8("0 %");
            else if (prev == 0)
                change = QString::fromUtf8("\xE2\x80\x94");   // em dash, no ratio against zero
            else
            {
                const double pct = (double)(cur - prev) * 100.0 / std::fabs((double)prev);
                change = (cur < prev ? downArrow : upArrow) + QString("%1%2 %").arg(pct > 0 ? "+" : "").arg(pct, 0, 'f', 1);
            }
            it->setText(3, change);

            if (cur != prev)
                it->setForeground(3, QBrush(cur < prev ? down : up));

            for (int c = 1; c <= 3; c++)
                it->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    twSearchResults->setUpdatesEnabled(true);

    // Results page or empty-state page
    swResults->setCurrentIndex(shown > 0 ? 1 : 0);
    if (!hasScannedRegions())
        lEmpty->setText(tr_("<h3>Find where a game stores a value</h3>"
                            "<p>1. Pick what you know on the left, then press <b>Search</b> or <b>Take snapshot</b>.<br>"
                            "2. Play until the value changes, then reopen this window.<br>"
                            "3. Search again until only a few addresses remain.<br>"
                            "4. Double-click an address to turn it into a cheat.</p>"));
    else
        lEmpty->setText(tr_("<h3>No address matches</h3>"
                            "<p>Try a larger tolerance, check the data size in <i>Advanced options</i>, "
                            "or press <b>New search</b> to start over.</p>"));

    // Result counter and narrowing history
    if (!hasScannedRegions())
        lResultCount->setText(tr_("No search yet"));
    else if (total == 1)
        lResultCount->setText(tr_("1 result"));
    else
        lResultCount->setText(tr_("%1 results").arg(loc.toString(total)));

    QString history;
    if (passes >= 2)
    {
        QStringList steps;
        const int first = qMax(0, passes - 6);
        for (int k = first; k < passes; k++)
        {
            qint64 n = 0;
            for (int j = 0; j < search.count(); j++)
                if (search[j].scanned && k < search[j].history.size())
                    n += search[j].history[k];
            steps << loc.toString(n);
        }
        history = tr_("Narrowing down: ") + (first > 0 ? QString::fromUtf8("\xE2\x80\xA6 \xE2\x86\x92 ") : QString())
                  + steps.join(QString::fromUtf8(" \xE2\x86\x92 "));
    }
    lHistory->setText(history);
    lHistory->setVisible(!history.isEmpty());

    if (shown == 0)
        lSelectHint->setText(QString());
    else if (total > shown)
        lSelectHint->setText(tr_("Showing the first %1. Narrow down the search to see the rest.").arg(loc.toString(shown)));
    else
        lSelectHint->setText(tr_("Double-click an address to add it as a cheat."));
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

void UICheatSearch::updateSteps(int step)
{
    const QColor on = palette().color(QPalette::Highlight);
    const QColor done = palette().color(QPalette::WindowText);
    const QColor off = dimColor(this);
    const char *names[3] = { "Search", "Narrow down", "Add cheat" };

    QStringList parts;
    for (int i = 0; i < 3; i++)
    {
        QString text = tr_(names[i]);
        QString mark = (i < step) ? QString::fromUtf8("\xE2\x9C\x93") : QString::number(i + 1);
        QString html = QString("%1&nbsp;%2").arg(mark).arg(text);
        if (i == step)
            html = QString("<b><span style=\"color:%1\">%2</span></b>").arg(on.name()).arg(html);
        else
            html = QString("<span style=\"color:%1\">%2</span>").arg((i < step ? done : off).name()).arg(html);
        parts << html;
    }
    lSteps->setText(parts.join(QString::fromUtf8("&nbsp;&nbsp;\xE2\x80\xBA&nbsp;&nbsp;")));
}

void UICheatSearch::updateUiState()
{
    const bool scanned = hasScannedRegions();
    const int mode = compareMode();
    const QLocale loc;

    qint64 candidates = 0;
    for (int i = 0; i < search.count(); i++)
        candidates += search[i].results.size();

    // Only the options that make sense for the chosen kind of search are shown
    swOptions->setCurrentIndex(categoryOf(mode));

    // Size and memory areas are fixed once a search has started, otherwise the
    // stored values and addresses would no longer match what is being read
    rbDataSize->setEnabled(!scanned);
    gbRegions->setEnabled(!scanned);

    const bool relativeFirstPass = isRelativeMode(mode) && !scanned;
    pbSearch->setText(tr_(relativeFirstPass ? "Take snapshot" : "Search"));

    bool canSearch = true;
    if (needsValue(mode) && leSearchValue->text().isEmpty())
        canSearch = false;
    if (!scanned && !cbHighWram->isChecked() && !cbLowWram->isChecked())
        canSearch = false;
    if (scanned && candidates == 0)
        canSearch = false;
    pbSearch->setEnabled(canSearch);

    pbRestart->setEnabled(scanned);
    pbAddCheat->setEnabled(twSearchResults->selectedItems().count() > 0);

    // Percentage example
    if (isPercentMode(mode))
    {
        const double p = dsbPercent->value();
        if (mode == CmpDecreasedByPercent)
            lPercentPreview->setText(p < 100.0
                ? tr_("Example: 100 \xE2\x86\x92 %1 is a loss of %2 %.").arg(100.0 - p, 0, 'f', 1).arg(p, 0, 'f', 1)
                : tr_("A loss of 100 % or more means the value reached zero."));
        else
            lPercentPreview->setText(tr_("Example: 100 \xE2\x86\x92 %1 is a gain of %2 %.").arg(100.0 + p, 0, 'f', 1).arg(p, 0, 'f', 1));
    }

    // Advanced options summary, visible even when the section is folded
    const QString sep = QString::fromUtf8(" \xC2\xB7 ");
    const int size = rb8Bit->isChecked() ? 8 : (rb16Bit->isChecked() ? 16 : 32);
    QString regions;
    if (cbHighWram->isChecked() && cbLowWram->isChecked()) regions = tr_("High + Low WRAM");
    else if (cbHighWram->isChecked()) regions = tr_("High WRAM");
    else if (cbLowWram->isChecked()) regions = tr_("Low WRAM");
    else regions = tr_("no memory selected");
    lAdvSummary->setText(QString("%1-bit").arg(size) + sep + (rbSigned->isChecked() ? tr_("Signed") : tr_("Unsigned")) + sep + regions);

    // Contextual help
    QString hint;
    switch (categoryOf(mode))
    {
        case 0:
            hint = tr_("Search, change the value in the game, then search again until only a few addresses remain.");
            break;
        case 1:
            hint = tr_("No need to know the real value: each address is compared with what it held at the previous search.");
            break;
        default:
            hint = tr_("A life bar going from full to 60 % lost 40 %: enter 40. The tolerance absorbs rounding.");
            break;
    }
    if (relativeFirstPass)
        hint = tr_("Take a snapshot now, close this window, play until the value changes, then reopen it and search.");
    lHint->setText(hint);

    // Step indicator: search -> narrow down -> add cheat
    int step = 0;
    if (scanned)
        step = (candidates >= 1 && candidates <= FewCandidates) ? 2 : 1;
    updateSteps(step);
}

void UICheatSearch::onCompareChanged()
{
    updateUiState();
}

void UICheatSearch::on_tbAdvanced_toggled(bool checked)
{
    wAdvanced->setVisible(checked);
    tbAdvanced->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
}

void UICheatSearch::on_twSearchResults_itemSelectionChanged()
{
    pbAddCheat->setEnabled(twSearchResults->selectedItems().count() > 0);
}

void UICheatSearch::on_twSearchResults_itemDoubleClicked(QTreeWidgetItem *item, int)
{
    if (item)
        on_pbAddCheat_clicked();
}

void UICheatSearch::on_leSearchValue_textChanged(const QString &)
{
    updateUiState();
}

void UICheatSearch::on_rbUnsigned_toggled(bool checked)
{
    if (!checked) return;
    adjustSearchValueQValidator();
    listResults();
    updateUiState();
}

void UICheatSearch::on_rbSigned_toggled(bool checked)
{
    if (!checked) return;
    adjustSearchValueQValidator();
    listResults();
    updateUiState();
}

void UICheatSearch::on_rb8Bit_toggled(bool checked) { if (checked) { adjustSearchValueQValidator(); updateUiState(); } }
void UICheatSearch::on_rb16Bit_toggled(bool checked) { if (checked) { adjustSearchValueQValidator(); updateUiState(); } }
void UICheatSearch::on_rb32Bit_toggled(bool checked) { if (checked) { adjustSearchValueQValidator(); updateUiState(); } }

void UICheatSearch::on_pbRestart_clicked()
{
    search.clear();
    twSearchResults->clear();
    listResults();
    updateUiState();
}

void UICheatSearch::on_pbSearch_clicked()
{
    if (!LowWram || !HighWram)
        return;

    setSearchTypes();

    ScanParams p;
    p.mode = compareMode();
    p.size = searchType & 0x3;
    p.isSigned = rbSigned->isChecked();
    p.value = 0;
    p.percent = dsbPercent->value();
    p.tolerance = dsbTolerance->value();

    if (needsValue(p.mode))
    {
        bool ok = false;
        p.value = leSearchValue->text().toLongLong(&ok);

        qint64 minVal, maxVal;
        valueRange(p.size, p.isSigned, &minVal, &maxVal);
        if (!ok || p.value < minVal || p.value > maxVal)
        {
            lHint->setText(QString("<span style=\"color:#d13438\">%1</span>")
                           .arg(tr_("Value out of range for this data type (%1 to %2).").arg(minVal).arg(maxVal)));
            return;
        }
    }

    if (search.isEmpty())
    {
        if (cbHighWram->isChecked())
        {
            cheatsearch_struct hw;
            hw.startAddr = HighWramStart;
            hw.endAddr = HighWramEnd;
            search.append(hw);
        }
        if (cbLowWram->isChecked())
        {
            cheatsearch_struct lw;
            lw.startAddr = LowWramStart;
            lw.endAddr = LowWramEnd;
            search.append(lw);
        }
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int i = 0; i < search.count(); i++)
        runPass(search[i], p);
    QApplication::restoreOverrideCursor();

    savePercentSettings();
    listResults();
    updateUiState();
}

void UICheatSearch::on_pbAddCheat_clicked()
{
    QTreeWidgetItem *it = twSearchResults->currentItem();
    if (!it) return;

    UICheatRaw d(this);
    d.leAddress->setText(it->text(0).mid(2));   // drop the "0x" prefix used for display
    d.leValue->setText(QString::number(it->data(1, Qt::UserRole).toUInt(), 16).toUpper());
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
