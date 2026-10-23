#include "strip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"

// Always send the maximum length: LEDs beyond the configured count get black, so a
// smaller per_side setting also switches off the surplus LEDs of a longer strip.
#define MAX_LEDS (2 * STRIP_MAX_PER_SIDE)
#define US_PER_LED 30  // 24 bits at 800 kHz
#define RESET_US 350   // WS2812E needs >= 300 µs low between frames

static rgb_t pixels[MAX_LEDS];
static uint32_t out[MAX_LEDS];
static unsigned per_side = STRIP_DEFAULT_PER_SIDE;
static unsigned crystal_pairs = STRIP_DEFAULT_CRYSTAL_PAIRS;
static bool dead[STRIP_MAX_PER_SIDE];
static bool show_dead;
// Full brightness: the staff animations stay below the budget (fire ~2 A at most); the limiter
// dims brighter frames (lightning flashes).
static uint8_t brightness = 255;
static color_order_t order = ORDER_GRB;
static uint32_t base_ma = STRIP_DEFAULT_BASE_MA;
static uint32_t ua_per_channel = STRIP_DEFAULT_UA_PER_CHANNEL;
static uint32_t last_ma, last_output_sum;
static bool last_limited;

static PIO pio;
static uint sm, dma_ch;
static uint64_t next_frame_us;

static const char *const order_names[ORDER_COUNT] = {"GRB", "RGB", "BRG", "RBG", "GBR", "BGR"};

bool strip_init(unsigned pin, unsigned n) {
    uint offset;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_program, &pio, &sm, &offset, pin, 1, true))
        return false;

    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_config c = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    const int cycles_per_bit = ws2812_T1 + ws2812_T2 + ws2812_T3;
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (800000.0f * cycles_per_bit));
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    dma_ch = (uint)dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));
    dma_channel_configure(dma_ch, &dc, &pio->txf[sm], out, MAX_LEDS, false);

    strip_set_per_side(n);
    strip_set_dead_list(STRIP_DEFAULT_DEAD);
    strip_show();
    return true;
}

// Data enters at the bottom of the front: front LED i is at height i, back LED 2N-1-i too.
static inline unsigned phys_index(unsigned h, unsigned side) { return side == 0 ? h : 2 * per_side - 1 - h; }

static inline unsigned height_of(unsigned i) { return i < per_side ? i : 2 * per_side - 1 - i; }

unsigned strip_led_number(unsigned h, unsigned side) { return phys_index(h, side) + 1; }

void strip_set(unsigned h, unsigned side, rgb_t c) {
    if (h < per_side && side < 2)
        pixels[phys_index(h, side)] = c;
}

void strip_set_both(unsigned h, rgb_t c) {
    strip_set(h, 0, c);
    strip_set(h, 1, c);
}

void strip_set_raw(unsigned i, rgb_t c) {
    if (i < 2 * per_side)
        pixels[i] = c;
}

void strip_fill(rgb_t c) {
    for (unsigned i = 0; i < 2 * per_side; i++)
        pixels[i] = c;
}

static inline uint32_t pack(rgb_t c) {
    uint32_t r = c.r, g = c.g, b = c.b;
    switch (order) {
    case ORDER_RGB: return (r << 16) | (g << 8) | b;
    case ORDER_BRG: return (b << 16) | (r << 8) | g;
    case ORDER_RBG: return (r << 16) | (b << 8) | g;
    case ORDER_GBR: return (g << 16) | (b << 8) | r;
    case ORDER_BGR: return (b << 16) | (g << 8) | r;
    default:        return (g << 16) | (r << 8) | b;
    }
}

void strip_show(void) {
    // Scale by global brightness. The crystal keeps that brightness; the rest of the strip is
    // limited to the budget left over by the crystal. Dead LEDs are always off.
    const unsigned n = 2 * per_side, crystal_lo = strip_body_height();
    static uint8_t kind[MAX_LEDS]; // 0 off, 1 body, 2 crystal
    uint32_t sum_body = 0, sum_crystal = 0;
    for (unsigned i = 0; i < MAX_LEDS; i++) {
        unsigned h = height_of(i);
        kind[i] = i >= n || (dead[h] && !show_dead) ? 0 : h >= crystal_lo ? 2 : 1;
        uint32_t v = (uint32_t)pixels[i].r + pixels[i].g + pixels[i].b;
        if (kind[i] == 1)
            sum_body += v;
        else if (kind[i] == 2)
            sum_crystal += v;
    }
    // mA of a channel sum at the current brightness
#define SUM_TO_MA(sum) ((uint32_t)((uint64_t)(sum) * brightness * ua_per_channel / (255u * 255u * 1000u)))
    uint32_t ma_crystal = SUM_TO_MA(sum_crystal);
    uint32_t ma_body = SUM_TO_MA(sum_body);
#undef SUM_TO_MA
    uint32_t fixed = base_ma + ma_crystal;
    uint32_t budget = STRIP_BUDGET_MA > fixed ? STRIP_BUDGET_MA - fixed : 0;

    uint32_t scale_crystal = 256 * brightness / 255; // 8.8 fixed point, applied to the raw pixels
    uint32_t scale_body = scale_crystal;
    last_limited = ma_body > budget;
    if (last_limited) {
        scale_body = ma_body ? (uint32_t)((uint64_t)scale_body * budget / ma_body) : 0;
        ma_body = budget;
    }
    last_ma = fixed + ma_body;

    // The DMA may still read the previous frame; wait for it and for the reset gap.
    dma_channel_wait_for_finish_blocking(dma_ch);
    uint32_t out_sum = 0;
    for (unsigned i = 0; i < MAX_LEDS; i++) {
        uint32_t scale = kind[i] == 2 ? scale_crystal : kind[i] == 1 ? scale_body : 0;
        rgb_t c = pixels[i];
        c.r = (uint8_t)(c.r * scale >> 8);
        c.g = (uint8_t)(c.g * scale >> 8);
        c.b = (uint8_t)(c.b * scale >> 8);
        out_sum += (uint32_t)c.r + c.g + c.b;
        out[i] = pack(c) << 8;
    }
    last_output_sum = out_sum;
    while (time_us_64() < next_frame_us)
        tight_loop_contents();
    next_frame_us = time_us_64() + MAX_LEDS * US_PER_LED + RESET_US;
    dma_channel_set_read_addr(dma_ch, out, true);
}

void strip_set_per_side(unsigned n) {
    if (n < 1)
        n = 1;
    if (n > STRIP_MAX_PER_SIDE)
        n = STRIP_MAX_PER_SIDE;
    per_side = n;
    memset(pixels, 0, sizeof(pixels));
}

unsigned strip_per_side(void) { return per_side; }

void strip_set_crystal_pairs(unsigned pairs) { crystal_pairs = pairs < per_side ? pairs : per_side - 1; }
unsigned strip_crystal_pairs(void) { return crystal_pairs; }
unsigned strip_body_height(void) { return per_side - (crystal_pairs < per_side ? crystal_pairs : per_side - 1); }

void strip_set_crystal(rgb_t c) {
    for (unsigned h = strip_body_height(); h < per_side; h++)
        strip_set_both(h, c);
}

void strip_set_crystal_pair(unsigned i, rgb_t c) {
    unsigned h = strip_body_height() + i;
    if (h < per_side)
        strip_set_both(h, c);
}

void strip_set_dead(unsigned h, bool d) {
    if (h < STRIP_MAX_PER_SIDE)
        dead[h] = d;
}

bool strip_is_dead(unsigned h) { return h < STRIP_MAX_PER_SIDE && dead[h]; }

void strip_set_show_dead(bool show) { show_dead = show; }

bool strip_set_dead_list(const char *list) {
    bool d[STRIP_MAX_PER_SIDE] = {false};
    const char *p = list;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',')
            p++;
        if (!*p)
            break;
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            return false;
        p = end;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '-') {
            p++;
            b = strtol(p, &end, 10);
            if (end == p)
                return false;
            p = end;
        }
        if (a < 1 || b < a || b > STRIP_MAX_PER_SIDE)
            return false;
        for (long k = a; k <= b; k++)
            d[k - 1] = true;
    }
    memcpy(dead, d, sizeof(dead));
    return true;
}

void strip_print_layout(void) {
    unsigned n = per_side, lo = strip_body_height();
    printf("layout: %u LEDs per side: front #1 (bottom) .. #%u (top), back #%u (top) .. #%u (bottom)\n", n, n,
           n + 1, 2 * n);
    printf("        crystal: top %u pairs = front #%u-#%u, back #%u-#%u\n", n - lo, lo + 1, n, n + 1, 2 * n - lo);
    printf("        dead (front numbers, partners on the back too): ");
    bool any = false;
    for (unsigned h = 0; h < n; h++) {
        if (!dead[h] || (h > 0 && dead[h - 1]))
            continue;
        unsigned e = h;
        while (e + 1 < n && dead[e + 1])
            e++;
        printf(any ? ", %u" : "%u", h + 1);
        if (e > h)
            printf("-%u", e + 1);
        any = true;
    }
    printf(any ? "\n" : "none\n");
}
unsigned strip_count(void) { return 2 * per_side; }
void strip_set_brightness(uint8_t b) { brightness = b; }
uint8_t strip_get_brightness(void) { return brightness; }
void strip_set_order(color_order_t o) { order = o < ORDER_COUNT ? o : ORDER_GRB; }
color_order_t strip_get_order(void) { return order; }
const char *strip_order_name(color_order_t o) { return o < ORDER_COUNT ? order_names[o] : "?"; }
void strip_set_power_model(uint32_t base, uint32_t ua) {
    base_ma = base;
    ua_per_channel = ua;
}
uint32_t strip_base_ma(void) { return base_ma; }
uint32_t strip_ua_per_channel(void) { return ua_per_channel; }
uint32_t strip_last_ma(void) { return last_ma; }
uint32_t strip_last_output_sum(void) { return last_output_sum; }
bool strip_last_limited(void) { return last_limited; }

rgb_t rgb_scale(rgb_t c, uint8_t s) {
    return (rgb_t){(uint8_t)(c.r * s / 255), (uint8_t)(c.g * s / 255), (uint8_t)(c.b * s / 255)};
}

static inline uint8_t add_sat(uint8_t a, uint8_t b) { return a + b > 255 ? 255 : (uint8_t)(a + b); }

rgb_t rgb_add(rgb_t a, rgb_t b) { return (rgb_t){add_sat(a.r, b.r), add_sat(a.g, b.g), add_sat(a.b, b.b)}; }

rgb_t rgb_lerp(rgb_t a, rgb_t b, uint8_t t) {
    return (rgb_t){(uint8_t)(a.r + ((b.r - a.r) * t) / 255), (uint8_t)(a.g + ((b.g - a.g) * t) / 255),
                   (uint8_t)(a.b + ((b.b - a.b) * t) / 255)};
}
