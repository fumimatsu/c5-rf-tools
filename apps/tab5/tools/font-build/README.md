# フォント再生成

通常ビルドはmain/fonts/の生成済みCを使う。再生成時のみPythonのfonttools==4.60.1とNode.jsが必要。

1. このフォルダーで `npm ci`。
2. provenance.jsonのsourceからフォントを取得し、input/NotoSansJP.ttfへ保存。SHA256がprovenance.jsonと一致することを確認する。違う版を黙って採用しない。
3. プロジェクトルートで `python tools/font-build/build_fonts.py`。ソースの日本語文字から20/24pxを生成する。
4. UI文言と収録文字を照合し、通常ビルドと物理画面を確認する。

入力フォントとnode_modulesは保存版に含めない。ライセンスはmain/fonts/OFL.txt。
