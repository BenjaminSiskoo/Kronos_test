/*  Rewind for Kronos (Qt port): a ring of save states kept in memory.

    Every <interval> frames the emulation state is captured (same stream as a
    save state, without the screen picture). Only the newest state is kept
    whole; older ones are stored as compressed XOR differences to their
    successor, so going back one step is "newest ^ difference". A buffer of 30
    seconds costs a few tens of MB instead of hundreds.

    Settings > Debug > Rewind, Tools > Rewind (shortcut Backspace, hold to keep
    going back). Disabled while an input movie is loaded (a state load would
    rewrite the movie). Backup RAM is not rewound.
*/
#ifndef KRONOS_REWIND_H
#define KRONOS_REWIND_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* enabled: 0/1, interval: frames between two captures (1-60), seconds: length
   of the buffer (1-600), memoryMB: hard limit of the buffer (16-4096). */
void RewindConfigure(int enabled, int interval, int seconds, int memoryMB);
void RewindSetEnabled(int enabled);
int  RewindIsEnabled(void);

/* Called by YabauseEmulate() at the end of every frame. */
void RewindFrameEnd(void);

/* Goes back one step (<interval> frames at most). Returns 0 when the state was
   restored, -1 when there is nothing to go back to. Must be called between
   two frames, from the thread that runs the emulation. */
int  RewindStep(void);

/* Seconds of emulation that can still be rewound. */
int  RewindAvailableSeconds(void);

void RewindClear(void);
void RewindDeInit(void);

#ifdef __cplusplus
}
#endif

#endif
