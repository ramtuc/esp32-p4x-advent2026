# ULP のビルドは main の公開の依存（REQUIRES）からヘッダを探すので、ulp は REQUIRES に書く
idf_component_register(SRCS "ulp_button_count_main.c"
                       REQUIRES ulp esp_driver_gpio esp_hw_support
                       INCLUDE_DIRS "")

# ULP RISC-V のプログラム（main/ulp/）をビルドして、アプリに埋め込む
set(ulp_app_name ulp_main)
set(ulp_riscv_sources "ulp/main.c")
set(ulp_exp_dep_srcs "ulp_button_count_main.c")
ulp_embed_binary(${ulp_app_name} "${ulp_riscv_sources}" "${ulp_exp_dep_srcs}")
