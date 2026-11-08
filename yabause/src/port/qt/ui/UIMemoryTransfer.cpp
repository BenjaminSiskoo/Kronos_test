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
#include <QFile>
#include "UIMemoryTransfer.h"
#include "UIHexInput.h"
#include "../CommonDialogs.h"

#include <QPushButton>

UIMemoryTransfer::UIMemoryTransfer( YabauseThread *mYabauseThread, QWidget* p )
	: QDialog( p )
{
	// setup dialog
	setupUi( this );

   this->mYabauseThread = mYabauseThread;

   leStartAddress->setValidator(new HexValidator(0x00000000, 0xFFFFFFFF, leStartAddress));
   leEndAddress->setValidator(new HexValidator(0x00000000, 0xFFFFFFFF, leEndAddress));

   leStartAddress->setText("06004000");
   leEndAddress->setText("06100000");

   cbPC->setCheckState(Qt::Checked);
   rbUpload->setChecked(true);
   updateFileType();

	// retranslate widgets
	QtYabause::retranslateWidget( this );
}

bool UIMemoryTransfer::areSettingsValid()
{
   if (leFile->text().length() == 0)
      return false;
   if (rbUpload->isChecked() && MappedMemoryIsCpe(QFile::encodeName(leFile->text()).constData()))
      return true;
   if (leStartAddress->text().length() == 0)
      return false;

   bool result;
   u32 startAddress = leStartAddress->text().toUInt(&result, 16);
   if (!result)
      return false;

   if (rbDownload->isChecked())
   {
      if (leEndAddress->text().length() == 0)
         return false;

      u32 endAddress = leEndAddress->text().toUInt(&result, 16);
      if (!result)
         return false;

      if (startAddress >= endAddress)
         return false;
   }

   return true;
}

void UIMemoryTransfer::on_leFile_textChanged( const QString & text )
{
   updateFileType();
   dbbButtons->button(QDialogButtonBox::Ok)->setEnabled(areSettingsValid());
}

void UIMemoryTransfer::on_leStartAddress_textChanged( const QString & text )
{
   dbbButtons->button(QDialogButtonBox::Ok)->setEnabled(areSettingsValid());
}

void UIMemoryTransfer::on_leEndAddress_textChanged( const QString & text )
{
   dbbButtons->button(QDialogButtonBox::Ok)->setEnabled(areSettingsValid());
}

void UIMemoryTransfer::on_rbUpload_toggled(bool checked)
{
   updateFileType();

   dbbButtons->button(QDialogButtonBox::Ok)->setEnabled(areSettingsValid());
}

void UIMemoryTransfer::on_tbBrowse_clicked()
{
   if (rbDownload->isChecked())
   {
      const QString s = CommonDialogs::getSaveFileName( leFile->text(), QtYabause::translate( "Choose a location for binary file" ), QtYabause::translate( "Binary Files (*.bin)" ) );
      if ( !s.isNull() )
         leFile->setText( s );
   }
   else
   {
      /* Seuls les binaires bruts et les executables CPE (SN Systems) sont
         geres : les filtres COFF/ELF proposes jusqu'ici etaient charges comme
         des binaires bruts a l'adresse de depart, en-tetes compris. */
      const QString s = CommonDialogs::getOpenFileName( leFile->text(), QtYabause::translate( "Choose a binary or program file" ), QtYabause::translate( "Program Files (*.bin *.cpe);;Binary Files (*.bin);;SN Systems CPE Files (*.cpe);;All Files (*)" ) );
      if ( !s.isNull() )
         leFile->setText( s );
   }
}

/* Un CPE porte ses adresses de chargement et son point d'entree : l'adresse
   de depart ne sert pas. Reconnu a la signature "CPE" 01, pas a l'extension. */
void UIMemoryTransfer::updateFileType()
{
   const bool upload = rbUpload->isChecked();
   const bool cpe = upload && MappedMemoryIsCpe( QFile::encodeName( leFile->text() ).constData() );

   leStartAddress->setEnabled( !cpe );
   leEndAddress->setEnabled( !upload );
   cbPC->setEnabled( upload );
   cbPC->setText( cpe ? QtYabause::translate( "Run program (reset, PC = CPE entry point)" )
                      : QtYabause::translate( "Run program (reset, PC = Start Address)" ) );
}

void UIMemoryTransfer::accept()
{
   u32 startAddress = leStartAddress->text().toUInt(0, 16);
   u32 endAddress = leEndAddress->text().toUInt(0, 16);

   if (rbDownload->isChecked() && startAddress >= endAddress)
   {
      CommonDialogs::error(QtYabause::translate("Invalid Start/End Address Combination"), QtYabause::translate("Error"));
      return;
   }

   /* MSH2 n'existe qu'apres YabauseInit() : sans emulation demarree, l'appel
      dereferencait un pointeur nul. */
   if (mYabauseThread->init() < 0 || MSH2 == NULL)
   {
      CommonDialogs::error(QtYabause::translate("Start the emulation first."), QtYabause::translate("Error"));
      return;
   }

	if (mYabauseThread)
      mYabauseThread->pauseEmulation( false, false );

   if (rbDownload->isChecked())
   {
      // Let's do a ram dump
      MappedMemorySave(MSH2, QFile::encodeName(leFile->text()).constData(), startAddress, endAddress - startAddress);
   }
   else
   {
      /* Envoi en memoire, et lancement si la case est cochee. La case
         "Set PC to Start Address" n'etait jamais lue : le PC ne changeait
         pas. */
      const QByteArray file = QFile::encodeName(leFile->text());
      const int run = (cbPC->isEnabled() && cbPC->isChecked()) ? 1 : 0;
      u32 entry = 0;
      const int ret = MappedMemoryLoadExec(MSH2, file.constData(), startAddress, run, &entry);

      if (ret != 0)
      {
         QString msg;
         switch (ret)
         {
            case -2: msg = QtYabause::translate("Not enough memory to load the file."); break;
            case -3: msg = QtYabause::translate("Invalid or truncated CPE file."); break;
            case -4: msg = QtYabause::translate("This CPE file has no entry point: uncheck the run option to only load it."); break;
            default: msg = QtYabause::translate("Can't read the file."); break;
         }
         CommonDialogs::error(msg, QtYabause::translate("Error"));
         return;
      }
   }

	QDialog::accept();
}
