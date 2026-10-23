#include "encoder.h"

#include "hardware/sync.h"
#include "pico/stdlib.h"

#define PIN_A 2
#define PIN_B 3
#define SAMPLE_US 1000

static repeating_timer_t timer;
static uint8_t prev;   // last state, bit 1 = A, bit 0 = B (1 = contact open)
static int8_t acc;     // transitions since the last rest position
static volatile int steps;

// Direction of a transition prev -> cur (index prev << 2 | cur); invalid jumps count 0, so
// bounce on one contact only moves back and forth.
static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static bool sample(repeating_timer_t *t) {
    (void)t;
    uint32_t all = gpio_get_all();
    uint8_t cur = (uint8_t)((((all >> PIN_A) & 1) << 1) | ((all >> PIN_B) & 1));
    if (cur == prev)
        return true;
    acc += table[prev << 2 | cur];
    prev = cur;
    // A detent ends in a rest position (both open; for half-step types also both closed).
    bool rest = cur == 3 || (ENCODER_TRANSITIONS_PER_DETENT == 2 && cur == 0);
    if (rest) {
        int need = ENCODER_TRANSITIONS_PER_DETENT / 2;
        if (acc >= need)
            steps += ENCODER_REVERSE ? -1 : 1;
        else if (acc <= -need)
            steps += ENCODER_REVERSE ? 1 : -1;
        acc = 0;
    }
    return true;
}

void encoder_init(void) {
    for (unsigned pin = PIN_A; pin <= PIN_B; pin++) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
    sleep_us(10); // let the pull-ups settle before taking the first state
    uint32_t all = gpio_get_all();
    prev = (uint8_t)((((all >> PIN_A) & 1) << 1) | ((all >> PIN_B) & 1));
    add_repeating_timer_us(-SAMPLE_US, sample, NULL, &timer);
}

int encoder_take_steps(void) {
    uint32_t irq = save_and_disable_interrupts();
    int s = steps;
    steps = 0;
    restore_interrupts(irq);
    return s;
}
