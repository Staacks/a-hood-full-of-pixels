#include "buttons.h"

#include "pico/stdlib.h"

static const unsigned pins[BUTTON_COUNT] = {4, 5, 6};
#define DEBOUNCE_US 20000
#define LONG_PRESS_US 1000000

static struct {
    bool raw;           // last sampled level (true = pressed)
    bool stable;        // debounced state
    bool long_reported;
    uint64_t raw_since; // time the raw level last changed
    uint64_t down_since;
} btn[BUTTON_COUNT];

void buttons_init(void) {
    for (unsigned i = 0; i < BUTTON_COUNT; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
}

bool buttons_poll(button_event_t *ev) {
    uint64_t now = time_us_64();
    for (unsigned i = 0; i < BUTTON_COUNT; i++) {
        bool raw = !gpio_get(pins[i]);
        if (raw != btn[i].raw) {
            btn[i].raw = raw;
            btn[i].raw_since = now;
        }
        if (btn[i].stable != raw && now - btn[i].raw_since >= DEBOUNCE_US) {
            btn[i].stable = raw;
            if (raw) {
                btn[i].down_since = now;
                btn[i].long_reported = false;
            } else if (!btn[i].long_reported) {
                *ev = (button_event_t){i, BUTTON_SHORT};
                return true;
            }
        }
        if (btn[i].stable && !btn[i].long_reported && now - btn[i].down_since >= LONG_PRESS_US) {
            btn[i].long_reported = true;
            *ev = (button_event_t){i, BUTTON_LONG};
            return true;
        }
    }
    return false;
}

bool button_is_down(unsigned button) { return button < BUTTON_COUNT && btn[button].stable; }
