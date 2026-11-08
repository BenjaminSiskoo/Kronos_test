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
    l'identique pour reproduire un bug a partir du fichier d'un testeur.
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
	int ReadOnly;          /* lecture seule (defaut en lecture) */
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

void MovieToggleReadOnly(void);

/* Etats de sauvegarde : le film est range a la fin de l'etat. */
const char *MakeMovieStateName(const char *filename);
void SaveMovieInState(void ** stream);
/* available : octets disponibles dans l'etat a partir de la position
   courante (fin du bloc). */
void MovieReadState(const void * stream, int available);

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
