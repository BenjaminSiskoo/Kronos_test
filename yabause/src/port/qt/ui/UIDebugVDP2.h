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

#ifndef UIDEBUGVDP2_H
#define UIDEBUGVDP2_H

#include "ui_UIDebugVDP2.h"
#include "UIDebugVDP2Viewer.h"
#include "../QtYabause.h"
#include "UIYabause.h"

class UIDebugVDP2 : public QDialog, public Ui::UIDebugVDP2
{
    Q_OBJECT
public:
    explicit UIDebugVDP2(QWidget* parent = 0, YabauseLocker *lock = 0);
    ~UIDebugVDP2();

protected:
    bool updateInfoDisplay(void (*debugStats)(char *, int *), QGroupBox *cb, QPlainTextEdit *pte);
    // viewer déclaré avant mLock : Qt le détruira en premier (ordre RAII correct)
    UIDebugVDP2Viewer *viewer;

protected slots:
    void on_pbViewer_clicked();
    void on_pbNextButton_clicked();

private:
    // const : le pointeur n'est jamais réassigné après le constructeur
    YabauseLocker* const mLock;
    void updateScreenInfos();
};

#endif // UIDEBUGVDP2_H
