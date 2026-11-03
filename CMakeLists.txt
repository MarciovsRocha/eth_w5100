idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS priv_includes
    REQUIRES esp_driver_spi esp_eth esp_netif
    PRIV_REQUIRES esp_driver_gpio esp_event esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wmissing-prototypes)
