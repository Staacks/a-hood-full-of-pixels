#include "link.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "link_gatt.h" // generated from link_gatt.gatt
#include "link_proto.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "player.h"

#define VALUE_HANDLE ATT_CHARACTERISTIC_5e4b7084_2340_482e_8ed0_56093e6e9232_01_VALUE_HANDLE
#define CCC_HANDLE ATT_CHARACTERISTIC_5e4b7084_2340_482e_8ed0_56093e6e9232_01_CLIENT_CONFIGURATION_HANDLE
#define ADV_INTERVAL 160 // 100 ms (unit 0.625 ms)

static btstack_packet_callback_registration_t hci_callback;
static uint8_t adv_data[3 + 18];
static uint8_t scan_resp[2 + sizeof(LINK_DEVICE_NAME) - 1];

static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static bool notify_enabled;
static msg_sync_resp_t pending; // answer waiting for ATT_EVENT_CAN_SEND_NOW
static bool have_pending;
static bool heartbeat_due;      // answer to the controller's heartbeat, also waiting

// Scene requests from the controller (EVENT, or a HEARTBEAT naming an event we missed), taken
// over by link_poll() in the main loop. Written in the Bluetooth context, read under the lock.
static bool have_event_id;
static uint16_t last_event_id;
static bool req_pending;
static int req_scene;
static uint32_t req_start_us;
static uint32_t events, events_repeated, heartbeats, adopted;
static uint8_t controller_status;
static uint64_t last_heartbeat_us;

static volatile uint32_t pings, overwritten;
static volatile uint64_t last_ping_us;
static uint64_t status_since_us;
static uint16_t conn_interval; // unit 1.25 ms
static uint32_t connects;

static void lock(void) { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }
static void unlock(void) { async_context_release_lock(cyw43_arch_async_context()); }

static void start_advertising(void) {
    bd_addr_t null_addr = {0};
    // flags: LE general discoverable, no BR/EDR
    adv_data[0] = 2;
    adv_data[1] = BLUETOOTH_DATA_TYPE_FLAGS;
    adv_data[2] = 0x06;
    // our service UUID (little endian in advertising data)
    const uint8_t uuid[16] = LINK_SERVICE_UUID128;
    adv_data[3] = 17;
    adv_data[4] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    reverse_128(uuid, &adv_data[5]);
    scan_resp[0] = sizeof(scan_resp) - 1;
    scan_resp[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&scan_resp[2], LINK_DEVICE_NAME, sizeof(LINK_DEVICE_NAME) - 1);

    gap_advertisements_set_params(ADV_INTERVAL, ADV_INTERVAL, 0 /* ADV_IND */, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), adv_data);
    gap_scan_response_set_data(sizeof(scan_resp), scan_resp);
    gap_advertisements_enable(1); // BTstack re-enables it after each disconnect
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET)
        return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            bd_addr_t addr;
            gap_local_bd_addr(addr);
            printf("BLE: advertising as '%s' (%s)\n", LINK_DEVICE_NAME, bd_addr_to_str(addr));
            start_advertising();
        }
        break;
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE &&
            gap_subevent_le_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
            con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            conn_interval = gap_subevent_le_connection_complete_get_conn_interval(packet);
            connects++;
            printf("BLE: controller connected, interval %.2f ms\n", conn_interval * 1.25f);
        }
        break;
    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
            conn_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
            printf("BLE: connection interval now %.2f ms\n", conn_interval * 1.25f);
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == con_handle) {
            printf("BLE: controller disconnected (reason 0x%02x), advertising again\n",
                   hci_event_disconnection_complete_get_reason(packet));
            con_handle = HCI_CON_HANDLE_INVALID;
            notify_enabled = false;
            have_pending = false;
            heartbeat_due = false;
        }
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        if (con_handle == HCI_CON_HANDLE_INVALID)
            break;
        if (have_pending) { // time critical: first
            pending.t3 = time_us_32();
            att_server_notify(con_handle, VALUE_HANDLE, (uint8_t *)&pending, sizeof(pending));
            have_pending = false;
        } else if (heartbeat_due) {
            msg_heartbeat_t hb = {.type = MSG_HEARTBEAT,
                                  .status = player_has_video() ? (player_scene() ? LINK_STATUS_PLAYING
                                                                                 : LINK_STATUS_IDLE)
                                                               : LINK_STATUS_ERROR,
                                  .scene = (uint8_t)player_scene(),
                                  .event_id = last_event_id};
            att_server_notify(con_handle, VALUE_HANDLE, (uint8_t *)&hb, sizeof(hb));
            heartbeat_due = false;
        }
        if (have_pending || heartbeat_due)
            att_server_request_can_send_now_event(con_handle);
        break;
    default:
        break;
    }
}

static int att_write_callback(hci_con_handle_t handle, uint16_t att_handle, uint16_t transaction_mode,
                              uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)transaction_mode;
    (void)offset;
    uint32_t t2 = time_us_32();
    if (att_handle == CCC_HANDLE) {
        notify_enabled = buffer_size >= 2 &&
                         little_endian_read_16(buffer, 0) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
        con_handle = handle;
        return 0;
    }
    if (att_handle != VALUE_HANDLE || buffer_size < 1)
        return 0;
    if (buffer[0] == MSG_SYNC_REQ && buffer_size >= sizeof(msg_sync_req_t)) {
        msg_sync_req_t req;
        memcpy(&req, buffer, sizeof(req));
        pings++;
        last_ping_us = time_us_64();
        if (have_pending)
            overwritten++;
        pending = (msg_sync_resp_t){.type = MSG_SYNC_RESP, .seq = req.seq, .t1 = req.t1, .t2 = t2};
        have_pending = true;
    } else if (buffer[0] == MSG_EVENT && buffer_size >= sizeof(msg_event_t)) {
        msg_event_t ev;
        memcpy(&ev, buffer, sizeof(ev));
        if (have_event_id && ev.event_id == last_event_id) {
            events_repeated++;
            return 0;
        }
        events++;
        have_event_id = true;
        last_event_id = ev.event_id;
        req_scene = ev.scene;
        req_start_us = ev.flags & EVENT_HAS_TIME ? ev.start_us : t2 + ev.delay_us;
        if (!req_start_us)
            req_start_us = 1; // 0 means "now"
        req_pending = true;
        return 0;
    } else if (buffer[0] == MSG_HEARTBEAT && buffer_size >= sizeof(msg_heartbeat_t)) {
        msg_heartbeat_t hb;
        memcpy(&hb, buffer, sizeof(hb));
        heartbeats++;
        last_heartbeat_us = time_us_64();
        controller_status = hb.status;
        // An event we never got (lost, or we rebooted): show the controller's scene from now on.
        if (!have_event_id || hb.event_id != last_event_id) {
            adopted++;
            have_event_id = true;
            last_event_id = hb.event_id;
            if (hb.scene != player_scene()) {
                req_scene = hb.scene;
                req_start_us = 0;
                req_pending = true;
            }
        }
        heartbeat_due = true;
    } else {
        return 0;
    }
    if (notify_enabled)
        att_server_request_can_send_now_event(handle);
    return 0;
}

void link_init(void) {
    lock();
    l2cap_init();
    sm_init();
    att_server_init(profile_data, NULL, att_write_callback);
    hci_callback.callback = packet_handler;
    hci_add_event_handler(&hci_callback);
    att_server_register_packet_handler(packet_handler);
    status_since_us = time_us_64();
    hci_power_control(HCI_POWER_ON);
    unlock();
}

bool link_connected(void) { return con_handle != HCI_CON_HANDLE_INVALID; }

void link_poll(void) {
    lock();
    bool go = req_pending;
    int scene = req_scene;
    uint32_t start = req_start_us;
    req_pending = false;
    unlock();
    if (!go)
        return;
    if (scene < 0 || scene >= PLAYER_SCENES) {
        printf("link: unknown scene %d\n", scene);
        return;
    }
    int32_t in_us = start ? (int32_t)(start - time_us_32()) : 0;
    printf("link: scene %d %s, starts in %ld ms\n", scene, player_scene_name(scene), (long)(in_us / 1000));
    player_play(scene, start);
}

void link_print_status(void) {
    uint64_t now = time_us_64();
    uint32_t n = pings, lost = overwritten;
    pings = overwritten = 0;
    float secs = (float)(now - status_since_us) / 1e6f;
    status_since_us = now;
    if (!link_connected()) {
        printf("BLE: not connected, advertising (connects so far: %lu)\n", (unsigned long)connects);
        return;
    }
    printf("BLE: connected, interval %.2f ms, notifications %s | last %.0f s: %lu pings answered (%.1f/s)",
           conn_interval * 1.25f, notify_enabled ? "on" : "OFF", secs, (unsigned long)n, secs > 0 ? n / secs : 0.0f);
    if (lost)
        printf(", %lu answers dropped (next ping came first)", (unsigned long)lost);
    if (last_ping_us)
        printf(", last %.1f s ago", (now - last_ping_us) / 1e6f);
    printf("\nlink: %lu events (%lu repeats ignored), %lu heartbeats (last %.1f s ago, controller %s), "
           "%lu missed events taken from a heartbeat\n",
           (unsigned long)events, (unsigned long)events_repeated, (unsigned long)heartbeats,
           last_heartbeat_us ? (now - last_heartbeat_us) / 1e6f : -1.0f,
           controller_status == LINK_STATUS_SELECTED ? "scene selected" : controller_status == LINK_STATUS_PLAYING ? "playing"
                                                                                                      : "idle",
           (unsigned long)adopted);
}
