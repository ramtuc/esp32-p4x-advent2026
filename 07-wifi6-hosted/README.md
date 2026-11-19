# wifi6-hosted（ESP32-P4X、ESP-IDF v6.0.3）

オンボードの ESP32-C6 を ESP-Hosted（SDIO）で Wi-Fi に使う。P4 側は `esp_wifi_remote`（1.6.5）＋ `esp_hosted`（2.12.x）。

- SSID とパスワードはリポジトリにも `sdkconfig` にも書かない。ビルドのときの環境変数 `WIFI_SSID`・`WIFI_PASS` で渡す（無いビルドは、C6 の版を表示して止まる）
- 技適未取得機器を用いた実験等の特例で使うので、チャンネルは 1〜11、送信出力の上限は +15dBm にしている
