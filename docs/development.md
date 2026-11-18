# 開発手順

Tab5ファームウェアとPyramidアプリのセットアップ、テスト、更新手順です。動作条件と機能一覧は各アプリのREADMEを参照してください。

## Pyramid: Windows / LinuxのPCで検証

Python 3.10以上、Node.js（JS構文チェック用）を用意します。

```powershell
cd apps/pyramid
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements.txt
.venv/Scripts/python -m unittest -v test_protocol test_recording test_portability test_scanner test_scanner_http test_led_meter test_background_button
node --check ui.js
node --check labels-ui.js
.venv/Scripts/python server.py --serial 00:00:00:00:00:01 --data .local/dev-data
```

Linuxでは `.venv/bin/python` に読み替えます。`http://127.0.0.1:8765/` が開き、上記の架空シリアルでは実機に接続せず、切断状態の画面を確認できます。模擬RFは生成しません。
実機がPyramidに刺さっているとき、PC上のサーバーがLAN越しにそのUSBを直接取得する機能はありません。
受信を伴う試験はコードをPyramidに配置して行います。既存のWeb画面を見るだけなら配備は不要です。

## Pyramid: 実機への導入と更新

同梱の `apps/pyramid/c5-monitor.service` はsystemd用のテンプレートです。任意の配置先を使う場合は、ユニットの `WorkingDirectory` と `ExecStart` を合わせます。LED機能を使う場合は[LEDサービスの設定](pyramid-led-meter.md)も確認してください。

新規導入では、アプリとPython依存関係をPyramidに配置し、USBシリアルを
`python3 -m serial.tools.list_ports -v` で確認します。環境ファイルを設定した後、
ユニットを `/etc/systemd/system/` に配置し、`systemctl daemon-reload` と
`systemctl enable --now c5-monitor.service` で起動します。LEDサービスは
4ch監視がLIVE になってから同様に有効化します。

既存の環境を更新する手順は以下のとおりです。

1. 対象ホストとC5を確認します。C5のUSBシリアルは `/etc/c5-monitor.env` に `C5_SERIAL=<C5_SERIAL>` の形式で設定します。シリアル・接続設定・認証情報は展開先で管理します。
2. `systemctl cat c5-monitor.service` と `/api/state` で実行パス、版、C5シリアル、記録状態を確認。
3. 新しいソースを `/opt/c5-monitor/<新しい版>` など実行中の版と別の場所へ配置。
4. 新しい場所でPythonテストを実行。既存のUSB所有サービスを動かしたまま、2つ目の受信プロセスを起動しない。
5. 旧アプリとユニット定義を保存。記録と終了処理の停止を確認してから、環境ファイルと新ユニットのパスを照合。
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
縦UIのソフト描画試験は `apps/tab5/tools/ui_smoke/README.md`。実際のUIとフォントを使い、合成値で配置・ダイアログ・操作を確認します。LCD/タッチ/実RF/給電は別途実機確認が必要です。
生成済み日本語フォントCを含むので通常ビルドにNode/fontToolsは不要。再生成は `tools/font-build/README.md` とprovenanceを参照。
USB-C側の書き込み対象はTab5（ESP32-P4）。USB-AのC5と取り違えないでください。

## 実機データ

Pyramidは設定・背景・測定履歴を `/var/lib/c5-monitor` に保存します。Tab5は設定と基準を本体NVSに保存します。機器固有の設定、実測ログ、バックアップは各環境で管理してください。
