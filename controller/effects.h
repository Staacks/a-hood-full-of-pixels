#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "strip.h"

typedef enum {
    // Scene loops (procedural until .led animations exist)
    FX_DARK,    // staff off (base scene; the crystal still shows a selection)
    FX_SHIMMER,
    FX_FIRE,
    FX_TOXIC,
    FX_STORM,
    FX_RAINBOW,
    FX_RAIN,    // glyph rain: drops falling down both sides
    // Bring-up tests
    FX_TEST_LAYOUT, // marker moved with the encoder; crystal cyan, dead LEDs red
    FX_TEST_FOLD,   // marker walks up both sides at the same height
    FX_TEST_CHASE,  // one LED walks through the physical indices (count LEDs)
    FX_TEST_ORDER,  // first LEDs pure red, green, blue (colour order)
    FX_TEST_WHITE,  // full white / red / green / blue (current measurement, limiter active)
    FX_OFF,
    FX_COUNT
} fx_t;

const char *fx_name(fx_t fx);
bool fx_is_test(fx_t fx);

// Renders one frame. Scene effects draw on the body (below the crystal) and leave the crystal
// dark; tests draw on the whole strip. colour tints the scene effects.
void fx_render(fx_t fx, uint32_t t_ms, rgb_t colour);

// Impact wave rising from the bottom to the crystal, drawn on top of the current frame.
// Returns 0 when finished.
int fx_overlay_impact(uint32_t dt_ms, rgb_t colour);

typedef enum {
    CRYSTAL_IDLE,     // nothing selected: the current scene's colour, steady and dim
    CRYSTAL_SELECTED, // a scene is selected (a stomp fires it): its colour, breathing bright
    CRYSTAL_BLINK,    // notices (speaker pairing): slow blink
} crystal_mode_t;

// Draws the crystal (the top-most pairs): a gradient through three colours, bottom to top.
#define CRYSTAL_STOPS 3
void fx_crystal(uint32_t t_ms, crystal_mode_t mode, const rgb_t stops[CRYSTAL_STOPS]);
// One colour on all pairs.
void fx_crystal_solid(uint32_t t_ms, crystal_mode_t mode, rgb_t colour);
// A rainbow running through the crystal (the fun playlist was chosen).
void fx_crystal_rainbow(uint32_t t_ms);

// Colour of FX_TEST_WHITE: cycles white, red, green, blue; returns the new colour's name.
const char *fx_cycle_test_colour(void);

// Marker height of FX_TEST_LAYOUT.
void fx_set_marker(unsigned h);
unsigned fx_marker(void);

// Physical index lit by FX_TEST_CHASE at time t_ms.
unsigned fx_chase_index(uint32_t t_ms);
