idf_component_register(SRCS "buddy.cpp" "buddy_display.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_spi esp_timer)
