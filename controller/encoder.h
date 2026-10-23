// EC11 rotary encoder on GP2 (A) / GP3 (B), C pin to GND, internal pull-ups. Sampled at 1 kHz
// in a timer interrupt (no GPIO interrupt: that callback belongs to the LIS3DH, and sampling
// also filters contact bounce). The push button is a normal button (buttons.h).
#pragma once

// Quadrature transitions per detent: 4 for the common EC11 with as many pulses as detents
// (rests with A and B open), 2 for types with half as many pulses as detents.
#define ENCODER_TRANSITIONS_PER_DETENT 4
// Set to 1 if clockwise counts down.
#define ENCODER_REVERSE 0

void encoder_init(void);

// Detents turned since the last call: positive = clockwise.
int encoder_take_steps(void);
