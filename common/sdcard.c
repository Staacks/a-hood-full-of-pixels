#include "sdcard.h"

#include <stdio.h>

#include "f_util.h"
#include "ff.h"
#include "hardware/spi.h"
#include "hw_config.h"
#include "pico/stdlib.h"

#define TEST_FILE "0:/sdtest.bin"
#define TEST_SIZE (4u * 1024 * 1024)
#define CHUNK 32768 // one RGB565 128x128 video frame
#define FRAME_BUDGET_US 40000 // 25 fps

// --- Hardware configuration, required by the library (sd_get_num / sd_get_by_num) ---

static spi_t spi = {
    .hw_inst = spi0,
    .miso_gpio = 16,
    .mosi_gpio = 19,
    .sck_gpio = 18,
    .baud_rate = SDCARD_DEFAULT_HZ,
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 17,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) { return num == 0 ? &sd_card : NULL; }

// --- Mount and clock ---

static bool mounted;

void sdcard_set_clock(uint32_t hz) { spi.baud_rate = hz; }

uint32_t sdcard_get_clock(void) {
    // Actual rate (clk_peri / even divider) once the SPI is set up, else the requested one.
    return mounted ? spi_get_baudrate(spi.hw_inst) : spi.baud_rate;
}

void sdcard_cycle_clock(void) {
    uint32_t hz = sdcard_get_clock();
    hz = hz > 30000000 ? 12500000 : hz > 20000000 ? 37500000 : 25000000;
    sdcard_set_clock(hz);
    printf("SD SPI clock %.1f MHz from the next test on\n", hz / 1e6);
}

bool sdcard_mount(void) {
    if (mounted) {
        f_unmount("0:");
        mounted = false;
    }
    // Force a full card init (new card, new clock) instead of trusting the old state.
    sd_card.state.m_Status |= STA_NOINIT;
    FRESULT fr = f_mount(&sd_card.state.fatfs, "0:", 1);
    if (fr != FR_OK) {
        printf("sd: mount failed: %s (%d)\n", FRESULT_str(fr), fr);
        return false;
    }
    mounted = true;
    return true;
}

// --- Test ---

static uint8_t buf[CHUNK] __attribute__((aligned(4)));

static void fill(uint32_t offset, uint32_t seed) {
    uint32_t *w = (uint32_t *)buf;
    for (uint32_t i = 0; i < CHUNK / 4; i++)
        w[i] = ((offset / 4 + i) * 2654435761u) ^ seed;
}

static uint32_t count_bad_words(uint32_t offset, uint32_t seed) {
    const uint32_t *w = (const uint32_t *)buf;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < CHUNK / 4; i++)
        bad += w[i] != (((offset / 4 + i) * 2654435761u) ^ seed);
    return bad;
}

static void print_card_info(void) {
    const uint8_t *cid = sd_card.state.CID;
    printf("card: SDHC/SDXC, %.1f GB, manufacturer 0x%02x, OEM %c%c, product %c%c%c%c%c, rev %u.%u, "
           "date %u/%02u\n",
           sd_card.state.sectors / 2097152.0, cid[0], cid[1], cid[2], cid[3], cid[4], cid[5], cid[6], cid[7],
           cid[8] >> 4, cid[8] & 15, 2000 + ((cid[13] & 15) << 4 | cid[14] >> 4), cid[14] & 15);
}

static void print_fs_info(void) {
    FATFS *fs = &sd_card.state.fatfs;
    const char *type = fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_EXFAT ? "exFAT"
                     : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12";
    DWORD free_clusters = 0;
    FATFS *fsp;
    f_getfree("0:", &free_clusters, &fsp);
    printf("fs: %s, cluster %u KB, %.1f GB free%s\n", type, fs->csize / 2,
           (double)free_clusters * fs->csize / 2097152.0,
           fs->fs_type == FS_FAT32 ? "" : "  (expected FAT32; reformat with mkfs.vfat -F 32)");
}

static void list_root(void) {
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, "0:/") != FR_OK)
        return;
    printf("root directory:\n");
    int n = 0;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (n++ < 20)
            printf("  %-24s %10lu%s\n", fi.fname, (unsigned long)fi.fsize, (fi.fattrib & AM_DIR) ? "  <dir>" : "");
    }
    if (n > 20)
        printf("  ... %d entries\n", n);
    else if (n == 0)
        printf("  (empty)\n");
    f_closedir(&dir);
}

static bool write_test_file(uint32_t seed) {
    FIL f;
    FRESULT fr = f_open(&f, TEST_FILE, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("write: open failed: %s\n", FRESULT_str(fr));
        return false;
    }
    // Contiguous, like the media files on a freshly formatted card.
    bool contiguous = f_expand(&f, TEST_SIZE, 1) == FR_OK;
    uint64_t t0 = time_us_64();
    for (uint32_t off = 0; off < TEST_SIZE; off += CHUNK) {
        fill(off, seed);
        UINT bw;
        fr = f_write(&f, buf, CHUNK, &bw);
        if (fr != FR_OK || bw != CHUNK) {
            printf("write: failed at %lu: %s\n", (unsigned long)off, FRESULT_str(fr));
            f_close(&f);
            return false;
        }
    }
    fr = f_close(&f);
    uint64_t dt = time_us_64() - t0;
    if (fr != FR_OK) {
        printf("write: close failed: %s\n", FRESULT_str(fr));
        return false;
    }
    printf("write: %u KB in %.2f s = %.2f MB/s%s\n", TEST_SIZE / 1024, dt / 1e6, TEST_SIZE / (double)dt,
           contiguous ? "" : "  (not contiguous: card full or fragmented)");
    return true;
}

static bool read_test_file(uint32_t seed) {
    FIL f;
    FRESULT fr = f_open(&f, TEST_FILE, FA_READ);
    if (fr != FR_OK) {
        printf("read: open failed: %s\n", FRESULT_str(fr));
        return false;
    }
    uint32_t chunks = 0, errors = 0, bad_words = 0, slow = 0;
    uint64_t total = 0, worst = 0;
    for (uint32_t off = 0; off < TEST_SIZE; off += CHUNK) {
        UINT br;
        uint64_t t0 = time_us_64();
        fr = f_read(&f, buf, CHUNK, &br);
        uint64_t dt = time_us_64() - t0;
        total += dt;
        if (dt > worst)
            worst = dt;
        if (dt > FRAME_BUDGET_US)
            slow++;
        chunks++;
        if (fr != FR_OK || br != CHUNK) {
            errors++;
            if (f_lseek(&f, off + CHUNK) != FR_OK) // skip the chunk, like the player will
                break;
            continue;
        }
        bad_words += count_bad_words(off, seed);
    }
    f_close(&f);
    printf("read: %lu chunks of 32 KB, %.2f MB/s, avg %.1f ms, worst %.1f ms per chunk "
           "(25 fps budget 40 ms, %lu over)\n",
           (unsigned long)chunks, (double)chunks * CHUNK / (double)total, total / 1000.0 / chunks, worst / 1000.0,
           (unsigned long)slow);
    printf("read: %lu read errors, %lu corrupt words\n", (unsigned long)errors, (unsigned long)bad_words);
    return errors == 0 && bad_words == 0;
}

void sdcard_test(void) {
    printf("\n--- SD card test ---\n");
    uint64_t t0 = time_us_64();
    if (!sdcard_mount()) {
        printf("SD FAIL: no card or not readable. Check: card inserted, wiring MISO=GP16, CS=GP17, "
               "SCK=GP18, MOSI=GP19, VCC=3V3 with 10 uF, GND, FAT32 formatted.\n"
               "With long breadboard wires try a lower SD clock.\n");
        return;
    }
    printf("init + mount: %.0f ms, SPI clock %.2f MHz\n", (time_us_64() - t0) / 1000.0, sdcard_get_clock() / 1e6);
    print_card_info();
    print_fs_info();
    list_root();

    uint32_t seed = (uint32_t)time_us_64();
    bool ok = write_test_file(seed) && read_test_file(seed);
    f_unlink(TEST_FILE);

    double mbps_needed = 25.0 * CHUNK / 1e6;
    printf("%s (video needs %.2f MB/s sustained, target > 1 MB/s)\n", ok ? "SD OK" : "SD FAIL", mbps_needed);
}
