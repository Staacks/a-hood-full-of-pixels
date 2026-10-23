#pragma once

#include <stdbool.h>
#include <stdint.h>

// microSD card on SPI0 (GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI), same wiring on both boards.
// FatFs via carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico; the volume is "0:".

#define SDCARD_DEFAULT_HZ 25000000 // SD default-speed limit; clk_peri 150 MHz / 6

// (Re)initialises the card and mounts the volume. Safe to call again after a card swap.
bool sdcard_mount(void);

// SPI clock for data transfers, takes effect on the next sdcard_mount(). The hardware uses the
// nearest rate <= hz that clk_peri (150 MHz) / even divider allows; get returns that once mounted.
void sdcard_set_clock(uint32_t hz);
uint32_t sdcard_get_clock(void);

// Bring-up: cycles the clock 12.5 -> 25 -> 37.5 MHz (above SD spec, shows the margin) and
// prints the new value.
void sdcard_cycle_clock(void);

// Bring-up test on USB serial: card info, file system, root directory, write + verify of a
// test file, sequential read benchmark in 32 KB chunks (one video frame). Blocks for a few
// seconds.
void sdcard_test(void);
