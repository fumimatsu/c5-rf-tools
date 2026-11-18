# C5 RF Tools

ESP32-C5とC5MKを使う、5.8 GHz帯の電波監視・計測用ソフトウェアです。Tab5とAI Pyramid-Proの2つのアプリを収録しています。

| アプリ | 機能 | 使い方 |
|---|---|---|
| Tab5 0.6.1 | 4ch監視、ウォーターフォール、背景保存、レベル・安定性の表示 | [Tab5ガイド](apps/tab5/README.md) |
| Pyramid 0.2.5 | LANでの監視、2/4 MHz帯域走査、全受信フレーム・RH記録、独立した通過ラベル、LEDメーターと背景取得ボタン | [Pyramidガイド](apps/pyramid/README.md) |

## 動作環境

- **受信機** — XIAO ESP32-C5と [C5MK 1.2.5+xiaoc5](https://github.com/LouisHitchcock/FPVGateC5MK)。ファームウェアの入手と書き込みは上流の案内を参照してください。
- **Tab5** — M5Stack Tab5（ESP32-P4）、ESP-IDF 5.4.2。C5をTab5のUSB-Aへ接続します。
- **Pyramid** — AI Pyramid-Pro、Ubuntu 22.04、Python 3.10以上とpySerial 3.5。C5をUSBで接続します。Web画面は信頼できるLAN内で使用してください。Webサービスはユーザー認証やTLSを提供しません。

2つのアプリは独立しています。同時に使用する場合は各端末にC5を1台ずつ用意します。

## 開発と検証

- [セットアップ・ビルド・テスト・実機更新](docs/development.md)
- [ウォーターフォールの色範囲と検証](docs/jet-waterfalls.md)
- [Pyramidの記録形式と検証](apps/pyramid/docs/0.2.2-recording-labels.md)
- [PyramidのLEDと背景取得ボタン](docs/pyramid-led-meter.md)

不具合や改善案は [Issues](https://github.com/fumimatsu/c5-rf-tools/issues) で共有できます。

## 計測の範囲

受信値は未校正の相対値（receiver dB）です。RH 1〜254は計測モードの別スケールです。dBm・SNR・混信率・映像品質を報告する機能はありません。物理ゲートの通過精度は未検証です。

## ライセンス

アプリの配布条件は [CC BY-NC-SA 4.0](LICENSE) です。C5MK由来のプロトコルと同梱コンポーネント・フォントの個別ライセンスは [NOTICE](NOTICE.md) を参照してください。
