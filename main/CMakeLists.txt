idf_component_register(SRCS "station_example_main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_driver_gpio esp_netif esp_event esp_timer mqtt mbedtls
                    INCLUDE_DIRS ".")