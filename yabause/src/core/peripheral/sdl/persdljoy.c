/*  Copyright 2005 Guillaume Duhamel
	Copyright 2005-2006 Theo Berkau
	Copyright 2008 Filipe Azevedo

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

/*! \file persdljoy.c
    \brief SDL joystick peripheral interface.

    Hotplug: the joystick table used to be built once, in PERSDLJoyInit().
    A pad that was configured but unplugged when Kronos started was never
    opened, and plugging it in afterwards did nothing until Kronos was
    restarted. The table is now rebuilt whenever SDL's device list changes
    (a device added or removed, detected by comparing the SDL instance ids),
    from both PERSDLJoyHandleEvents() (emulation) and PERSDLJoyScan()
    (controller settings dialog).

    The rebuild opens the devices in SDL's device-index order, exactly as a
    restart of Kronos would, so the key codes stored in the settings
    ((device index << 18) | ...) keep the same meaning as after a restart.
    Before the old table is closed, every key it may hold pressed is
    released and every axis is set back to neutral, so unplugging a pad
    with a button held does not leave that button stuck on the Saturn side.
*/

#ifdef HAVE_LIBSDL
#if defined(__APPLE__) || defined(GEKKO)
 #ifdef HAVE_LIBSDL2
  #include <SDL2/SDL.h>
 #else
  #include <SDL/SDL.h>
 #endif
#else
 #include "SDL.h"
#endif

#include <stdlib.h>
#include <stdio.h>

#include "debug.h"
#include "persdljoy.h"

#define SDL_HAT_VALUE 0x200000
#define SDL_PERSF_AXIS_VALUE 0x4000
#define SDL_PERSF_HAT_MAX_VALUE 0x110000
#define SDL_PERSF_HAT_MIN_VALUE 0x100000
#define SDL_BUTTON_PRESSED 1
#define SDL_BUTTON_RELEASED 0

/* Hotplug needs SDL_JoystickGetDeviceInstanceID() (SDL 2.0.6). With an
   older SDL (or SDL 1.2) the device list is still read only once, at init. */
#if SDL_VERSION_ATLEAST(2,0,6)
 #define PERSDLJOY_HOTPLUG 1
#else
 #define PERSDLJOY_HOTPLUG 0
#endif

int PERSDLJoyInit(void);
void PERSDLJoyDeInit(void);
int PERSDLJoyHandleEvents(void);

u32 PERSDLJoyScan(u32 flags);
void PERSDLJoyFlush(void);
void PERSDLKeyName(u32 key, char * name, int size);

PerInterface_struct PERSDLJoy = {
PERCORE_SDLJOY,
"SDL Joystick Interface",
PERSDLJoyInit,
PERSDLJoyDeInit,
PERSDLJoyHandleEvents,
PERSDLJoyScan,
1,
PERSDLJoyFlush,
PERSDLKeyName
};

typedef struct {
	SDL_Joystick* mJoystick;
#if PERSDLJOY_HOTPLUG
	SDL_JoystickID mInstanceId;
#endif
	int mNumAxes;
	int mNumButtons;
	int mNumHats;
	s16* mScanStatus;
	s16* mInitValue;
	Uint8* mHatStatus;
} PERSDLJoystick;

unsigned int SDL_PERCORE_INITIALIZED = 0;
unsigned int SDL_PERCORE_JOYSTICKS_INITIALIZED = 0;
PERSDLJoystick* SDL_PERCORE_JOYSTICKS = 0;
unsigned int SDL_HAT_VALUES[] = { SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_LEFT, SDL_HAT_DOWN };
const unsigned int SDL_HAT_VALUES_NUM = sizeof(SDL_HAT_VALUES) / sizeof(SDL_HAT_VALUES[0]);

/* PERSDLJoyHandleEvents() runs on the emulation thread (SMPC) while
   PERSDLJoyScan() runs on the UI thread (controller settings dialog): the
   table can now be rebuilt from either one, so every access to it is done
   under this lock. It is created once and never destroyed, so a late call
   from one thread can never use a freed mutex. */
static SDL_mutex* SDL_PERCORE_LOCK = NULL;

static void PERSDLJoyLock(void)
{
	if ( SDL_PERCORE_LOCK ) SDL_LockMutex( SDL_PERCORE_LOCK );
}

static void PERSDLJoyUnlock(void)
{
	if ( SDL_PERCORE_LOCK ) SDL_UnlockMutex( SDL_PERCORE_LOCK );
}

//////////////////////////////////////////////////////////////////////////////

/* Opens every joystick SDL currently knows, in device-index order.
   Caller holds the lock. */
static void PERSDLJoyOpenAll(void)
{
	int i, j;
	int count = SDL_NumJoysticks();

	SDL_PERCORE_JOYSTICKS_INITIALIZED = 0;
	SDL_PERCORE_JOYSTICKS = 0;

	if ( count <= 0 )
	{
		return;
	}

	SDL_PERCORE_JOYSTICKS = (PERSDLJoystick*)calloc( count, sizeof(PERSDLJoystick) );
	if ( !SDL_PERCORE_JOYSTICKS )
	{
		return;
	}
	SDL_PERCORE_JOYSTICKS_INITIALIZED = count;

	for ( i = 0; i < count; i++ )
	{
		PERSDLJoystick* pj = &SDL_PERCORE_JOYSTICKS[ i ];
		SDL_Joystick* joy = SDL_JoystickOpen( i );

#if PERSDLJOY_HOTPLUG
		/* Kept even when the open failed, so a device that cannot be opened
		   does not make the list look "changed" on every call. */
		pj->mInstanceId = SDL_JoystickGetDeviceInstanceID( i );
#endif

		SDL_JoystickUpdate();

		pj->mJoystick = joy;
		if ( !joy )
		{
			continue;
		}

		pj->mNumAxes = SDL_JoystickNumAxes( joy );
		pj->mNumButtons = SDL_JoystickNumButtons( joy );
		pj->mNumHats = SDL_JoystickNumHats( joy );
		if ( pj->mNumAxes < 0 ) pj->mNumAxes = 0;
		if ( pj->mNumButtons < 0 ) pj->mNumButtons = 0;
		if ( pj->mNumHats < 0 ) pj->mNumHats = 0;

		pj->mScanStatus = (s16*)calloc( pj->mNumAxes ? pj->mNumAxes : 1, sizeof(s16) );
		pj->mInitValue = (s16*)calloc( pj->mNumAxes ? pj->mNumAxes : 1, sizeof(s16) );
		pj->mHatStatus = (Uint8*)calloc( pj->mNumHats ? pj->mNumHats : 1, sizeof(Uint8) );

		if ( !pj->mScanStatus || !pj->mInitValue || !pj->mHatStatus )
		{
			free( pj->mScanStatus );
			free( pj->mInitValue );
			free( pj->mHatStatus );
			pj->mScanStatus = 0;
			pj->mInitValue = 0;
			pj->mHatStatus = 0;
			SDL_JoystickClose( joy );
			pj->mJoystick = 0;
			continue;
		}

		for ( j = 0; j < pj->mNumAxes; j++ )
		{
#if SDL_VERSION_ATLEAST(2,0,6)
			SDL_JoystickGetAxisInitialState( joy, j, &pj->mInitValue[ j ] );
#else
			pj->mInitValue[ j ] = 0;
#endif
			pj->mScanStatus[ j ] = SDL_JoystickGetAxis( joy, j ) - pj->mInitValue[ j ];
		}
		/* Hats start "centered": a direction already held when the pad is
		   opened is reported as a press by the next PERSDLJoyHandleEvents()
		   (before, it was swallowed until the hat was released). */
		for ( j = 0; j < pj->mNumHats; j++ )
		{
			pj->mHatStatus[ j ] = SDL_HAT_CENTERED;
		}
	}
}

//////////////////////////////////////////////////////////////////////////////

/* Releases everything the given table entry may hold pressed on the
   Saturn side. Caller holds the lock. */
static void PERSDLJoyReleaseAll(int joyId, PERSDLJoystick* pj)
{
	int i, j;

	if ( !pj->mJoystick )
	{
		return;
	}

	for ( i = 0; i < pj->mNumAxes; i++ )
	{
		PerAxisValue( (joyId << 18) | SDL_PERSF_AXIS_VALUE | i, 0x80 );
		PerKeyUp( (joyId << 18) | SDL_PERSF_HAT_MIN_VALUE | i );
		PerKeyUp( (joyId << 18) | SDL_PERSF_HAT_MAX_VALUE | i );
	}
	for ( i = 0; i < pj->mNumButtons; i++ )
	{
		PerKeyUp( (joyId << 18) | (i +1) );
	}
	for ( i = 0; i < pj->mNumHats; i++ )
	{
		for ( j = 0; j < (int)SDL_HAT_VALUES_NUM; j++ )
		{
			PerKeyUp( (joyId << 18) | SDL_HAT_VALUE | (SDL_HAT_VALUES[ j ] << 4) | i );
		}
	}
}

//////////////////////////////////////////////////////////////////////////////

/* Closes the whole table. When releaseKeys is set, the keys it may hold
   pressed are released first (hotplug rebuild). Caller holds the lock. */
static void PERSDLJoyCloseAll(int releaseKeys)
{
	unsigned int i;

	for ( i = 0; i < SDL_PERCORE_JOYSTICKS_INITIALIZED; i++ )
	{
		PERSDLJoystick* pj = &SDL_PERCORE_JOYSTICKS[ i ];

		if ( !pj->mJoystick )
		{
			continue;
		}

		if ( releaseKeys )
		{
			PERSDLJoyReleaseAll( (int)i, pj );
		}

		SDL_JoystickClose( pj->mJoystick );
		free( pj->mScanStatus );
		free( pj->mInitValue );
		free( pj->mHatStatus );
		pj->mJoystick = 0;
	}

	free( SDL_PERCORE_JOYSTICKS );
	SDL_PERCORE_JOYSTICKS = 0;
	SDL_PERCORE_JOYSTICKS_INITIALIZED = 0;
}

//////////////////////////////////////////////////////////////////////////////

/* Must be called right after SDL_JoystickUpdate(), which is what runs SDL's
   device detection (it does so even with joystick events ignored).
   Caller holds the lock. */
static void PERSDLJoyCheckHotplug(void)
{
#if PERSDLJOY_HOTPLUG
	int count = SDL_NumJoysticks();
	int changed = 0;
	int i;

	if ( count < 0 )
	{
		count = 0;
	}

	if ( (unsigned int)count != SDL_PERCORE_JOYSTICKS_INITIALIZED )
	{
		changed = 1;
	}
	else
	{
		/* Same count: a pad unplugged and another (or the same one) plugged
		   back in gets a new instance id. */
		for ( i = 0; i < count; i++ )
		{
			if ( SDL_JoystickGetDeviceInstanceID( i ) != SDL_PERCORE_JOYSTICKS[ i ].mInstanceId )
			{
				changed = 1;
				break;
			}
		}
	}

	if ( !changed )
	{
		return;
	}

	LOG( "SDL joystick: device list changed (%u -> %d), reopening joysticks\n",
	     SDL_PERCORE_JOYSTICKS_INITIALIZED, count );

	PERSDLJoyCloseAll( 1 );
	PERSDLJoyOpenAll();
#endif
}

//////////////////////////////////////////////////////////////////////////////

int PERSDLJoyInit(void) {
	// does not need init if already done
	if ( SDL_PERCORE_INITIALIZED )
	{
		return 0;
	}

#if defined (_MSC_VER) && SDL_VERSION_ATLEAST(2,0,0)
   SDL_SetMainReady();
#endif

#if defined(_WIN32) && defined(SDL_HINT_JOYSTICK_THREAD)
	/* Windows: device arrival/removal is signalled by WM_DEVICECHANGE to a
	   window SDL creates in the thread that initialises the subsystem. Init
	   can happen on the UI thread (settings dialog) or on the emulation
	   thread (PerInit), and only the former pumps Win32 messages; with this
	   hint SDL handles the notifications in its own thread, so hotplug is
	   detected whatever thread did the init. */
	SDL_SetHint( SDL_HINT_JOYSTICK_THREAD, "1" );
#endif

	if ( !SDL_PERCORE_LOCK )
	{
		SDL_PERCORE_LOCK = SDL_CreateMutex();
	}

	// init joysticks
	if ( SDL_InitSubSystem( SDL_INIT_JOYSTICK ) == -1 )
	{
		return -1;
	}

	// ignore joysticks event in sdl event loop
	SDL_JoystickEventState( SDL_IGNORE );

	// open joysticks
	PERSDLJoyLock();
	PERSDLJoyOpenAll();
	SDL_PERCORE_INITIALIZED = 1;
	PERSDLJoyUnlock();

	// success
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

void PERSDLJoyDeInit(void) {
	PERSDLJoyLock();

	// close joysticks
	if ( SDL_PERCORE_INITIALIZED == 1 )
	{
		PERSDLJoyCloseAll( 0 );
	}

	SDL_PERCORE_JOYSTICKS_INITIALIZED = 0;
	SDL_PERCORE_INITIALIZED = 0;

	PERSDLJoyUnlock();

	// close sdl joysticks
	SDL_QuitSubSystem( SDL_INIT_JOYSTICK );
}

//////////////////////////////////////////////////////////////////////////////

int PERSDLJoyHandleEvents(void) {
	int joyId;
	int i;
	int j;
	SDL_Joystick* joy;
	Sint32 cur, curAxis;
	Uint8 buttonState;
	Uint8 newHatState;
	Uint8 oldHatState;
	int hatValue;

	if ( !SDL_PERCORE_INITIALIZED )
	{
		return 0;
	}

	PERSDLJoyLock();

	// update joysticks states (also runs SDL's device detection)
	SDL_JoystickUpdate();

	// a pad plugged in or unplugged since the last call: reopen the list
	PERSDLJoyCheckHotplug();

	// check each joysticks
	for ( joyId = 0; joyId < (int)SDL_PERCORE_JOYSTICKS_INITIALIZED; joyId++ )
	{
		PERSDLJoystick* pj = &SDL_PERCORE_JOYSTICKS[ joyId ];
		joy = pj->mJoystick;

		if ( !joy )
		{
			continue;
		}

		// check axis
		for ( i = 0; i < pj->mNumAxes; i++ )
		{
			cur = SDL_JoystickGetAxis( joy, i );
			if (abs(cur) <= abs(pj->mInitValue[ i ])) curAxis = 0;
			else curAxis = cur;

			cur -= pj->mInitValue[ i ];

			PerAxisValue((joyId << 18) | SDL_PERSF_AXIS_VALUE | i, (u8)(((int)curAxis+32768) >> 8));

			if (( cur > 16384 ) || ( cur < -16384 ))
			{
				if ( cur > 16384 )
					PerKeyDown( (joyId << 18) | SDL_PERSF_HAT_MAX_VALUE | i );
				else
					PerKeyDown( (joyId << 18) | SDL_PERSF_HAT_MIN_VALUE | i );
			} else
			{
				if ( cur > -8196 )
					PerKeyUp( (joyId << 18) | SDL_PERSF_HAT_MIN_VALUE | i );
				if ( cur < 8196 )
					PerKeyUp( (joyId << 18) | SDL_PERSF_HAT_MAX_VALUE | i );
			}
		}

		// check buttons
		for ( i = 0; i < pj->mNumButtons; i++ )
		{
			buttonState = SDL_JoystickGetButton( joy, i );

			if ( buttonState == SDL_BUTTON_PRESSED )
			{
				PerKeyDown( (joyId << 18) | (i +1) );
			}
			else if ( buttonState == SDL_BUTTON_RELEASED )
			{
				PerKeyUp( (joyId << 18) | (i +1) );
			}
		}

		// check hats
		for ( i = 0; i < pj->mNumHats; i++ )
		{
			newHatState = SDL_JoystickGetHat( joy, i );
			oldHatState = pj->mHatStatus[ i ];

			for ( j = 0 ; j < (int)SDL_HAT_VALUES_NUM; j++ )
			{
				hatValue = SDL_HAT_VALUES[ j ];
				if ( oldHatState & hatValue && ~newHatState & hatValue )
				{
					PerKeyUp( (joyId << 18) | SDL_HAT_VALUE | (hatValue << 4) | i );
				}
			}
			for ( j = 0 ; j < (int)SDL_HAT_VALUES_NUM; j++ )
			{
				hatValue = SDL_HAT_VALUES[ j ];
				if ( ~oldHatState & hatValue && newHatState & hatValue )
				{
					PerKeyDown( (joyId << 18) | SDL_HAT_VALUE | (hatValue << 4) | i);
				}
			}

			pj->mHatStatus[ i ] = newHatState;
		}
	}

	PERSDLJoyUnlock();

	// return success
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 PERSDLJoyScan( u32 flags ) {
	// init vars
	int joyId;
	int i;
	SDL_Joystick* joy;
	Sint32 cur;
	Uint8 hatState;
	u32 result = 0;

	if ( !SDL_PERCORE_INITIALIZED )
	{
		return 0;
	}

	PERSDLJoyLock();

	// update joysticks states (also runs SDL's device detection)
	SDL_JoystickUpdate();

	// a pad plugged in while the settings dialog is open becomes usable
	// for configuration right away
	PERSDLJoyCheckHotplug();

	// check each joysticks
	for ( joyId = 0; joyId < (int)SDL_PERCORE_JOYSTICKS_INITIALIZED && !result; joyId++ )
	{
		PERSDLJoystick* pj = &SDL_PERCORE_JOYSTICKS[ joyId ];
		joy = pj->mJoystick;

		if ( !joy )
		{
			continue;
		}

		// check axis
		for ( i = 0; i < pj->mNumAxes && !result; i++ )
		{
			cur = SDL_JoystickGetAxis( joy, i ) - pj->mInitValue[ i ];

			if ( cur != pj->mScanStatus[ i ] )
			{
				if (flags & PERSF_AXIS) {
					if (( cur > 16384 ) || (cur < -16384)) {
						result = (joyId << 18) | SDL_PERSF_AXIS_VALUE | i;
						break;
					}
				}
				if (flags & PERSF_HAT) {
					if ( cur > 16384 ) {
						result = (joyId << 18) | SDL_PERSF_HAT_MAX_VALUE | i;
						break;
					}
					if ( cur < -16384 ) {
						result = (joyId << 18) | SDL_PERSF_HAT_MIN_VALUE | i;
						break;
					}
				}
			}
		}
		if ( result )
		{
			break;
		}

		if (flags & PERSF_BUTTON)
		{
			// check buttons
			for ( i = 0; i < pj->mNumButtons; i++ )
			{
				if ( SDL_JoystickGetButton( joy, i ) == SDL_BUTTON_PRESSED )
				{
					result = (joyId << 18) | (i +1);
					break;
				}
			}
			if ( result )
			{
				break;
			}
		}

		if (flags & PERSF_HAT)
		{
			// check hats
			for ( i = 0; i < pj->mNumHats; i++ )
			{
				hatState = SDL_JoystickGetHat( joy, i );
				switch (hatState)
				{
					case SDL_HAT_UP:
					case SDL_HAT_RIGHT:
					case SDL_HAT_DOWN:
					case SDL_HAT_LEFT:
						result = (joyId << 18) | SDL_HAT_VALUE | (hatState << 4) | i;
						break;
					default:
						break;
				}
				if ( result )
				{
					break;
				}
			}
		}
	}

	PERSDLJoyUnlock();

	return result;
}

void PERSDLJoyFlush(void) {
}

void PERSDLKeyName(u32 key, char * name, UNUSED int size)
{
	sprintf(name, "%x", (int)key);
}

#endif
