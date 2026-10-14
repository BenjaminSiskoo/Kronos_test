#ifndef SH2INT_KRONOS_H
#define SH2INT_KRONOS_H

#define SH2CORE_KRONOS_INTERPRETER 8
#define SH2CORE_KRONOS_DEBUG_INTERPRETER 9

typedef void (FASTCALL *opcode_func)(SH2_struct *);
typedef u16 (FASTCALL *fetchfunc)(SH2_struct *context, u32);

extern SH2Interface_struct SH2KronosInterpreter;

extern fetchfunc krfetchlist[0x1000];
extern opcode_func opcodeTable[0x10000];

/* Apres YabLoadStateStream() : invalide le cache de decodage et repose les
 * crochets (BUP, retour d'interruption) qui ne sont pas dans l'etat. */
void SH2KronosInterpreterPostLoadState(SH2_struct *context);


#endif
