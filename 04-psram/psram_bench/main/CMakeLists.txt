idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_psram esp_driver_dma esp_mm esp_timer heap freertos
                       INCLUDE_DIRS "")
