# Attribution and changes

Protocol formats derive from **FPVGateC5MK**, copyright (c) 2026 Louis Hitchcock:
https://github.com/LouisHitchcock/FPVGateC5MK — CC BY-NC-SA 4.0.
Our changes: separate Tab5/LVGL and Linux/Web clients, display, background capture,
native RH snapshot archiving/replay, immutable scanner payload archiving, separate human-label ledgers, training-source exports, portability and development documentation.
The original C5 RF firmware is not modified or distributed here.

Vendored `apps/tab5/components/{m5stack_tab5,esp_lcd_st7121,esp_lvgl_port}` come
from M5Stack M5Tab5-UserDemo commit fe8aa051beb9385468f7e46b909e764169df3f7a:
https://github.com/m5stack/M5Tab5-UserDemo/tree/fe8aa051beb9385468f7e46b909e764169df3f7a
They retain original copyright/SPDX Apache-2.0 (some example files are CC0-1.0); full Apache text in
apps/tab5/components/m5stack_tab5/LICENSE. Local BSP changes are documented in
apps/tab5/README.md (dependency/audio-name corrections, PPA buffer alignment,
RGB565 capacity and LCD transfer checking). They do not alter the C5 RF estimator.

Generated Noto Sans JP font subsets retain SIL OFL 1.1, supplied at
apps/tab5/main/fonts/OFL.txt; source/hash/tools in font-build/provenance.json.
pySerial and ESP-IDF-managed components are installed dependencies and retain
their own licences. The noncommercial app licence does not replace those terms.

CC0-1.0 example files in esp_lvgl_port retain their file-level notices; text is
provided in LICENSES/CC0-1.0.txt. LVGL-derived code in the upstream port also
retains its Apache/SPDX headers; the upstream LVGL MIT notice is additionally
preserved in LICENSES/LVGL-MIT.txt. These notices apply only to their respective
third-party material, independently of the application licence.
