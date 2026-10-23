// Strip animations from the controller's SD card (.led files from tools/make_scenes.py, format
// in tools/staffled.py): a scene's intro once, then its loop. The files have 25 fps; frames are
// blended for the strip's faster frame rate. Call from the main loop (core 0); the card must be
// mounted (clips_init()).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Starts intro (may be missing) then loop (may be missing: the animation ends after the intro)
// at start_us (time_us_64()). False if neither file opens or matches the strip: then the caller
// shows the scene's built-in effect.
bool staffanim_play(const char *intro, const char *loop, uint64_t start_us);
void staffanim_stop(void);

// Draws the frame for now_us into the whole strip (the crystal is dark; draw it afterwards).
// False if no animation plays (ended, read error, nothing started): draw something else.
bool staffanim_render(uint64_t now_us);

bool staffanim_in_intro(void);

void staffanim_print_status(void);
