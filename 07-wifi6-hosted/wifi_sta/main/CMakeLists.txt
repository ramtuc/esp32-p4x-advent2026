idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "")

# Wi-Fi の SSID とパスワードは、リポジトリにも sdkconfig にも書かない。
# ビルドのときの環境変数 WIFI_SSID・WIFI_PASS から渡す（無ければ空のまま。接続はしない）
if(DEFINED ENV{WIFI_SSID})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "WIFI_SSID=\"$ENV{WIFI_SSID}\"" "WIFI_PASS=\"$ENV{WIFI_PASS}\"")
endif()
