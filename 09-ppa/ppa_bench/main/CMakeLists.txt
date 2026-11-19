idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_driver_ppa esp_timer esp_psram freertos heap log
                       INCLUDE_DIRS "")
