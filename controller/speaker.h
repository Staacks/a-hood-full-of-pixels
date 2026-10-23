// Bluetooth speaker: A2DP source: one permanently open SBC stream (silence while idle),
// sounds are mixed in at a scheduled stream time.
//
// Stream time: sample n of the stream belongs to time t0 + n / 44100 s (time_us_64 base), where
// t0 is the moment the stream started. Samples are encoded close to their stream time and the
// speaker plays them a constant L_bt later (while the stream runs steadily), so a sound scheduled
// at time t is heard at t + L_bt. The backlog is capped, so a stalled radio drops audio instead
// of building up latency.
//
// All functions are called from thread context (core 0); they take the cyw43 async_context lock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SPEAKER_SAMPLE_RATE 44100

typedef enum {
    SPEAKER_OFF,        // Bluetooth not up yet
    SPEAKER_NO_SPEAKER, // nothing paired yet: start pairing
    SPEAKER_WAITING,    // paired speaker not reachable, retrying periodically
    SPEAKER_CONNECTING,
    SPEAKER_PAIRING, // searching for a speaker in pairing mode
    SPEAKER_CONNECTED, // signalling up, stream not running (yet)
    SPEAKER_STREAMING,
} speaker_state_t;

// Renders up to `frames` interleaved stereo frames (L, R) into lr, starting at frame `pos` of
// the sound. Returns the number of frames written; fewer than requested ends the sound.
// Called from the Bluetooth context: keep it fast, no blocking.
typedef unsigned (*speaker_sound_fn)(int16_t *lr, unsigned frames, uint32_t pos, void *ctx);

// Call between bt_setup_init() and bt_setup_power_on(). Connects to the stored speaker once
// Bluetooth is up.
void speaker_init(void);

// Search for a speaker in pairing mode, connect to the first one found and remember it.
void speaker_pair(void);
// Forget the stored speaker and all link keys.
void speaker_forget(void);
// Drop the connection and reconnect (testing).
void speaker_reconnect(void);

// Schedule a sound at stream time start_us (replaces a playing sound). A start time that has
// already been encoded is moved to the next free sample. Returns the stream time the sound
// actually starts at, or 0 if no stream is running (the sound is dropped).
uint64_t speaker_play(speaker_sound_fn fn, void *ctx, uint64_t start_us);
void speaker_stop_sound(void);
bool speaker_sound_playing(void);

// Very quiet noise instead of digital silence while idle (in case the speaker powers off on
// silence).
void speaker_set_idle_noise(bool on);
bool speaker_idle_noise(void);

// Full HCI packet log on the console, only in the bt_test_hcidump build (returns the new state;
// always false otherwise). The log pauses itself when the stream starts.
bool speaker_toggle_hci_log(void);

speaker_state_t speaker_state(void);
const char *speaker_state_name(speaker_state_t s);

// Prints connection, codec and stream health since the last call, then resets the interval
// statistics.
void speaker_print_status(void);
