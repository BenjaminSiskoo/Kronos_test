#ifndef __DECRYPT_STV_H__
#define __DECRYPT_STV_H__

extern u16 cryptoDecrypt();
extern void cryptoReset();
extern void cryptoSetKey(u32 privKey);
extern void cyptoSetLowAddr(u16 val);
extern void cyptoSetHighAddr(u16 val);
extern void cyptoSetSubkey(u16 subKey);

/* 315-5838 / 317-0229 compression+encryption device (Decathlete).
 * Ported from MAME's src/mame/sega/315-5838_317-0229_comp.cpp
 * (David Haywood, Samuel Neves, Peter Wilhelmsen, Morten Shearman Kirkegaard).
 * Source data is read straight from the cartridge ROM (CartridgeArea->rom),
 * bank-switched over 3 possible 8MB windows selected by the address bits
 * used in decathlt5838SetSrcAddr(). */
extern void decathlt5838Reset(void);
extern void decathlt5838SetBank(u32 addr);                /* call on every write in the 24MB window, see cs0.c */
extern void decathlt5838SetSrcAddr(u32 val, u32 mem_mask);   /* srcaddr_w equivalent, mem_mask like COMBINE_DATA */
extern void decathlt5838SetTableUploadMode(u16 val);      /* upper half of data_w: mode select, resets cursor */
extern void decathlt5838UploadTableData(u16 val);         /* lower half of data_w: payload, cursor auto-increments */
extern u16 decathlt5838DataRead(void);                    /* data_r equivalent (16-bit half) */

#endif
