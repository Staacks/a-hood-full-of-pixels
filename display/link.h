// Display side of the link: BLE peripheral for the controller. Answers SYNC_REQ pings and
// heartbeats, starts the scenes the controller fires (EVENT). See common/link_proto.h.
#pragma once

#include <stdbool.h>

// Call after cyw43_arch_init(). Starts Bluetooth and advertises until the controller connects
// (again after every disconnect).
void link_init(void);

bool link_connected(void);

// Call from the main loop: starts a scene the controller requested.
void link_poll(void);

// Prints the connection state, the pings answered since the last call and the event counters.
void link_print_status(void);
