// Sound clips from the controller's SD card (WAV, PCM 16-bit, 44.1 kHz, stereo; made by
// tools/make_sounds.py) into the speaker stream.
//
// The speaker encodes in the Bluetooth context, which must not block, so the clip is streamed
// through a RAM ring buffer: clips_play() opens the file and pre-fills the buffer, clips_poll()
// in the main loop tops it up. Call everything from the main loop (core 0).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Mounts the card (no card is fine: clips_play() retries now and then).
void clips_init(void);

// Plays a clip from the card at stream time start_us (time_us_64(): the moment of the stomp;
// the speaker sounds L_bt later) or, since reading the card takes a few ms, as soon after it
// as possible. Replaces a playing clip. Returns the stream time the clip actually starts at, or
// 0 if the card, the file or the stream is missing (then nothing plays).
uint64_t clips_play(const char *path, uint64_t start_us);

// Call from the main loop: refills the ring buffer.
void clips_poll(void);

// Stops the clip and closes the file, e.g. before the SD test uses the card; the card is
// mounted again on the next clips_play().
void clips_stop(void);

void clips_print_status(void);
