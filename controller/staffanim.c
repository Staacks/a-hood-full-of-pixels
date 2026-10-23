#include "staffanim.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "pico/stdlib.h"
#include "strip.h"

#define HEADER_BYTES 512
#define MAX_FRAME_BYTES (2 * STRIP_MAX_PER_SIDE * 3)

static FIL file;
static bool file_open, active, in_intro;
static char loop_path[32];
static char current[32] = "none";
static uint64_t start_us;
static uint32_t fps, count, frame_bytes;
static uint32_t errors;

// The two frames around the current time, blended per strip frame.
static uint8_t frame_a[MAX_FRAME_BYTES], frame_b[MAX_FRAME_BYTES];
static uint32_t index_a, index_b; // UINT32_MAX: empty

static void close_file(void) {
    if (file_open)
        f_close(&file);
    file_open = false;
}

static bool open_file(const char *path) {
    close_file();
    index_a = index_b = UINT32_MAX;
    if (!path || !*path || f_open(&file, path, FA_READ) != FR_OK)
        return false;
    uint8_t h[16];
    UINT br;
    bool ok = f_read(&file, h, sizeof(h), &br) == FR_OK && br == sizeof(h) && !memcmp(h, "PLED", 4);
    uint16_t leds = (uint16_t)(h[6] | h[7] << 8), rate = (uint16_t)(h[10] | h[11] << 8);
    uint32_t n = (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
    if (!ok || leds != strip_count() || rate == 0 || rate > 100 || n == 0) {
        printf("staffanim: %s: not a .led file for %u LEDs\n", path, strip_count());
        f_close(&file);
        return false;
    }
    file_open = true;
    snprintf(current, sizeof(current), "%s", path);
    fps = rate;
    count = n;
    frame_bytes = leds * 3u;
    return true;
}

static bool load(uint32_t index, uint8_t *dst) {
    FSIZE_t pos = HEADER_BYTES + (FSIZE_t)index * frame_bytes;
    UINT br;
    if ((f_tell(&file) != pos && f_lseek(&file, pos) != FR_OK) || f_read(&file, dst, frame_bytes, &br) != FR_OK ||
        br != frame_bytes) {
        printf("staffanim: %s: read error\n", current);
        errors++;
        return false;
    }
    return true;
}

// Makes frame_a = frame ia and frame_b = frame ib, reading only what is missing.
static bool fetch(uint32_t ia, uint32_t ib) {
    if (index_a != ia) {
        if (index_b == ia) {
            memcpy(frame_a, frame_b, frame_bytes);
            index_b = UINT32_MAX;
        } else if (!load(ia, frame_a)) {
            return false;
        }
        index_a = ia;
    }
    if (index_b != ib) {
        if (!load(ib, frame_b))
            return false;
        index_b = ib;
    }
    return true;
}

bool staffanim_play(const char *intro, const char *loop, uint64_t at_us) {
    staffanim_stop();
    snprintf(loop_path, sizeof(loop_path), "%s", loop ? loop : "");
    in_intro = open_file(intro);
    if (!in_intro && !open_file(loop))
        return false;
    start_us = at_us;
    active = true;
    return true;
}

void staffanim_stop(void) {
    active = false;
    close_file();
}

bool staffanim_in_intro(void) { return active && in_intro; }

bool staffanim_render(uint64_t now_us) {
    if (!active || now_us < start_us)
        return false;
    uint64_t pos = (now_us - start_us) * fps * 256 / 1000000; // in 1/256 frames
    uint32_t f = (uint32_t)(pos >> 8), frac = (uint32_t)(pos & 255);
    if (f >= count) {
        uint64_t length_us = (uint64_t)count * 1000000 / fps;
        if (in_intro) { // the loop starts where the intro ended
            start_us += length_us;
            in_intro = false;
            if (!open_file(loop_path)) {
                active = false; // no loop: the caller's effect takes over
                return false;
            }
            return staffanim_render(now_us);
        }
        start_us += (uint64_t)(f / count) * length_us;
        f %= count;
    }
    // next frame: wraps in the loop; the intro's last frame is held
    uint32_t next = f + 1 < count ? f + 1 : in_intro ? f : 0;
    if (!fetch(f, next)) {
        staffanim_stop();
        return false;
    }
    for (unsigned i = 0, o = 0; i < frame_bytes / 3; i++, o += 3) {
        rgb_t c = {(uint8_t)((frame_a[o] * (256 - frac) + frame_b[o] * frac) >> 8),
                   (uint8_t)((frame_a[o + 1] * (256 - frac) + frame_b[o + 1] * frac) >> 8),
                   (uint8_t)((frame_a[o + 2] * (256 - frac) + frame_b[o + 2] * frac) >> 8)};
        strip_set_raw(i, c);
    }
    return true;
}

void staffanim_print_status(void) {
    printf("staff animation: %s, %s (%s), %lu read errors\n", active ? "playing" : "off (built-in effect)", current,
           in_intro ? "intro" : "loop", (unsigned long)errors);
}
