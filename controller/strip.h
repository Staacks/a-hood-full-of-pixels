#pragma once

#include <stdbool.h>
#include <stdint.h>

// Staff strip: data enters at the bottom (electronics), runs up side 0 (front), folds at the
// top and returns down side 1 (back). LED i and LED 2N-1-i are at the same height.
//
// Numbering: firmware indices start at 0; LED numbers shown to the user start at 1 (LED #1 at
// the bottom of the front, #N at the top of the front, #N+1 at the top of the back, #2N at the
// bottom of the back). Ranges in settings use front LED numbers (1..N); each also covers the
// partner LED on the back.
#define STRIP_MAX_PER_SIDE 120
#define STRIP_DEFAULT_PER_SIDE 90

// The top-most pairs light the crystal. The current limiter never dims them.
#define STRIP_DEFAULT_CRYSTAL_PAIRS 3
// Heights hidden by the grip and connector rings (front LED numbers): they count as positions
// for effects but always stay off. Measured on the assembled staff (2026-10-10).
#define STRIP_DEFAULT_DEAD "65-72, 87"

// Current model at the power bank: base + sum over all channels (value / 255) * per channel.
// Base = Pico + strip powered but black (measured without Bluetooth). The limiter keeps the
// estimate below the budget (the power bank trips at 3 A; margin for Bluetooth and errors).
#define STRIP_BUDGET_MA 2600
#define STRIP_DEFAULT_BASE_MA 136         // measured 2026-10-10 (assembled staff)
#define STRIP_DEFAULT_UA_PER_CHANNEL 8931  // full white at the limit, measured 2026-10-10

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef enum { ORDER_GRB, ORDER_RGB, ORDER_BRG, ORDER_RBG, ORDER_GBR, ORDER_BGR, ORDER_COUNT } color_order_t;

bool strip_init(unsigned pin, unsigned per_side);

// Logical access: h = 0 bottom .. per_side-1 top, side 0 (front) or 1 (back).
void strip_set(unsigned h, unsigned side, rgb_t c);
// Same colour on both sides at height h.
void strip_set_both(unsigned h, rgb_t c);
// Physical LED index (0 = first LED at the electronics).
void strip_set_raw(unsigned i, rgb_t c);
void strip_fill(rgb_t c);

// Applies brightness, switches dead LEDs off, limits the current of everything except the
// crystal and sends the frame (non-blocking DMA, waits only if the previous frame and reset
// gap are not finished yet).
void strip_show(void);

void strip_set_per_side(unsigned per_side);
unsigned strip_per_side(void);
unsigned strip_count(void);

// Crystal: the top `pairs` heights. Effects draw on the body below (heights 0 .. body - 1).
void strip_set_crystal_pairs(unsigned pairs);
unsigned strip_crystal_pairs(void);
unsigned strip_body_height(void);
void strip_set_crystal(rgb_t c);
// One crystal pair, 0 = the lowest.
void strip_set_crystal_pair(unsigned i, rgb_t c);

// Dead heights (hidden LEDs, always off).
void strip_set_dead(unsigned h, bool dead);
bool strip_is_dead(unsigned h);
// Replaces all dead heights from a list of front LED numbers ("31-42, 61-63"). Returns false
// (and changes nothing) on a syntax error.
bool strip_set_dead_list(const char *list);
// Shows dead LEDs anyway (layout test).
void strip_set_show_dead(bool show);

// Prints per side, crystal and dead ranges in the user's numbering.
void strip_print_layout(void);
// 1-based LED number of a logical position, as the user counts.
unsigned strip_led_number(unsigned h, unsigned side);

void strip_set_brightness(uint8_t b);
uint8_t strip_get_brightness(void);

void strip_set_order(color_order_t order);
color_order_t strip_get_order(void);
const char *strip_order_name(color_order_t order);

// Current model (see STRIP_BUDGET_MA): base current and µA per channel at full value.
void strip_set_power_model(uint32_t base_ma, uint32_t ua_per_channel);
uint32_t strip_base_ma(void);
uint32_t strip_ua_per_channel(void);

// Estimated current at the power bank for the last frame (after limiting, base included) and
// whether the limiter scaled it.
uint32_t strip_last_ma(void);
bool strip_last_limited(void);
// Sum of all channel values actually sent in the last frame (full white LED = 3 * 255).
uint32_t strip_last_output_sum(void);

rgb_t rgb_scale(rgb_t c, uint8_t s);
rgb_t rgb_add(rgb_t a, rgb_t b);
rgb_t rgb_lerp(rgb_t a, rgb_t b, uint8_t t);
