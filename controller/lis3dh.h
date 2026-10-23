#pragma once

#include <stdbool.h>
#include <stdint.h>

// LIS3DH on I2C0 (GP20 SDA, GP21 SCL), INT1 on GP22. ±16 g, 400 Hz, high resolution,
// FIFO in stream mode, hardware single-click detection (high-pass filtered) on INT1.
#define LIS3DH_ODR_HZ 400
#define LIS3DH_CLICK_LSB_MG 125 // click threshold resolution at ±16 g (full scale / 128)

typedef struct {
    int16_t x, y, z; // mg
} accel_t;

// CLICK_SRC bits
#define LIS3DH_CLICK_X 0x01
#define LIS3DH_CLICK_Y 0x02
#define LIS3DH_CLICK_Z 0x04
#define LIS3DH_CLICK_NEG 0x08
#define LIS3DH_CLICK_SINGLE 0x10
#define LIS3DH_CLICK_ACTIVE 0x40

bool lis3dh_init(void);

// Reads all samples waiting in the FIFO (oldest first), up to max. Returns the count,
// or -1 on an I2C error.
int lis3dh_read_fifo(accel_t *out, int max);

// threshold in units of LIS3DH_CLICK_LSB_MG (1..127), time_limit in ODR ticks (2.5 ms).
void lis3dh_set_click(uint8_t threshold, uint8_t time_limit);

// True if INT1 rose since the last call; *t_us is the time of the rising edge.
bool lis3dh_take_click_irq(uint64_t *t_us);

// Reads (and thereby clears) the latched click source register.
uint8_t lis3dh_read_click_src(void);

// Bring-up: prints all registers, live samples (FIFO bypassed) and a self-test result.
void lis3dh_diagnose(void);
