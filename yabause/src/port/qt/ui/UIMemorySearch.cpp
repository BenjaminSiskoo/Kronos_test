/*	Copyright 2013 Theo Berkau <cwx@cyberwarriorx.com>

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

#include "UIMemorySearch.h"
#include "UIHexInput.h"
#include "../CommonDialogs.h"
#include <QPushButton>

namespace {

// Each change of search type installs a new validator on the value field.
// Since HexValidator now forwards its parent, every validator is owned by
// the line edit, but the replaced ones stayed alive as its children until
// the dialog closed. Delete the previous one as soon as it is replaced.
void setValueValidator(QLineEdit *le, QValidator *v)
{
   const QValidator *old = le->validator();
   le->setValidator(v);
   if (old && old != v && old->parent() == le)
      delete old;
}

}

UIMemorySearch::UIMemorySearch( QWidget* p )
	: QDialog( p )
{
	// set up dialog
	setupUi( this );
	if ( p && !p->isFullScreen() )
		setWindowFlags( Qt::Sheet );

   QStringList typeList = (QStringList() << 
                           QtYabause::translate("Hex value(s)") << 
                           QtYabause::translate("Text") << 
                           QtYabause::translate("8-bit Relative value(s)") << 
                           QtYabause::translate("16-bit Relative value(s)") << 
                           QtYabause::translate("Unsigned 8-bit value") << 
                           QtYabause::translate("Signed 8-bit value") <<
                           QtYabause::translate("Unsigned 16-bit value") <<
                           QtYabause::translate("Signed 16-bit value") <<
                           QtYabause::translate("Unsigned 32-bit value") <<
                           QtYabause::translate("Signed 32-bit value"));
   QList<int> flagList;
   flagList << SEARCHHEX << SEARCHSTRING << SEARCHREL8BIT << SEARCHREL16BIT << 
               (SEARCHUNSIGNED|SEARCHBYTE) << (SEARCHSIGNED|SEARCHBYTE) <<
               (SEARCHUNSIGNED|SEARCHWORD) << (SEARCHSIGNED|SEARCHWORD) <<
               (SEARCHUNSIGNED|SEARCHLONG) << (SEARCHSIGNED|SEARCHLONG);

   cbType->addItems(typeList);

   for (int i = 0; i < flagList.count(); i++)
      cbType->setItemData(i, flagList[i]);

   adjustSearchValueQValidator();

   leStartAddress->setValidator(new HexValidator(0x00000000, 0xFFFFFFFF, leStartAddress));
   leEndAddress->setValidator(new HexValidator(0x00000000, 0xFFFFFFFF, leEndAddress));

	(dbbButtons->button(QDialogButtonBox::Ok))->setEnabled(!(leValue->text()).isEmpty());

	// retranslate widgets
	QtYabause::retranslateWidget( this );
}

void UIMemorySearch::setParameters(int type, QString string, u32 startAddress, u32 endAddress)
{
   int index = cbType->findData(type);
   if (index != -1)
      cbType->setCurrentIndex(index);
   leValue->setText(string);
   leStartAddress->setText(QString("%1").arg(startAddress, 8, 16, QChar('0')).toUpper());
   leEndAddress->setText(QString("%1").arg(endAddress, 8, 16, QChar('0')).toUpper());
}

void UIMemorySearch::adjustSearchValueQValidator()
{
   // Figure out range
   // 64-bit so that the 32-bit range fits: in an int, 0xFFFFFFFF became -1
   // and the signed 32-bit bounds came out as min = 0, max = -1.
   qint64 min, max;

   min = 0;

   int data = cbType->itemData(cbType->currentIndex()).toInt();

   switch (data)
   {
      case SEARCHHEX:
         setValueValidator(leValue, new HexValidator(0x00000000, 0xFFFFFFFF, leValue));
         leValue->setText("");
         break;
      case SEARCHSTRING:
         setValueValidator(leValue, NULL);
         leValue->setText("");
         break;
      case SEARCHREL8BIT:
         setValueValidator(leValue, new QRegExpValidator(QRegExp("([0-9a-fA-F]{1,2})(,\\s*[0-9a-fA-F]{1,2})*"), leValue));
         leValue->setText("");
         break;
      case SEARCHREL16BIT:
         setValueValidator(leValue, new QRegExpValidator(QRegExp("([0-9a-fA-F]{1,4})(,\\s*[0-9a-fA-F]{1,4})*"), leValue));
         leValue->setText("");
         break;
      default: 
      {
         bool isSigned = data & SEARCHSIGNED;
         data &= ~SEARCHSIGNED;

         if (data == SEARCHBYTE)
            max = 0xFF;
         else if (data == SEARCHWORD)
            max = 0xFFFF;
         else if (data == SEARCHLONG)
            max = 0xFFFFFFFFLL;
			else
				max = 0xFF;

         if (isSigned)
         {
            min = -(max >> 1) - 1;
            max >>= 1;
         }

         if (data == SEARCHLONG)
         {
            if (isSigned)
               setValueValidator(leValue, new QRegExpValidator(QRegExp("-?\\d{1,10}"), leValue));
            else
               setValueValidator(leValue, new QRegExpValidator(QRegExp("\\d{1,10}"), leValue));
         }
         else
            // Only 8 and 16-bit values reach this point (32-bit ones use the
            // regular expressions above), so both bounds fit in an int.
            setValueValidator(leValue, new QIntValidator(static_cast<int>(min), static_cast<int>(max), leValue));
         leValue->setText("");
         break;
      }
   }
}

void UIMemorySearch::on_leValue_textChanged( const QString & text )
{
	(dbbButtons->button(QDialogButtonBox::Ok))->setEnabled(!text.isEmpty());
}

void UIMemorySearch::accept()
{
   QString value = leValue->text();
   // The hex validators accept an empty field, and toUInt() then returns 0:
   // report the field that is wrong instead of a misleading range error.
   bool startOk = false;
   bool endOk = false;
   u32 startAddress = leStartAddress->text().trimmed().toUInt(&startOk, 16);
   u32 endAddress = leEndAddress->text().trimmed().toUInt(&endOk, 16);

   if (!startOk)
   {
      CommonDialogs::error(QtYabause::translate("Invalid Start Address"), QtYabause::translate("Error"));
      leStartAddress->setFocus();
      return;
   }

   if (!endOk)
   {
      CommonDialogs::error(QtYabause::translate("Invalid End Address"), QtYabause::translate("Error"));
      leEndAddress->setFocus();
      return;
   }

   if (startAddress >= endAddress)
   {
      CommonDialogs::error(QtYabause::translate("Invalid Start/End Address Combination"), QtYabause::translate("Error"));
      return;
   }

   QDialog::accept();
}

void UIMemorySearch::on_cbType_currentIndexChanged(int index)
{
   adjustSearchValueQValidator();
}
