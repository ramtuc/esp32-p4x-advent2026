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

### C6 側の送信出力の上限

C6 の `slave` をビルドするときに、`slave/sdkconfig.defaults.esp32c6` の末尾へ `c6_ota/slave_sdkconfig.defaults.esp32c6.add` の中身（`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=15`）を足す。ホストの `esp_wifi_set_max_tx_power()` は `esp_wifi_start()` の後なので、その前も C6 側で +15dBm 以下にするため。`wifi_sta` は、ホストが上限をかける前の値（C6 のファームの上限）も表示する。

同じ版（2.12.12）の C6 に設定違いのファームを入れ直すときは、版の確認を切った `sdkconfig.reota` でビルドする:
`idf.py -B build_reota -D SDKCONFIG=build_reota/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.reota build`

## wifi_iperf の性能の設定

ESP-Hosted 2.12.12 の `docs/performance_optimization.md`（§1.1 P4＋C6、§2.1 SDIO）の設定を段階的に足したもの。おすすめは `sdkconfig.perf_c`（SDIO は既定の 40MHz）。

| ファイル | 中身 |
| --- | --- |
| `sdkconfig.defaults` | 既定（lwIP の TCP の窓・送信バッファ 5760） |
| `sdkconfig.perf_b` | ＋ lwIP の TCP の窓・送信バッファ 65534、mbox 64、SACK |
| `sdkconfig.perf_c` | ＋ Wi-Fi のバッファ（RX 16/64、TX 64）と BA の窓 32 |
| `sdkconfig.perf_d` | ＋ SDIO 50MHz |

ビルド: `idf.py -B build_c -D SDKCONFIG=build_c/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.perf_c build`

実測（11/21、ch 5、Mbit/s）: TCP は既定 約 13 → C で PC→P4 34〜37・P4→PC 約 42。UDP は C で PC→P4 48〜50・P4→PC 51〜52（PC で受けた量）。D は C と変わらない。
