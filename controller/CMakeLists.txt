add_executable(controller
    main.c
    ble_link.c
    bt_setup.c
    speaker.c
    clips.c
    staffanim.c
    strip.c
    effects.c
    buttons.c
    encoder.c
    lis3dh.c
    stomp.c
)

pico_generate_pio_header(controller ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_compile_options(controller PRIVATE -Wall -Wextra -Wno-unused-parameter)

# btstack_config.h (here) and link_proto.h (common)
target_include_directories(controller PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_SOURCE_DIR}/common)

target_link_libraries(controller
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_classic
    pico_btstack_ble
    pico_btstack_sbc_encoder
    hardware_pio
    hardware_dma
    hardware_i2c
    hardware_watchdog
    pico_rand
    sdcard
)

pico_enable_stdio_usb(controller 1)
pico_enable_stdio_uart(controller 0)
pico_add_extra_outputs(controller)

# Bluetooth test firmware (milestone 4): A2DP stream + BLE link to the display at the same time.
# bt_test_hcidump is the same with BTstack's log and every HCI packet on the console.
function(add_bt_test name)
    add_executable(${name}
        bt_test.c
        bt_setup.c
        speaker.c
        ble_link.c
        strip.c
        buttons.c
        encoder.c
    )

    pico_generate_pio_header(${name} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ws2812.pio)

    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)

    # btstack_config.h (here) and link_proto.h (common)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR} ${CMAKE_SOURCE_DIR}/common)

    target_link_libraries(${name}
        pico_stdlib
        pico_cyw43_arch_none
        pico_btstack_cyw43
        pico_btstack_classic
        pico_btstack_ble
        pico_btstack_sbc_encoder
        hardware_pio
        hardware_dma
    )

    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

add_bt_test(bt_test)
add_bt_test(bt_test_hcidump)
target_compile_definitions(bt_test_hcidump PRIVATE WANT_HCI_DUMP=1)
