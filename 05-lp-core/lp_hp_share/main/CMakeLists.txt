set(app_sources "main.c")

idf_component_register(SRCS ${app_sources}
                       REQUIRES ulp esp_hw_support
                       WHOLE_ARCHIVE)

# LP コアのプログラム（main/ulp/）をビルドして、HP のアプリに埋め込む
set(ulp_app_name ulp_${COMPONENT_NAME})
set(ulp_sources "ulp/lp_main.c")
set(ulp_exp_dep_srcs ${app_sources})
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
