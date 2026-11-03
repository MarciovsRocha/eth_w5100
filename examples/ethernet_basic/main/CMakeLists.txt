idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_driver_spi esp_eth esp_event esp_netif esp_timer lwip mqtt)
