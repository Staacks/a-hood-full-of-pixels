// Controller side of the link to the display: BLE central. Scans for the display's service,
// connects, reconnects by itself. Sends SYNC_REQ pings (round-trip / clock offset statistics),
// scene events and heartbeats (see common/link_proto.h). Call from thread context (core 0).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link_proto.h"

// Call between bt_setup_init() and bt_setup_power_on().
void ble_link_init(void);

// Call from the main loop: sends events, heartbeats and pings.
void ble_link_poll(void);

// A scene was fired: the display starts it at start_us (controller time_us_32(); converted with
// the clock offset). Sent up to 3 times while the link is up; dropped 1 s after the start time.
void ble_link_fire(uint16_t event_id, int scene, uint32_t start_us);

// The staff's state for the heartbeats (about 1 Hz): status, current scene and the event that
// set it. A display that missed the event takes the scene from the heartbeat.
void ble_link_set_scene(link_status_t status, int scene, uint16_t event_id);

// The display's scene and whether it plays video (not the fallback eyes), from its last
// heartbeat. False while not connected or before the first heartbeat.
bool ble_link_display_scene(int *scene, bool *has_video);

// Ping rate in Hz; 0 switches the link off (disconnect, no scanning), for comparisons.
void ble_link_set_rate(unsigned hz);
unsigned ble_link_rate(void);

// Connected and set up.
bool ble_link_connected(void);
// Connected and the display answered within the last second.
bool ble_link_up(void);

// Clock offset display - controller in µs (mod 2^32: display time_us_32() = controller
// time_us_32() + offset), median of the last connection-event estimates. False until the first
// usable answer.
bool ble_link_clock_offset(uint32_t *offset_us);

// Prints link state and ping statistics since the last call, then resets them.
void ble_link_print_status(void);
