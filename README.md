# C5 RF Tools

ESP32-C5 / C5MKを使う5.8GHz電波監視・計測実験。Tab5用とAI Pyramid-Pro用を同じリポジトリで管理します。

| ディレクトリ | 用途 | 現行ソース |
|---|---|---|
| `apps/tab5/` | Tab5のUSBホスト・縦4ch/WF・背景・レベル/安定性・明るさ | 0.6.0 |
| `apps/pyramid/` | Linux上のUSB受信・LAN Web画面・全フレーム/RH記録・独立ラベル | 0.2.2 |
| `docs/` | 共通の測定境界、別PCでの開発・引き継ぎ | |

Pyramid 0.2.2を実機配備・確認済み。Tab5は縦画面と明るさ調整を加えた0.6.0を配備済みです。別PCへのcloneは自動配備や測定履歴のコピーを行いません。
[0.2.2の記録形式・実機検証・戻し方](apps/pyramid/docs/0.2.2-recording-labels.md)。

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

## 次に実装する機能（2026-11-14合意）

**PyramidのC5で4波の比較測定を行い、その記録・検証を土台にAI/NPUを活用する。**
最初はE2/E1/F3/F5を、全OFF・各1台・他3台・全4台の10状態で比較する。
0.2.2で全フレーム記録・独立ラベル・学習用出力の基盤を追加しました。10状態の比較モードは未実装。測定手順・計算・表示・完了条件は[4波比較測定の仕様](docs/four-wave-comparison.md)、
別PCでの着手順は[引き継ぎ書](docs/handoff.md)を参照。

## 状態と限界

Pyramidでは4ch監視、2/4MHz帯域走査/WF、全機OFF背景、C5 RH36の設定、時刻付きスナップショット記録/再生に加え、scannerの全受信ペイロード保存・raw/filt再生、独立した人のラベル、学習用JSONL出力を実装。
Tab5ではチャンネル選択、背景、4ch履歴/WF、正常基準と暫定のレベル/安定性評価、通信欠落時の評価中断・診断表示、縦画面と明るさ調整を実装。[0.6.0検証](apps/tab5/docs/rf4-0.6.0-verification.md)。
画面の模擬プレビューは実測の証拠ではありません。受信dBはdBm・SNR・混信率・映像品質を示しません。
AI/NPU分類、自動しきい値適用、MADSYSTEM送信は未実装です。人の通過ラベルを保存する機能はありますが、独立した物理ゲート通過精度は未検証です。

## ライセンス

アプリの配布条件は **CC BY-NC-SA 4.0**（[LICENSE](LICENSE)）。C5MK由来のプロトコルと変更点は [NOTICE](NOTICE.md)。
同梱M5Stack/Espressifコンポーネントは元のApache-2.0、フォントはSIL OFL 1.1を維持します。ファイル単位のCC0表記とLVGL由来のMIT表記も保持します。
C5ファームのバイナリー、SDK、実機バックアップ、認証情報は含めません。

ビルド・テストの実施範囲は[移管時の検証](docs/repository-readiness.md)。不具合の再現方法や改善案はIssuesで共有できます。
