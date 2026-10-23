// Bluetooth test firmware (milestone 4): A2DP stream to the speaker and the BLE link to the
// display running at the same time on the one CYW43439 radio.
//
// Answers: does the stream stay stable with the BLE link active, how do link round trips look with
// the stream running, what is the speaker's latency L_bt (click + strip flash, adjust until
// they match), how much CPU does the SBC encoder take.

#include <math.h>
#include <stdio.h>

#include "speaker.h"
#include "ble_link.h"
#include "bt_setup.h"
#include "buttons.h"
#include "encoder.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "strip.h"

#define PIN_STRIP 0
#define LEAD_US 20000       // sounds are scheduled this far ahead of the trigger
#define FLASH_US 60000
#define STRIP_FRAME_US 20000
#define AUTO_STATUS_US 10000000
#define GAIN 0.3f           // test sound level (start with the speaker volume low)

static int32_t l_bt_ms = 250; // JBL Flip 7, metronome test 2026-10-10
static bool metronome;
static bool tone_on;
static bool auto_status = true;
static uint64_t next_tick_us, flash_at_us, flash_until_us;

// ------------------------------------------------------------------ test sounds

static float sine(uint32_t pos, uint32_t hz) {
    uint32_t ph = (uint32_t)(((uint64_t)pos * hz) % SPEAKER_SAMPLE_RATE); // exact phase, no drift
    return sinf(6.2831853f * (float)ph / SPEAKER_SAMPLE_RATE);
}

static void put(int16_t *lr, float v) { lr[0] = lr[1] = (int16_t)(v * 32767.0f); }

// 2 kHz tick with a sharp onset and 3 ms decay: easy to line up in a slow-motion video.
static unsigned click_fn(int16_t *lr, unsigned frames, uint32_t pos, void *ctx) {
    (void)ctx;
    const uint32_t len = SPEAKER_SAMPLE_RATE / 60;
    unsigned n = 0;
    for (; n < frames && pos + n < len; n++) {
        uint32_t p = pos + n;
        put(lr + 2 * n, 2.5f * GAIN * expf(-(float)p / (SPEAKER_SAMPLE_RATE * 0.003f)) * sine(p, 2000));
    }
    return n;
}

// 1.5 s exponential sweep 2 kHz -> 40 Hz, a crude bass drop.
static unsigned sweep_fn(int16_t *lr, unsigned frames, uint32_t pos, void *ctx) {
    (void)ctx;
    const float T = 1.5f, f0 = 2000.0f, k = logf(40.0f / 2000.0f);
    const uint32_t len = (uint32_t)(T * SPEAKER_SAMPLE_RATE);
    unsigned n = 0;
    for (; n < frames && pos + n < len; n++) {
        float t = (float)(pos + n) / SPEAKER_SAMPLE_RATE;
        float phase = 6.2831853f * f0 * T / k * (expf(k * t / T) - 1.0f);
        float fade = t > T - 0.1f ? (T - t) / 0.1f : 1.0f;
        put(lr + 2 * n, GAIN * fade * sinf(phase));
    }
    return n;
}

// Endless 440 Hz: gaps and clicks from dropouts are easy to hear on a pure tone.
static unsigned tone_fn(int16_t *lr, unsigned frames, uint32_t pos, void *ctx) {
    (void)ctx;
    for (unsigned n = 0; n < frames; n++)
        put(lr + 2 * n, 0.6f * GAIN * sine(pos + n, 440));
    return frames;
}

// Click now (+ lead) and flash the strip L_bt after the click's stream time.
static void click(bool quiet) {
    uint64_t start = time_us_64() + LEAD_US;
    uint64_t actual = speaker_play(click_fn, NULL, start);
    flash_at_us = (actual ? actual : start) + (uint64_t)l_bt_ms * 1000;
    tone_on = false;
    if (!actual && !quiet)
        printf("no audio stream: flash only\n");
}

// ------------------------------------------------------------------ UI

static void print_help(void) {
    printf("\n=== matrixmask Bluetooth test (A2DP + BLE link) ===\n"
           "Crystal = speaker (red blink: no speaker, blue: pairing, yellow: connecting,\n"
           "green: streaming). Pair below the crystal = link to the display (green: display\n"
           "answers, yellow: connected but no answer, red blink: searching, off: link off).\n\n"
           "c / encoder push       click + strip flash L_bt later\n"
           "m / encoder long push  metronome on/off (click + flash every second): tune L_bt\n"
           "+ - / turn encoder     L_bt +/- 5 ms      < >  L_bt +/- 50 ms\n"
           "s / button 1           bass-drop sweep\n"
           "t / button 1 long      continuous 440 Hz tone on/off (listen for dropouts)\n"
           "P / button 2 long      pair: speaker in pairing mode first\n"
           "X            forget the speaker        R  reconnect Bluetooth\n"
           "l            link pings: 20 Hz -> 5 Hz -> off (link disconnected) -> 20 Hz\n"
           "z            idle noise instead of silence on/off\n"
           "v            status every 10 s on/off  p / button 2  status now\n"
           "D            HCI packet log on/off (only in bt_test_hcidump)\n"
           "h            this help\n\n");
}

static void print_status(void) {
    printf("--- %.0f s | L_bt %ld ms | metronome %s | idle noise %s\n", time_us_64() / 1e6, (long)l_bt_ms,
           metronome ? "on" : "off", speaker_idle_noise() ? "on" : "off");
    speaker_print_status();
    ble_link_print_status();
}

static void handle(int ch) {
    switch (ch) {
    case 'c': click(false); break;
    case 'm':
        metronome = !metronome;
        next_tick_us = time_us_64();
        printf("metronome %s\n", metronome ? "on" : "off");
        break;
    case 's':
        speaker_play(sweep_fn, NULL, time_us_64() + LEAD_US);
        tone_on = false;
        break;
    case 't':
        tone_on = !tone_on;
        if (tone_on)
            speaker_play(tone_fn, NULL, time_us_64() + LEAD_US);
        else
            speaker_stop_sound();
        printf("tone %s\n", tone_on ? "on" : "off");
        break;
    case '+': l_bt_ms += 5; printf("L_bt %ld ms\n", (long)l_bt_ms); break;
    case '-': l_bt_ms -= 5; printf("L_bt %ld ms\n", (long)l_bt_ms); break;
    case '>': l_bt_ms += 50; printf("L_bt %ld ms\n", (long)l_bt_ms); break;
    case '<': l_bt_ms -= 50; printf("L_bt %ld ms\n", (long)l_bt_ms); break;
    case 'P': speaker_pair(); break;
    case 'X': speaker_forget(); break;
    case 'R': speaker_reconnect(); break;
    case 'l': {
        unsigned r = ble_link_rate();
        ble_link_set_rate(r == 20 ? 5 : r == 5 ? 0 : 20);
        printf("link pings: %u Hz%s\n", ble_link_rate(), ble_link_rate() ? "" : " (link off)");
        break;
    }
    case 'z':
        speaker_set_idle_noise(!speaker_idle_noise());
        printf("idle noise %s\n", speaker_idle_noise() ? "on" : "off");
        break;
    case 'D':
        printf("HCI packet log %s\n", speaker_toggle_hci_log() ? "on" : "off (or not in this build: use bt_test_hcidump)");
        break;
    case 'v':
        auto_status = !auto_status;
        printf("periodic status %s\n", auto_status ? "on" : "off");
        break;
    case 'p': print_status(); break;
    case 'h':
    case '?': print_help(); break;
    default: break;
    }
}

static rgb_t blink(rgb_t c, uint32_t t_ms, uint32_t period_ms) { return (t_ms / period_ms) & 1 ? c : (rgb_t){0, 0, 0}; }

static rgb_t bt_colour(uint32_t t) {
    switch (speaker_state()) {
    case SPEAKER_NO_SPEAKER: return blink((rgb_t){255, 0, 0}, t, 500);
    case SPEAKER_PAIRING: return blink((rgb_t){0, 0, 255}, t, 150);
    case SPEAKER_WAITING:
    case SPEAKER_CONNECTING: return blink((rgb_t){255, 160, 0}, t, 500);
    case SPEAKER_CONNECTED: return (rgb_t){255, 160, 0};
    case SPEAKER_STREAMING: return (rgb_t){0, 255, 0};
    default: return (rgb_t){40, 0, 0};
    }
}

static rgb_t link_colour(uint32_t t) {
    if (ble_link_rate() == 0)
        return (rgb_t){0, 0, 0};
    if (ble_link_up())
        return (rgb_t){0, 255, 0};
    if (ble_link_connected())
        return (rgb_t){255, 160, 0};
    return blink((rgb_t){255, 0, 0}, t, 500);
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init() != 0) {
        while (true) {
            printf("cyw43_arch_init failed\n");
            sleep_ms(1000);
        }
    }
    buttons_init();
    encoder_init();
    strip_init(PIN_STRIP, STRIP_DEFAULT_PER_SIDE);
    bt_setup_init();
    speaker_init();
    ble_link_init();
    bt_setup_power_on();

    bool was_connected = false, flashing = false;
    uint64_t next_strip_us = 0, next_status_us = AUTO_STATUS_US;

    while (true) {
        uint64_t now = time_us_64();
        uint32_t t_ms = (uint32_t)(now / 1000);

        bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            sleep_ms(100);
            print_help();
            print_status();
        }
        was_connected = connected;

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT)
            handle(ch);

        // Encoder: L_bt in 5 ms steps (e.g. while the metronome runs). Push: click, long push:
        // metronome. Button 1: sweep, long: tone. Button 2: status, long: pairing.
        for (int steps = encoder_take_steps(); steps; steps += steps > 0 ? -1 : 1)
            handle(steps > 0 ? '+' : '-');
        button_event_t be;
        while (buttons_poll(&be)) {
            static const char short_keys[BUTTON_COUNT] = {'c', 's', 'p'};
            static const char long_keys[BUTTON_COUNT] = {'m', 't', 'P'};
            handle(be.type == BUTTON_SHORT ? short_keys[be.button] : long_keys[be.button]);
        }

        ble_link_poll();

        if (metronome && now >= next_tick_us) {
            next_tick_us += 1000000;
            if (next_tick_us < now)
                next_tick_us = now + 1000000;
            click(true);
        }

        if (auto_status && now >= next_status_us) {
            next_status_us = now + AUTO_STATUS_US;
            print_status();
        }

        // Strip: redraw immediately when the flash starts or ends, else every 20 ms.
        if (flash_at_us && now >= flash_at_us) {
            flash_until_us = flash_at_us + FLASH_US;
            flash_at_us = 0;
        }
        bool flash = now < flash_until_us;
        if (flash != flashing || now >= next_strip_us) {
            flashing = flash;
            next_strip_us = now + STRIP_FRAME_US;
            strip_fill(flash ? (rgb_t){160, 160, 160} : (rgb_t){0, 0, 0});
            strip_set_crystal(bt_colour(t_ms));
            strip_set_both(strip_body_height() - 1, link_colour(t_ms));
            strip_show();
        }

        // Onboard LED heartbeat; it sits on the radio chip, so only write changes.
        static bool led;
        if (led != ((t_ms / 500) & 1)) {
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        }
    }
}
