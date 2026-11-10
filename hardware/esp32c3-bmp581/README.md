# hardware / esp32c3-bmp581

ESP32-C3 + BMP581 版の回路図・基板データを置く場所です。

回路図の画像は GitHub Pages の資料と共用で、次の場所にあります。

- [`docs/assets/images/esp32c3-bmp581/schematic.webp`](../../docs/assets/images/esp32c3-bmp581/schematic.webp)（2026-10-26 版、V1.1）
- [`docs/assets/images/esp32c3-bmp581/schematic-v1.0.webp`](../../docs/assets/images/esp32c3-bmp581/schematic-v1.0.webp)（2026-10-05 版、V1.0）

V1.1 では、電源の切り替えを TPS2116（U3）による自動の切り替えにし、SW1 を電池の電源スイッチにしました。どちらから給電しているかは、IO3 で読めます。

EasyEDA のプロジェクトファイル、BOM、ガーバーなどの製造データも、このフォルダに置く想定です。
