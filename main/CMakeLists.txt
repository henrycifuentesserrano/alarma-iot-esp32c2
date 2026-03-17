idf_component_register(SRCS "blynk_rele_main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_driver_gpio esp_netif esp_event esp_timer mqtt mbedtls
                    INCLUDE_DIRS ".")