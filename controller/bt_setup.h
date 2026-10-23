// Shared BTstack setup for the speaker (A2DP) and the link to the display (BLE):
//   cyw43_arch_init(); bt_setup_init(); speaker_init(); ble_link_init(); bt_setup_power_on();
#pragma once

void bt_setup_init(void);
void bt_setup_power_on(void);
