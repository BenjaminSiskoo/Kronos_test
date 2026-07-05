/*  Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

    This file is part of Yabause. (GPL v2+)
*/
#ifndef UIDEBUGSCUDSP_H
#define UIDEBUGSCUDSP_H

#include "UIDebugCPU.h"
#include "../QtYabause.h"
#include <QTabWidget>
#include <QPlainTextEdit>

class UIDebugSCUDSP : public UIDebugCPU
{
    Q_OBJECT

public:
    explicit UIDebugSCUDSP(YabauseThread *mYabauseThread, QWidget *parent = 0);

    // UIDebugCPU interface — override only where base is virtual
    void updateRegList();
    void updateCodeList(u32 addr);
    void updateAll();           // not virtual in UIDebugCPU base
    u32  getRegister(int index, int *size);
    void setRegister(int index, u32 value);
    bool addCodeBreakpoint(u32 addr);
    bool delCodeBreakpoint(u32 addr);
    void stepInto();

    // Save functions (bound to pbReserved1-5)
    void reserved1();   // Save Program RAM
    void reserved2();   // Save MD0
    void reserved3();   // Save MD1
    void reserved4();   // Save MD2
    void reserved5();   // Save MD3

private:
    QString decodeDMAChannel(u32 R, u32 W, u32 C, u32 AD, u32 EN, u32 MD, int ch) const;
    QString decodeInterruptMask(u32 IMS, u32 IST) const;
    QString decodeTimers() const;
    QString formatMDBank(int bank, const u32 *md) const;

    QTabWidget     *m_tabExtra;
    QPlainTextEdit *m_pteDMA;
    QPlainTextEdit *m_pteIRQ;
    QPlainTextEdit *m_pteMD;
    QPlainTextEdit *m_pteTimers;

protected slots:
    void onTabChanged(int idx);
};

#endif // UIDEBUGSCUDSP_H
