# esp32-p4x-ev-board-series

Qiita Advent Calendar 2026「ESP32」の ESP32-P4X-Function-EV-Board シリーズのサンプルコードです。

- Qiita 記事（公開予定）: ESP32-P4X 入門：開封から Hello World まで

## 動作を確認した環境

| 項目 | 内容 |
| --- | --- |
| ボード | ESP32-P4X-Function-EV-Board（基板のシルクは「ESP32-P4 Function_EV_Board V1.6」） |
| チップ | ESP32-P4 revision v3.2、Flash 16MB、PSRAM 32MB |
| ESP-IDF | v5.5.3 |
| PC | Windows 11、PowerShell |
| 接続 | J19（USB Serial/JTAG）の Type-C |

## 中身

| フォルダ | 内容 | 記事 |
| --- | --- | --- |
| `01-intro/hello_world_psram` | ESP-IDF の `examples/get-started/hello_world` に、Flash 16MB と PSRAM を有効にする `sdkconfig.defaults` と、`main/CMakeLists.txt` の依存 `esp_psram` を足したもの | 入門 |

## ビルドと書き込み

```powershell
cd 01-intro/hello_world_psram
idf.py set-target esp32p4
idf.py build
idf.py -p COM3 flash monitor
```

COM 番号は環境に合わせてください。

## ライセンス

Apache License 2.0（`LICENSE`）。ただし ESP-IDF の例から持ってきたファイルは、そのファイルの先頭に書かれたライセンス（`hello_world_main.c` は CC0-1.0）に従います。
