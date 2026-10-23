// Controller Pico firmware: WS2812 strip, encoder + buttons, LIS3DH stomp detection, the BLE
// link to the display, the sound clips on the Bluetooth speaker and the strip animations.
//
// Controls (the crystal shows the state):
//   encoder turn        select a scene of the current playlist (the crystal breathes in its
//                       colour); a stomp fires the selected scene
//   encoder push        drop the selection             long: pair the speaker (crystal blinks blue)
//   button 1            fire the selected scene        long: fire the base scene
//   button 2            back to the base scene; if already there, select the playlist's first scene
//                       long: switch playlist (scenes / fun / family; the crystal shows white /
//                       rainbow / pink-gold)
// On a fire the sound starts at once; the scene's intro on staff and mask starts L_bt later,
// when the speaker plays the sound (the display gets the start time over the link).

#include <math.h>
#include <stdio.h>

#include "ble_link.h"
#include "bt_setup.h"
#include "buttons.h"
#include "clips.h"
#include "effects.h"
#include "encoder.h"
#include "hardware/watchdog.h"
#include "lis3dh.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "sdcard.h"
#include "speaker.h"
#include "staffanim.h"
#include "stomp.h"
#include "strip.h"

#define PIN_STRIP 0
#define FRAME_MS 16
// Fired scenes start this long after the stomp on staff and mask: the speaker's latency (JBL
// Flip 7), so the sound, started at the stomp, and the picture line up.
#define L_BT_US 250000
#define LINK_PING_HZ 5
#define WATCHDOG_MS 5000       // a hang reboots into the base scene
#define PLAYLIST_NOTE_MS 1500  // crystal shows the chosen playlist

typedef struct {
    const char *name;
    rgb_t crystal[CRYSTAL_STOPS]; // crystal gradient, bottom to top: tells the scenes apart
    rgb_t colour;                 // tint of the built-in effect
    fx_t fx;                      // built-in effect while the staff animation is missing
    const char *file; // base name on the controller's SD card: <file>.wav, <file>.led, <file>_intro.led
} scene_t;

// Scene numbers are shared with the display (display/player.c); scene 0 is the base scene.
static const scene_t scenes[] = {
    {"eyes (base)", {{255, 60, 0}, {255, 110, 10}, {255, 190, 60}}, {255, 90, 10}, FX_DARK, "eyes"},
    {"fire skull", {{255, 0, 0}, {255, 70, 0}, {255, 180, 0}}, {255, 60, 0}, FX_FIRE, "fireskull"},
    {"matrix", {{0, 120, 0}, {20, 255, 40}, {170, 255, 170}}, {20, 255, 40}, FX_RAIN, "matrix"},
    {"lightning skull", {{0, 20, 255}, {60, 140, 255}, {210, 225, 255}}, {60, 110, 255}, FX_STORM, "lightning"},
    {"rainbow fire skull", {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}}, {255, 0, 200}, FX_RAINBOW, "rainbow"},
    {"youtube", {{255, 0, 0}, {255, 0, 0}, {255, 255, 255}}, {255, 0, 0}, FX_SHIMMER, "youtube"},
    {"darth maul", {{255, 0, 0}, {255, 120, 110}, {255, 0, 0}}, {255, 0, 0}, FX_SHIMMER, "maul"},
    {"creeper", {{20, 255, 0}, {0, 0, 0}, {20, 255, 0}}, {20, 255, 0}, FX_TOXIC, "creeper"},
    {"rick astley", {{255, 0, 140}, {255, 200, 0}, {0, 200, 255}}, {255, 0, 140}, FX_RAINBOW, "rick"},
    {"pumpkin", {{255, 70, 0}, {255, 90, 0}, {40, 200, 0}}, {255, 80, 0}, FX_FIRE, "pumpkin"},
    // private: faces from my own photos (tools/family.py), the files are not published
    {"family", {{255, 30, 110}, {255, 120, 60}, {255, 210, 150}}, {255, 140, 60}, FX_SHIMMER, "family"},
    {"me", {{0, 200, 160}, {255, 255, 255}, {255, 110, 0}}, {0, 200, 160}, FX_SHIMMER, "me"},
};
#define SCENE_COUNT (int)(sizeof(scenes) / sizeof(scenes[0]))

// Playlists for the encoder; each starts with the base scene.
static const uint8_t playlist_scenes[] = {0, 1, 2, 3, 6, 9};
static const uint8_t playlist_fun[] = {0, 4, 5, 7, 8, 11};
static const uint8_t playlist_family[] = {0, 10};
static const struct {
    const char *name;
    const uint8_t *scenes;
    int count;
} playlists[] = {
    {"scenes", playlist_scenes, sizeof(playlist_scenes)},
    {"fun", playlist_fun, sizeof(playlist_fun)},
    {"family", playlist_family, sizeof(playlist_family)},
};
#define PLAYLIST_COUNT (int)(sizeof(playlists) / sizeof(playlists[0]))

static int current_scene;
static int selected_scene;      // shown on the crystal; == current_scene: nothing selected
static int playlist;
static uint32_t playlist_note_until;
static fx_t fx;                 // built-in effect (scene fallback or test)
static rgb_t fx_colour;
static uint64_t impact_at_us;   // impact wave: starts at (may be in the future)
static bool impact_active;
// Fire timing (trying out, keys i ( ) ): the impact wave starts at the stomp or with the
// sound (L_bt later); the scene changes on staff and mask scene_after_ms after the sound.
// Scenes with a staff intro drop the wave when the intro starts.
static bool impact_with_sound = true;
static uint32_t scene_after_ms;
static bool accel_ok;
static bool base_measured;      // 'm' with the strip dark since boot
static bool have_bt;
static bool watchdog_reset;     // the last reset came from the watchdog
static uint16_t event_id;       // last fired event (random base per boot, see link_proto.h)
static bool fx_pending;         // the fired scene starts on the staff at fx_start_us
static uint64_t fx_start_us;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static const char *path(int s, const char *suffix) {
    static char buf[3][32];
    static unsigned next;
    char *p = buf[next++ % 3];
    snprintf(p, sizeof(buf[0]), "0:/%s%s", scenes[s].file, suffix);
    return p;
}

static void print_help(void) {
    printf("\n=== matrixmask controller ===\n"
           "Encoder: turn = select a scene of the playlist (crystal breathes; a stomp fires it),\n"
           "         push = drop the selection, long push = pair the speaker (put it in pairing\n"
           "         mode first; the crystal blinks blue while searching).\n"
           "         In TEST layout: turn = move the marker, push = mark / unmark a dead height.\n"
           "Button 1: short = fire the selected scene directly, long = fire the base scene\n"
           "Button 2: short = back to the base scene (there: select the playlist's first scene),\n"
           "          long = switch playlist (crystal: white = scenes, rainbow = fun, pink-gold = family)\n\n"
           "0-9  select a scene       d  drop the selection   f  fake stomp\n"
           "g    switch playlist      l  Bluetooth status: speaker, clips, link to the display\n"
           "P    pair the speaker     X  forget the speaker    b  back to the scene (from a test)\n"
           "i    impact wave at the stomp / with the sound   ( )  scene change 0-1000 ms after the sound\n"
           "e E  next / previous built-in effect (includes LED tests)\n"
           "+ -  brightness         n N  LEDs per side -/+    k K  crystal pairs -/+\n"
           "o    cycle colour order L  print the strip layout (dead ranges for the config)\n"
           "m    enter the USB-C meter reading (mA): strip off = base current, lit = calibrates\n"
           "     the mA per channel (procedure in docs/breadboard.md, controller test 5)\n"
           "w    colour of the full-colour test: white / red / green / blue\n"
           "t    toggle accelerometer CSV stream (tools/plot_accel.py)\n"
           "[ ]  click threshold -/+ (125 mg)   { }  click time limit -/+ (2.5 ms)\n"
           "x    cycle staff axis   a  toggle max tilt 30/90 deg\n"
           "r    accelerometer register dump + self-test\n"
           "s    SD card test (info, write/verify, read speed; strip pauses)\n"
           "c    cycle SD SPI clock 12.5 / 25 / 37.5 MHz\n"
           "p    status             h  this help\n\n");
}

static void print_status(void) {
    stomp_config_t c = stomp_get_config();
    printf("scene: %s | selected: %s | playlist: %s | effect: %s%s\n", scenes[current_scene].name,
           selected_scene != current_scene ? scenes[selected_scene].name : "-", playlists[playlist].name,
           fx_name(fx), watchdog_reset ? " | LAST RESET BY THE WATCHDOG" : "");
    staffanim_print_status();
    printf("strip: %u LEDs, order %s, brightness %u/255, estimate at the power bank ~%lu mA%s\n", strip_count(),
           strip_order_name(strip_get_order()), strip_get_brightness(), (unsigned long)strip_last_ma(),
           strip_last_limited() ? " (LIMITED)" : "");
    printf("current model: base %lu mA, %lu.%03lu mA per channel at full value%s\n", (unsigned long)strip_base_ma(),
           (unsigned long)(strip_ua_per_channel() / 1000), (unsigned long)(strip_ua_per_channel() % 1000),
           base_measured ? "" : " (base not measured yet)");
    strip_print_layout();
    if (accel_ok) {
        float g[3];
        stomp_get_gravity(g);
        printf("stomp: axis %c, threshold %u mg, time limit %u ms, refractory %lu ms, max tilt %u deg, "
               "current tilt %.0f deg\n",
               "XYZ"[c.axis], c.threshold * 125, c.time_limit * 5 / 2, (unsigned long)c.refractory_ms,
               c.max_tilt_deg, stomp_tilt_deg());
        printf("accel: gravity x %.0f y %.0f z %.0f mg (|g| %.0f), %lu samples, %lu I2C errors\n", g[0], g[1],
               g[2], sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), (unsigned long)stomp_sample_count(),
               (unsigned long)stomp_error_count());
    } else
        printf("stomp: LIS3DH NOT FOUND or config readback failed (check wiring SDA=GP20, SCL=GP21, "
               "INT1=GP22, Vin=3V3)\n");
}

// Starts scene s on the staff at at_us: its animation from the card, else its built-in effect.
static void start_staff(int s, uint64_t at_us) {
    fx = scenes[s].fx;
    fx_colour = scenes[s].colour;
    strip_set_show_dead(false);
    staffanim_play(path(s, "_intro.led"), path(s, ".led"), at_us);
    if (staffanim_in_intro())
        impact_active = false; // the intro is the impact
}

static void select_scene(int s) {
    selected_scene = s;
    if (s == current_scene)
        printf("selection dropped\n");
    else
        printf("selected: %s (a stomp fires it)\n", scenes[s].name);
}

// Encoder: steps through the current playlist, starting from the selection.
static void select_step(int steps) {
    const uint8_t *list = playlists[playlist].scenes;
    int n = playlists[playlist].count, pos = 0;
    for (int i = 0; i < n; i++)
        if (list[i] == selected_scene)
            pos = i;
    select_scene(list[((pos + steps) % n + n) % n]);
}

static void switch_playlist(void) {
    playlist = (playlist + 1) % PLAYLIST_COUNT;
    selected_scene = current_scene;
    playlist_note_until = now_ms() + PLAYLIST_NOTE_MS;
    printf("playlist: %s\n", playlists[playlist].name);
}

// Fires scene s for an impact at t_us (time_us_64). The sound starts as soon as possible after
// the stomp (reading the card takes a few ms) and is heard L_bt after its actual start; the
// impact wave and the scene change are timed from that moment.
static void fire(int s, const char *why, uint64_t t_us) {
    printf("FIRE %s (%s)%s\n", scenes[s].name, why, have_bt && !ble_link_up() ? ", display not connected" : "");
    current_scene = s;
    selected_scene = s;
    uint64_t sound_us = have_bt ? clips_play(path(s, ".wav"), t_us) : 0;
    uint64_t heard_us = (sound_us ? sound_us : t_us) + L_BT_US;
    impact_at_us = impact_with_sound ? heard_us : time_us_64();
    impact_active = true;
    fx_pending = true;
    fx_start_us = heard_us + scene_after_ms * 1000u;
    event_id++;
    if (have_bt)
        ble_link_fire(event_id, s, (uint32_t)fx_start_us);
}

static void set_effect(int delta) {
    fx = (fx_t)(((int)fx + delta + FX_COUNT) % FX_COUNT);
    strip_set_show_dead(fx == FX_TEST_LAYOUT);
    printf("effect: %s\n", fx_name(fx));
    if (fx == FX_TEST_WHITE)
        printf("full colour: compare the USB-C meter with the estimate from 'p', 'm' calibrates, 'w' colour\n");
    if (fx == FX_TEST_LAYOUT)
        printf("marker at height %u: LEDs #%u (front) and #%u (back)\n", fx_marker(), strip_led_number(fx_marker(), 0),
               strip_led_number(fx_marker(), 1));
}

static void back_to_scene(void) {
    fx = scenes[current_scene].fx;
    strip_set_show_dead(false);
    printf("back to the scene %s\n", scenes[current_scene].name);
}

static void move_marker(int steps) {
    int n = (int)strip_per_side(), h = (int)fx_marker() + steps;
    h = h < 0 ? 0 : h >= n ? n - 1 : h;
    fx_set_marker((unsigned)h);
    printf("marker at height %d: LEDs #%u (front) and #%u (back)%s%s\n", h, strip_led_number(h, 0),
           strip_led_number(h, 1), h >= (int)strip_body_height() ? ", crystal" : "",
           strip_is_dead(h) ? ", dead" : "");
}

static void toggle_dead_at_marker(void) {
    unsigned h = fx_marker();
    strip_set_dead(h, !strip_is_dead(h));
    printf("height %u (LEDs #%u / #%u) %s\n", h, strip_led_number(h, 0), strip_led_number(h, 1),
           strip_is_dead(h) ? "marked dead" : "no longer dead");
    strip_print_layout();
}

// Reads a decimal number typed on the console (Enter ends, Esc or 60 s idle cancels). The
// strip keeps showing the last frame meanwhile.
static bool read_number(const char *prompt, uint32_t *value) {
    printf("%s", prompt);
    uint32_t v = 0;
    unsigned digits = 0;
    absolute_time_t give_up = make_timeout_time_ms(60000);
    while (true) {
        watchdog_update();
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT) {
            if (time_reached(give_up)) {
                printf(" cancelled\n");
                return false;
            }
            continue;
        }
        if (ch == 27) {
            printf(" cancelled\n");
            return false;
        }
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            if (!digits)
                return false;
            *value = v;
            return true;
        }
        if ((ch == 8 || ch == 127) && digits) {
            v /= 10, digits--;
            printf("\b \b");
        } else if (ch >= '0' && ch <= '9' && digits < 6) {
            v = v * 10 + (uint32_t)(ch - '0'), digits++;
            putchar(ch);
        }
    }
}

// Meter reading with the strip dark: base current. With LEDs lit: the current above the base
// divided by what was sent gives the current per channel at full value.
static void enter_meter_reading(void) {
    uint32_t sum = strip_last_output_sum(), ma;
    if (!read_number(sum ? "meter reading with the strip lit, mA: " : "meter reading with the strip off (base), mA: ",
                     &ma))
        return;
    if (!sum) {
        strip_set_power_model(ma, strip_ua_per_channel());
        base_measured = true;
        printf("base current = %lu mA\n", (unsigned long)ma);
        return;
    }
    if (!base_measured)
        printf("note: base not measured yet (switch the effect to 'off' and press m), using %lu mA\n",
               (unsigned long)strip_base_ma());
    if (ma <= strip_base_ma()) {
        printf("reading must be above the base current\n");
        return;
    }
    uint32_t ua = (uint32_t)((uint64_t)(ma - strip_base_ma()) * 1000u * 255u / sum);
    printf("%lu mA for %lu.%02lu full-white-LED equivalents above the base -> %lu.%03lu mA per channel "
           "(was %lu.%03lu)\n",
           (unsigned long)(ma - strip_base_ma()), (unsigned long)(sum / 765), (unsigned long)(sum % 765 * 100 / 765),
           (unsigned long)(ua / 1000), (unsigned long)(ua % 1000), (unsigned long)(strip_ua_per_channel() / 1000),
           (unsigned long)(strip_ua_per_channel() % 1000));
    strip_set_power_model(strip_base_ma(), ua);
    printf("for strip.h: STRIP_DEFAULT_BASE_MA %lu, STRIP_DEFAULT_UA_PER_CHANNEL %lu\n",
           (unsigned long)strip_base_ma(), (unsigned long)ua);
}

static void stomp_or_fake(const char *why, uint64_t t_us) {
    if (selected_scene != current_scene)
        fire(selected_scene, why, t_us);
    else
        impact_at_us = time_us_64(), impact_active = true; // nothing selected: impact flash only
}

static void handle_char(int ch) {
    stomp_config_t c = stomp_get_config();
    uint8_t b = strip_get_brightness();
    switch (ch) {
    case '0' ... '9':
        if (ch - '0' < SCENE_COUNT)
            select_scene(ch - '0');
        break;
    case 'd': select_scene(current_scene); break;
    case 'f': stomp_or_fake("fake stomp", time_us_64()); break;
    case 'g': switch_playlist(); break;
    case 'e': set_effect(1); break;
    case 'E': set_effect(-1); break;
    case '+': strip_set_brightness(b > 250 ? 255 : b + 5); break;
    case '-': strip_set_brightness(b < 5 ? 0 : b - 5); break;
    case 'n': strip_set_per_side(strip_per_side() - 1); break;
    case 'N': strip_set_per_side(strip_per_side() + 1); break;
    case 'k': strip_set_crystal_pairs(strip_crystal_pairs() > 0 ? strip_crystal_pairs() - 1 : 0); break;
    case 'K': strip_set_crystal_pairs(strip_crystal_pairs() + 1); break;
    case 'L': strip_print_layout(); return;
    case 'l':
        if (have_bt) {
            speaker_print_status();
            clips_print_status();
            ble_link_print_status();
        } else
            printf("no Bluetooth (cyw43 init failed)\n");
        return;
    case 'P': if (have_bt) speaker_pair(); return;
    case 'X': if (have_bt) speaker_forget(); return;
    case 'b': back_to_scene(); return;
    case 'i':
        impact_with_sound = !impact_with_sound;
        printf("impact wave %s\n", impact_with_sound ? "with the sound (L_bt after the stomp)" : "at the stomp");
        return;
    case '(':
    case ')':
        if (ch == ')' && scene_after_ms < 1000)
            scene_after_ms += 100;
        else if (ch == '(' && scene_after_ms)
            scene_after_ms -= 100;
        printf("scene change %lu ms after the sound (the wave reaches the crystal after 400 ms)\n",
               (unsigned long)scene_after_ms);
        return;
    case 'm': enter_meter_reading(); break;
    case 'w': printf("test colour: %s\n", fx_cycle_test_colour()); break;
    case 'o': strip_set_order((color_order_t)((strip_get_order() + 1) % ORDER_COUNT)); break;
    case 't': stomp_set_streaming(!stomp_streaming()); break;
    case '[': if (c.threshold > 1) c.threshold--; break;
    case ']': if (c.threshold < 127) c.threshold++; break;
    case '{': if (c.time_limit > 1) c.time_limit--; break;
    case '}': if (c.time_limit < 127) c.time_limit++; break;
    case 'x': c.axis = (uint8_t)((c.axis + 1) % 3); break;
    case 'a': c.max_tilt_deg = c.max_tilt_deg == 30 ? 90 : 30; break;
    case 'r': if (accel_ok) lis3dh_diagnose(); return;
    case 's':
        // takes several seconds: no watchdog meanwhile; the card is mounted again afterwards
        staffanim_stop();
        clips_stop();
        watchdog_disable();
        sdcard_test();
        watchdog_enable(WATCHDOG_MS, true);
        return;
    case 'c': sdcard_cycle_clock(); return;
    case 'h': case '?': print_help(); return;
    default: break;
    }
    if (accel_ok)
        stomp_set_config(&c);
    if (!stomp_streaming())
        print_status();
}

static void handle_controls(void) {
    int steps = encoder_take_steps();
    if (steps) {
        if (fx == FX_TEST_LAYOUT)
            move_marker(steps);
        else
            select_step(steps);
    }

    button_event_t be;
    while (buttons_poll(&be)) {
        bool lng = be.type == BUTTON_LONG;
        switch (be.button) {
        case BUTTON_ENC:
            if (fx == FX_TEST_LAYOUT) {
                if (!lng)
                    toggle_dead_at_marker();
            } else if (lng) {
                if (have_bt)
                    speaker_pair();
            } else {
                select_scene(current_scene);
            }
            break;
        case BUTTON_1:
            fire(lng ? 0 : selected_scene, lng ? "button 1 long: base scene" : "button 1", time_us_64());
            break;
        case BUTTON_2:
            if (lng)
                switch_playlist();
            else if (current_scene != 0)
                fire(0, "button 2: base scene", time_us_64());
            else if (playlists[playlist].count > 1)
                select_scene(playlists[playlist].scenes[1]);
            break;
        default:
            break;
        }
    }
}

static void draw_crystal(uint32_t t) {
    if (have_bt && speaker_state() == SPEAKER_PAIRING) {
        fx_crystal_solid(t, CRYSTAL_BLINK, (rgb_t){0, 40, 255}); // searching for the speaker
    } else if ((int32_t)(playlist_note_until - t) > 0) {
        if (playlist == 1)
            fx_crystal_rainbow(t);
        else if (playlist == 2)
            fx_crystal(t, CRYSTAL_BLINK, scenes[10].crystal);
        else
            fx_crystal_solid(t, CRYSTAL_BLINK, (rgb_t){255, 255, 255});
    } else if (selected_scene != current_scene) {
        fx_crystal(t, CRYSTAL_SELECTED, scenes[selected_scene].crystal);
    } else if (scenes[current_scene].fx != FX_DARK) { // the dark staff of the base scene: no crystal
        fx_crystal(t, CRYSTAL_IDLE, scenes[current_scene].crystal);
    }
}

int main(void) {
    stdio_init_all();
    watchdog_reset = watchdog_enable_caused_reboot();
    bool have_led = cyw43_arch_init() == 0;

    buttons_init();
    encoder_init();
    bool strip_ok = strip_init(PIN_STRIP, STRIP_DEFAULT_PER_SIDE);
    accel_ok = stomp_init(&stomp_default_config);
    clips_init(); // mounts the card (sounds and strip animations)
    start_staff(0, time_us_64());
    fx_set_marker(strip_per_side() - 1);

    // A new event id range per boot: a display that saw our last event before a reboot takes
    // the base scene from the first heartbeat.
    event_id = (uint16_t)get_rand_32();
    have_bt = have_led;
    if (have_bt) {
        bt_setup_init();
        speaker_init();
        ble_link_init();
        ble_link_set_rate(LINK_PING_HZ);
        ble_link_set_scene(LINK_STATUS_IDLE, 0, event_id);
        bt_setup_power_on();
    }
    watchdog_enable(WATCHDOG_MS, true);

    bool was_connected = false;
    uint32_t next_frame = 0;
    bool led = false;

    while (true) {
        watchdog_update();
        bool connected = stdio_usb_connected();
        if (connected && !was_connected) {
            sleep_ms(100);
            print_help();
            print_status();
        }
        was_connected = connected;

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT)
            handle_char(ch);

        handle_controls();

        stomp_event_t se;
        if (accel_ok && stomp_poll(&se)) {
            if (!stomp_streaming())
                printf("stomp: src 0x%02x, tilt %.0f deg -> %s\n", se.click_src, se.tilt_deg,
                       stomp_verdict_name(se.verdict));
            if (se.verdict == STOMP_OK)
                stomp_or_fake("stomp", se.t_us);
        }

        uint32_t t = now_ms();

        // Status LED: slow blink = ok, fast blink = accelerometer or strip missing. The LED sits
        // on the radio chip: only write changes.
        bool want_led = (t / ((accel_ok && strip_ok) ? 500 : 100)) & 1;
        if (have_led && want_led != led) {
            led = want_led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        }

        if (fx_pending && time_us_64() >= fx_start_us) {
            fx_pending = false;
            start_staff(current_scene, fx_start_us);
        }

        if (have_bt)
            ble_link_poll();
        clips_poll();

        if ((int32_t)(t - next_frame) >= 0) {
            next_frame = t + FRAME_MS;
            uint64_t now_us = time_us_64();
            bool scene_fx = !fx_is_test(fx) && fx != FX_OFF;
            if (!scene_fx || !staffanim_render(now_us))
                fx_render(fx, t, fx_colour);
            if (have_bt)
                ble_link_set_scene(selected_scene != current_scene ? LINK_STATUS_SELECTED
                                   : current_scene                 ? LINK_STATUS_PLAYING
                                                                   : LINK_STATUS_IDLE,
                                   current_scene, event_id);
            static unsigned last_chase = ~0u;
            if (fx == FX_TEST_CHASE && fx_chase_index(t) != last_chase && !stomp_streaming()) {
                last_chase = fx_chase_index(t);
                printf("chase: LED #%u\n", last_chase + 1);
            }
            if (scene_fx) {
                if (impact_active && now_us >= impact_at_us)
                    impact_active = fx_overlay_impact((uint32_t)((now_us - impact_at_us) / 1000),
                                                      scenes[current_scene].colour) != 0;
                draw_crystal(t);
            }
            strip_show();
        }
    }
}
