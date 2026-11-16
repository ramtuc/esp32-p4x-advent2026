# hello_world_psram

ESP-IDF v5.5.3 の `examples/get-started/hello_world` を ESP32-P4X-Function-EV-Board 向けにしたものです。

元の例からの変更は次の2つだけです。

- `sdkconfig.defaults` を足した（Flash 16MB、PSRAM 有効）。`set-target esp32p4` の既定は Flash 2MB・PSRAM なしのため
- `main/CMakeLists.txt` の `PRIV_REQUIRES` に `esp_psram` を足した。この例は `MINIMAL_BUILD ON` なので、足さないと `CONFIG_SPIRAM=y` が効かない

`sdkconfig.defaults` は `sdkconfig` が無いときに読まれます。`sdkconfig` が残っているときは消してからビルドしてください。
