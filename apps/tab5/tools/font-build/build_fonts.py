from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
import hashlib,json,subprocess,os
p=Path(__file__).resolve().parents[2];inp=p/'tools/font-build/input';meta=json.loads((p/'tools/font-build/provenance.json').read_text(encoding='utf-8'))
assert hashlib.sha256((inp/'NotoSansJP.ttf').read_bytes()).hexdigest()==meta['source_sha256'],'Font input changed'
if not (inp/'NotoSansJP-500.ttf').exists():
 font=instantiateVariableFont(TTFont(inp/'NotoSansJP.ttf'),{'wght':500},inplace=True);font.save(inp/'NotoSansJP-500.ttf')
text=''.join(f.read_text(encoding='utf-8') for f in (p/'main').glob('*') if f.suffix in ('.cpp','.h'));symbols=''.join(sorted({c for c in text if ord(c)>127}));(p/'tools/font-build/symbols.txt').write_text(symbols,encoding='utf-8')
for size in [20,24]:
 args=['node',str(p/'tools/font-build/node_modules/lv_font_conv/lv_font_conv.js'),'--font','tools/font-build/input/NotoSansJP-500.ttf','--size',str(size),'--bpp','4','--format','lvgl','--range','0x20-0x7e','--symbols',symbols,'--no-compress','--no-kerning','--lv-include','lvgl.h','--lv-font-name',f'rf_jp{size}','-o',f'main/fonts/rf_jp{size}.c']
 subprocess.run(args,check=True,cwd=p)
meta['unicode_glyphs']=len(symbols);(p/'tools/font-build/provenance.json').write_text(json.dumps(meta,indent=2),encoding='utf-8');print('Font glyphs:',len(symbols))