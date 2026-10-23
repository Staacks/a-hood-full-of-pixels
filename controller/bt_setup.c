#include "bt_setup.h"

#include "btstack.h"
#include "pico/cyw43_arch.h"

void bt_setup_init(void) {
    async_context_acquire_lock_blocking(cyw43_arch_async_context());
    l2cap_init();
    sm_init(); // needed by BLE even without pairing
    async_context_release_lock(cyw43_arch_async_context());
}

void bt_setup_power_on(void) {
    async_context_acquire_lock_blocking(cyw43_arch_async_context());
    hci_power_control(HCI_POWER_ON);
    async_context_release_lock(cyw43_arch_async_context());
}
