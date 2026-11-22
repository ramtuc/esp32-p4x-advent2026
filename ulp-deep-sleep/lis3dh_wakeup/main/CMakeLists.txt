idf_component_register(SRCS "lis3dh_wakeup_main.c"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_hw_support
                       INCLUDE_DIRS "")
