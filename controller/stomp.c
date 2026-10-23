#include "stomp.h"

#include <math.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "lis3dh.h"
#include "pico/stdlib.h"

#define PIN_INT1 22
#define GRAVITY_ALPHA 0.02f // low-pass for the gravity vector, tau ~125 ms at 400 Hz
#define SAMPLE_US (1000000 / LIS3DH_ODR_HZ)

const stomp_config_t stomp_default_config = {
    .axis = 1, // Y: measured on the assembled staff (2026-10-10)
    .threshold = 24, // 3 g
    .time_limit = 10, // 25 ms
    .refractory_ms = 1500,
    .max_tilt_deg = 30,
};

static stomp_config_t cfg;
static float gravity[3];
static uint64_t last_accepted_us;
static bool streaming;
static uint32_t sample_count, error_count;

bool stomp_init(const stomp_config_t *c) {
    if (!lis3dh_init())
        return false;
    stomp_set_config(c);
    gravity[cfg.axis] = 1000; // start upright, the low-pass settles within ~0.5 s
    return true;
}

void stomp_set_config(const stomp_config_t *c) {
    cfg = *c;
    if (cfg.axis > 2)
        cfg.axis = 2;
    lis3dh_set_click(cfg.threshold, cfg.time_limit);
}

stomp_config_t stomp_get_config(void) { return cfg; }

float stomp_tilt_deg(void) {
    float n = sqrtf(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);
    if (n < 300.0f) // no sensible gravity estimate (free fall, sensor missing)
        return 90.0f;
    float c = fabsf(gravity[cfg.axis]) / n;
    return acosf(c > 1.0f ? 1.0f : c) * (180.0f / (float)M_PI);
}

void stomp_get_gravity(float g[3]) {
    for (int i = 0; i < 3; i++)
        g[i] = gravity[i];
}

uint32_t stomp_sample_count(void) { return sample_count; }
uint32_t stomp_error_count(void) { return error_count; }

static void read_samples(void) {
    accel_t s[32];
    int n = lis3dh_read_fifo(s, 32);
    if (n < 0)
        error_count++;
    if (n <= 0)
        return;
    sample_count += (uint32_t)n;
    uint64_t now = time_us_64();
    for (int i = 0; i < n; i++) {
        gravity[0] += GRAVITY_ALPHA * (s[i].x - gravity[0]);
        gravity[1] += GRAVITY_ALPHA * (s[i].y - gravity[1]);
        gravity[2] += GRAVITY_ALPHA * (s[i].z - gravity[2]);
        if (streaming) {
            uint64_t t = now - (uint64_t)(n - 1 - i) * SAMPLE_US;
            printf("A,%llu,%d,%d,%d\n", t / 1000, s[i].x, s[i].y, s[i].z);
        }
    }
}

bool stomp_poll(stomp_event_t *ev) {
    static uint64_t int_high_since;

    read_samples();

    uint64_t t;
    if (!lis3dh_take_click_irq(&t)) {
        // Recovery: if INT1 stays high without a pending edge (missed edge), clear it.
        if (gpio_get(PIN_INT1)) {
            if (!int_high_since)
                int_high_since = time_us_64();
            else if (time_us_64() - int_high_since > 100000) {
                lis3dh_read_click_src();
                int_high_since = 0;
            }
        } else {
            int_high_since = 0;
        }
        return false;
    }
    int_high_since = 0;

    ev->t_us = t;
    ev->click_src = lis3dh_read_click_src();
    ev->tilt_deg = stomp_tilt_deg();
    if (last_accepted_us && t - last_accepted_us < (uint64_t)cfg.refractory_ms * 1000)
        ev->verdict = STOMP_REFRACTORY;
    else if (ev->tilt_deg > cfg.max_tilt_deg)
        ev->verdict = STOMP_TILTED;
    else {
        ev->verdict = STOMP_OK;
        last_accepted_us = t;
    }
    if (streaming)
        printf("S,%llu,0x%02x,%.0f,%s\n", t / 1000, ev->click_src, ev->tilt_deg, stomp_verdict_name(ev->verdict));
    return true;
}

void stomp_set_streaming(bool on) { streaming = on; }
bool stomp_streaming(void) { return streaming; }

const char *stomp_verdict_name(stomp_verdict_t v) {
    switch (v) {
    case STOMP_OK: return "ok";
    case STOMP_REFRACTORY: return "refractory";
    case STOMP_TILTED: return "tilted";
    }
    return "?";
}
