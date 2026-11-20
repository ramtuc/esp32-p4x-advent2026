# esp32-p4x-advent2026

Qiita Advent Calendar 2026「ESP32」に書いた記事（ESP32-P4X-Function-EV-Board と ESP32-S3）のサンプルコードです。

- Qiita 記事（公開予定）: ESP-IDF v6.0.3 で開発環境を作る（ESP32-S3 と ESP32-P4X のマルチターゲット）
- Qiita 記事（公開予定）: ESP32-P4X の Hello World と基本入出力（HP/LP GPIO・I2C・SPI・UART）
- Qiita 記事（公開予定）: ESP32-S3 の極限の省電力と ULP（Deep Sleep とタイマ・GPIO・ULP RISC-V での復帰）
- Qiita 記事（公開予定）: 産業用バス TWAI/CAN（4 ノードのマルチドロップとバス調停）
- Qiita 記事（公開予定）: エッジ FFT 対決（ESP32-S3 Xtensa vs ESP32-P4 RISC-V）

## 動作を確認した環境

| 項目 | 内容 |
| --- | --- |
| ボード | ESP32-P4X-Function-EV-Board（基板のシルクは「ESP32-P4 Function_EV_Board V1.6」）、ESPr Developer S3 Type-C（ESP32-S3-WROOM-1-N16R8） |
| チップ | ESP32-P4 revision v3.2、Flash 16MB、PSRAM 32MB |
| ESP-IDF | v6.0.3 |
| PC | Windows 11、PowerShell |
| 接続 | J19（USB Serial/JTAG）の Type-C |

## 中身

| フォルダ | 内容 | 記事 |
| --- | --- | --- |
| `01-intro/hello_world` | ESP-IDF の `hello_world` を、1つのプロジェクトで ESP32-P4 と ESP32-S3 の両方にビルドできるようにしたもの（`CMakePresets.json` と `sdkconfig.defaults.<ターゲット>`） | ESP-IDF v6.0.3 の開発環境 |
| `02-basic-io/hello_usj` | J19（USB Serial/JTAG）をコンソールの主にして、Hello World と 1 行ずつのエコー | Hello World と基本入出力 |
| `02-basic-io/gpio_isr_blink` | BOOT ボタン（GPIO35）の割り込みで、HP GPIO（GPIO20）と LP GPIO（GPIO4）の LED を反転 | 同上 |
| `02-basic-io/i2c_lis3dh` | `i2c_master` で I2C バス（GPIO7/8）をスキャンし、LIS3DH を読む | 同上 |
| `02-basic-io/spi_lis3dh` | LIS3DH を SPI（GPIO マトリクス経由、5MHz、モード 3）で読む | 同上 |
| `02-basic-io/uart_echo` | UART1（GPIO21 TX / GPIO22 RX）をジャンパでループバック。RX にプルアップ | 同上 |
| `02-basic-io/io_voltage` | J1 の GPIO46〜48（VDD_IO_5）を High にして、LDO の VO4 を 3300mV にする前後の電圧を測る | 同上 |
| `02-basic-io/reset_usj.ttl` | Tera Term で COM を開いたまま RTS でリセットするマクロ（起動ログを頭から見る） | 同上 |
| `ulp-deep-sleep/sleep_wakeup` | ESP32-S3: Deep Sleep からタイマ（10 秒）と GPIO0（FLASH スイッチ、EXT1）で起こし、起きた理由と眠っていた時間を出す | 極限の省電力＆ULP |
| `ulp-deep-sleep/ulp_button_count` | ESP32-S3: ULP RISC-V が 20ms ごとに GPIO0 を見て、3 回押されたらメインの CPU を起こす | 同上 |
| `twai-can/can_node` | ESP32-S3 と ESP32-P4: esp_twai（v6.0）で CAN の 1 マスタ＋スレーブ。受け入れフィルタ、バスオフからの復帰。トランシーバなしのセルフテストも（ボードは `sdkconfig.esp32s3`・`sdkconfig.esp32p4`・`sdkconfig.esp32p4_rev1`、役割は `sdkconfig.master`・`sdkconfig.slave1`〜`3`。下の「can_node のビルド」） | 産業用バス TWAI/CAN |
| `fft-s3-vs-p4/fft_bench` | ESP-DSP（1.8.2）の FFT（fc32 基数 2・4、sc16 基数 2、C の版と最適化版）のサイクル数を、合成の sin 波で 1024／4096 点。S3 と P4 のプリセット | エッジ FFT 対決（S3 vs P4） |
| `06-ethernet-usb/eth_link` | EMAC と IP101（`espressif/ip101`）で Ethernet を起こし、PHY の ID・リンク・DHCP の IP を表示（MAC アドレスは出さない） | 有線通信（Ethernet と USB HS） |
| `09-ppa/ppa_bench` | PPA と CPU で、PSRAM 上の 1024×600 の画像の回転・拡大縮小・色の変換・塗りつぶしの時間を比べる | ハードウェアアクセラレータ PPA |

## ビルドと書き込み

```powershell
cd 01-intro/hello_world
idf.py --preset esp32p4 build              # ESP32-P4X（build/esp32p4 に出る）
idf.py --preset esp32p4 -p COM3 flash monitor
idf.py --preset esp32s3 -p COM5 flash monitor  # ESP32-S3
```

COM 番号は環境に合わせてください。

### can_node のビルド（ボードと役割の組み合わせ）

`twai-can/can_node` は、ボードのファイルと役割のファイルを `SDKCONFIG_DEFAULTS` に並べてビルドします。ビルドのディレクトリは組み合わせごとに分けます。

```powershell
cd twai-can/can_node
# ESPr Developer S3 Type-C をマスタに
idf.py -B build_s3_master -D SDKCONFIG=build_s3_master/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32s3;sdkconfig.master" build
# ESP32-P4（チップ revision v3.x、P4X-Function-EV-Board など）をスレーブ 2 に
idf.py -B build_p4_slave2 -D SDKCONFIG=build_p4_slave2/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32p4;sdkconfig.slave2" build
# ESP32-P4（チップ revision v1.x）をスレーブ 3 に
idf.py -B build_p4rev1_slave3 -D SDKCONFIG=build_p4rev1_slave3/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32p4_rev1;sdkconfig.slave3" build
```

TX・RX は既定で GPIO4・GPIO5（menuconfig の `CAN node`）。

## ライセンス

Apache License 2.0（`LICENSE`）。ただし ESP-IDF の例から持ってきたファイルは、そのファイルの先頭に書かれたライセンス（`hello_world_main.c` は CC0-1.0）に従います。
