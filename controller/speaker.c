// A2DP source with SBC encoder, based on BTstack's example/a2dp_source_demo.c (without AVRCP;
// Copyright (C) 2016 BlueKitchen GmbH, BTstack licence, see README.md "Licence").
// See a2dp.h for the stream time model.

#include "speaker.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "hci_dump.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define TICK_MS 5                                   // encoder timer period
#define MAX_BACKLOG_FRAMES (SPEAKER_SAMPLE_RATE / 8)   // 125 ms: drop audio beyond this
#define RETRY_MS 5000                               // reconnect interval to a paired speaker
#define STREAM_START_TIMEOUT_MS 5000                // connected, but no stream: retry / reconnect
#define INQUIRY_1280MS 10                           // pairing search, ~13 s
#define SBC_STORAGE_SIZE 1030
#define TLV_TAG_SPEAKER 0x4d4d5350u                 // 'MMSP': address of the paired speaker

// Audio service class + audio/video major device class
#define COD_AUDIO_SINK 0x200400u

static uint8_t sbc_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO, // 44.1 kHz only (our WAV clips), stereo
    0xff,                                      // all block lengths, subbands, allocation methods
    2, 53,                                     // bitpool min, max
};
static uint8_t sbc_configuration[4];
static uint8_t sdp_a2dp_source_buffer[150];
static btstack_packet_callback_registration_t hci_callback;

static speaker_state_t state = SPEAKER_OFF;
static bd_addr_t speaker;
static bool have_speaker;
static bool pairing_connect;       // connecting to a speaker found by the pairing search
static bool pair_after_disconnect; // start the pairing search once the link is down
static bool have_candidate;        // audio sink found by the search, connect once it has stopped
static bd_addr_t candidate;
static uint16_t a2dp_cid;
static uint8_t local_seid;
static bool stream_opened;
static unsigned start_attempts;
static btstack_timer_source_t retry_timer;
static uint16_t delay_report_100us; // latency reported by the speaker (0 = none)

static const btstack_sbc_encoder_t *enc;
static btstack_sbc_encoder_bluedroid_t enc_state;
static struct {
    int sampling_frequency, block_length, subbands, min_bitpool, max_bitpool, channel_mode, allocation_method;
} sbc_conf;

static struct {
    bool running;
    uint64_t t0_us;       // stream time of frame 0
    uint64_t frames_done; // frames encoded or skipped
    uint32_t rtp_timestamp;
    uint8_t storage[SBC_STORAGE_SIZE]; // [0] = SBC media header (number of frames)
    uint16_t storage_count;
    uint8_t storage_frames;
    bool ready_to_send;
    int max_payload;
    uint64_t request_us;
    btstack_timer_source_t timer;
} m;

static struct {
    speaker_sound_fn fn;
    void *ctx;
    uint64_t start_frame;
} snd;

static bool idle_noise;
static uint32_t noise_seed = 1;

// Interval statistics (reset by speaker_print_status) and totals.
typedef struct {
    uint64_t since_us;
    uint32_t packets, sbc_frames;
    uint32_t max_send_gap_us, max_wait_us;
    uint32_t skipped_frames, max_backlog_frames;
    uint64_t encode_us;
    uint64_t last_send_us;
} stats_t;
static stats_t st;
static uint32_t total_connects, total_disconnects, total_stream_starts, total_suspends, total_skipped_frames;

static void lock(void) { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }
static void unlock(void) { async_context_release_lock(cyw43_arch_async_context()); }

const char *speaker_state_name(speaker_state_t s) {
    static const char *const names[] = {"off", "no speaker paired", "waiting to reconnect", "connecting",
                                        "pairing (searching)", "connected, no stream", "streaming"};
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

// ------------------------------------------------------------------ media

static uint64_t frames_due(uint64_t now_us) { return (now_us - m.t0_us) * SPEAKER_SAMPLE_RATE / 1000000; }

static void render(int16_t *lr, unsigned n, uint64_t frame) {
    memset(lr, 0, n * 2 * sizeof(int16_t));
    bool idle = true;
    if (snd.fn && frame + n > snd.start_frame) {
        unsigned off = snd.start_frame > frame ? (unsigned)(snd.start_frame - frame) : 0;
        unsigned want = n - off;
        unsigned got = snd.fn(lr + 2 * off, want, (uint32_t)(frame + off - snd.start_frame), snd.ctx);
        if (got < want)
            snd.fn = NULL;
        idle = false;
    }
    if (idle && idle_noise) {
        for (unsigned i = 0; i < 2 * n; i++) {
            noise_seed = noise_seed * 1664525u + 1013904223u;
            lr[i] = (int16_t)((int32_t)(noise_seed >> 29) - 4); // -4..3, about -80 dBFS
        }
    }
}

static void request_send(void) {
    m.ready_to_send = true;
    m.request_us = time_us_64();
    a2dp_source_stream_endpoint_request_can_send_now(a2dp_cid, local_seid);
}

// Encode all frames whose stream time has come, until the packet is full.
static void fill(void) {
    if (!m.running || m.ready_to_send)
        return;
    uint64_t due = frames_due(time_us_64());
    if (due > m.frames_done + MAX_BACKLOG_FRAMES) {
        // The radio stalled: skip ahead instead of building up latency. The RTP timestamp
        // jumps too, so the speaker can see the gap.
        uint32_t skip = (uint32_t)(due - m.frames_done);
        m.frames_done += skip;
        m.rtp_timestamp += skip;
        st.skipped_frames += skip;
        total_skipped_frames += skip;
    }
    uint32_t backlog = (uint32_t)(due - m.frames_done);
    if (backlog > st.max_backlog_frames)
        st.max_backlog_frames = backlog;

    unsigned n = enc->num_audio_frames(&enc_state);
    while (m.frames_done + n <= due) {
        uint16_t len = enc->sbc_buffer_length(&enc_state);
        if (1 + m.storage_count + len > m.max_payload)
            break;
        int16_t pcm[256 * 2];
        render(pcm, n, m.frames_done);
        uint64_t t = time_us_64();
        enc->encode_signed_16(&enc_state, pcm, &m.storage[1 + m.storage_count]);
        st.encode_us += time_us_64() - t;
        m.storage_count += enc->sbc_buffer_length(&enc_state);
        m.storage_frames++;
        m.frames_done += n;
    }
    if (m.storage_frames && 1 + m.storage_count + enc->sbc_buffer_length(&enc_state) > m.max_payload)
        request_send();
}

static void send_packet(void) {
    uint64_t now = time_us_64();
    m.storage[0] = m.storage_frames;
    a2dp_source_stream_send_media_payload_rtp(a2dp_cid, local_seid, 0, m.rtp_timestamp, m.storage,
                                              m.storage_count + 1);
    m.rtp_timestamp += m.storage_frames * enc->num_audio_frames(&enc_state);
    st.packets++;
    st.sbc_frames += m.storage_frames;
    if (st.last_send_us && now - st.last_send_us > st.max_send_gap_us)
        st.max_send_gap_us = (uint32_t)(now - st.last_send_us);
    st.last_send_us = now;
    if (now - m.request_us > st.max_wait_us)
        st.max_wait_us = (uint32_t)(now - m.request_us);
    m.storage_count = 0;
    m.storage_frames = 0;
    m.ready_to_send = false;
}

static void media_tick(btstack_timer_source_t *ts) {
    btstack_run_loop_set_timer(ts, TICK_MS);
    btstack_run_loop_add_timer(ts);
    fill();
}

static void media_start(void) {
    m.max_payload = btstack_min(a2dp_max_media_payload_size(a2dp_cid, local_seid), SBC_STORAGE_SIZE);
    m.t0_us = time_us_64();
    m.frames_done = 0;
    m.storage_count = 0;
    m.storage_frames = 0;
    m.ready_to_send = false;
    m.running = true;
    snd.fn = NULL;
    st.last_send_us = 0;
    btstack_run_loop_remove_timer(&m.timer);
    btstack_run_loop_set_timer_handler(&m.timer, media_tick);
    btstack_run_loop_set_timer(&m.timer, TICK_MS);
    btstack_run_loop_add_timer(&m.timer);
}

static void media_stop(void) {
    m.running = false;
    snd.fn = NULL;
    btstack_run_loop_remove_timer(&m.timer);
}

// ------------------------------------------------------------------ connection

static void schedule_retry(uint32_t ms);

static void connect_now(void) {
    printf("BT: connecting to %s\n", bd_addr_to_str(speaker));
    state = SPEAKER_CONNECTING;
    uint8_t status = a2dp_source_establish_stream(speaker, &a2dp_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("BT: connect failed immediately (0x%02x)\n", status);
        state = SPEAKER_WAITING;
        schedule_retry(RETRY_MS);
    }
}

static void start_inquiry(void) {
    have_candidate = false;
    printf("BT: searching for speakers in pairing mode (~13 s)...\n");
    state = SPEAKER_PAIRING;
    pair_after_disconnect = false;
    btstack_run_loop_remove_timer(&retry_timer);
    gap_inquiry_start(INQUIRY_1280MS);
}

static void idle_state(void) {
    state = have_speaker ? SPEAKER_WAITING : SPEAKER_NO_SPEAKER;
    if (have_speaker)
        schedule_retry(RETRY_MS);
}

static void retry_handler(btstack_timer_source_t *ts) {
    (void)ts;
    switch (state) {
    case SPEAKER_WAITING:
        connect_now();
        break;
    case SPEAKER_CONNECTED:
        if (stream_opened && start_attempts < 3) {
            start_attempts++;
            printf("BT: no stream, starting it (attempt %u)\n", start_attempts);
            a2dp_source_start_stream(a2dp_cid, local_seid);
            schedule_retry(STREAM_START_TIMEOUT_MS);
        } else {
            printf("BT: no stream, reconnecting\n");
            a2dp_source_disconnect(a2dp_cid);
        }
        break;
    default:
        break;
    }
}

static void schedule_retry(uint32_t ms) {
    btstack_run_loop_remove_timer(&retry_timer);
    btstack_run_loop_set_timer_handler(&retry_timer, retry_handler);
    btstack_run_loop_set_timer(&retry_timer, ms);
    btstack_run_loop_add_timer(&retry_timer);
}

static void store_speaker(const bd_addr_t addr) {
    memcpy(speaker, addr, sizeof(bd_addr_t));
    have_speaker = true;
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (tlv)
        tlv->store_tag(ctx, TLV_TAG_SPEAKER, speaker, sizeof(bd_addr_t));
    printf("BT: speaker %s stored\n", bd_addr_to_str(speaker));
}

static void load_speaker(void) {
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    have_speaker = tlv && tlv->get_tag(ctx, TLV_TAG_SPEAKER, speaker, sizeof(bd_addr_t)) == sizeof(bd_addr_t);
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET)
        return;
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING)
            return;
        gap_local_bd_addr(addr);
        printf("BT: up, own address %s\n", bd_addr_to_str(addr));
        if (have_speaker)
            connect_now();
        else {
            state = SPEAKER_NO_SPEAKER;
            printf("BT: no speaker paired yet: put the speaker in pairing mode and start pairing\n");
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        printf("BT: legacy PIN request, answering 0000\n");
        gap_pin_code_response(addr, "0000");
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        gap_ssp_confirmation_response(addr);
        break;
    case GAP_EVENT_INQUIRY_RESULT: {
        if (state != SPEAKER_PAIRING)
            break;
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
        char name[64] = "";
        if (gap_event_inquiry_result_get_name_available(packet)) {
            int len = gap_event_inquiry_result_get_name_len(packet);
            if (len > (int)sizeof(name) - 1)
                len = sizeof(name) - 1;
            memcpy(name, gap_event_inquiry_result_get_name(packet), (size_t)len);
            name[len] = 0;
        }
        int rssi = gap_event_inquiry_result_get_rssi_available(packet) ? (int8_t)gap_event_inquiry_result_get_rssi(packet) : 0;
        bool audio = (cod & COD_AUDIO_SINK) == COD_AUDIO_SINK;
        printf("BT: found %s cod %06" PRIx32 " rssi %d '%s'%s\n", bd_addr_to_str(addr), cod, rssi, name,
               audio && !have_candidate ? " -> audio sink, stopping the search" : "");
        if (audio && !have_candidate) {
            // Connect only after the search has stopped (GAP_EVENT_INQUIRY_COMPLETE).
            have_candidate = true;
            memcpy(candidate, addr, sizeof(bd_addr_t));
            gap_inquiry_stop();
        }
        break;
    }
    case GAP_EVENT_INQUIRY_COMPLETE:
        if (state != SPEAKER_PAIRING)
            break;
        if (!have_candidate) {
            printf("BT: no speaker found (is it in pairing mode?)\n");
            idle_state();
            break;
        }
        have_candidate = false;
        printf("BT: connecting to %s for pairing\n", bd_addr_to_str(candidate));
        pairing_connect = true;
        state = SPEAKER_CONNECTING;
        memcpy(speaker, candidate, sizeof(bd_addr_t));
        {
            uint8_t status = a2dp_source_establish_stream(candidate, &a2dp_cid);
            if (status != ERROR_CODE_SUCCESS) {
                printf("BT: connect failed immediately (0x%02x)\n", status);
                pairing_connect = false;
                load_speaker();
                idle_state();
            }
        }
        break;

    // Diagnostics: the steps of connecting and pairing (status/reason codes: Bluetooth Core
    // spec, Vol 1 Part F, e.g. 0x04 page timeout, 0x05 authentication failure, 0x06 key missing,
    // 0x13 remote user ended, 0x16 ended by us, 0x22/0x28 LMP timeout/instant passed).
    case HCI_EVENT_CONNECTION_COMPLETE:
        printf("BT hci: ACL connection complete, status 0x%02x\n", hci_event_connection_complete_get_status(packet));
        break;
    case HCI_EVENT_ROLE_CHANGE:
        printf("BT hci: role change, status 0x%02x, our role %s\n", packet[2], packet[9] ? "peripheral" : "central");
        break;
    case HCI_EVENT_LINK_KEY_REQUEST:
        printf("BT hci: speaker asks for the link key\n");
        break;
    case HCI_EVENT_IO_CAPABILITY_RESPONSE:
        printf("BT hci: speaker IO capability %u, authentication requirements 0x%02x\n",
               hci_event_io_capability_response_get_io_capability(packet),
               hci_event_io_capability_response_get_authentication_requirements(packet));
        break;
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        printf("BT hci: simple pairing complete, status 0x%02x\n", hci_event_simple_pairing_complete_get_status(packet));
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        printf("BT hci: new link key (bonded), key type %u\n", packet[24]);
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        printf("BT hci: authentication complete, status 0x%02x\n", hci_event_authentication_complete_get_status(packet));
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        printf("BT hci: encryption change, status 0x%02x, enabled %u\n", hci_event_encryption_change_get_status(packet),
               packet[5]);
        break;
    case GAP_EVENT_PAIRING_STARTED:
        printf("BT hci: pairing started (%s, initiated by %s)\n",
               gap_event_pairing_started_get_ssp(packet) ? "secure simple pairing" : "legacy PIN",
               gap_event_pairing_started_get_initiator(packet) ? "us" : "speaker");
        break;
    case GAP_EVENT_PAIRING_COMPLETE:
        printf("BT hci: pairing complete, status 0x%02x\n", gap_event_pairing_complete_get_status(packet));
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("BT hci: disconnected, reason 0x%02x\n", hci_event_disconnection_complete_get_reason(packet));
        break;
    default:
        break;
    }
}

static void speaker_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META)
        return;
    bd_addr_t addr;
    uint8_t status;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
        status = a2dp_subevent_signaling_connection_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            printf("BT: connection to %s failed (0x%02x)\n", bd_addr_to_str(addr), status);
            a2dp_cid = 0;
            if (pairing_connect) {
                pairing_connect = false;
                load_speaker(); // keep the previously paired speaker
            }
            idle_state();
            break;
        }
        a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        total_connects++;
        printf("BT: connected to %s\n", bd_addr_to_str(addr));
        if (pairing_connect) {
            pairing_connect = false;
            store_speaker(addr);
        }
        state = SPEAKER_CONNECTED;
        start_attempts = 0;
        schedule_retry(STREAM_START_TIMEOUT_MS); // watchdog until the stream runs
        break;

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
        sbc_conf.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
        sbc_conf.block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
        sbc_conf.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
        sbc_conf.min_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
        sbc_conf.max_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
        sbc_conf.allocation_method = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
        avdtp_channel_mode_t mode =
            (avdtp_channel_mode_t)a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
        btstack_sbc_channel_mode_t cm;
        switch (mode) {
        case AVDTP_CHANNEL_MODE_JOINT_STEREO: cm = SBC_CHANNEL_MODE_JOINT_STEREO; break;
        case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: cm = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
        case AVDTP_CHANNEL_MODE_MONO: cm = SBC_CHANNEL_MODE_MONO; break;
        default: cm = SBC_CHANNEL_MODE_STEREO; break;
        }
        sbc_conf.channel_mode = cm;
        enc = btstack_sbc_encoder_bluedroid_init_instance(&enc_state);
        enc->configure(&enc_state, SBC_MODE_STANDARD, sbc_conf.block_length, sbc_conf.subbands,
                       (btstack_sbc_allocation_method_t)(sbc_conf.allocation_method - 1),
                       sbc_conf.sampling_frequency, sbc_conf.max_bitpool, cm);
        printf("BT: SBC %d Hz, %d blocks, %d subbands, bitpool %d-%d, channel mode %d\n",
               sbc_conf.sampling_frequency, sbc_conf.block_length, sbc_conf.subbands, sbc_conf.min_bitpool,
               sbc_conf.max_bitpool, (int)cm);
        break;
    }

    case A2DP_SUBEVENT_SIGNALING_DELAY_REPORT:
        delay_report_100us = a2dp_subevent_signaling_delay_report_get_delay_100us(packet);
        printf("BT: speaker reports a delay of %u.%u ms\n", delay_report_100us / 10, delay_report_100us % 10);
        break;

    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            printf("BT: stream setup failed (0x%02x)\n", status);
            break; // the watchdog reconnects
        }
        stream_opened = true;
        a2dp_source_start_stream(a2dp_cid, local_seid);
        break;

    case A2DP_SUBEVENT_STREAM_STARTED:
        btstack_run_loop_remove_timer(&retry_timer);
        media_start();
        state = SPEAKER_STREAMING;
#if WANT_HCI_DUMP
        hci_dump_enable_packet_log(false);
        printf("BT: HCI log paused while streaming ('D' toggles)\n");
#endif
        total_stream_starts++;
        printf("BT: streaming (max payload %d bytes)\n", m.max_payload);
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        if (m.running && m.ready_to_send) {
            send_packet();
            fill();
        }
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        // e.g. the play/pause button on the speaker: we want the stream back
        printf("BT: stream suspended by the speaker, restarting\n");
        total_suspends++;
        media_stop();
        state = SPEAKER_CONNECTED;
        start_attempts = 0;
        schedule_retry(1000);
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        printf("BT: stream released\n");
        media_stop();
        stream_opened = false;
        if (a2dp_cid) {
            state = SPEAKER_CONNECTED;
            start_attempts = 3; // no stream endpoint left: reconnect
            schedule_retry(1000);
        }
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        printf("BT: disconnected\n");
        total_disconnects++;
        media_stop();
        stream_opened = false;
        a2dp_cid = 0;
        if (pair_after_disconnect)
            start_inquiry();
        else
            idle_state();
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------------ API

void speaker_init(void) {
    lock();
    load_speaker();

    hci_set_master_slave_policy(0); // we stay central, the speaker becomes peripheral
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    a2dp_source_init();
    a2dp_source_register_packet_handler(speaker_handler);
    avdtp_stream_endpoint_t *ep =
        a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, sbc_capabilities, sizeof(sbc_capabilities),
                                           sbc_configuration, sizeof(sbc_configuration));
    if (!ep) {
        printf("BT: cannot create the stream endpoint\n");
        unlock();
        return;
    }
    avdtp_set_preferred_sampling_frequency(ep, SPEAKER_SAMPLE_RATE);
    local_seid = avdtp_local_seid(ep);
    avdtp_source_register_delay_reporting_category(local_seid);

    sdp_init();
    memset(sdp_a2dp_source_buffer, 0, sizeof(sdp_a2dp_source_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_buffer, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp_source_buffer);

    gap_set_local_name("matrixmask staff 00:00:00:00:00:00");
    gap_set_class_of_device(0x200408);
    gap_discoverable_control(0);
    gap_connectable_control(1); // the speaker may reconnect by itself
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);

    hci_callback.callback = hci_handler;
    hci_add_event_handler(&hci_callback);

    st.since_us = time_us_64();
    unlock();
}

void speaker_pair(void) {
    lock();
    if (state == SPEAKER_PAIRING) {
        unlock();
        return;
    }
    btstack_run_loop_remove_timer(&retry_timer);
    if (a2dp_cid) {
        pair_after_disconnect = true;
        a2dp_source_disconnect(a2dp_cid);
    } else {
        start_inquiry();
    }
    unlock();
}

void speaker_forget(void) {
    lock();
    btstack_run_loop_remove_timer(&retry_timer);
    const btstack_tlv_t *tlv;
    void *ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (tlv)
        tlv->delete_tag(ctx, TLV_TAG_SPEAKER);
    gap_delete_all_link_keys();
    have_speaker = false;
    printf("BT: speaker and link keys deleted\n");
    if (a2dp_cid)
        a2dp_source_disconnect(a2dp_cid);
    else if (state != SPEAKER_OFF)
        state = SPEAKER_NO_SPEAKER;
    unlock();
}

void speaker_reconnect(void) {
    lock();
    if (a2dp_cid)
        a2dp_source_disconnect(a2dp_cid);
    else if (state == SPEAKER_WAITING)
        schedule_retry(1);
    unlock();
}

uint64_t speaker_play(speaker_sound_fn fn, void *ctx, uint64_t start_us) {
    uint64_t actual = 0;
    lock();
    if (m.running) {
        uint64_t frame = start_us > m.t0_us
                             ? ((start_us - m.t0_us) * SPEAKER_SAMPLE_RATE + 999999) / 1000000
                             : 0;
        if (frame < m.frames_done)
            frame = m.frames_done;
        snd.fn = fn;
        snd.ctx = ctx;
        snd.start_frame = frame;
        actual = m.t0_us + frame * 1000000 / SPEAKER_SAMPLE_RATE;
    }
    unlock();
    return actual;
}

void speaker_stop_sound(void) {
    lock();
    snd.fn = NULL;
    unlock();
}

bool speaker_sound_playing(void) { return snd.fn != NULL; }

void speaker_set_idle_noise(bool on) { idle_noise = on; }

bool speaker_idle_noise(void) { return idle_noise; }

bool speaker_toggle_hci_log(void) {
#if WANT_HCI_DUMP
    static bool on = true;
    on = !on;
    lock();
    hci_dump_enable_packet_log(on);
    unlock();
    return on;
#else
    return false;
#endif
}

speaker_state_t speaker_state(void) { return state; }

void speaker_print_status(void) {
    lock();
    stats_t s = st;
    uint64_t now = time_us_64();
    memset(&st, 0, sizeof(st));
    st.since_us = now;
    st.last_send_us = s.last_send_us;
    speaker_state_t cur = state;
    bool running = m.running;
    uint64_t backlog = running ? frames_due(now) - m.frames_done : 0;
    unlock();

    float secs = (float)(now - s.since_us) / 1e6f;
    printf("BT: %s", speaker_state_name(cur));
    if (have_speaker)
        printf(", speaker %s", bd_addr_to_str(speaker));
    if (delay_report_100us)
        printf(", speaker delay report %u.%u ms", delay_report_100us / 10, delay_report_100us % 10);
    printf(" | connects %lu, disconnects %lu, stream starts %lu, suspends %lu, skipped total %lu ms\n",
           (unsigned long)total_connects, (unsigned long)total_disconnects, (unsigned long)total_stream_starts,
           (unsigned long)total_suspends, (unsigned long)(total_skipped_frames * 1000ull / SPEAKER_SAMPLE_RATE));
    if (running && secs > 0) {
        printf("    last %.0f s: %lu packets (%.1f/s), max gap %.1f ms, max wait for send %.1f ms, "
               "skipped %lu ms, max backlog %.1f ms (now %.1f), SBC encoder CPU %.1f %%\n",
               secs, (unsigned long)s.packets, s.packets / secs, s.max_send_gap_us / 1000.0f, s.max_wait_us / 1000.0f,
               (unsigned long)(s.skipped_frames * 1000ull / SPEAKER_SAMPLE_RATE),
               s.max_backlog_frames * 1000.0f / SPEAKER_SAMPLE_RATE, backlog * 1000.0f / SPEAKER_SAMPLE_RATE,
               (float)s.encode_us / (secs * 1e4f));
    }
}
