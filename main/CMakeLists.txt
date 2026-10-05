idf_component_register(SRCS "blackout.c" "blackout_shared.c" "oled_display.c"
                            "buddy.cpp" "buddy_display.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_spi esp_timer
                                esp_wifi esp_netif esp_event nvs_flash)
