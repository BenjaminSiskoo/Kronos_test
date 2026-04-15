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
#include "UICheats.h"
#include "UICheatAR.h"
#include "UICheatRaw.h"
#include "../CommonDialogs.h"

UICheats::UICheats( QWidget* p )
    : QDialog( p )
{
    setupUi( this );
    if ( p && !p->isFullScreen() )
        setWindowFlags( Qt::Sheet );

    int cheatsCount = 0;
    mCheats = CheatGetList( &cheatsCount );

    for ( int id = 0; id < cheatsCount; id++ )
        addCode( id );

    pbSaveFile->setEnabled( cheatsCount > 0 );
    
    QtYabause::retranslateWidget( this );
}

void UICheats::addCode( int id )
{
    QString s;
    switch ( mCheats[id].type )
    {
        case CHEATTYPE_ENABLE:
            s = QtYabause::translate( "Enable Code : %1 %2" ).arg( (int)mCheats[id].addr, 8, 16, QChar( '0' ) ).arg( (int)mCheats[id].val, 8, 16, QChar( '0' ) );
            break;
        case CHEATTYPE_BYTEWRITE:
            s = QtYabause::translate( "Byte Write : %1 %2" ).arg( (int)mCheats[id].addr, 8, 16, QChar( '0' ) ).arg( (int)mCheats[id].val, 2, 16, QChar( '0' ) );
            break;
        case CHEATTYPE_WORDWRITE:
            s = QtYabause::translate( "Word Write : %1 %2" ).arg( (int)mCheats[id].addr, 8, 16, QChar( '0' ) ).arg( (int)mCheats[id].val, 4, 16, QChar( '0' ) );
            break;
        case CHEATTYPE_LONGWRITE:
            s = QtYabause::translate( "Long Write : %1 %2" ).arg( (int)mCheats[id].addr, 8, 16, QChar( '0' ) ).arg( (int)mCheats[id].val, 8, 16, QChar( '0' ) );
            break;
        default: break;
    }

    QTreeWidgetItem* it = new QTreeWidgetItem( twCheats );
    it->setText( 0, s );
    it->setText( 1, mCheats[id].desc );
    
    // AMÉLIORATION : Coloration du statut
    if ( mCheats[id].enable ) {
        it->setText( 2, QtYabause::translate( "Enabled" ) );
        it->setForeground( 2, Qt::darkGreen );
    } else {
        it->setText( 2, QtYabause::translate( "Disabled" ) );
        it->setForeground( 2, Qt::gray );
    }

    pbClear->setEnabled( true );
    pbSaveFile->setEnabled( true );
}

void UICheats::addARCode( const QString& c, const QString& d )
{
    if ( CheatAddARCode( c.toLatin1().constData() ) != 0 )
    {
        CommonDialogs::error( QtYabause::translate( "Unable to add code" ) );
        return;
    }

    int cheatsCount;
    mCheats = CheatGetList( &cheatsCount );
    CheatChangeDescriptionByIndex( cheatsCount - 1, d.toLatin1().data() );
    addCode( cheatsCount - 1 );
}

void UICheats::addRawCode( int t, const QString& a, const QString& v, const QString& d )
{
    bool bAddr, bVal;
    u32 addr = a.toUInt( &bAddr, 16 );
    u32 val = v.toUInt( &bVal, 16 );

    if ( !bAddr || !bVal )
    {
        CommonDialogs::error( QtYabause::translate( "Invalid Address or Value" ) );
        return;
    }

    if ( CheatAddCode( t, addr, val ) != 0 )
    {
        CommonDialogs::error( QtYabause::translate( "Unable to add code" ) );
        return;
    }

    int cheatsCount;
    mCheats = CheatGetList( &cheatsCount );
    CheatChangeDescriptionByIndex( cheatsCount - 1, d.toLatin1().data() );
    addCode( cheatsCount - 1 );
}

void UICheats::on_twCheats_itemSelectionChanged()
{ 
    pbDelete->setEnabled( twCheats->selectedItems().count() > 0 ); 
}

void UICheats::on_twCheats_itemDoubleClicked( QTreeWidgetItem* it, int )
{
    if ( !it ) return;
    int id = twCheats->indexOfTopLevelItem( it );
    if ( id == -1 ) return;

    if ( mCheats[id].enable )
        CheatDisableCode( id );
    else
        CheatEnableCode( id );

    // Mise à jour visuelle immédiate
    it->setText( 2, mCheats[id].enable ? QtYabause::translate( "Enabled" ) : QtYabause::translate( "Disabled" ) );
    it->setForeground( 2, mCheats[id].enable ? Qt::darkGreen : Qt::gray );
}

void UICheats::on_pbDelete_clicked()
{
    QTreeWidgetItem* it = twCheats->currentItem();
    if ( it )
    {
        int id = twCheats->indexOfTopLevelItem( it );
        if ( CheatRemoveCodeByIndex( id ) == 0 )
        {
            delete it;
            pbClear->setEnabled( twCheats->topLevelItemCount() > 0 );
            pbSaveFile->setEnabled( twCheats->topLevelItemCount() > 0 );
        }
    }
}

void UICheats::on_pbClear_clicked()
{
    CheatClearCodes();
    twCheats->clear();
    pbDelete->setEnabled( false );
    pbClear->setEnabled( false );
    pbSaveFile->setEnabled( false );
}

void UICheats::on_pbAR_clicked()
{
    UICheatAR d( this );
    if ( d.exec() )
        addARCode( d.leCode->text(), d.teDescription->toPlainText() );
}

void UICheats::on_pbRaw_clicked()
{
    UICheatRaw d( this );
    if ( d.exec() && d.type() != -1 )
        addRawCode( d.type(), d.leAddress->text(), d.leValue->text(), d.teDescription->toPlainText() );
}

void UICheats::on_pbSaveFile_clicked()
{
    const QString s = CommonDialogs::getSaveFileName( ".", QtYabause::translate( "Choose a cheat file to save to" ), QtYabause::translate( "Kronos Cheat Files (*.yct);;All Files (*)" ) );
    if ( !s.isEmpty() )
        if ( CheatSave( s.toLatin1().constData() ) != 0 )
            CommonDialogs::error( QtYabause::translate( "Unable to save file" ) );
}

void UICheats::on_pbLoadFile_clicked()
{
    const QString s = CommonDialogs::getOpenFileName( ".", QtYabause::translate( "Choose a cheat file to open" ), QtYabause::translate( "Kronos Cheat Files (*.yct);;All Files (*)" ) );
    if ( !s.isEmpty() && CheatLoad( s.toLatin1().constData() ) == 0 )
    {
        twCheats->clear();
        int cheatsCount;
        mCheats = CheatGetList( &cheatsCount );
        for ( int i = 0; i < cheatsCount; i++ )
            addCode( i );
    }
}
