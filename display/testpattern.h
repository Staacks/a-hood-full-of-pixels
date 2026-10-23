#pragma once

#include <stdint.h>

typedef enum {
    PATTERN_MAPPING,    // "TOP"/"BOT" labels, coloured borders, corner markers
    PATTERN_BARS,       // vertical colour bars
    PATTERN_GRADIENTS,  // R, G, B, white ramps
    PATTERN_MOVING,     // moving diagonal lines (tearing / refresh check)
    PATTERN_CHECKER,    // 8 px checkerboard (ghosting check)
    PATTERN_ROW_SCAN,   // one lit row walking down (address line check)
    PATTERN_WHITE,      // full white (current measurement)
    PATTERN_BLACK,
    PATTERN_COUNT
} pattern_t;

const char *pattern_name(pattern_t p);

// Draws pattern p for time t_ms into a 128x128 RGB565 canvas. Returns the lit panel
// row for PATTERN_ROW_SCAN (for logging), otherwise -1.
int pattern_draw(pattern_t p, uint32_t t_ms, uint16_t *canvas);
