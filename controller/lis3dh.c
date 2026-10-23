#include "lis3dh.h"

#include <stdio.h>

#include "hardware/i2c.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define I2C i2c0
#define PIN_SDA 20
#define PIN_SCL 21
#define PIN_INT1 22
#define ADDR 0x18 // SDO open/low on the Adafruit breakout

#define REG_WHO_AM_I 0x0f
#define REG_CTRL1 0x20
#define REG_CTRL2 0x21
#define REG_CTRL3 0x22
#define REG_CTRL4 0x23
#define REG_CTRL5 0x24
#define REG_CTRL6 0x25
#define REG_STATUS 0x27
#define REG_OUT_X_L 0x28
#define REG_FIFO_CTRL 0x2e
#define REG_FIFO_SRC 0x2f
#define REG_CLICK_CFG 0x38
#define REG_CLICK_SRC 0x39
#define REG_CLICK_THS 0x3a
#define REG_TIME_LIMIT 0x3b
#define REG_TIME_LATENCY 0x3c
#define REG_TIME_WINDOW 0x3d

#define AUTO_INCREMENT 0x80
#define MG_PER_DIGIT_HR_16G 12 // 12-bit high-resolution output at ±16 g

static volatile bool irq_pending;
static volatile uint64_t irq_time_us;

static bool write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(I2C, ADDR, buf, 2, false, 2000) == 2;
}

static bool read_regs(uint8_t reg, uint8_t *dst, size_t len) {
    if (len > 1)
        reg |= AUTO_INCREMENT;
    if (i2c_write_timeout_us(I2C, ADDR, &reg, 1, true, 2000) != 1)
        return false;
    return i2c_read_timeout_us(I2C, ADDR, dst, len, false, 2000 + 100 * len) == (int)len;
}

static void int1_callback(uint gpio, uint32_t events) {
    if (gpio == PIN_INT1 && (events & GPIO_IRQ_EDGE_RISE)) {
        irq_time_us = time_us_64();
        irq_pending = true;
    }
}

bool lis3dh_init(void) {
    i2c_init(I2C, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);

    uint8_t id = 0;
    if (!read_regs(REG_WHO_AM_I, &id, 1) || id != 0x33)
        return false;

    // No BOOT (CTRL5 bit 7) here: it only reloads the trimming values, which power-on already
    // did, and if it is still running when CTRL5 is written it clears FIFO_EN again (seen on the
    // breadboard: FIFO off, OUT registers frozen). Write the config and read it back instead.
    static const uint8_t config[][2] = {
        {REG_CTRL1, 0x77},        // 400 Hz, normal/high-res mode, X Y Z enabled
        {REG_CTRL2, 0x04},        // high-pass filter for the click function only
        {REG_CTRL3, 0x80},        // click interrupt on INT1
        {REG_CTRL4, 0xb8},        // block data update, ±16 g, high resolution
        {REG_CTRL5, 0x40},        // FIFO enabled
        {REG_CTRL6, 0x00},        // INT active high
        {REG_FIFO_CTRL, 0x00},    // bypass first: clears the FIFO after a warm reset
        {REG_FIFO_CTRL, 0x80},    // FIFO stream mode
        {REG_CLICK_CFG, 0x15},    // single click on X, Y and Z
        {REG_TIME_LATENCY, 20},   // 50 ms quiet time after a click
        {REG_TIME_WINDOW, 0},     // no double click
    };
    for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
        if (!write_reg(config[i][0], config[i][1]))
            return false;
    }
    for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
        uint8_t v;
        if (!read_regs(config[i][0], &v, 1))
            return false;
        if (v != config[i][1] && config[i][0] != REG_FIFO_CTRL)
            return false;
    }
    uint8_t fifo_ctrl;
    if (!read_regs(REG_FIFO_CTRL, &fifo_ctrl, 1) || fifo_ctrl != 0x80)
        return false;
    lis3dh_set_click(24, 10); // 3 g, 25 ms; tuned later on the real staff

    gpio_init(PIN_INT1);
    gpio_set_dir(PIN_INT1, GPIO_IN);
    gpio_set_irq_enabled_with_callback(PIN_INT1, GPIO_IRQ_EDGE_RISE, true, int1_callback);
    lis3dh_read_click_src(); // clear a stale latched click
    irq_pending = false;
    return true;
}

void lis3dh_set_click(uint8_t threshold, uint8_t time_limit) {
    write_reg(REG_CLICK_THS, 0x80 | (threshold & 0x7f)); // latched until CLICK_SRC is read
    write_reg(REG_TIME_LIMIT, time_limit & 0x7f);
}

int lis3dh_read_fifo(accel_t *out, int max) {
    uint8_t src;
    if (!read_regs(REG_FIFO_SRC, &src, 1))
        return -1;
    int n = (src & 0x20) ? 0 : (src & 0x40) ? 32 : (src & 0x1f);
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++) {
        uint8_t raw[6];
        if (!read_regs(REG_OUT_X_L, raw, 6))
            return -1;
        out[i].x = (int16_t)((int16_t)(raw[0] | raw[1] << 8) >> 4) * MG_PER_DIGIT_HR_16G;
        out[i].y = (int16_t)((int16_t)(raw[2] | raw[3] << 8) >> 4) * MG_PER_DIGIT_HR_16G;
        out[i].z = (int16_t)((int16_t)(raw[4] | raw[5] << 8) >> 4) * MG_PER_DIGIT_HR_16G;
    }
    return n;
}

bool lis3dh_take_click_irq(uint64_t *t_us) {
    if (!irq_pending)
        return false;
    uint32_t save = save_and_disable_interrupts();
    *t_us = irq_time_us;
    irq_pending = false;
    restore_interrupts(save);
    return true;
}

uint8_t lis3dh_read_click_src(void) {
    uint8_t src = 0;
    read_regs(REG_CLICK_SRC, &src, 1);
    return src;
}

static void dump_samples(const char *label) {
    for (int i = 0; i < 4; i++) {
        uint8_t st = 0, raw[6] = {0};
        sleep_ms(10); // > 1/ODR, so every line should be a new sample
        bool ok = read_regs(REG_STATUS, &st, 1) && read_regs(REG_OUT_X_L, raw, 6);
        printf("  %-9s status %02x  raw %02x %02x %02x %02x %02x %02x  x %5d y %5d z %5d mg%s\n", label, st,
               raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
               ((int16_t)(raw[0] | raw[1] << 8) >> 4) * MG_PER_DIGIT_HR_16G,
               ((int16_t)(raw[2] | raw[3] << 8) >> 4) * MG_PER_DIGIT_HR_16G,
               ((int16_t)(raw[4] | raw[5] << 8) >> 4) * MG_PER_DIGIT_HR_16G, ok ? "" : "  I2C ERROR");
    }
}

void lis3dh_diagnose(void) {
    uint8_t regs[0x40 - 0x07];
    printf("LIS3DH registers 0x07-0x3f:");
    for (uint8_t r = 0x07; r < 0x40; r++) {
        if (!read_regs(r, &regs[r - 0x07], 1)) {
            printf(" I2C ERROR at 0x%02x\n", r);
            return;
        }
        if ((r - 0x07) % 16 == 0)
            printf("\n  %02x:", r);
        printf(" %02x", regs[r - 0x07]);
    }
    printf("\n");

    // Bypass the FIFO so OUT_X_L..OUT_Z_H show the live sample, then run the self-test:
    // with ST0 set the sensor deflects its own mass electrostatically, so the output must
    // change by a few hundred mg. No change = MEMS dead or not actually measuring.
    uint8_t ctrl4 = regs[0x23 - 0x07];
    write_reg(REG_FIFO_CTRL, 0x00);
    dump_samples("normal");
    write_reg(REG_CTRL4, (ctrl4 & ~0x06) | 0x02);
    sleep_ms(100);
    dump_samples("selftest");
    write_reg(REG_CTRL4, ctrl4);
    sleep_ms(100);
    dump_samples("normal");
    write_reg(REG_FIFO_CTRL, 0x80);
    lis3dh_read_click_src();
}
