// Link between the controller (BLE central, time master) and the display (BLE peripheral).
//
// The display offers one GATT service with one characteristic (display/link_gatt.gatt): the
// controller writes messages to it (write without response), the display answers with
// notifications. Every message fits into one ATT packet with the default MTU (20 bytes).
//
// Times are time_us_32() of the sender (except msg_event_t.start_us, which the controller
// converts to the display's clock). They wrap after 71 minutes; only differences are used.
#pragma once

#include <stdint.h>

// Big-endian (as written): 5e4b7083-2340-482e-8ed0-56093e6e9232 / ...7084...; keep in sync with
// display/link_gatt.gatt.
#define LINK_SERVICE_UUID128 \
    {0x5e, 0x4b, 0x70, 0x83, 0x23, 0x40, 0x48, 0x2e, 0x8e, 0xd0, 0x56, 0x09, 0x3e, 0x6e, 0x92, 0x32}
#define LINK_CHARACTERISTIC_UUID128 \
    {0x5e, 0x4b, 0x70, 0x84, 0x23, 0x40, 0x48, 0x2e, 0x8e, 0xd0, 0x56, 0x09, 0x3e, 0x6e, 0x92, 0x32}
#define LINK_DEVICE_NAME "matrixmask mask"

typedef enum {
    MSG_SYNC_REQ = 1,  // controller -> display
    MSG_SYNC_RESP = 2, // display -> controller
    MSG_EVENT = 3,     // controller -> display
    MSG_HEARTBEAT = 4, // both ways
} msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t seq;
    uint32_t t1; // controller send time
} msg_sync_req_t;

// NTP-style: RTT = (t4 - t1) - (t3 - t2) with t4 = controller receive time;
// display clock - controller clock = (t2 - t1) - RTT / 2 (mod 2^32).
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t seq;
    uint32_t t1; // echoed
    uint32_t t2; // display receive time
    uint32_t t3; // display send time
} msg_sync_resp_t;

// A scene was fired: the display starts it at start_us. The controller sends every event up to
// 3 times; the display ignores repeats (same event_id).
#define EVENT_HAS_TIME 0x01 // start_us is valid (display time base); else start at receipt + delay_us
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    uint16_t event_id;  // new random base at every controller boot
    uint8_t scene;
    uint32_t start_us;  // display time_us_32(), converted by the controller with its clock offset
    uint32_t delay_us;  // start - send time (fallback while the clock offset is unknown)
} msg_event_t;

typedef enum {
    LINK_STATUS_IDLE = 0,    // base scene, nothing selected
    LINK_STATUS_SELECTED = 1, // controller: a scene is selected (a stomp fires it)
    LINK_STATUS_PLAYING = 2, // another scene than the base scene
    LINK_STATUS_ERROR = 3,   // display: no SD card / video file (fallback eyes)
} link_status_t;

// Controller -> display about once per second: the scene the staff shows and the event that set
// it. A display that missed that event (or rebooted) switches to the scene right away. The
// display answers with its own heartbeat (its scene and the last event it saw).
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t status;     // link_status_t
    uint8_t scene;
    uint16_t event_id;  // last event (controller: the event that set the scene)
} msg_heartbeat_t;
