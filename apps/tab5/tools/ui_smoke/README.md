# Portrait UI software smoke test

実際の `main/ui.cpp` と日本語フォントを、LVGL 9.2.2のソフト描画で動かす。ハードウェア、受信、PWMはスタブ。ここだけで使う合成RF値は実機・原記録へ送らず、画像のフッターにも `HOST TEST / synthetic RF / not measurements` と明記する。

ESP-IDFビルドで解決済みの `managed_components/lvgl__lvgl` を使う。別の配置なら `-DLVGL_DIR=<LVGL 9.2.2 source>`。C/C++コンパイラ、CMake、Ninjaが必要。assertを有効にするためDebugで実行する。

```sh
cmake -S apps/tab5/tools/ui_smoke -B .local/tab5-ui-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .local/tab5-ui-build
mkdir -p .local/tab5-ui-evidence
cd .local/tab5-ui-evidence
../tab5-ui-build/ui_smoke
```

WindowsはVisual StudioのDeveloper Command Promptで最後を `..\tab5-ui-build\ui_smoke.exe` に読み替える。出力はカレントディレクトリのPPM画像。生成物は `.local/` に置き、Gitへ追加しない。

確認内容: 表示中のオブジェクトの画面内配置、背景/正常基準/チャンネル/明るさのダイアログ、チャンネル選択、4ch/帯域切替、明るさの上下限と失敗時の表示値保持。明るさはスタブなので100%も試すが、実機の給電やタッチ座標、USB、LCD、NVSは検証しない。画面画像も目視確認する。
