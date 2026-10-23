# SD card (both boards): FatFs + SPI driver from carlk3, hardware config in sdcard.c.
add_library(sdcard INTERFACE)
target_sources(sdcard INTERFACE ${CMAKE_CURRENT_LIST_DIR}/sdcard.c)
target_include_directories(sdcard INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sdcard INTERFACE no-OS-FatFS-SD-SDIO-SPI-RPi-Pico hardware_spi)
