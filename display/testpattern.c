#include "testpattern.h"

#include <string.h>

#include "hub75.h"

#define W HUB75_WIDTH
#define H HUB75_HEIGHT

#define rgb565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define WHITE rgb565(255, 255, 255)
#define RED rgb565(255, 0, 0)
#define GREEN rgb565(0, 255, 0)
#define BLUE rgb565(0, 0, 255)
#define YELLOW rgb565(255, 255, 0)

static const char *const names[PATTERN_COUNT] = {
    "mapping (TOP/BOT labels)", "colour bars", "gradients", "moving lines",
    "checkerboard", "row scan", "full white", "black",
};

const char *pattern_name(pattern_t p) { return p < PATTERN_COUNT ? names[p] : "?"; }

// 5x7 glyphs, one byte per row, bit 4 = leftmost column.
static const struct {
    char c;
    uint8_t rows[7];
} font[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}}, {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
};

static void fill_rect(uint16_t *cv, int x0, int y0, int w, int h, uint16_t c) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < W && y >= 0 && y < H)
                cv[y * W + x] = c;
}

static void draw_char(uint16_t *cv, int x0, int y0, char ch, int scale, uint16_t c) {
    for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); i++) {
        if (font[i].c != ch)
            continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (font[i].rows[row] & (0x10 >> col))
                    fill_rect(cv, x0 + col * scale, y0 + row * scale, scale, scale, c);
        return;
    }
}

static void draw_text(uint16_t *cv, int x0, int y0, const char *s, int scale, uint16_t c) {
    for (; *s; s++, x0 += 6 * scale)
        draw_char(cv, x0, y0, *s, scale, c);
}

static void draw_border(uint16_t *cv, int y0, uint16_t c) {
    fill_rect(cv, 0, y0, W, 1, c);
    fill_rect(cv, 0, y0 + 63, W, 1, c);
    fill_rect(cv, 0, y0, 1, 64, c);
    fill_rect(cv, W - 1, y0, 1, 64, c);
}

int pattern_draw(pattern_t p, uint32_t t_ms, uint16_t *cv) {
    memset(cv, 0, W * H * sizeof(uint16_t));

    switch (p) {
    case PATTERN_MAPPING:
        // Top panel: red border, "TOP". Bottom panel: green border, "BOT".
        // Logical top-left corner: white square; top-right: yellow; bottom-left: blue.
        draw_border(cv, 0, RED);
        draw_border(cv, 64, GREEN);
        draw_text(cv, 20, 18, "TOP", 5, RED);
        draw_text(cv, 20, 82, "BOT", 5, GREEN);
        fill_rect(cv, 2, 2, 8, 8, WHITE);
        fill_rect(cv, W - 10, 2, 8, 8, YELLOW);
        fill_rect(cv, 2, H - 10, 8, 8, BLUE);
        break;

    case PATTERN_BARS: {
        static const uint16_t bars[8] = {
            rgb565(255, 255, 255), rgb565(255, 255, 0), rgb565(0, 255, 255), rgb565(0, 255, 0),
            rgb565(255, 0, 255),   rgb565(255, 0, 0),   rgb565(0, 0, 255),   0,
        };
        for (int b = 0; b < 8; b++)
            fill_rect(cv, b * 16, 0, 16, H, bars[b]);
        break;
    }

    case PATTERN_GRADIENTS:
        for (int x = 0; x < W; x++) {
            uint8_t v = (uint8_t)(x * 255 / (W - 1));
            fill_rect(cv, x, 0, 1, 32, rgb565(v, 0, 0));
            fill_rect(cv, x, 32, 1, 32, rgb565(0, v, 0));
            fill_rect(cv, x, 64, 1, 32, rgb565(0, 0, v));
            fill_rect(cv, x, 96, 1, 32, rgb565(v, v, v));
        }
        break;

    case PATTERN_MOVING: {
        int off = (int)(t_ms / 20);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (((x + y + off) & 31) < 3)
                    cv[y * W + x] = ((x + off) & 64) ? rgb565(0, 160, 255) : rgb565(255, 80, 0);
        break;
    }

    case PATTERN_CHECKER:
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (((x >> 3) ^ (y >> 3)) & 1)
                    cv[y * W + x] = WHITE;
        break;

    case PATTERN_ROW_SCAN: {
        // Walks one row per 250 ms through the 64 rows of both panels at once, so a
        // stuck address line shows up as a skipped or doubled row.
        int row = (int)((t_ms / 250) % 64);
        fill_rect(cv, 0, row, W, 1, RED);
        fill_rect(cv, 0, 64 + row, W, 1, GREEN);
        return row;
    }

    case PATTERN_WHITE:
        fill_rect(cv, 0, 0, W, H, WHITE);
        break;

    default:
        break;
    }
    return -1;
}
