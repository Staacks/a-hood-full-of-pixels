#include "clips.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "sdcard.h"
#include "speaker.h"

#define RING_BYTES 65536         // 370 ms of audio; power of two
#define PREFILL_BYTES 16384      // read before the clip starts (~8 ms from the card)
#define CHUNK_BYTES 8192         // per clips_poll() call
#define MOUNT_RETRY_US 10000000  // a failed mount blocks for a while: not on every stomp
#define FRAME_BYTES 4            // 16-bit stereo

// Ring buffer: the main loop writes (head), the speaker's encoder reads (tail) in the Bluetooth
// context on the same core. Counters run freely; index = counter % RING_BYTES.
static uint8_t ring[RING_BYTES] __attribute__((aligned(4)));
static volatile uint32_t head, tail;
static volatile bool all_read; // the rest of the clip is in the ring: an empty ring ends it

static FIL file;
static bool file_open, mounted;
static uint32_t data_left; // bytes of the data chunk not read yet
static uint64_t next_mount_us;
static char current[32] = "none";
static uint32_t played, underruns, read_errors;

static unsigned clip_fn(int16_t *lr, unsigned frames, uint32_t pos, void *ctx) {
    (void)pos;
    (void)ctx;
    uint32_t want = frames * FRAME_BYTES, avail = (head - tail) & ~(uint32_t)(FRAME_BYTES - 1);
    uint32_t n = avail < want ? avail : want;
    uint32_t i = tail % RING_BYTES, first = n < RING_BYTES - i ? n : RING_BYTES - i;
    memcpy(lr, &ring[i], first);
    memcpy((uint8_t *)lr + first, ring, n - first);
    tail += n;
    if (n == want)
        return frames;
    if (all_read)
        return n / FRAME_BYTES; // end of the clip
    underruns++; // the card was too slow: silence here (already zeroed), keep going
    return frames;
}

static void close_file(void) {
    if (file_open)
        f_close(&file);
    file_open = false;
}

// Reads up to max bytes from the file into the ring.
static void refill(uint32_t max) {
    while (file_open && max) {
        uint32_t free = RING_BYTES - (head - tail), i = head % RING_BYTES;
        uint32_t n = RING_BYTES - i;
        if (n > free)
            n = free;
        if (n > data_left)
            n = data_left;
        if (n > max)
            n = max;
        if (!n)
            break;
        UINT br;
        if (f_read(&file, &ring[i], n, &br) != FR_OK || br == 0) {
            printf("clips: %s: read error, clip ends here\n", current);
            read_errors++;
            data_left = 0;
            mounted = false; // card gone or swapped: mount again next time
        } else {
            __dmb(); // data before the counter
            head += br;
            data_left -= br;
            max -= br;
        }
        if (!data_left) {
            close_file();
            all_read = true;
        }
    }
}

// Finds the data chunk of a PCM 16-bit 44.1 kHz stereo WAV; leaves the file at its start.
static bool open_wav(const char *path) {
    if (f_open(&file, path, FA_READ) != FR_OK) {
        printf("clips: %s missing\n", path);
        return false;
    }
    file_open = true;
    uint8_t h[16];
    UINT br;
    bool fmt_ok = false;
    bool riff = f_read(&file, h, 12, &br) == FR_OK && br == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4);
    while (riff && f_read(&file, h, 8, &br) == FR_OK && br == 8) {
        uint32_t size = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
        if (!memcmp(h, "data", 4)) {
            if (!fmt_ok)
                break;
            data_left = size;
            return true;
        }
        FSIZE_t next = f_tell(&file) + size + (size & 1);
        if (!memcmp(h, "fmt ", 4)) {
            if (size < 16 || f_read(&file, h, 16, &br) != FR_OK || br != 16)
                break;
            uint16_t format = (uint16_t)(h[0] | h[1] << 8), channels = (uint16_t)(h[2] | h[3] << 8);
            uint32_t rate = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
            uint16_t bits = (uint16_t)(h[14] | h[15] << 8);
            fmt_ok = format == 1 && channels == 2 && rate == SPEAKER_SAMPLE_RATE && bits == 16;
            if (!fmt_ok)
                break;
        }
        if (f_lseek(&file, next) != FR_OK)
            break;
    }
    printf("clips: %s is not a 16-bit 44.1 kHz stereo WAV (tools/make_sounds.py)\n", path);
    close_file();
    return false;
}

void clips_init(void) {
    mounted = sdcard_mount();
    if (!mounted)
        printf("clips: no SD card: no sounds\n");
}

static void stop(void) {
    speaker_stop_sound(); // takes the Bluetooth lock: clip_fn is not running after this
    close_file();
    head = tail = 0;
    all_read = false;
    data_left = 0;
}

uint64_t clips_play(const char *path, uint64_t start_us) {
    stop();
    if (!path)
        return 0;
    if (speaker_state() != SPEAKER_STREAMING) {
        printf("clips: no speaker stream, %s not played\n", path);
        return 0;
    }
    if (!mounted) {
        if (time_us_64() < next_mount_us)
            return 0;
        next_mount_us = time_us_64() + MOUNT_RETRY_US;
        if (!(mounted = sdcard_mount()))
            return 0;
    }
    if (!open_wav(path))
        return 0;
    snprintf(current, sizeof(current), "%s", path);
    refill(PREFILL_BYTES);
    uint64_t actual = speaker_play(clip_fn, NULL, start_us);
    if (!actual) {
        close_file();
        return 0;
    }
    played++;
    return actual;
}

void clips_poll(void) {
    if (file_open)
        refill(CHUNK_BYTES);
}

void clips_stop(void) {
    stop();
    mounted = false;
    next_mount_us = 0;
}

void clips_print_status(void) {
    printf("clips: card %s, last %s, %lu played, %lu underruns (card too slow), %lu read errors\n",
           mounted ? "mounted" : "NOT mounted", current, (unsigned long)played, (unsigned long)underruns,
           (unsigned long)read_errors);
}
