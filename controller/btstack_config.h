// BTstack configuration for the controller: Bluetooth Classic (A2DP source to the speaker) and
// BLE central (link to the display).
// Based on pico-examples bluetooth/config/btstack_config_common.h (Raspberry Pi, BSD-3-Clause).
#pragma once

#define ENABLE_LOG_ERROR
#if WANT_HCI_DUMP
// bt_test_hcidump: BTstack's own log and every HCI packet on the console (ACL truncated)
#define ENABLE_LOG_INFO
#define HCI_DUMP_STDOUT_MAX_SIZE_ACL 64
#endif
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_LE_CENTRAL

// Buffers and sizes
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVRCP_CONNECTIONS 0
#define MAX_NR_BNEP_CHANNELS 0
#define MAX_NR_BNEP_SERVICES 0
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HCI_CONNECTIONS 3 // speaker, display, spare
#define MAX_NR_HID_HOST_CONNECTIONS 0
#define MAX_NR_HIDS_CLIENTS 0
#define MAX_NR_HFP_CONNECTIONS 0
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 3
#define MAX_NR_RFCOMM_CHANNELS 0
#define MAX_NR_RFCOMM_MULTIPLEXERS 0
#define MAX_NR_RFCOMM_SERVICES 0
#define MAX_NR_SERVICE_RECORD_ITEMS 2
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4
#define MAX_ATT_DB_SIZE 512 // BLE builds compile att_db_util.c; we use no local GATT database

// Limit the ACL buffers used by the stack to avoid overrunning the cyw43 shared bus
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

// HCI controller-to-host flow control, also against cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link keys in flash (TLV on the pico flash bank, set up by btstack_cyw43)
#define NVM_NUM_LINK_KEYS 4
#define NVM_NUM_DEVICE_DB_ENTRIES 4

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
