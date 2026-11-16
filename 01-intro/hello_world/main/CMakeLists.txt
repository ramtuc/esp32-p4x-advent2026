# MINIMAL_BUILD では、PSRAM のコンポーネント（esp_psram）を依存に書かないと CONFIG_SPIRAM が効かない
idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash esp_psram
                       INCLUDE_DIRS "")
