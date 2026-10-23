// Video player on core 1: owns the panel refresh (hub75_show) and the SD card. Plays a scene's
// intro once, then its loop, from .vid files in the root of the SD card (tools/make_scenes.py,
// tools/convert_video.py). Core 0 only sends requests (any function below).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Scenes until config.ini exists; the same order as on the controller. Scene 0 is the base
// scene and plays after boot.
#define PLAYER_SCENES 12

// Starts core 1 with the base scene. Call after hub75_init().
void player_init(void);

// Plays a scene: intro (if any), then its loop. start_us = time_us_32() at which the first
// frame shows (for a start synchronised with the controller); 0 = now.
void player_play(int scene, uint32_t start_us);
int player_scene(void);
// False while the built-in fallback eyes show (no SD card or no video file).
bool player_has_video(void);
const char *player_scene_name(int scene);

// Bring-up: shows a test pattern (testpattern.h) instead of video; < 0 = back to the video.
void player_show_pattern(int pattern);

// Runs sdcard_test() on core 1 (the core that owns the card).
void player_sd_test(void);

// Watchdog: counts up while core 1 runs; busy = a long test runs (no progress expected).
uint32_t player_alive(void);
bool player_busy(void);

void player_print_status(void);
