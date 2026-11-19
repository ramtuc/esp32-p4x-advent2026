# esp32-p4x-advent2026

Qiita Advent Calendar 2026「ESP32」に書いた記事（ESP32-P4X-Function-EV-Board と ESP32-S3）のサンプルコードです。

- Qiita 記事（公開予定）: ESP-IDF v6.0.3 で開発環境を作る（ESP32-S3 と ESP32-P4X のマルチターゲット）
- Qiita 記事（公開予定）: ESP32-P4X の Hello World と基本入出力（HP/LP GPIO・I2C・SPI・UART）

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

## ビルドと書き込み

```powershell
cd 01-intro/hello_world
idf.py --preset esp32p4 build              # ESP32-P4X（build/esp32p4 に出る）
idf.py --preset esp32p4 -p COM3 flash monitor
idf.py --preset esp32s3 -p COM5 flash monitor  # ESP32-S3
```

COM 番号は環境に合わせてください。

## ライセンス

Apache License 2.0（`LICENSE`）。ただし ESP-IDF の例から持ってきたファイルは、そのファイルの先頭に書かれたライセンス（`hello_world_main.c` は CC0-1.0）に従います。
