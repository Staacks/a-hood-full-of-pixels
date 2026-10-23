#include "player.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hub75.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "sdcard.h"
#include "testpattern.h"

#define HEADER_BYTES 512
#define FRAME_BYTES (HUB75_WIDTH * HUB75_HEIGHT * 2)
#define MOUNT_RETRY_US 2000000
#define MAX_READ_ERRORS 3 // consecutive, then remount
#define SWITCH_LEAD_US 50000 // a scheduled scene: keep playing the old one until this long before

typedef struct {
    const char *name;
    const char *intro; // NULL: none
    const char *loop;
} scene_t;

// Same scene numbers as the controller (controller/main.c). Missing intros are skipped.
static const scene_t scenes[PLAYER_SCENES] = {
    {"eyes (base)", "0:/eyes_intro.vid", "0:/eyes.vid"},
    {"fire skull", "0:/fireskull_intro.vid", "0:/fireskull.vid"},
    {"matrix", "0:/matrix_intro.vid", "0:/matrix.vid"},
    {"lightning skull", "0:/lightning_intro.vid", "0:/lightning.vid"},
    {"rainbow fire skull", "0:/rainbow_intro.vid", "0:/rainbow.vid"},
    {"youtube", "0:/youtube_intro.vid", "0:/youtube.vid"},
    {"darth maul", "0:/maul_intro.vid", "0:/maul.vid"},
    {"creeper", "0:/creeper_intro.vid", "0:/creeper.vid"},
    {"rick astley", "0:/rick_intro.vid", "0:/rick.vid"},
    {"pumpkin", "0:/pumpkin_intro.vid", "0:/pumpkin.vid"},
    {"family", "0:/family_intro.vid", "0:/family.vid"}, // private, tools/family.py
    {"me", "0:/me_intro.vid", "0:/me.vid"},
};

// Requests from core 0 (single writer each; core 1 compares sequence numbers).
static volatile int req_scene;
static volatile uint32_t req_start_us;
static volatile uint32_t req_seq;
static volatile int req_pattern = -1;
static volatile bool req_sd_test;
static volatile uint32_t alive;

static uint16_t canvas[HUB75_WIDTH * HUB75_HEIGHT] __attribute__((aligned(4)));

// Core 1 state (read by player_print_status for diagnostics only).
static int scene;
static bool in_intro;
static FIL file;
static volatile bool file_open;
static bool mounted;
static uint32_t fps, frame_count;
static uint32_t shown, skipped, read_errors, remounts;
static uint32_t read_us_max, show_us_max, read_us_sum, show_us_sum, timed_frames, timed_shows;
static const char *last_error = "none";

// Built-in fallback while there is no card or no file: dim eyes above the slit.
static void draw_fallback(void) {
    memset(canvas, 0, sizeof(canvas));
    for (int e = 0; e < 2; e++) {
        int cx = e ? 81 : 46, cy = 57;
        for (int y = cy - 3; y <= cy + 3; y++)
            for (int x = cx - 9; x <= cx + 9; x++) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx * 9 + dy * dy * 81 <= 81 * 9)
                    canvas[y * HUB75_WIDTH + x] = (uint16_t)((31 << 11) | (24 << 5) | 2); // orange
            }
    }
}

static void close_file(void) {
    if (file_open)
        f_close(&file);
    file_open = false;
}

static bool open_file(const char *path) {
    close_file();
    FRESULT fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        printf("player: %s: open failed (%d)\n", path, fr);
        last_error = "file missing";
        if (fr == FR_NOT_READY || fr == FR_DISK_ERR || fr == FR_INVALID_OBJECT)
            mounted = false; // card gone or swapped: mount again
        return false;
    }
    uint8_t h[HEADER_BYTES];
    UINT br;
    fr = f_read(&file, h, sizeof(h), &br);
    uint16_t w = (uint16_t)(h[6] | h[7] << 8), hh = (uint16_t)(h[8] | h[9] << 8);
    uint16_t rate = (uint16_t)(h[10] | h[11] << 8), fmt = (uint16_t)(h[16] | h[17] << 8);
    uint32_t count = (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
    if (fr != FR_OK || br != sizeof(h) || memcmp(h, "PVID", 4) != 0 || w != HUB75_WIDTH || hh != HUB75_HEIGHT ||
        fmt != 1 || rate == 0 || rate > 60 || count == 0) {
        printf("player: %s: not a 128x128 RGB565 .vid file\n", path);
        last_error = "bad file";
        f_close(&file);
        return false;
    }
    file_open = true;
    fps = rate;
    frame_count = count;
    printf("player: %s: %lu frames at %lu fps\n", path, (unsigned long)count, (unsigned long)rate);
    return true;
}

// Opens the scene's intro (or loop). Returns false if neither can be opened.
static bool start_scene(int s) {
    scene = s;
    in_intro = scenes[s].intro != NULL;
    if (in_intro && open_file(scenes[s].intro))
        return true;
    in_intro = false;
    return open_file(scenes[s].loop);
}

static bool read_frame(uint32_t index) {
    FSIZE_t pos = HEADER_BYTES + (FSIZE_t)index * FRAME_BYTES;
    if (f_tell(&file) != pos && f_lseek(&file, pos) != FR_OK)
        return false;
    UINT br;
    return f_read(&file, canvas, FRAME_BYTES, &br) == FR_OK && br == FRAME_BYTES;
}

static void show(void) {
    uint64_t t0 = time_us_64();
    hub75_show(canvas);
    uint32_t dt = (uint32_t)(time_us_64() - t0);
    show_us_sum += dt;
    timed_shows++;
    if (dt > show_us_max)
        show_us_max = dt;
}

static void core1_main(void) {
    multicore_lockout_victim_init(); // flash writes on core 0 (BTstack) pause this core safely
    uint32_t seen_seq = ~0u;
    uint32_t start_us = 0, last_index = ~0u, errors_in_row = 0;
    uint64_t next_try_us = 0;
    bool waiting_start = false;

    while (true) {
        alive++;
        if (req_sd_test) {
            close_file();
            sdcard_test();
            mounted = false; // the test leaves the card in its own state: mount again
            req_sd_test = false;
        }

        int pattern = req_pattern;
        if (pattern >= 0) {
            static int last_row = -1;
            int row = pattern_draw((pattern_t)pattern, to_ms_since_boot(get_absolute_time()), canvas);
            if (row >= 0 && row != last_row)
                printf("row scan: lit panel row %d (address %d, %s half of the scan)\n", row, row % 32,
                       row < 32 ? "R1/G1/B1" : "R2/G2/B2");
            last_row = row;
            show();
            seen_seq = ~0u; // back from the pattern: restart the scene
            continue;
        }

        // New scene requested. A start time in the future: the old scene keeps playing until
        // just before it, then the new file is opened and its first frame waits for the start.
        uint32_t seq = req_seq, req_start = req_start_us;
        if (seq != seen_seq &&
            !(file_open && req_start && (int32_t)(req_start - time_us_32()) > SWITCH_LEAD_US)) {
            seen_seq = seq;
            scene = req_scene;
            start_us = req_start ? req_start : time_us_32();
            waiting_start = true;
            close_file();
            next_try_us = 0;
        }

        // No file: (re)mount and open, every 2 s; meanwhile the built-in eyes.
        if (!file_open) {
            if (time_us_64() >= next_try_us) {
                next_try_us = time_us_64() + MOUNT_RETRY_US;
                if (!mounted && !(mounted = sdcard_mount()))
                    last_error = "no SD card";
                if (mounted && start_scene(scene))
                    last_index = ~0u;
            }
            if (!file_open) {
                draw_fallback();
                show();
                continue;
            }
        }

        // Which frame is due? Before the start time the current picture stays.
        int32_t since = (int32_t)(time_us_32() - start_us);
        if (waiting_start && since < 0) {
            sleep_us(since < -2000 ? 1000 : (uint32_t)-since);
            continue;
        }
        waiting_start = false;
        uint32_t index = (uint32_t)((uint64_t)since * fps / 1000000u);
        if (index >= frame_count) {
            uint32_t loops = index / frame_count;
            start_us += (uint32_t)((uint64_t)loops * frame_count * 1000000u / fps);
            last_index = ~0u;
            if (in_intro) { // intro done: the loop starts where the intro ended
                in_intro = false;
                open_file(scenes[scene].loop);
                continue;
            }
            index -= loops * frame_count;
        }
        if (index == last_index) {
            sleep_us(500);
            continue;
        }
        if (last_index != ~0u && index > last_index + 1)
            skipped += index - last_index - 1;

        uint64_t t0 = time_us_64();
        bool ok = read_frame(index);
        uint32_t dt = (uint32_t)(time_us_64() - t0);
        last_index = index; // on error: skip this frame, keep the last picture
        if (!ok) {
            read_errors++;
            last_error = "read error";
            if (++errors_in_row >= MAX_READ_ERRORS) {
                printf("player: %lu read errors in a row, remounting\n", (unsigned long)errors_in_row);
                close_file();
                mounted = false;
                remounts++;
                errors_in_row = 0;
            }
            continue;
        }
        errors_in_row = 0;
        read_us_sum += dt;
        if (dt > read_us_max)
            read_us_max = dt;
        show();
        timed_frames++;
        shown++;
    }
}

void player_init(void) {
    req_scene = 0;
    req_seq = 1;
    multicore_launch_core1(core1_main);
}

void player_play(int s, uint32_t start_us) {
    if (s < 0 || s >= PLAYER_SCENES)
        return;
    req_pattern = -1;
    req_scene = s;
    req_start_us = start_us;
    __dmb();
    req_seq++;
}

int player_scene(void) { return req_scene; }

bool player_has_video(void) { return file_open; }

const char *player_scene_name(int s) { return s >= 0 && s < PLAYER_SCENES ? scenes[s].name : "?"; }

void player_show_pattern(int pattern) { req_pattern = pattern; }

void player_sd_test(void) { req_sd_test = true; }

uint32_t player_alive(void) { return alive; }

bool player_busy(void) { return req_sd_test; }

void player_print_status(void) {
    if (req_pattern >= 0) {
        printf("player: test pattern %d\n", req_pattern);
        return;
    }
    printf("player: scene %d %s (%s), %s, %lu fps, %lu frames\n", scene, scenes[scene].name,
           in_intro ? "intro" : "loop", file_open ? "playing" : mounted ? "file missing: fallback eyes"
                                                                       : "no SD card: fallback eyes",
           (unsigned long)fps, (unsigned long)frame_count);
    uint32_t n = timed_frames ? timed_frames : 1, ns = timed_shows ? timed_shows : 1;
    printf("player: %lu shown, %lu skipped (late), %lu read errors, %lu remounts, last error: %s\n",
           (unsigned long)shown, (unsigned long)skipped, (unsigned long)read_errors, (unsigned long)remounts,
           last_error);
    printf("player: read avg %lu / max %lu us, show avg %lu / max %lu us per frame (budget %lu us)\n",
           (unsigned long)(read_us_sum / n), (unsigned long)read_us_max, (unsigned long)(show_us_sum / ns),
           (unsigned long)show_us_max, (unsigned long)(fps ? 1000000 / fps : 0));
    read_us_max = show_us_max = 0;
    read_us_sum = show_us_sum = 0;
    timed_frames = timed_shows = 0;
}
