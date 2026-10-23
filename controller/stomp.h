#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t axis;          // staff axis in sensor coordinates: 0 = X, 1 = Y, 2 = Z
    uint8_t threshold;     // click threshold in 125 mg steps
    uint8_t time_limit;    // max click duration in 2.5 ms steps
    uint32_t refractory_ms;
    uint8_t max_tilt_deg;  // staff must be within this angle of vertical
} stomp_config_t;

typedef enum { STOMP_OK, STOMP_REFRACTORY, STOMP_TILTED } stomp_verdict_t;

typedef struct {
    uint64_t t_us;        // time of the INT1 edge (use this as the event time)
    uint8_t click_src;
    float tilt_deg;
    stomp_verdict_t verdict;
} stomp_event_t;

extern const stomp_config_t stomp_default_config;

bool stomp_init(const stomp_config_t *cfg);
void stomp_set_config(const stomp_config_t *cfg);
stomp_config_t stomp_get_config(void);

// Call from the main loop (every few ms). Returns true and fills *ev for every
// detected click, accepted or rejected.
bool stomp_poll(stomp_event_t *ev);

// Tuning mode: print every sample as "A,<ms>,<x_mg>,<y_mg>,<z_mg>" and every click as
// "S,<ms>,<click_src>,<tilt_deg>,<verdict>" on USB serial (see tools/plot_accel.py).
void stomp_set_streaming(bool on);
bool stomp_streaming(void);

float stomp_tilt_deg(void);

// Diagnostics: low-pass gravity vector in mg (sensor coordinates), samples read from the
// FIFO and failed FIFO reads since boot.
void stomp_get_gravity(float g[3]);
uint32_t stomp_sample_count(void);
uint32_t stomp_error_count(void);
const char *stomp_verdict_name(stomp_verdict_t v);
