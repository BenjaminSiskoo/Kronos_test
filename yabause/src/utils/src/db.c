#include <sys/stat.h>

#include "cs0.h"
#include "cs2.h"
#include "db.h"

#define NB_GAMES_DB 31

static GameDB GameDBList[NB_GAMES_DB] = {
   // note : SNK games were developped for 1MB cart,
   // several of them are known for having issues with 4MB,
   // let's play it safe by only using 1MB on those
   { "T-3105G", 0, CART_DRAM8MBIT, NULL }, // Real Bout Garou Densetsu
   { "T-3119G", 0, CART_DRAM8MBIT, NULL }, // Real Bout Garou Densetsu Special
   { "T-3116G", 0, CART_DRAM8MBIT, NULL }, // Samurai Spirits - Amakusa Kourin
   { "T-3104G", 0, CART_DRAM8MBIT, NULL }, // Samurai Spirits - Zankurou Musouken
   { "T-3108G", 0, CART_DRAM8MBIT, NULL }, // The King of Fighters '96
   { "T-3121G", 0, CART_DRAM8MBIT, NULL }, // The King of Fighters '97
   { "T-1515G", 0, CART_DRAM8MBIT, NULL }, // Waku Waku 7
   { "T-3111G", 0, CART_DRAM8MBIT, NULL }, // Metal Slug
   { "GS-9107", 0, CART_DRAM8MBIT, NULL }, // Fighter's History Dynamite
   // other games should be fine with 4MB cart
   { "T-1521G", 0, CART_DRAM32MBIT, NULL }, // Astra Superstars
   { "T-9904G", 0, CART_DRAM32MBIT, NULL }, // Magical Night Dreams - Cotton 2
   { "T-1217G", 0, CART_DRAM32MBIT, NULL }, // Cyberbots - Fullmetal Madness
   { "T-1245G", 0, CART_DRAM32MBIT, NULL }, // Dungeons & Dragons Collection - Shadow over Mystara
   { "T-1248G", 0, CART_DRAM32MBIT, NULL }, // Final Fight Revenge
   { "T-20109G", 0, CART_DRAM32MBIT, NULL }, // Friends: Seishun no Kagayaki
   { "T-14411G", 0, CART_DRAM32MBIT, NULL }, // Groove On Fight: Gouketsuji Ichizoku 3
   { "T-7032H-50VV1.000", 0, CART_DRAM32MBIT, NULL }, // Marvel Super Heroes
   { "T-1214H", 0, CART_DRAM32MBIT, NULL }, // Marvel Super Heroes
   { "T-1521G", 0, CART_DRAM32MBIT, NULL }, // Marvel Super Heroes
   { "T-1238G", 0, CART_DRAM32MBIT, NULL }, // Marvel Super Heroes vs. Street Fighter
   { "T-22205G", 0, CART_DRAM32MBIT, NULL }, // Noël 3
   { "T-20114G", 0, CART_DRAM32MBIT, NULL }, // Pia Carrot e Youkoso!! 2
   { "T-1230G", 0, CART_DRAM32MBIT, NULL }, // Pocket Fighter
   { "T-1246G", 0, CART_DRAM32MBIT, NULL }, // Street Fighter Alpha 3
   { "T-16510G", 0, CART_DRAM32MBIT, NULL }, // Super Real Mahjong P7
   { "T-1229G", 0, CART_DRAM32MBIT, NULL }, // Vampire Savior
   { "T-1226G", 0, CART_DRAM32MBIT, NULL }, // X-Men vs. Street Fighter
   // the following rom filenames are based on MAME, themselves based on stickers/labels on the cart's chip
   { "MK-81088", 0, CART_ROM16MBIT, "mpr-18811-mx.ic1" }, // The King of Fighters '95
   { "T-3101G", 0, CART_ROM16MBIT, "mpr-18811-mx.ic1" }, // The King of Fighters '95
   { "T-13308G", 0, CART_ROM16MBIT, "mpr-19367-mx.ic1" }, // Ultraman: Hikari no Kyojin Densetsu
   // this game is a prototype without game_code
   { NULL, 0x3939393939393939, CART_DRAM128MBIT, NULL } // Heart of Darkness
};

/* Jeux qui ont besoin de l'emulation du cache SH-2 : elle est activee pour
   eux meme si l'option est desactivee (voir DBLookupForceSH2Cache()).

   Sur la console le cache des deux SH-2 existe toujours ; sans son
   emulation, Kronos applique un modele de temps simplifie (chaque lecture
   d'instruction en Work RAM se paie) et ne reproduit ni les donnees perimees
   en cache ni les durees relatives maitre/esclave dont certains jeux
   dependent.

   Sources :
   - verifie dans Kronos : Tennis Arena ;


   Seul Tennis Arena a ete teste dans Kronos. Un jeu de cette liste qui
   regresserait avec le cache emule doit en etre retire. */
static const char * const SH2CacheDBList[] = {
   // verifie dans Kronos
   "T-17703G",   // Tennis Arena (Japan) -- ecran noir apres le BIOS
   NULL
};

/* Jeux qui gardent l'ancien cout du fetch d'instruction sans emulation du
   cache (voir SH2FetchWord() dans memory.c) : chaque fetch en Work RAM paie
   un changement de rangee DRAM, rangee partagee avec les donnees.

   Space Jam : l'ordonnanceur de taches du jeu (06020242) lit EXTEN puis
   VCNT au debut de chaque trame et en deduit le temps deja ecoule
   (VCNT x 229 ticks du FRT par ligne, 06020258) ; une tache de chargement
   (06013674) ne travaille que si ce temps reste sous un budget d'environ
   196 lignes (06013712). Le maitre y arrive depuis le V-Blank OUT par un
   chemin d'environ 700 instructions (trois interruptions, lecture des
   manettes au SMPC). Sur la console ce chemin depasse une ligne et VCNT est
   lu a 0. Dans Kronos, ou les acces aux registres SCU/VDP/SMPC, l'entree en
   interruption et les ecritures ne coutent presque rien, il ne tient plus
   qu'avec l'ancien cout du fetch : sinon VCNT est lu a 312, le budget est
   depasse, les taches ne finissent jamais, la reserve de 40 blocs se remplit
   et le jeu s'arrete (lecture volontaire en 00000001, 0602054C) juste avant
   d'entrer en jeu.

   A retirer quand le cout des acces aux registres (Mednafen : SCU 8 cycles,
   B-Bus 5 a 10 cycles) sera modelise. Codes : Mednafen, qui classe aussi ce
   jeu parmi ceux qui dependent du temps du cache (CPUCACHE_EMUMODE_FULL). */
static const char * const SH2LegacyFetchDBList[] = {
   "T-8119G",    // Space Jam (Japan)
   "T-8125H",    // Space Jam (USA)
   "T-8125H-50", // Space Jam (Europe)
   NULL
};

/* Retourne 1 si le jeu en cours figure dans SH2LegacyFetchDBList. */
int DBLookupLegacySH2FetchTiming(void)
{
   const char* game_code;
   int i;
   Cs2GetIP(0);
   game_code = Cs2GetCurrentGmaecode();
   if (game_code == NULL) return 0;
   for (i = 0; SH2LegacyFetchDBList[i] != NULL; i++) {
      if (strcmp(SH2LegacyFetchDBList[i], game_code) == 0) return 1;
   }
   return 0;
}

/* Retourne 1 si le jeu en cours figure dans SH2CacheDBList. */
int DBLookupForceSH2Cache(void)
{
   const char* game_code;
   int i;
   Cs2GetIP(0);
   game_code = Cs2GetCurrentGmaecode();
   if (game_code == NULL) return 0;
   for (i = 0; SH2CacheDBList[i] != NULL; i++) {
      if (strcmp(SH2CacheDBList[i], game_code) == 0) return 1;
   }
   return 0;
}

static int does_file_exist(const char *filename)
{
   struct stat st;
   int result = stat(filename, &st);
   return result == 0;
}

void DBLookup(int* const cart_type, const char** cart_path, const char * support_dir)
{
#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif
   Cs2GetIP(1);
   const char* game_code = Cs2GetCurrentGmaecode();
   const u64 game_id = Cs2GetGameId();
   for (int i=0; i<NB_GAMES_DB; i++) {
      // Heart of Darkness has no game_code, but i lack information on game_id's uniqueness, so a false positive is not impossible
      if ((GameDBList[i].game_code != NULL && game_code != NULL && strcmp(GameDBList[i].game_code, game_code) == 0)
      || (GameDBList[i].game_id != 0 && game_id != 0 && GameDBList[i].game_id == game_id)) {
         if (GameDBList[i].filename != NULL && support_dir != NULL) {
            char rompath[512];
            snprintf(rompath, sizeof(rompath), "%s%c%s", support_dir, slash, GameDBList[i].filename);
            if (does_file_exist(rompath)) *cart_path = strdup(rompath);
         }
         *cart_type = GameDBList[i].cart_type;
         return;
      }
   }
}
