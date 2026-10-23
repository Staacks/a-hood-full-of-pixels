#include "ble_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "link_proto.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

// Scanning while the link is down: 20 ms every 100 ms, leaves most airtime to A2DP.
#define SCAN_INTERVAL 160 // unit 0.625 ms
#define SCAN_WINDOW 32
// Connection: 30 ms interval, no peripheral latency, 2 s supervision timeout.
#define CONN_INTERVAL 24 // unit 1.25 ms
#define CONN_SUPERVISION_TIMEOUT 200 // unit 10 ms
#define CONNECT_TIMEOUT_MS 5000
#define SILENT_US 1000000 // no answer for this long: link counts as down
#define RSSI_PERIOD_US 2000000
#define HEARTBEAT_US 1000000
#define EVENT_SENDS 3          // every event is sent this often (the display ignores repeats)
#define EVENT_SPACING_US 10000
#define EVENT_GIVE_UP_US 1000000 // after the start time

typedef enum { LINK_OFF, LINK_SCANNING, LINK_CONNECTING, LINK_DISCOVER_SERVICE, LINK_DISCOVER_CHAR,
               LINK_ENABLE_NOTIFY, LINK_READY } link_state_t;

static const uint8_t service_uuid[16] = LINK_SERVICE_UUID128;
static const uint8_t characteristic_uuid[16] = LINK_CHARACTERISTIC_UUID128;

static btstack_packet_callback_registration_t hci_callback;
static link_state_t state = LINK_OFF;
static bool bt_up;
static unsigned rate_hz = 20;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static gatt_client_service_t service;
static gatt_client_characteristic_t characteristic;
static gatt_client_notification_t listener;
static bool listening;
static btstack_timer_source_t connect_timer;
static uint16_t conn_interval;
static int8_t rssi;

static uint64_t next_ping_us, next_rssi_us, next_heartbeat_us;
static uint8_t seq;
static volatile uint64_t last_answer_us;

// Scene state for heartbeats, the event being sent, the display's last heartbeat.
static msg_heartbeat_t own = {.type = MSG_HEARTBEAT};
static struct {
    bool active;
    uint16_t id;
    uint8_t scene;
    uint32_t start_us; // controller time
    unsigned sent;
    uint64_t next_us;
} event;
static msg_heartbeat_t display_hb;
static bool have_display_hb;
static uint64_t display_hb_us;
static uint32_t events_sent, events_without_time, events_given_up;

// RTT histogram upper bounds in ms (last bucket: everything above)
static const unsigned rtt_bounds_ms[] = {15, 30, 45, 60, 100, 200};
#define RTT_BUCKETS (sizeof(rtt_bounds_ms) / sizeof(rtt_bounds_ms[0]) + 1)

// Spread of offset estimates, relative to a reference offset (offsets are mod 2^32).
typedef struct {
    uint32_t n;
    int32_t min, max;
    int64_t sum;
} spread_t;

typedef struct {
    uint64_t since_us;
    uint32_t sent, send_busy, received;
    uint32_t rtt_min_us, rtt_max_us;
    uint64_t rtt_sum_us;
    uint32_t hist[RTT_BUCKETS];
    spread_t ntp, event; // both relative to offset_ref
} stats_t;
static stats_t st;
static uint32_t total_connects, total_disconnects;

// Clock offset (display clock - controller clock, mod 2^32) from connection events, see
// handle_sync_resp(). Filtered: median of the last OFFSET_HISTORY samples, relative to offset_ref.
#define OFFSET_HISTORY 9
static bool have_offset;
static uint32_t offset_ref;
static int32_t offset_hist[OFFSET_HISTORY];
static unsigned offset_count, offset_next;
static uint32_t prev_mean_offset; // for the drift estimate between status prints
static uint64_t prev_mean_us;
static bool have_prev_mean;

static void lock(void) { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }
static void unlock(void) { async_context_release_lock(cyw43_arch_async_context()); }

static void spread_reset(spread_t *sp) {
    sp->n = 0;
    sp->sum = 0;
    sp->min = INT32_MAX;
    sp->max = INT32_MIN;
}

static void spread_add(spread_t *sp, int32_t d) {
    sp->n++;
    sp->sum += d;
    if (d < sp->min)
        sp->min = d;
    if (d > sp->max)
        sp->max = d;
}

static void reset_stats(uint64_t now) {
    memset(&st, 0, sizeof(st));
    st.since_us = now;
    st.rtt_min_us = UINT32_MAX;
    spread_reset(&st.ntp);
    spread_reset(&st.event);
}

static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static const char *state_name(link_state_t s) {
    static const char *const names[] = {"off", "scanning", "connecting", "discovering", "discovering",
                                        "enabling notifications", "connected"};
    return names[s];
}

// ------------------------------------------------------------------ connection

static void start_scan(void) {
    state = LINK_SCANNING;
    gap_set_scan_parameters(0 /* passive */, SCAN_INTERVAL, SCAN_WINDOW);
    gap_start_scan();
}

static void connect_timeout(btstack_timer_source_t *ts) {
    (void)ts;
    if (state != LINK_CONNECTING)
        return;
    printf("BLE: connect timed out, scanning again\n");
    gap_connect_cancel();
    start_scan();
}

static void handle_sync_resp(const uint8_t *data, uint16_t len, uint32_t t4);

static void handle_message(const uint8_t *data, uint16_t len) {
    uint32_t t4 = time_us_32();
    if (len >= sizeof(msg_sync_resp_t) && data[0] == MSG_SYNC_RESP) {
        handle_sync_resp(data, len, t4);
    } else if (len >= sizeof(msg_heartbeat_t) && data[0] == MSG_HEARTBEAT) {
        memcpy(&display_hb, data, sizeof(display_hb));
        have_display_hb = true;
        display_hb_us = time_us_64();
        last_answer_us = display_hb_us;
    }
}

static void handle_sync_resp(const uint8_t *data, uint16_t len, uint32_t t4) {
    (void)len;
    msg_sync_resp_t m;
    memcpy(&m, data, sizeof(m));
    if ((int32_t)(m.t1 - (uint32_t)st.since_us) < 0)
        return; // ping from before the last statistics reset
    uint32_t rtt = (t4 - m.t1) - (m.t3 - m.t2);
    st.received++;
    st.rtt_sum_us += rtt;
    if (rtt < st.rtt_min_us)
        st.rtt_min_us = rtt;
    if (rtt > st.rtt_max_us)
        st.rtt_max_us = rtt;
    unsigned b = 0;
    while (b < RTT_BUCKETS - 1 && rtt >= rtt_bounds_ms[b] * 1000u)
        b++;
    st.hist[b]++;

    // Clock offset from connection events: the display received the ping (t2) at connection
    // event k, we received the answer (t4) at event k + n, and events are exactly one interval I
    // apart, so display - controller = t2 - t4 + n * I (plus the difference of the two radio-to-
    // software delays, ~1 ms). A ping cannot be sent before t1, so RTT >= n * I: an answer with
    // RTT < 2 I took exactly one interval. Longer ones (skipped events, retransmissions) are
    // dropped. The NTP estimate (t2 - t1) - RTT / 2 is kept for comparison; with BLE it is
    // biased by about half an interval and scatters by up to one interval.
    uint32_t interval_us = conn_interval * 1250u;
    uint32_t ntp = (m.t2 - m.t1) - rtt / 2;
    if (interval_us && rtt + 2000 < 2 * interval_us) {
        uint32_t offset = m.t2 - t4 + interval_us;
        if (!have_offset) {
            have_offset = true;
            offset_ref = offset;
        }
        int32_t d = (int32_t)(offset - offset_ref);
        offset_hist[offset_next] = d;
        offset_next = (offset_next + 1) % OFFSET_HISTORY;
        if (offset_count < OFFSET_HISTORY)
            offset_count++;
        spread_add(&st.event, d);
    }
    if (have_offset)
        spread_add(&st.ntp, (int32_t)(ntp - offset_ref));
    last_answer_us = time_us_64();
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)size;
    uint8_t type = hci_event_packet_get_type(packet);

    if (type == GATT_EVENT_NOTIFICATION) {
        if (state == LINK_READY)
            handle_message(gatt_event_notification_get_value(packet), gatt_event_notification_get_value_length(packet));
        return;
    }
    switch (state) {
    case LINK_DISCOVER_SERVICE:
        if (type == GATT_EVENT_SERVICE_QUERY_RESULT) {
            gatt_event_service_query_result_get_service(packet, &service);
        } else if (type == GATT_EVENT_QUERY_COMPLETE) {
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                printf("BLE: service discovery failed (0x%02x)\n", gatt_event_query_complete_get_att_status(packet));
                gap_disconnect(con_handle);
                break;
            }
            state = LINK_DISCOVER_CHAR;
            gatt_client_discover_characteristics_for_service_by_uuid128(gatt_handler, con_handle, &service,
                                                                        characteristic_uuid);
        }
        break;
    case LINK_DISCOVER_CHAR:
        if (type == GATT_EVENT_CHARACTERISTIC_QUERY_RESULT) {
            gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
        } else if (type == GATT_EVENT_QUERY_COMPLETE) {
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                printf("BLE: characteristic discovery failed (0x%02x)\n",
                       gatt_event_query_complete_get_att_status(packet));
                gap_disconnect(con_handle);
                break;
            }
            gatt_client_listen_for_characteristic_value_updates(&listener, gatt_handler, con_handle, &characteristic);
            listening = true;
            state = LINK_ENABLE_NOTIFY;
            gatt_client_write_client_characteristic_configuration(gatt_handler, con_handle, &characteristic,
                                                                  GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
        }
        break;
    case LINK_ENABLE_NOTIFY:
        if (type == GATT_EVENT_QUERY_COMPLETE) {
            if (gatt_event_query_complete_get_att_status(packet) != ATT_ERROR_SUCCESS) {
                printf("BLE: enabling notifications failed (0x%02x)\n",
                       gatt_event_query_complete_get_att_status(packet));
                gap_disconnect(con_handle);
                break;
            }
            state = LINK_READY;
            printf("BLE: link to the display ready\n");
        }
        break;
    default:
        break;
    }
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET)
        return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        bt_up = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
        if (bt_up && rate_hz > 0)
            start_scan();
        break;

    case GAP_EVENT_ADVERTISING_REPORT: {
        if (state != LINK_SCANNING)
            break;
        if (!ad_data_contains_uuid128(gap_event_advertising_report_get_data_length(packet),
                                      gap_event_advertising_report_get_data(packet), service_uuid))
            break;
        bd_addr_t addr;
        gap_event_advertising_report_get_address(packet, addr);
        bd_addr_type_t addr_type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet);
        printf("BLE: display found at %s (rssi %d dBm), connecting\n", bd_addr_to_str(addr),
               (int8_t)gap_event_advertising_report_get_rssi(packet));
        gap_stop_scan();
        state = LINK_CONNECTING;
        gap_set_connection_parameters(SCAN_INTERVAL, SCAN_WINDOW, CONN_INTERVAL, CONN_INTERVAL, 0,
                                      CONN_SUPERVISION_TIMEOUT, 0, 8);
        gap_connect(addr, addr_type);
        btstack_run_loop_set_timer_handler(&connect_timer, connect_timeout);
        btstack_run_loop_set_timer(&connect_timer, CONNECT_TIMEOUT_MS);
        btstack_run_loop_add_timer(&connect_timer);
        break;
    }

    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE ||
            state != LINK_CONNECTING)
            break;
        btstack_run_loop_remove_timer(&connect_timer);
        if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
            printf("BLE: connection failed (0x%02x), scanning again\n",
                   gap_subevent_le_connection_complete_get_status(packet));
            start_scan();
            break;
        }
        con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
        conn_interval = gap_subevent_le_connection_complete_get_conn_interval(packet);
        total_connects++;
        printf("BLE: connected, interval %.2f ms, discovering the link service\n", conn_interval * 1.25f);
        state = LINK_DISCOVER_SERVICE;
        gatt_client_discover_primary_services_by_uuid128(gatt_handler, con_handle, service_uuid);
        break;

    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE &&
            hci_subevent_le_connection_update_complete_get_connection_handle(packet) == con_handle)
            conn_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
        break;

    case GAP_EVENT_RSSI_MEASUREMENT:
        if (gap_event_rssi_measurement_get_con_handle(packet) == con_handle)
            rssi = (int8_t)gap_event_rssi_measurement_get_rssi(packet);
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) != con_handle)
            break; // e.g. the speaker
        printf("BLE: disconnected from the display (reason 0x%02x)\n",
               hci_event_disconnection_complete_get_reason(packet));
        total_disconnects++;
        con_handle = HCI_CON_HANDLE_INVALID;
        have_display_hb = false;
        rssi = 0;
        if (listening) {
            gatt_client_stop_listening_for_characteristic_value_updates(&listener);
            listening = false;
        }
        if (rate_hz > 0 && bt_up)
            start_scan();
        else
            state = LINK_OFF;
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------------ API

void ble_link_init(void) {
    lock();
    gatt_client_init();
    hci_callback.callback = hci_handler;
    hci_add_event_handler(&hci_callback);
    reset_stats(time_us_64());
    unlock();
}

static bool ble_link_clock_offset_locked(uint32_t *offset_us);

// Sends the pending event (lock held). Returns true if something was sent.
static bool send_event(uint64_t now) {
    if (!event.active || now < event.next_us)
        return false;
    int32_t until_start = (int32_t)(event.start_us - time_us_32());
    if (until_start < -(int32_t)EVENT_GIVE_UP_US) {
        event.active = false;
        if (!event.sent)
            events_given_up++;
        return false;
    }
    uint32_t offset;
    msg_event_t m = {.type = MSG_EVENT, .event_id = event.id, .scene = event.scene,
                     .delay_us = until_start > 0 ? (uint32_t)until_start : 0};
    if (ble_link_clock_offset_locked(&offset)) {
        m.flags = EVENT_HAS_TIME;
        m.start_us = event.start_us + offset;
    }
    if (gatt_client_write_value_of_characteristic_without_response(con_handle, characteristic.value_handle,
                                                                   sizeof(m), (uint8_t *)&m) != ERROR_CODE_SUCCESS)
        return true; // busy: next poll
    if (!event.sent++) {
        events_sent++;
        if (!(m.flags & EVENT_HAS_TIME))
            events_without_time++;
    }
    event.next_us = now + EVENT_SPACING_US;
    if (event.sent >= EVENT_SENDS)
        event.active = false;
    return true;
}

void ble_link_poll(void) {
    uint64_t now = time_us_64();
    if (state != LINK_READY)
        return;
    lock();
    bool busy = state == LINK_READY && send_event(now);
    if (!busy && state == LINK_READY && !event.active && now >= next_heartbeat_us) {
        next_heartbeat_us = now + HEARTBEAT_US;
        gatt_client_write_value_of_characteristic_without_response(con_handle, characteristic.value_handle,
                                                                   sizeof(own), (uint8_t *)&own);
        busy = true;
    }
    unlock();
    if (busy)
        return;
    if (now >= next_rssi_us) {
        next_rssi_us = now + RSSI_PERIOD_US;
        lock();
        gap_read_rssi(con_handle);
        unlock();
    }
    if (rate_hz == 0 || now < next_ping_us)
        return;
    next_ping_us = now + 1000000 / rate_hz;
    lock();
    msg_sync_req_t m = {.type = MSG_SYNC_REQ, .seq = ++seq, .t1 = time_us_32()};
    if (state == LINK_READY) {
        if (gatt_client_write_value_of_characteristic_without_response(con_handle, characteristic.value_handle,
                                                                       sizeof(m), (uint8_t *)&m) == ERROR_CODE_SUCCESS)
            st.sent++;
        else
            st.send_busy++;
    }
    unlock();
}

void ble_link_set_scene(link_status_t status, int scene, uint16_t event_id) {
    lock();
    own.status = (uint8_t)status;
    own.scene = (uint8_t)scene;
    own.event_id = event_id;
    unlock();
}

void ble_link_fire(uint16_t event_id, int scene, uint32_t start_us) {
    lock();
    event.active = true;
    event.id = event_id;
    event.scene = (uint8_t)scene;
    event.start_us = start_us;
    event.sent = 0;
    event.next_us = 0;
    own.scene = (uint8_t)scene;
    own.event_id = event_id;
    unlock();
}

bool ble_link_display_scene(int *scene, bool *has_video) {
    lock();
    bool ok = have_display_hb && state == LINK_READY;
    *scene = display_hb.scene;
    *has_video = display_hb.status != LINK_STATUS_ERROR;
    unlock();
    return ok;
}

void ble_link_set_rate(unsigned hz) {
    lock();
    if (hz == 0 && rate_hz > 0) {
        if (con_handle != HCI_CON_HANDLE_INVALID)
            gap_disconnect(con_handle); // the disconnect event sets LINK_OFF
        else {
            if (state == LINK_SCANNING)
                gap_stop_scan();
            else if (state == LINK_CONNECTING)
                gap_connect_cancel();
            btstack_run_loop_remove_timer(&connect_timer);
            state = LINK_OFF;
        }
    } else if (hz > 0 && rate_hz == 0 && bt_up && state == LINK_OFF) {
        start_scan();
    }
    rate_hz = hz;
    unlock();
}

unsigned ble_link_rate(void) { return rate_hz; }

bool ble_link_connected(void) { return state == LINK_READY; }

static bool ble_link_clock_offset_locked(uint32_t *offset_us) {
    unsigned n = offset_count;
    if (n == 0)
        return false;
    int32_t v[OFFSET_HISTORY];
    memcpy(v, offset_hist, sizeof(v));
    qsort(v, n, sizeof(v[0]), cmp_i32);
    *offset_us = offset_ref + (uint32_t)v[n / 2];
    return true;
}

bool ble_link_clock_offset(uint32_t *offset_us) {
    lock();
    bool ok = ble_link_clock_offset_locked(offset_us);
    unlock();
    return ok;
}

bool ble_link_up(void) { return state == LINK_READY && time_us_64() - last_answer_us < SILENT_US; }

// Clock sync quality since the last status: spread of the connection-event estimates (what the
// filter gets), spread of the NTP estimates, NTP bias, drift between the two clocks.
static void print_sync(const stats_t *s, uint64_t now) {
    if (s->event.n == 0) {
        if (s->received)
            printf("    sync: no usable answers (all RTTs >= 2 intervals)\n");
        return;
    }
    float ev_mean = (float)s->event.sum / s->event.n;
    printf("    sync: %lu of %lu answers usable, connection-event offset spread %.2f ms", (unsigned long)s->event.n,
           (unsigned long)s->received, (s->event.max - s->event.min) / 1000.0f);
    if (s->ntp.n)
        printf(" (NTP: spread %.1f ms, bias %+.1f ms)", (s->ntp.max - s->ntp.min) / 1000.0f,
               ((float)s->ntp.sum / s->ntp.n - ev_mean) / 1000.0f);
    uint32_t filtered;
    if (ble_link_clock_offset(&filtered))
        printf(", filtered offset %+.3f s", (int32_t)filtered / 1e6f);
    // Drift: change of the mean offset since the previous status print.
    uint32_t mean_abs = offset_ref + (uint32_t)(int32_t)ev_mean;
    if (have_prev_mean && now > prev_mean_us)
        printf(", drift %+.1f ppm", (int32_t)(mean_abs - prev_mean_offset) * 1e6f / (float)(now - prev_mean_us));
    prev_mean_offset = mean_abs;
    prev_mean_us = now;
    have_prev_mean = true;
    printf("\n");
}

void ble_link_print_status(void) {
    uint64_t now = time_us_64();
    lock();
    stats_t s = st;
    reset_stats(now);
    link_state_t cur = state;
    unlock();

    printf("BLE: %s", rate_hz ? state_name(cur) : "off");
    if (cur == LINK_READY)
        printf(", interval %.2f ms, RSSI %d dBm", conn_interval * 1.25f, rssi);
    printf(", %u Hz pings, connects %lu, disconnects %lu", rate_hz, (unsigned long)total_connects,
           (unsigned long)total_disconnects);
    if (s.sent == 0 && s.send_busy == 0) {
        printf("\n");
        return;
    }
    float secs = (float)(now - s.since_us) / 1e6f;
    // Pings sent in the last ~100 ms may still be in flight; they count as lost here.
    int32_t lost = (int32_t)s.sent - (int32_t)s.received;
    printf(" | last %.0f s: sent %lu, answered %lu, lost %ld", secs, (unsigned long)s.sent, (unsigned long)s.received,
           (long)(lost > 0 ? lost : 0));
    if (s.send_busy)
        printf(", not sent (busy) %lu", (unsigned long)s.send_busy);
    if (s.received) {
        printf(", RTT min %.1f avg %.1f max %.1f ms\n    RTT histogram:", s.rtt_min_us / 1000.0f,
               (float)s.rtt_sum_us / s.received / 1000.0f, s.rtt_max_us / 1000.0f);
        for (unsigned b = 0; b < RTT_BUCKETS; b++) {
            if (b < RTT_BUCKETS - 1)
                printf(" <%ums %lu", rtt_bounds_ms[b], (unsigned long)s.hist[b]);
            else
                printf(" >=%ums %lu", rtt_bounds_ms[b - 1], (unsigned long)s.hist[b]);
        }
    }
    printf("\n");
    print_sync(&s, now);
    lock();
    uint32_t sent = events_sent, no_time = events_without_time, given_up = events_given_up;
    msg_heartbeat_t hb = display_hb;
    uint16_t own_event = own.event_id;
    bool have_hb = have_display_hb;
    uint64_t hb_us = display_hb_us;
    unlock();
    printf("    events: %lu sent (%lu without clock offset, %lu not sent: link busy)", (unsigned long)sent,
           (unsigned long)no_time, (unsigned long)given_up);
    if (have_hb)
        printf(" | display: scene %u, %s, last event %u (ours %u), heartbeat %.1f s ago", hb.scene,
               hb.status == LINK_STATUS_ERROR ? "NO VIDEO (SD card?)" : "video ok", hb.event_id, own_event,
               (now - hb_us) / 1e6f);
    printf("\n");
}
