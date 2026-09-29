/*	Copyright 2008 Filipe Azevedo <pasnox@gmail.com>

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
#include "UIBackupRam.h"
#include "../CommonDialogs.h"
#include "../QtYabause.h"

#include <QTextCodec>
#include <stdlib.h>
#include <string.h>

u32 currentbupdevice = 0;
deviceinfo_struct* devices = NULL;
int numbupdevices = 0;
saveinfo_struct* saves = NULL;
int numsaves = 0;

/*
	Backup RAM strings (file name : 11 bytes, comment : 10 bytes) are raw
	1-byte character codes, not UTF-8.
	Japanese titles store their names with the JIS X 0201 set used by the
	Saturn BIOS memory manager : ASCII (0x20-0x7E) + half-width katakana
	(0xA1-0xDF, dakuten 0xDE / handakuten 0xDF written as separate bytes).
	A few titles may also contain Shift-JIS double-byte characters.

	Passing these bytes directly to QString (implicit fromUtf8) produced
	invalid UTF-8 sequences, so Japanese names were not displayed.

	Decoding :
	- 0x20-0x7E            -> ASCII
	- 0xA1-0xDF            -> half-width katakana U+FF61-U+FF9F
	- Shift-JIS lead+trail -> decoded with the Shift-JIS codec (if available)
	- anything else        -> space
	Then NFKC normalization turns half-width katakana into full-width and
	merges dakuten / handakuten (ｶﾞ -> ガ, ﾊﾟ -> パ, ｰ -> ー).
*/
static bool isSjisLeadByte( u8 c )
{ return ( c >= 0x81 && c <= 0x9F ) || ( c >= 0xE0 && c <= 0xFC ); }

static bool isSjisTrailByte( u8 c )
{ return ( c >= 0x40 && c <= 0x7E ) || ( c >= 0x80 && c <= 0xFC ); }

static QString saturnStringToQString( const char* data, int maxLength )
{
	static QTextCodec* sjisCodec = QTextCodec::codecForName( "Shift-JIS" );
	QString result;
	int length = 0;

	// the field is fixed size, stop at the first NUL
	while ( length < maxLength && data[length] != '\0' )
		length++;

	for ( int i = 0; i < length; i++ )
	{
		const u8 c = (u8)data[i];

		if ( c >= 0x20 && c <= 0x7E )
			result.append( QChar( (ushort)c ) );
		else if ( c >= 0xA1 && c <= 0xDF )
			result.append( QChar( (ushort)( 0xFF61 + ( c - 0xA1 ) ) ) );
		else if ( sjisCodec && isSjisLeadByte( c ) && ( i + 1 ) < length && isSjisTrailByte( (u8)data[i + 1] ) )
		{
			QTextCodec::ConverterState state( QTextCodec::ConvertInvalidToNull );
			const QString ch = sjisCodec->toUnicode( data + i, 2, &state );
			if ( state.invalidChars == 0 && state.remainingChars == 0 && !ch.isEmpty() )
			{
				result.append( ch );
				i++;
			}
			else
				result.append( QLatin1Char( ' ' ) );
		}
		else
			result.append( QLatin1Char( ' ' ) );
	}

	return result.normalized( QString::NormalizationForm_KC ).trimmed();
}

UIBackupRam::UIBackupRam( QWidget* p )
	: QDialog( p )
{
	//setup dialog
	setupUi( this );
	if ( p && !p->isFullScreen() )
		setWindowFlags( Qt::Sheet );

	// release the device list of a previous dialog (allocated by BupGetDeviceList)
	if ( devices )
	{
		free( devices );
		devices = NULL;
	}
	numbupdevices = 0;

	// get available devices
	if ( ( devices = BupGetDeviceList( &numbupdevices ) ) == NULL )
		return;

	// add to combobox
	for ( int i = 0; i < numbupdevices; i++ )
		cbDeviceList->addItem( devices[i].name, devices[i].id );

	// get save list for current devices
	refreshSaveList();

	// retranslate widgets
	QtYabause::retranslateWidget( this );
}

void UIBackupRam::refreshSaveList()
{
	// blocks
	u32 fs = 0, ms = 0;
	u32 id = cbDeviceList->itemData( cbDeviceList->currentIndex() ).toInt();

	// clear listwidget
	lwSaveList->clear();

	// release previous save list (allocated by BupGetSaveList)
	if ( saves )
	{
		free( saves );
		saves = NULL;
	}
	numsaves = 0;

	// get save list
	saves = BupGetSaveList(id, &numsaves);
	if ( !saves )
		numsaves = 0;

	// add item to listwidget
	for ( int i = 0; i < numsaves; i++ )
	{
		const QString fileName = saturnStringToQString( saves[i].filename, 11 );
		const QString comment = saturnStringToQString( saves[i].comment, 10 );
		const QString text = comment.isEmpty() ? fileName : QString( "%1 - %2" ).arg( fileName, comment );

		QListWidgetItem* item = new QListWidgetItem( text, lwSaveList );
		item->setToolTip( text );
		// keep the index of the save : the raw file name is used for deletion
		item->setData( Qt::UserRole, i );
	}

	// set infos about blocks
	BupGetStats(id, &fs, &ms );
	lBlocks->setText( QtYabause::translate( "%1/%2 blocks free" ).arg( fs ).arg( ms ) );

	// enable/disable button delete according to available item
	pbDelete->setEnabled( lwSaveList->count() );

	// select first item in the item list
	if ( lwSaveList->count() )
		lwSaveList->setCurrentRow( 0 );
	on_lwSaveList_itemSelectionChanged();
}

void UIBackupRam::on_cbDeviceList_currentIndexChanged( int )
{ refreshSaveList(); }

void UIBackupRam::on_lwSaveList_itemSelectionChanged()
{
	// get current save id
	int id = lwSaveList->currentRow();

	// update gui
	if ( id != -1 && saves && id < numsaves )
	{
		leFileName->setText( saturnStringToQString( saves[id].filename, 11 ) );
		leComment->setText( saturnStringToQString( saves[id].comment, 10 ) );
		switch ( saves[id].language )
		{
			case 0:
				leLanguage->setText( QtYabause::translate( "Japanese" ) );
				break;
			case 1:
				leLanguage->setText( QtYabause::translate( "English" ) );
				break;
			case 2:
				leLanguage->setText( QtYabause::translate( "French" ) );
				break;
			case 3:
				leLanguage->setText( QtYabause::translate( "German" ) );
				break;
			case 4:
				leLanguage->setText( QtYabause::translate( "Spanish" ) );
				break;
			case 5:
				leLanguage->setText( QtYabause::translate( "Italian" ) );
				break;
			default:
				leLanguage->setText( QtYabause::translate( "Unknown (%1)" ).arg( saves[id].language ) );
				break;
		}
		leDataSize->setText( QString::number( saves[id].datasize ) );
		leBlockSize->setText( QString::number( saves[id].blocksize ) );
	}
	else
	{
		// clear gui
		leFileName->clear();
		leComment->clear();
		leLanguage->clear();
		leDataSize->clear();
		leBlockSize->clear();
	}
}

void UIBackupRam::on_pbDelete_clicked()
{
	if ( QListWidgetItem* it = lwSaveList->selectedItems().value( 0 ) )
	{
		const int saveIndex = it->data( Qt::UserRole ).toInt();
		if ( !saves || saveIndex < 0 || saveIndex >= numsaves )
			return;

		u32 id = cbDeviceList->itemData( cbDeviceList->currentIndex() ).toInt();
		if ( CommonDialogs::question( QtYabause::translate( "Are you sure you want to delete '%1' ?" ).arg( it->text() ) ) )
		{
			// use the raw file name bytes, not the decoded display text
			char rawName[12];
			memcpy( rawName, saves[saveIndex].filename, sizeof( rawName ) );
			rawName[11] = '\0';
			BupDeleteSave(id, rawName );
			refreshSaveList();
		}
	}
}

void UIBackupRam::on_pbFormat_clicked()
{
	u32 id = cbDeviceList->itemData( cbDeviceList->currentIndex() ).toInt();
	if ( CommonDialogs::question( QtYabause::translate( "Are you sure you want to format '%1' ?" ).arg( devices[id].name ) ) )
	{
		BupFormat( id );
		refreshSaveList();
	}
}
