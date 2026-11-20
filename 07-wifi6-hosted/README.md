# wifi6-hosted（ESP32-P4X、ESP-IDF v6.0.3）

オンボードの ESP32-C6 を ESP-Hosted（SDIO）で Wi-Fi に使う。P4 側は `esp_wifi_remote`（1.6.5）＋ `esp_hosted`（2.12.x）。

- SSID とパスワードはリポジトリにも `sdkconfig` にも書かない。ビルドのときの環境変数 `WIFI_SSID`・`WIFI_PASS` で渡す（無いビルドは、C6 の版を表示して止まる）
- 技適未取得機器を用いた実験等の特例で使うので、チャンネルは 1〜11、送信出力の上限は +15dBm にしている

## プロジェクト

| ディレクトリ | 内容 |
| --- | --- |
| `wifi_sta` | つないで、チャンネル・RSSI・AP の規格・実際の規格（`esp_wifi_sta_get_negotiated_phymode()`）・C6 の版を表示 |
| `wifi_iperf` | つないだあと、J19 に `iperf`（iperf 2 互換。iperf3 とは話せない）と `ping` のコンソールを開く |
| `c6_ota` | C6 のファームを P4 から SDIO で更新する（ESP-Hosted 2.12.12 の `examples/host_performs_slave_ota` にこのボードの設定を足したもの、Apache-2.0） |

## C6 のファームの更新（c6_ota）

1. ESP-Hosted 2.12.12 の `slave` を ESP32-C6 向けにビルドする（`idf.py set-target esp32c6` → `idf.py build`。通信は SDIO が既定）
   - `slave` は、ホスト側のプロジェクトの `managed_components/espressif__esp_hosted/slave`（2.12.12）を使った
2. できた `build/network_adapter.bin` を `c6_ota/components/ota_littlefs/slave_fw_bin/` に置く（.bin はリポジトリに入れない）
3. `c6_ota` をビルドして P4 に書き込む。起動すると P4 が C6 にファームを送り、P4 が再起動する。再起動後に `Slave firmware version: 2.12.12` と `OTA not required` が出れば完了

注意: 新しいファームが SDIO で起動しないと P4 からは戻せない。そのときは基板の PROG_C6 ヘッダに ESP-Prog などをつないで C6 に直接書く。工場出荷の C6 のファームはバックアップできないので、自己責任で。
