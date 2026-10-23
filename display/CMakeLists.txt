add_executable(display
    main.c
    hub75.c
    testpattern.c
    link.c
    player.c
)

pico_generate_pio_header(display ${CMAKE_CURRENT_LIST_DIR}/hub75.pio)

target_compile_options(display PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_include_directories(display PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/common)

target_link_libraries(display
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_ble
    hardware_pio
    hardware_dma
    sdcard
)

pico_enable_stdio_usb(display 1)
pico_enable_stdio_uart(display 0)
# GATT database of the link (generates link_gatt.h)
pico_btstack_make_gatt_header(display PRIVATE "${CMAKE_CURRENT_LIST_DIR}/link_gatt.gatt")

pico_add_extra_outputs(display)
