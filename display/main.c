// Display Pico firmware: plays the scene videos from SD (player.c, core 1), shows test patterns
// on request over USB serial, and is the BLE link peripheral for the controller (clock sync
// pings, scene events, heartbeats).

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hub75.h"
#include "link.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "player.h"
#include "sdcard.h"
#include "testpattern.h"

// Set in watchdog scratch before a reboot to request the FM6126A init at boot.
#define SCRATCH_FM6126A_MAGIC 0xf6126a00u
// A hang of either core reboots into the base scene: core 0 feeds the watchdog only while
// core 1 (video player) makes progress.
#define WATCHDOG_MS 5000
#define CORE1_STALL_US 3000000

static bool watchdog_reset;

// Feeds the watchdog while core 1 is alive.
static void feed_watchdog(void) {
    static uint32_t last_alive;
    static uint64_t alive_us;
    uint64_t now = time_us_64();
    uint32_t a = player_alive();
    if (a != last_alive || player_busy() || !alive_us) {
        last_alive = a;
        alive_us = now;
    }
    if (now - alive_us < CORE1_STALL_US)
        watchdog_update();
}

static void print_help(void) {
    printf("\n=== matrixmask display ===\n"
           "v V  next / previous scene (video from SD):\n");
    for (int s = 0; s < PLAYER_SCENES; s++)
        printf("       %d  %s\n", s, player_scene_name(s));
    printf("0-7  test pattern instead of the video (v returns to the video):\n");
    for (int p = 0; p < PATTERN_COUNT; p++)
        printf("       %d  %s\n", p, pattern_name((pattern_t)p));
    printf("+ -  brightness up/down\n"
           "t    swap panels (which connector is the top panel)\n"
           "r    toggle 180 deg rotation of the top panel\n"
           "R    toggle 180 deg rotation of the bottom panel\n"
           "a    row addressing: SM5368 shift register / binary A-E\n"
           "b    colour order: B-G-R / R-G-B\n"
           "c C  CLK slower / faster\n"
           "f    reboot WITH FM6126A init    F  reboot without\n"
           "s    SD card test (info, write/verify, read speed; the video pauses)\n"
           "S    cycle SD SPI clock 12.5 / 25 / 37.5 MHz\n"
           "l    BLE link status (pings answered since the last l, events, heartbeats)\n"
           "p    print status               h  this help\n\n");
}

static void print_status(bool fm6126a) {
    static uint32_t last_frames;
    static uint64_t last_us;
    uint32_t frames = hub75_frame_count();
    uint64_t now = time_us_64();
    float hz = last_us ? (frames - last_frames) * 1e6f / (float)(now - last_us) : 0.0f;
    last_frames = frames;
    last_us = now;

    hub75_mapping_t m = hub75_get_mapping();
    uint div = hub75_get_clkdiv();
    if (watchdog_reset)
        printf("LAST RESET BY THE WATCHDOG\n");
    player_print_status();
    printf("brightness %u/255 (limiter: %u) | CLK %.1f MHz (div %u) | refresh %.0f Hz (since last p)\n",
           hub75_get_brightness(), hub75_get_applied_brightness(), clock_get_hz(clk_sys) / 2e6f / div, div, hz);
    printf("mapping: top panel = panel %u, rotate top %s, rotate bottom %s | FM6126A init %s\n",
           m.top_half + 1, m.rotate_top ? "yes" : "no", m.rotate_bottom ? "yes" : "no", fm6126a ? "done" : "off");
    printf("rows: %s | colour order %s\n",
           hub75_get_row_mode() == HUB75_ROWS_SM5368 ? "SM5368 shift register (A clk, B BK, C data)" : "binary A-E",
           m.swap_rb ? "B-G-R" : "R-G-B");
    uint32_t load = hub75_last_load_permille();
    printf("frame load %lu permille of full white, estimate %lu mA at the power bank (budget %u mA)\n",
           (unsigned long)load,
           (unsigned long)(HUB75_IDLE_MA + (uint64_t)HUB75_FULL_WHITE_MA * load * hub75_get_applied_brightness() /
                                               255 / 1000),
           HUB75_BUDGET_MA);
}

int main(void) {
    // FM6126A init must happen before the PIO owns the pins.
    watchdog_reset = watchdog_enable_caused_reboot();
    bool fm6126a = watchdog_hw->scratch[0] == SCRATCH_FM6126A_MAGIC;
    watchdog_hw->scratch[0] = 0;
    if (fm6126a)
        hub75_fm6126a_init();

    stdio_init_all();
    bool have_led = cyw43_arch_init() == 0;

    if (!hub75_init()) {
        while (true) {
            printf("hub75_init failed: no free PIO state machines\n");
            sleep_ms(1000);
        }
    }

    player_init();
    if (have_led)
        link_init();

    bool was_connected = false;
    bool pattern_mode = false;
    watchdog_enable(WATCHDOG_MS, true);

    while (true) {
        feed_watchdog();
        uint32_t t = to_ms_since_boot(get_absolute_time());
        if (have_led)
            link_poll();

        // The LED sits on the radio chip: only write changes, to keep its bus free for Bluetooth.
        static bool led;
        if (have_led && led != ((t / 500) & 1)) {
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        }

        bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            sleep_ms(100);
            print_help();
            print_status(fm6126a);
        }
        was_connected = connected;

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            hub75_mapping_t m = hub75_get_mapping();
            uint8_t b = hub75_get_brightness();
            switch (ch) {
            case '0' ... '7':
                player_show_pattern(ch - '0');
                pattern_mode = true;
                printf("pattern %d (%s)\n", ch - '0', pattern_name((pattern_t)(ch - '0')));
                if (ch - '0' == PATTERN_WHITE)
                    printf("full white: read the USB-C meter; the limiter caps the estimate at %u mA\n",
                           HUB75_BUDGET_MA);
                break;
            case 'v':
            case 'V': {
                // from a test pattern back to the current scene, else the next / previous one
                int s = pattern_mode ? player_scene()
                                     : (player_scene() + (ch == 'v' ? 1 : PLAYER_SCENES - 1)) % PLAYER_SCENES;
                pattern_mode = false;
                player_play(s, 0);
                printf("scene %d: %s\n", s, player_scene_name(s));
                break;
            }
            case '+':
                hub75_set_brightness(b > 255 - 5 ? 255 : b + 5);
                break;
            case '-':
                hub75_set_brightness(b < 5 ? 0 : b - 5);
                break;
            case 't':
                m.top_half ^= 1;
                break;
            case 'r':
                m.rotate_top = !m.rotate_top;
                break;
            case 'R':
                m.rotate_bottom = !m.rotate_bottom;
                break;
            case 'a':
                hub75_set_row_mode(hub75_get_row_mode() == HUB75_ROWS_SM5368 ? HUB75_ROWS_BINARY
                                                                             : HUB75_ROWS_SM5368);
                break;
            case 'b':
                m.swap_rb = !m.swap_rb;
                break;
            case 'c':
                hub75_set_clkdiv(hub75_get_clkdiv() + 1);
                break;
            case 'C':
                hub75_set_clkdiv(hub75_get_clkdiv() - 1);
                break;
            case 'f':
            case 'F':
                printf("rebooting %s FM6126A init...\n", ch == 'f' ? "with" : "without");
                sleep_ms(50);
                watchdog_hw->scratch[0] = ch == 'f' ? SCRATCH_FM6126A_MAGIC : 0;
                watchdog_reboot(0, 0, 0);
                while (true)
                    tight_loop_contents();
            case 's':
                player_sd_test();
                break;
            case 'S':
                sdcard_cycle_clock();
                break;
            case 'l':
                link_print_status();
                break;
            case 'h':
            case '?':
                print_help();
                break;
            default:
                break;
            }
            hub75_set_mapping(&m);
            if (ch == 'p' || ch == '+' || ch == '-' || ch == 't' || ch == 'r' || ch == 'R' || ch == 'a' ||
                ch == 'b' || ch == 'c' || ch == 'C')
                print_status(fm6126a);
        }
        sleep_ms(1);
    }
}
