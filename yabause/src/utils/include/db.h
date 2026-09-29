#ifndef __DB_INCLUDE_H__
#define __DB_INCLUDE_H__

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

void DBLookup(int* const cart_type, const char**  cartpath, const char * support_dir);
int DBLookupForceSH2Cache(void);
int DBLookupLegacySH2FetchTiming(void);


typedef struct GameDB_s{
  const char* game_code;
  const u64 game_id;
  int cart_type;
  const char* filename;
} GameDB;

#ifdef __cplusplus
}
#endif

#endif
