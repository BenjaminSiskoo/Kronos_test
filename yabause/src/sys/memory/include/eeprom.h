#ifndef __EEPROM__H__
#define __EEPROM__H__

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void eeprom_set_clk(u8 state);
extern void eeprom_set_di(u8 state);
extern void eeprom_set_cs(u8 state);
extern int eeprom_do_read();
extern void eeprom_init(const char* filename);
extern void eeprom_deinit();
extern void eeprom_start(const u8* table);
/* Save states: "EEPR" block of the CART chunk (cs0.c); load returns 0 when applied */
extern void eeprom_save_state(void ** stream);
extern int eeprom_load_state(const void * stream, int size);

#ifdef __cplusplus
}
#endif

#endif

