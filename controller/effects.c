#include "effects.h"

#include <math.h>
#include <string.h>

#define MAX_H STRIP_MAX_PER_SIDE

static const char *const names[FX_COUNT] = {
    "dark", "shimmer", "fire", "toxic", "storm", "rainbow", "rain",
    "TEST layout (turn the encoder to move the marker, push to mark a dead height; crystal cyan, dead red)",
    "TEST fold (marker must be at the same height on both sides)",
    "TEST chase (count the LEDs)", "TEST colour order (red, green, blue from the bottom of the front)",
    "TEST full colour (current limiter active, crystal excluded; w = white / red / green / blue)", "off",
};

static unsigned marker;
static unsigned test_colour;

static const struct {
    const char *name;
    rgb_t c;
} test_colours[] = {
    {"white", {255, 255, 255}}, {"red", {255, 0, 0}}, {"green", {0, 255, 0}}, {"blue", {0, 0, 255}}};

const char *fx_cycle_test_colour(void) {
    test_colour = (test_colour + 1) % (sizeof(test_colours) / sizeof(test_colours[0]));
    return test_colours[test_colour].name;
}

const char *fx_name(fx_t fx) { return fx < FX_COUNT ? names[fx] : "?"; }

bool fx_is_test(fx_t fx) { return fx >= FX_TEST_LAYOUT && fx < FX_OFF; }

void fx_set_marker(unsigned h) { marker = h; }

unsigned fx_marker(void) { return marker; }

static uint32_t rng_state = 0x12345678;

static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static uint8_t rnd8(void) { return (uint8_t)(rnd() >> 24); }

static rgb_t hsv(uint8_t h, uint8_t s, uint8_t v) {
    uint8_t region = h / 43, rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
    uint8_t q = (uint8_t)((v * (255 - ((s * rem) >> 8))) >> 8);
    uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) >> 8))) >> 8);
    switch (region) {
    case 0: return (rgb_t){v, t, p};
    case 1: return (rgb_t){q, v, p};
    case 2: return (rgb_t){p, v, t};
    case 3: return (rgb_t){p, q, v};
    case 4: return (rgb_t){t, p, v};
    default: return (rgb_t){v, p, q};
    }
}

static void shimmer(rgb_t colour) {
    static uint8_t level[2][MAX_H];
    unsigned n = strip_body_height();
    for (unsigned s = 0; s < 2; s++) {
        for (unsigned h = 0; h < n; h++) {
            level[s][h] = level[s][h] > 6 ? level[s][h] - 6 : 0;
            if (rnd8() < 4)
                level[s][h] = 255;
            rgb_t base = rgb_scale(colour, 40);
            strip_set(h, s, rgb_add(base, rgb_scale(colour, level[s][h])));
        }
    }
}

// Fire2012 (Mark Kriegsman), one column per side, burning from the bottom up.
static void fire(void) {
    static uint8_t heat[2][MAX_H];
    unsigned n = strip_body_height();
    for (unsigned s = 0; s < 2; s++) {
        uint8_t *hh = heat[s];
        for (unsigned i = 0; i < n; i++) {
            uint8_t cool = (uint8_t)(rnd() % ((55 * 10) / n + 2));
            hh[i] = hh[i] > cool ? hh[i] - cool : 0;
        }
        for (unsigned k = n - 1; k >= 2 && n >= 3; k--)
            hh[k] = (uint8_t)((hh[k - 1] + hh[k - 2] + hh[k - 2]) / 3);
        if (rnd8() < 120) {
            unsigned y = rnd() % 7;
            unsigned v = hh[y] + 160 + rnd() % 96;
            hh[y] = v > 255 ? 255 : (uint8_t)v;
        }
        for (unsigned h = 0; h < n; h++) {
            uint8_t t192 = (uint8_t)(hh[h] * 191 / 255);
            uint8_t ramp = (uint8_t)((t192 & 0x3f) << 2);
            rgb_t c = t192 > 0x80 ? (rgb_t){255, 255, ramp} : t192 > 0x40 ? (rgb_t){255, ramp, 0} : (rgb_t){ramp, 0, 0};
            strip_set(h, s, c);
        }
    }
}

static void toxic(uint32_t t, rgb_t colour) {
    unsigned n = strip_body_height();
    for (unsigned h = 0; h < n; h++) {
        float w = sinf((float)h * 0.35f - (float)t * 0.004f);
        uint8_t v = (uint8_t)(30 + 225 * (w > 0 ? w * w : 0));
        strip_set_both(h, rgb_scale(colour, v));
    }
}

static void storm(uint32_t t, rgb_t colour) {
    static uint32_t flash_until;
    static unsigned flash_lo, flash_hi;
    unsigned n = strip_body_height();
    if (t > flash_until && rnd8() < 3) {
        flash_until = t + 40 + rnd() % 120;
        flash_lo = rnd() % n;
        flash_hi = flash_lo + n / 4 + rnd() % (n / 2 + 1);
    }
    for (unsigned h = 0; h < n; h++) {
        rgb_t c = rgb_scale(colour, (uint8_t)(20 + 15 * sinf((float)h * 0.2f + (float)t * 0.002f)));
        if (t <= flash_until && h >= flash_lo && h <= flash_hi)
            c = rgb_add(c, (rgb_t){200, 180, 255});
        strip_set_both(h, c);
    }
}

static void rainbow(uint32_t t) {
    unsigned n = strip_body_height();
    for (unsigned h = 0; h < n; h++)
        strip_set_both(h, hsv((uint8_t)(h * 256 / n + t / 10), 255, 255));
}

// Digital rain: drops fall from the top of the body to the bottom, bright head, fading trail.
static void rain(rgb_t colour) {
    enum { DROPS = 4 };
    static struct {
        float h, speed, trail;
        bool on;
    } d[2][DROPS];
    const float dt = 0.016f;
    unsigned n = strip_body_height();
    for (unsigned s = 0; s < 2; s++) {
        for (unsigned i = 0; i < DROPS; i++) {
            if (!d[s][i].on) {
                if (rnd8() < 3) {
                    d[s][i].on = true;
                    d[s][i].h = (float)n + 2;
                    d[s][i].speed = 25.0f + (float)(rnd() % 40);    // heights per second
                    d[s][i].trail = 6.0f + (float)(rnd() % 16);
                }
                continue;
            }
            d[s][i].h -= d[s][i].speed * dt;
            if (d[s][i].h + d[s][i].trail < 0)
                d[s][i].on = false;
        }
        for (unsigned h = 0; h < n; h++) {
            float v = 0;
            bool head = false;
            for (unsigned i = 0; i < DROPS; i++) {
                if (!d[s][i].on)
                    continue;
                float above = (float)h - d[s][i].h; // the trail is above the falling head
                if (above >= -0.5f && above < 0.5f)
                    head = true;
                else if (above > 0 && above < d[s][i].trail) {
                    float w = 1.0f - above / d[s][i].trail;
                    v = fmaxf(v, w * w);
                }
            }
            rgb_t c = head ? (rgb_t){140, 255, 150} : rgb_scale(colour, (uint8_t)(8 + 247 * v));
            strip_set(h, s, c);
        }
    }
}

unsigned fx_chase_index(uint32_t t_ms) { return (t_ms / 250) % strip_count(); }

void fx_render(fx_t fx, uint32_t t, rgb_t colour) {
    unsigned n = strip_per_side();
    strip_fill((rgb_t){0, 0, 0});
    switch (fx) {
    case FX_SHIMMER: shimmer(colour); break;
    case FX_FIRE: fire(); break;
    case FX_TOXIC: toxic(t, colour); break;
    case FX_STORM: storm(t, colour); break;
    case FX_RAINBOW: rainbow(t); break;
    case FX_RAIN: rain(colour); break;
    case FX_TEST_LAYOUT: {
        unsigned body = strip_body_height();
        for (unsigned h = 0; h < n; h++)
            strip_set_both(h, h >= body ? (rgb_t){0, 140, 140} : strip_is_dead(h) ? (rgb_t){90, 0, 0}
                                                                                 : (rgb_t){12, 12, 12});
        strip_set_both(marker < n ? marker : n - 1, (rgb_t){255, 255, 255});
        break;
    }
    case FX_TEST_FOLD: {
        unsigned h = (t / 100) % n;
        strip_set(h, 0, (rgb_t){255, 0, 0});
        strip_set(h, 1, (rgb_t){0, 0, 255});
        strip_set_both(0, (rgb_t){0, 80, 0});         // bottom (electronics): green
        strip_set_both(n - 1, (rgb_t){80, 80, 80});   // top (fold): white
        break;
    }
    case FX_TEST_CHASE: strip_set_raw(fx_chase_index(t), (rgb_t){255, 255, 255}); break;
    case FX_TEST_ORDER:
        strip_set_raw(0, (rgb_t){255, 0, 0});
        strip_set_raw(1, (rgb_t){0, 255, 0});
        strip_set_raw(2, (rgb_t){0, 0, 255});
        break;
    case FX_TEST_WHITE: strip_fill(test_colours[test_colour].c); break;
    default: break;
    }
}

int fx_overlay_impact(uint32_t dt, rgb_t colour) {
    const uint32_t duration = 900;
    if (dt >= duration)
        return 0;
    unsigned n = strip_body_height();
    float front = (float)dt / 400.0f * (float)n; // reaches the crystal after 400 ms
    uint8_t fade = (uint8_t)(255 - dt * 255 / duration);
    for (unsigned h = 0; h < n; h++) {
        float d = front - (float)h;
        if (d < 0)
            continue;
        float tail = expf(-d / ((float)n * 0.3f)); // bright front, fading tail
        rgb_t c = rgb_scale(rgb_lerp(colour, (rgb_t){255, 255, 255}, (uint8_t)(tail * 160)), (uint8_t)(tail * fade));
        strip_set_both(h, c);
    }
    return 1;
}

void fx_crystal(uint32_t t, crystal_mode_t mode, const rgb_t stops[CRYSTAL_STOPS]) {
    uint8_t v;
    switch (mode) {
    case CRYSTAL_SELECTED: v = (uint8_t)(190 + 65 * sinf((float)t * 0.008f)); break;
    case CRYSTAL_BLINK: v = (t / 400) & 1 ? 160 : 30; break;
    default: v = 90; break;
    }
    unsigned pairs = strip_crystal_pairs();
    for (unsigned i = 0; i < pairs; i++) {
        // position along the stops in 1/256: 0 = bottom stop, 256 * (STOPS - 1) = top stop
        unsigned pos = pairs > 1 ? i * 256 * (CRYSTAL_STOPS - 1) / (pairs - 1) : 256;
        unsigned k = pos / 256 < CRYSTAL_STOPS - 1 ? pos / 256 : CRYSTAL_STOPS - 2;
        rgb_t c = rgb_lerp(stops[k], stops[k + 1], (uint8_t)(pos - k * 256 > 255 ? 255 : pos - k * 256));
        strip_set_crystal_pair(i, rgb_scale(c, v));
    }
}

void fx_crystal_solid(uint32_t t, crystal_mode_t mode, rgb_t colour) {
    const rgb_t stops[CRYSTAL_STOPS] = {colour, colour, colour};
    fx_crystal(t, mode, stops);
}

void fx_crystal_rainbow(uint32_t t) {
    for (unsigned i = 0; i < strip_crystal_pairs(); i++)
        strip_set_crystal_pair(i, hsv((uint8_t)(t / 4 + i * 40), 255, 255));
}
