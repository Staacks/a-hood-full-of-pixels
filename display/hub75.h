#pragma once

#include <stdbool.h>
#include <stdint.h>

// Logical canvas: two stacked 128x64 panels.
#define HUB75_WIDTH 128
#define HUB75_HEIGHT 128

// Two 128x64 1/32-scan panels without an OUT connector, each on its own connector. They share
// the data, address, LAT and OE lines but have separate clocks: per row the data SM shifts
// panel 1's pixels with CLK1, then panel 2's with CLK2, and one LAT latches both.
#define HUB75_PANEL_W 128
#define HUB75_PANEL_H 64
#define HUB75_CHAIN_W (2 * HUB75_PANEL_W) // pixels shifted per row (both panels)
#define HUB75_SCAN_ROWS (HUB75_PANEL_H / 2)

// Bitplanes per channel (BCM depth). 8 = 8-bit colour; 10 needs a faster CLK to
// keep the refresh rate >= ~150 Hz.
#ifndef HUB75_PLANES
#define HUB75_PLANES 8
#endif

// Pins (fixed, see docs/design.md). Side-set pairs: CLK2 = CLK1 + 1, OE = LAT + 1.
#define HUB75_PIN_DATA_BASE 0 // R1 G1 B1 R2 G2 B2 (both panels)
#define HUB75_PIN_ROW_BASE 6  // A B C D E
#define HUB75_PIN_CLK1 11     // panel 1 (connector J1)
#define HUB75_PIN_CLK2 12     // panel 2 (connector J3)
#define HUB75_PIN_LAT 13
#define HUB75_PIN_OE 14

typedef struct {
    // Which panel is mounted on top: 0 = panel 1 (CLK1, J1), 1 = panel 2 (CLK2, J3).
    uint8_t top_half;
    bool rotate_top;    // top panel mounted rotated by 180°
    bool rotate_bottom; // bottom panel mounted rotated by 180°
    bool swap_rb;       // panel colour order B-G-R (red and blue inputs swapped)
} hub75_mapping_t;

// How the panel selects its rows.
typedef enum {
    HUB75_ROWS_BINARY, // A-E carry the row address (74HC138-style decoders)
    HUB75_ROWS_SM5368, // shift-register row drivers (SM5368): A = row clock, B = BK, C = row data
} hub75_row_mode_t;

// Sends the FM6126A register init sequence by bit-banging. Must run before hub75_init().
void hub75_fm6126a_init(void);

// Claims PIO state machines and DMA channels and starts the refresh (panels dark).
bool hub75_init(void);

// Current model of both panels at the power bank (measured 2026-10, docs/design.md "Power limiting"):
// I = idle + full white * linear load * brightness / 255. hub75_show() lowers the brightness of
// a frame that would exceed the budget (runtime limiter; the power bank trips at 3 A).
#define HUB75_IDLE_MA 430
#define HUB75_FULL_WHITE_MA 7200 // both panels, full white at brightness 255, minus idle
#define HUB75_BUDGET_MA 2600

// Renders an RGB565 canvas (HUB75_WIDTH x HUB75_HEIGHT, row-major) into the back buffer
// and makes it visible at the next frame boundary. Blocks until the flip happened. Call it
// from one core only (the refresh core); the setters below may be called from any core.
void hub75_show(const uint16_t *canvas);

// Global brightness 0..255 (OE duty). Applied with the next hub75_show().
void hub75_set_brightness(uint8_t brightness);
uint8_t hub75_get_brightness(void);
// Brightness of the last frame after the current limiter.
uint8_t hub75_get_applied_brightness(void);

// Data SM clock divider: CLK = clk_sys / div / 2.
void hub75_set_clkdiv(unsigned div);
unsigned hub75_get_clkdiv(void);

void hub75_set_mapping(const hub75_mapping_t *m);
hub75_mapping_t hub75_get_mapping(void);

void hub75_set_row_mode(hub75_row_mode_t mode);
hub75_row_mode_t hub75_get_row_mode(void);

// Number of complete frames scanned out since init (refresh counter).
uint32_t hub75_frame_count(void);

// Average linear load of the last rendered frame in per mille of full white
// (before global brightness). Basis for the power limiter.
uint32_t hub75_last_load_permille(void);
