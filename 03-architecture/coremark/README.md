# coremark（ESP32-P4X、ESP-IDF v6.0.3）

Espressif のコンポーネント `espressif/coremark`（1.1.0~2）の例 `coremark_example` を `idf.py create-project-from-example` で取り出し、ESP32-P4 向けに `sdkconfig.defaults` だけ変えたものです。

- CoreMark 本体は `managed_components/espressif__coremark`（ビルドのときに自動で取ってくる。-O3 でビルドされる）
- `main/coremark_example_main.c` は元の例のまま（CC0-1.0）

```powershell
idf.py -p COM3 flash monitor
```

ESP32-S3（ESPr Developer S3 Type-C、240MHz）で同じ物を回すとき:

```powershell
idf.py -B build_s3 -D SDKCONFIG=build_s3/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.esp32s3 set-target esp32s3 build
idf.py -B build_s3 -D SDKCONFIG=build_s3/sdkconfig -p COM5 flash monitor
```
