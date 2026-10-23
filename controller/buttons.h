#pragma once

#include <stdbool.h>

// Switches on the staff, each to GND with the internal pull-up: the rotary encoder's push button
// (GP4), button 1 (GP5) and button 2 (GP6, pairing / menu). The encoder itself: encoder.h.
#define BUTTON_COUNT 3
#define BUTTON_ENC 0
#define BUTTON_1 1
#define BUTTON_2 2

typedef enum {
    BUTTON_SHORT, // released before the long-press time
    BUTTON_LONG,  // held for the long-press time (reported once, while still held)
} button_event_type_t;

typedef struct {
    unsigned button;
    button_event_type_t type;
} button_event_t;

void buttons_init(void);

// Call often (>= every 5 ms). Returns true and fills *ev if an event happened.
bool buttons_poll(button_event_t *ev);

bool button_is_down(unsigned button);
