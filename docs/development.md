# 開発手順

0.2.2の記録・教示基盤は配備済み。[実装・検証と戻し方](../apps/pyramid/docs/0.2.2-recording-labels.md)を確認し、その上で[4波比較測定](four-wave-comparison.md)へ進む。

## Pyramid: Windows / LinuxのPCで検証

Python 3.10以上、Node.js（JS構文チェック用）を用意します。

```powershell
cd apps/pyramid
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements.txt
.venv/Scripts/python -m unittest -v test_protocol test_recording test_portability test_scanner test_scanner_http
node --check ui.js
node --check labels-ui.js
.venv/Scripts/python server.py --serial 00:00:00:00:00:01 --data .local/dev-data
```

Linuxでは `.venv/bin/python` に読み替えます。`http://127.0.0.1:8765/` が開き、上記の架空シリアルでは実機に接続せず、切断状態の画面を確認できます。模擬RFは生成しません。
実機がPyramidに刺さっているとき、PC上のサーバーがLAN越しにそのUSBを直接取得する機能はありません。
受信を伴う試験はコードをPyramidに配置して行います。既存のWeb画面を見るだけなら配備は不要です。

## Pyramid: 実機への更新

1. SSH先と認証を手元で確認します。認証情報・host keyはリポジトリに保存しません。
2. `systemctl cat c5-monitor.service` と `/api/state` で実行パス、版、C5シリアル、記録状態を確認。
3. 新しいソースを `/opt/c5-monitor/<新しい版>` など実行中の版と別の場所（現行は `/opt/c5-monitor/0.2.2`）へ配置。
4. 新しい場所でPythonテストを実行。既存のUSB所有サービスを動かしたまま、2つ目の受信プロセスを起動しない。
5. 既存サービス定義をバックアップ。記録停止を確認し、`/etc/c5-monitor.env` に対象の `C5_SERIAL` を設定して `c5-monitor.service` テンプレートのパスを確認。
6. サービス定義を更新し、daemon-reload / restart。`/var/lib/c5-monitor` を保持する。
7. 版、LIVE、ch、背景保存、記録/再生を確認。失敗時は旧サービス定義へ戻す。

システムPythonにpySerial 3.5があれば追加インストール不要。venvを使う場合はExecStartをそのPythonへ合わせます。
LAN向けは明示的に `--bind 0.0.0.0`。既存トークン/Host/Origin検査はLAN内の操作制限で、ユーザー認証やTLSの代わりではありません。

## Tab5

ESP-IDF **5.4.2** とESP32-P4用ツールチェーンをインストールし、そのPowerShellを使います。

```powershell
cd apps/tab5
./build.ps1
# 端末の型番・シリアル・バックアップを確認してから:
./build.ps1 -Flash -Port COMx
```

通常PowerShellからは `./build.ps1 -IdfPath <ESP-IDFのパス>`。IDF_PYTHON_ENV_PATHが必要な環境ではそのPCの値を設定してください。
LinuxはESP-IDFをexportした後、同じディレクトリで `idf.py build`。依存はdependencies.lockで固定し、managed_componentsとbuildは生成物として除外。
コアのPC回帰試験は `apps/tab5/tools/host_self_test.cpp`（READMEのC++17コマンド）。CIのTab5 jobはコア試験であり、実ESP-IDFビルドは別に行います。
生成済み日本語フォントCを含むので通常ビルドにNode/fontToolsは不要。再生成は `tools/font-build/README.md` とprovenanceを参照。
USB-C側の書き込み対象はTab5（ESP32-P4）。USB-AのC5と取り違えないでください。

## データ・旧版

このリポジトリには旧release一式と実測ログを含めません。元PCの旧ディレクトリと実機の旧ランタイムを残しています。
明日必要な測定履歴は、まず同じLANのPyramid Web画面で確認してください。
