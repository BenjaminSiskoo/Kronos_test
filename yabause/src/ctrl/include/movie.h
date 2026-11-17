/*
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

/*! \file movie.h
    \brief Enregistreur / lecteur d'entrees ("films" .ymv), rejouables a
    l'identique pour reproduire un bug a partir du fichier d'un testeur, et
    outils de speedrun assiste (TAS) : re-record a partir des etats de
    sauvegarde, compteur de trames de lag, retour en arriere, edition des
    entrees trame par trame.
*/

#ifndef MOVIE_H
#define MOVIE_H

#include "core.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Stopped	  1
#define Recording 2
#define Playback  3

#define RunNormal   0
#define Paused      1
#define NeedAdvance 2

struct MovieStruct
{
	int Status;            /* 0 ou Stopped : rien ; Recording ; Playback */
	FILE *fp;
	int ReadOnly;          /* lecture seule (defaut en lecture) : charger un
	                          etat ne modifie pas le film */
	int Rerecords;
	int Size;              /* taille du fichier en octets */
	int Frames;            /* nombre de trames du film */
	const char* filename;
};

extern struct MovieStruct Movie;

/* Appele au debut de chaque trame par YabauseEmulate(). */
void DoMovie(void);

/* Demarre un enregistrement / une lecture : la machine est remise a zero
   (comme a la mise sous tension) puis le film commence a la trame 0.
   Renvoient 0 si tout va bien, -1 si le fichier ne s'ouvre pas, -2 si le
   film n'est pas valide ou ne correspond pas au jeu charge (voir
   MovieGetReport()). */
int SaveMovie(const char *filename);
int PlayMovie(const char *filename);

/* Arrete l'enregistrement ou la lecture. Apres une lecture, remet aussi en
   place la RAM de sauvegarde de l'utilisateur (voir MovieBackupLocked()). */
void StopMovie(void);

/* Lecture seule : charger un etat laisse le film tel quel et la lecture
   reprend a la trame de l'etat (un enregistrement devient une lecture).
   Lecture/ecriture : charger un etat reprend l'enregistrement a sa trame
   (re-record), et la fin du film enchaine sur un enregistrement. */
void MovieToggleReadOnly(void);
void MovieSetReadOnly(int readonly);

/* Etats de sauvegarde : le bloc du film est range a la fin de l'etat. Il
   est ecrit meme sans film (etat de synchronisation, voir movie.c). */
const char *MakeMovieStateName(const char *filename);
void SaveMovieInState(void ** stream);
/* available : octets disponibles dans l'etat a partir de la position
   courante (fin du bloc). */
void MovieReadState(const void * stream, int available);
/* 1 pendant qu'un instantane de MovieSeek() est pris ou recharge : etat sans
   capture d'ecran ni message a l'ecran (memory.c). */
int MovieStateIsLight(void);

/* Appele par YabauseEmulate() a la fin de chaque trame. */
void MovieFrameEnd(void);

void PauseOrUnpause(void);

/* 1 si un film est en cours d'enregistrement ou de lecture. */
int IsMovieLoaded(void);

/* 1 pendant un enregistrement ou une lecture : execution deterministe
   (synchronisation stricte du thread son, entrees prises une seule fois
   par trame). */
int MovieIsDeterministic(void);

/* Appele par YabauseReset() entre YabauseResetNoLoad() et le chargement
   rapide du jeu. */
void MovieResetHook(void);

/* 1 tant que les fichiers de sauvegarde ne doivent pas etre ecrits : la
   RAM de sauvegarde est celle du testeur (lecture d'un film). */
int MovieBackupLocked(void);

/* 1 tant qu'il y a quelque chose a arreter (film actif, ou RAM de
   sauvegarde du testeur encore en place apres la fin de la lecture). */
int MovieIsActive(void);

/* Informations pour l'interface. */
int MovieGetFrame(void);          /* trames ecoulees depuis le debut */
int MovieGetLength(void);         /* longueur du film (lecture) */
int MovieGetDesyncFrame(void);    /* -1, ou premiere trame divergente */
int MovieConsumeEndedFlag(void);  /* 1 une seule fois quand la lecture finit */
const char *MovieGetReport(void); /* avertissements du dernier demarrage */

/* --- Outils de TAS ------------------------------------------------------ */

int MovieGetFrameCounter(void);   /* trames executees (film ou pas) */
int MovieGetLagCount(void);       /* trames ou le jeu n'a pas lu les manettes */
int MovieLastFrameWasLag(void);   /* la derniere trame executee etait du lag */
int MovieGetRerecords(void);

/* Ligne "trame / lag / re-records / entrees" en bas de l'image. */
void MovieSetOSD(int on);
int MovieGetOSD(void);

/* Demande une pause quand 'frame' trames auront ete executees (-1 : aucune).
   La boucle d'emulation de l'interface appelle MovieConsumePauseRequest()
   apres chaque trame : 1 = se mettre en pause maintenant (trame demandee
   atteinte, ou fin d'une lecture seule). */
void MovieSetPauseFrame(int frame);
int MovieConsumePauseRequest(void);

/* Place la machine a 'target' trames executees ou avant (jamais apres) : a
   partir du dernier instantane utilisable, sinon du debut du film. Un
   enregistrement devient une lecture en lecture/ecriture. A appeler entre
   deux trames ; l'appelant fait ensuite tourner l'emulation jusqu'a la
   trame voulue (MovieSetPauseFrame()). Renvoie 0, ou -1 sans film. */
int MovieSeek(int target);

/* Instantanes pour MovieSeek() : un tous les 'interval_frames' trames (0 :
   aucun), dans la limite de 'budget_mb' Mo. */
void MovieSetSnapshots(int interval_frames, int budget_mb);
int MovieGetSnapshotCount(void);

/* 1 : charger un etat ne coupe jamais le film, meme en lecture/ecriture
   (editeur d'entrees ouvert). */
void MovieSetKeepOnLoad(int keep);

/* 1 si une trame deja executee a ete modifiee : la machine ne correspond
   plus au film tant que MovieSeek() ne l'a pas ramenee en arriere. */
int MovieIsStale(void);

/* Edition du film charge. 'frame' est l'indice de l'entree (0 = premiere
   trame apres le reset), 'port' 0 ou 1. Les fonctions qui modifient
   renvoient 0, ou -1 si le film ne peut pas etre modifie. Toute
   modification passe le film en lecture/ecriture. */
#define MOVIE_PAD_BUTTONS 13      /* U D L R Start A B C X Y Z L R */
int MovieCanEdit(void);
int MovieEditFrameCount(void);
int MovieEditPortLen(void);       /* octets de donnees par port */
/* Copie les donnees du port (PORTDATAx.data) ; renvoie leur taille utile
   (PORTDATAx.size), ou -1. */
int MovieEditGetPort(int frame, int port, u8 *data, int maxlen);
int MovieEditSetPort(int frame, int port, int size, const u8 *data, int len);
/* Manette numerique branchee directement : 1 appuye, 0 relache, -1 si le
   port n'est pas une manette numerique. */
int MovieEditGetButton(int frame, int port, int button);
int MovieEditSetButton(int frame, int port, int button, int pressed);
char MovieEditButtonLetter(int button);
int MovieEditGetLag(int frame);   /* 1 lag, 0 non, -1 inconnu */
int MovieEditInsert(int at, int count);
int MovieEditDelete(int at, int count);

extern int framecounter;
extern int LagFrameFlag;
extern int lagframecounter;
extern char MovieStatus[40];
extern char InputDisplayString[40];
extern int FrameAdvanceVariable;

#ifdef __cplusplus
}
#endif

#endif
