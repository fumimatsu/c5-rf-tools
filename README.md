# C5 RF Tools

ESP32-C5 / C5MKを使う5.8GHz電波監視・計測実験。Tab5用とAI Pyramid-Pro用を同じリポジトリで管理します。

| ディレクトリ | 用途 | 現行ソース |
|---|---|---|
| `apps/tab5/` | Tab5のUSBホスト・4ch/WF・背景・レベル/安定性表示 | 0.5.0 |
| `apps/pyramid/` | Linux上のUSB受信・LAN Web画面・RH履歴保存/再生 | 0.2.1（移植準備版） |
| `docs/` | 共通の測定境界、別PCでの開発・引き継ぎ | |

元の実機配備はPyramid 0.2.0 / Tab5 0.5.0。0.2.1は端末設定を外出ししたソースで、実機へ自動配備しません。

## 必要な機材・状態

これは開発者向けの**試作・計測実験用ソフトウェア**です。一般向けの完成品や校正済みスペクトラムアナライザーではありません。

- 共通受信機: XIAO ESP32-C5、公式 **C5MK 1.2.5+xiaoc5**。先に[上流の手順](https://github.com/LouisHitchcock/FPVGateC5MK)で用意します。ファームのバイナリーはここには含めません。
- Tab5用: M5Stack Tab5（ESP32-P4）、C5をUSB-Aへ接続。開発にはESP-IDF 5.4.2を使用。
- Pyramid用: AI Pyramid-Pro / Ubuntu 22.04で確認。C5をUSB-Aへ、PCとはLAN接続。Python 3.10以上とpySerial 3.5。現在の処理はCPU上で動作し、NPUを使用しません。
- 2つのアプリは独立しています。両方を同時に実測する場合は、それぞれの端末にC5を1台ずつ接続します。

PyramidのWebサーバーは信頼できるLAN内で使う構成です。ユーザー認証・TLSは備えていません。GitHubで公開するのはソースで、端末のWeb画面や測定履歴をインターネットへ公開する設定は行いません。

## 別PCから開発する

1. このリポジトリをcloneし、Codexで開きます。
2. [引き継ぎ](docs/handoff.md)と対象の `AGENTS.md` を読みます。
3. Pyramidは [開発手順](docs/development.md) のPCテストから開始。既存機の画面は `http://<PYRAMID_IP>:8765/`。
4. Tab5はESP-IDF **5.4.2**環境で `apps/tab5/build.ps1`。書き込みは端末確認後に別途実施します。

PCの場所が変わっても、同じLANのPyramidにはブラウザーとSSHで接続できます。IP、USBシリアル、SSH認証はローカルに保持し、Gitへ含めません。
測定履歴はPyramidの `/var/lib/c5-monitor`、Tab5の設定は本体NVSです。cloneしても測定データはコピーされません。

## 状態と限界

Pyramidでは4ch監視、2/4MHz帯域走査/WF、全機OFF背景、C5 RH36の設定、時刻付きスナップショット記録/再生を実装。
Tab5ではチャンネル選択、背景、4ch履歴/WF、正常基準と暫定のレベル/安定性評価を実装。
画面の模擬プレビューは実測の証拠ではありません。受信dBはdBm・SNR・混信率・映像品質を示しません。
AI/NPU分類、自動しきい値適用、MADSYSTEM送信、独立した物理ゲート通過精度は未実装/未検証です。

## ライセンス

アプリの配布条件は **CC BY-NC-SA 4.0**（[LICENSE](LICENSE)）。C5MK由来のプロトコルと変更点は [NOTICE](NOTICE.md)。
同梱M5Stack/Espressifコンポーネントは元のApache-2.0、フォントはSIL OFL 1.1を維持します。ファイル単位のCC0表記とLVGL由来のMIT表記も保持します。
C5ファームのバイナリー、SDK、実機バックアップ、認証情報は含めません。

ビルド・テストの実施範囲は[移管時の検証](docs/repository-readiness.md)。不具合の再現方法や改善案はIssuesで共有できます。
