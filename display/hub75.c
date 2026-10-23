// HUB75 driver for two 128x64 1/32-scan panels, mounted as 128x128. The panels have no OUT
// connector; they share data, address, LAT and OE and have separate clocks (see hub75.h).
//
// Refresh is done entirely by PIO + DMA (see hub75.pio): the data DMA streams the
// front framebuffer to the data SM and is re-armed by a control channel that reads
// the front buffer pointer, so a buffer flip is a pointer write. A second DMA pair
// streams the row table (row address + OE time per bitplane) to the row SM.
//
// Framebuffer layout: for each scan row r (0..31), for each plane p, CHAIN_W bytes
// in shift order: the first PANEL_W bytes go to panel 1 (CLK1), the rest to panel 2 (CLK2).
// Byte bits 0..5 = R1 G1 B1 R2 G2 B2 (rows r and r + 32 of the panel).

#include "hub75.h"

#include <math.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hub75.pio.h"
#include "pico/stdlib.h"

#define PLANE_BYTES HUB75_CHAIN_W
#define FB_BYTES (HUB75_SCAN_ROWS * HUB75_PLANES * PLANE_BYTES)
#define FB_WORDS (FB_BYTES / 4)
#define ROW_ENTRIES (HUB75_SCAN_ROWS * HUB75_PLANES)

// PIO IRQ flags 4 and 5 are used for the handshake between the two SMs.
#define DMA_IRQ_INDEX 1

static uint32_t fb[2][FB_WORDS];
static uint32_t row_table[2][ROW_ENTRIES];
static const void *volatile data_ptr; // read by the data control DMA channel
static const void *volatile row_ptr;  // read by the row control DMA channel
static uint back_fb = 1;
static uint back_rt = 1;

static PIO pio;
static uint sm_data, sm_row;
static uint off_data, off_row;
static uint data_ch, data_ctrl_ch, row_ch, row_ctrl_ch;

static volatile uint32_t frame_count;
static uint32_t last_load_permille;
// Full brightness: the scene loops stay below the current budget (fire skull, the brightest,
// ~18 % of full white = 1.7 A); brighter frames (flashes, bursts) are dimmed by the limiter.
static volatile uint8_t brightness = 255; // set by any core, applied by hub75_show()
static uint8_t applied_brightness;       // in the current row table (after the limiter)
static volatile bool rows_dirty;         // row mode or brightness changed
static uint clkdiv = 4; // 18.75 MHz CLK; the current calibration (HUB75_FULL_WHITE_MA) was measured here
static uint32_t lsb_cycles;
// Defaults match the project's panels: FM6124C column drivers, SM5368 row drivers, B-G-R order.
static hub75_mapping_t mapping = {.top_half = 0, .rotate_top = false, .rotate_bottom = false, .swap_rb = true};
static hub75_row_mode_t row_mode = HUB75_ROWS_SM5368;

static uint16_t lut5[32], lut6[64];

static void build_gamma(void) {
    const float max = (float)((1u << HUB75_PLANES) - 1);
    for (int i = 0; i < 32; i++)
        lut5[i] = (uint16_t)lroundf(powf(i / 31.0f, 2.2f) * max);
    for (int i = 0; i < 64; i++)
        lut6[i] = (uint16_t)lroundf(powf(i / 63.0f, 2.2f) * max);
}

// Row pin bits (relative to HUB75_PIN_ROW_BASE) for SM5368 row drivers.
#define ROW_CLK (1u << 0) // A
#define ROW_BK (1u << 1)  // B
#define ROW_DIN (1u << 2) // C

// Three row-pin states for row r, bitplane p (see hub75_row in hub75.pio).
static uint32_t row_states(uint r, uint p) {
    if (row_mode == HUB75_ROWS_BINARY)
        return r | (r << 5) | (r << 10);
    if (p != 0)
        return 0; // the row driver keeps its row for the remaining bitplanes
    // Advance the row shift register by one; a 1 enters at row 0 and walks down.
    uint32_t din = r == 0 ? ROW_DIN : 0;
    return (ROW_BK | din) | ((ROW_BK | din | ROW_CLK) << 5);
}

// Only called by hub75_init() and hub75_show() (the core that refreshes), so the two row tables
// are never written concurrently.
static void build_row_table(uint8_t b) {
    applied_brightness = b;
    uint32_t *t = row_table[back_rt];
    for (uint r = 0; r < HUB75_SCAN_ROWS; r++) {
        for (uint p = 0; p < HUB75_PLANES; p++) {
            uint32_t on = (uint32_t)(((uint64_t)(lsb_cycles << p) * b) / 255);
            if (on < 1)
                on = 1;
            if (on > (1u << 17))
                on = 1u << 17;
            t[r * HUB75_PLANES + p] = row_states(r, p) | ((on - 1) << 15);
        }
    }
    row_ptr = t;
    back_rt ^= 1;
}

static void __isr dma_irq_handler(void) {
    if (dma_irqn_get_channel_status(DMA_IRQ_INDEX, data_ch)) {
        dma_irqn_acknowledge_channel(DMA_IRQ_INDEX, data_ch);
        frame_count++;
    }
}

// Both programs must live in the same PIO block (shared IRQ flags).
static bool claim_pio(void) {
    for (uint i = 0; i < NUM_PIOS; i++) {
        PIO p = pio_get_instance(i);
        int a = pio_claim_unused_sm(p, false);
        if (a < 0)
            continue;
        int b = pio_claim_unused_sm(p, false);
        if (b < 0) {
            pio_sm_unclaim(p, (uint)a);
            continue;
        }
        if (!pio_can_add_program(p, &hub75_data_program)) {
            pio_sm_unclaim(p, (uint)a);
            pio_sm_unclaim(p, (uint)b);
            continue;
        }
        uint oa = pio_add_program(p, &hub75_data_program);
        if (!pio_can_add_program(p, &hub75_row_program)) {
            pio_remove_program(p, &hub75_data_program, oa);
            pio_sm_unclaim(p, (uint)a);
            pio_sm_unclaim(p, (uint)b);
            continue;
        }
        off_row = pio_add_program(p, &hub75_row_program);
        off_data = oa;
        pio = p;
        sm_data = (uint)a;
        sm_row = (uint)b;
        return true;
    }
    return false;
}

static void init_pins(void) {
    const uint32_t data_mask = 0x3fu << HUB75_PIN_DATA_BASE;
    const uint32_t row_mask = 0x1fu << HUB75_PIN_ROW_BASE;
    const uint32_t ctrl_mask = (1u << HUB75_PIN_CLK1) | (1u << HUB75_PIN_CLK2) | (1u << HUB75_PIN_LAT) |
                               (1u << HUB75_PIN_OE);
    const uint32_t all = data_mask | row_mask | ctrl_mask;

    // Set output values before handing the pins to PIO, so OE never glitches low.
    pio_sm_set_pins_with_mask(pio, sm_row, 1u << HUB75_PIN_OE, all);
    pio_sm_set_pindirs_with_mask(pio, sm_row, all, all);
    for (uint pin = 0; pin < 32; pin++)
        if (all & (1u << pin))
            pio_gpio_init(pio, pin);
}

static void init_sms(void) {
    pio_sm_config c = hub75_data_program_get_default_config(off_data);
    sm_config_set_out_pins(&c, HUB75_PIN_DATA_BASE, 6);
    sm_config_set_sideset_pins(&c, HUB75_PIN_CLK1); // CLK1, CLK2
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac8(&c, clkdiv, 0);
    pio_sm_init(pio, sm_data, off_data, &c);

    // Preload Y with the number of pixels per panel row - 1, then empty the OSR again.
    pio_sm_put(pio, sm_data, HUB75_PANEL_W - 1);
    pio_sm_exec(pio, sm_data, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm_data, pio_encode_mov(pio_y, pio_osr));
    pio_sm_exec(pio, sm_data, pio_encode_out(pio_null, 32));

    c = hub75_row_program_get_default_config(off_row);
    sm_config_set_out_pins(&c, HUB75_PIN_ROW_BASE, 5);
    sm_config_set_sideset_pins(&c, HUB75_PIN_LAT);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    pio_sm_init(pio, sm_row, off_row, &c);

    pio_interrupt_clear(pio, 4);
    pio_interrupt_clear(pio, 5);
}

// A stream channel feeding a PIO TX FIFO, re-armed forever by a control channel
// that copies *ptr_var into the stream channel's read address trigger.
static void init_dma_pair(uint stream, uint ctrl, uint sm, const volatile void *ptr_var, uint words) {
    dma_channel_config c = dma_channel_get_default_config(stream);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&c, ctrl);
    dma_channel_configure(stream, &c, &pio->txf[sm], NULL, words, false);

    c = dma_channel_get_default_config(ctrl);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(ctrl, &c, &dma_hw->ch[stream].al3_read_addr_trig, ptr_var, 1, false);
}

bool hub75_init(void) {
    if (!claim_pio())
        return false;

    lsb_cycles = clock_get_hz(clk_sys) / 2000000; // 0.5 µs for the least significant plane
    build_gamma();
    memset(fb, 0, sizeof(fb));
    data_ptr = fb[0];
    build_row_table(brightness);

    // SMs first: pio_sm_init() executes a jmp whose side-set drives OE low. init_pins()
    // then sets OE high before the pins are handed to the PIO.
    init_sms();
    init_pins();

    data_ch = (uint)dma_claim_unused_channel(true);
    data_ctrl_ch = (uint)dma_claim_unused_channel(true);
    row_ch = (uint)dma_claim_unused_channel(true);
    row_ctrl_ch = (uint)dma_claim_unused_channel(true);
    init_dma_pair(data_ch, data_ctrl_ch, sm_data, &data_ptr, FB_WORDS);
    init_dma_pair(row_ch, row_ctrl_ch, sm_row, &row_ptr, ROW_ENTRIES);

    dma_irqn_set_channel_enabled(DMA_IRQ_INDEX, data_ch, true);
    irq_add_shared_handler(dma_get_irq_num(DMA_IRQ_INDEX), dma_irq_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(dma_get_irq_num(DMA_IRQ_INDEX), true);

    pio_enable_sm_mask_in_sync(pio, (1u << sm_data) | (1u << sm_row));
    dma_start_channel_mask((1u << data_ctrl_ch) | (1u << row_ctrl_ch));
    return true;
}

// Canvas index for shift position i (0..CHAIN_W-1: panel 1, then panel 2) and panel row py (0..63).
static inline uint canvas_index(uint i, uint py) {
    uint half = i / HUB75_PANEL_W;
    uint x = i % HUB75_PANEL_W;
    bool top = half == mapping.top_half;
    if (top ? mapping.rotate_top : mapping.rotate_bottom) {
        x = HUB75_PANEL_W - 1 - x;
        py = HUB75_PANEL_H - 1 - py;
    }
    return (py + (top ? 0 : HUB75_PANEL_H)) * HUB75_WIDTH + x;
}

void hub75_show(const uint16_t *canvas) {
    uint8_t *dst = (uint8_t *)fb[back_fb];
    uint64_t load = 0;

    for (uint r = 0; r < HUB75_SCAN_ROWS; r++) {
        uint8_t *row = dst + r * HUB75_PLANES * PLANE_BYTES;
        for (uint i = 0; i < HUB75_CHAIN_W; i++) {
            uint16_t c0 = canvas[canvas_index(i, r)];
            uint16_t c1 = canvas[canvas_index(i, r + HUB75_SCAN_ROWS)];
            uint32_t v[6] = {lut5[c0 >> 11], lut6[(c0 >> 5) & 63], lut5[c0 & 31],
                             lut5[c1 >> 11], lut6[(c1 >> 5) & 63], lut5[c1 & 31]};
            if (mapping.swap_rb) {
                uint32_t t0 = v[0], t3 = v[3];
                v[0] = v[2], v[2] = t0;
                v[3] = v[5], v[5] = t3;
            }
            load += v[0] + v[1] + v[2] + v[3] + v[4] + v[5];
            for (uint p = 0; p < HUB75_PLANES; p++) {
                row[p * PLANE_BYTES + i] =
                    (uint8_t)(((v[0] >> p) & 1) | (((v[1] >> p) & 1) << 1) | (((v[2] >> p) & 1) << 2) |
                              (((v[3] >> p) & 1) << 3) | (((v[4] >> p) & 1) << 4) | (((v[5] >> p) & 1) << 5));
            }
        }
    }
    const uint64_t full = (uint64_t)HUB75_WIDTH * HUB75_HEIGHT * 3 * ((1u << HUB75_PLANES) - 1);
    last_load_permille = (uint32_t)(load * 1000 / full);

    // Current limiter: I = idle + full white * load * brightness / 255 must stay in the budget.
    uint8_t b = brightness;
    uint64_t max_load_x_b = (uint64_t)(HUB75_BUDGET_MA - HUB75_IDLE_MA) * 255 * 1000 / HUB75_FULL_WHITE_MA;
    if (last_load_permille && (uint64_t)last_load_permille * b > max_load_x_b)
        b = (uint8_t)(max_load_x_b / last_load_permille);
    if (b != applied_brightness || rows_dirty) {
        rows_dirty = false;
        build_row_table(b);
    }

    // Flip: the control channel picks up the new pointer at the next frame boundary.
    const uint32_t *new_fb = fb[back_fb];
    data_ptr = new_fb;
    absolute_time_t timeout = make_timeout_time_ms(100);
    uint32_t fc = frame_count;
    while (!time_reached(timeout)) {
        if (frame_count == fc)
            continue;
        uintptr_t ra = dma_hw->ch[data_ch].read_addr;
        if (ra >= (uintptr_t)new_fb && ra <= (uintptr_t)(new_fb + FB_WORDS))
            break;
        fc = frame_count;
    }
    back_fb ^= 1;
}

void hub75_set_brightness(uint8_t b) {
    brightness = b;
    rows_dirty = true;
}

uint8_t hub75_get_brightness(void) { return brightness; }

uint8_t hub75_get_applied_brightness(void) { return applied_brightness; }

void hub75_set_clkdiv(uint div) {
    if (div < 1)
        div = 1;
    clkdiv = div;
    pio_sm_set_clkdiv_int_frac8(pio, sm_data, div, 0);
}

uint hub75_get_clkdiv(void) { return clkdiv; }

void hub75_set_mapping(const hub75_mapping_t *m) { mapping = *m; }

hub75_mapping_t hub75_get_mapping(void) { return mapping; }

void hub75_set_row_mode(hub75_row_mode_t mode) {
    row_mode = mode;
    rows_dirty = true;
}

hub75_row_mode_t hub75_get_row_mode(void) { return row_mode; }

uint32_t hub75_frame_count(void) { return frame_count; }

uint32_t hub75_last_load_permille(void) { return last_load_permille; }

// FM6126A (and compatible) panels stay dark until two configuration registers are
// written. Sequence as used by ESP32-HUB75-MatrixPanel-DMA: shift the 16-bit pattern
// repeatedly across the panel and hold LAT high for the last 11/12 clocks. Both panels get
// the same registers, so both clocks run together.
static void fm6126a_write_reg(uint16_t value, int latch_clocks) {
    for (int l = 0; l < HUB75_PANEL_W; l++) {
        bool bit = (value >> (15 - (l % 16))) & 1;
        for (uint d = 0; d < 6; d++)
            gpio_put(HUB75_PIN_DATA_BASE + d, bit);
        gpio_put(HUB75_PIN_LAT, l > HUB75_PANEL_W - latch_clocks);
        busy_wait_us(1);
        gpio_put(HUB75_PIN_CLK1, 1);
        gpio_put(HUB75_PIN_CLK2, 1);
        busy_wait_us(1);
        gpio_put(HUB75_PIN_CLK1, 0);
        gpio_put(HUB75_PIN_CLK2, 0);
    }
    gpio_put(HUB75_PIN_LAT, 0);
}

void hub75_fm6126a_init(void) {
    for (uint pin = HUB75_PIN_DATA_BASE; pin <= HUB75_PIN_OE; pin++) {
        gpio_init(pin);
        gpio_put(pin, pin == HUB75_PIN_OE);
        gpio_set_dir(pin, GPIO_OUT);
    }
    fm6126a_write_reg(0x7fff, 12);
    fm6126a_write_reg(0x0040, 13);
}
