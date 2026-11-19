#!/usr/bin/env python3
"""メニュー用フォントを作る。

source/program 内の文字列リテラルに出てくる文字だけを残して(サブセット化)、
C++ の配列として source/program/generated/fonts_data.inc に書き出す。
メニューの文字を変えたら、ビルド時に自動で作り直される(CI で実行)。

使い方: make_fonts.py <google/fonts を置いたフォルダ> <リポジトリのルート>
"""
import io, os, re, sys
from fontTools import subset
from fontTools.ttLib import TTFont

gfonts, root = sys.argv[1], sys.argv[2]
src_dir = os.path.join(root, 'source', 'program')
out_dir = os.path.join(src_dir, 'generated')
os.makedirs(out_dir, exist_ok=True)

# 1. 文字列リテラルから文字を集める(コメントは除く)
lit = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
chars = set(chr(c) for c in range(0x20, 0x7F))
for name in sorted(os.listdir(src_dir)):
    if not name.endswith(('.cpp', '.hpp')):
        continue
    text = open(os.path.join(src_dir, name), encoding='utf-8').read()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    for m in lit.finditer(text):
        chars.update(m.group(1))
# フォント名(fonts.txt)の文字も入れる
entries = []
for line in open(os.path.join(root, 'tools', 'fonts.txt'), encoding='utf-8'):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    label, path, wght = [x.strip() for x in line.split('|')]
    entries.append((label, path, wght))
    chars.update(label)
chars.discard('\\')
chars.add('\\')
codes = sorted(ord(c) for c in chars if 0x20 <= ord(c) <= 0xFFFF)

# 2. 各フォントをサブセット化
blobs = []
for label, path, wght in entries:
    font = TTFont(os.path.join(gfonts, path))
    if wght != '-' and 'fvar' in font:
        from fontTools.varLib import instancer
        font = instancer.instantiateVariableFont(font, {'wght': float(wght)})
    opts = subset.Options()
    opts.hinting = False
    opts.desubroutinize = True
    opts.layout_features = []
    opts.name_IDs = []
    opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=codes)
    sub.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    blobs.append((label, path, buf.getvalue()))
    print(f'{label}: {len(buf.getvalue())} bytes')

# 3. C++ に書き出す
def ranges(codes):
    out, start, prev = [], None, None
    for c in codes:
        if start is None:
            start = prev = c
        elif c == prev + 1:
            prev = c
        else:
            out.append((start, prev)); start = prev = c
    if start is not None:
        out.append((start, prev))
    return out

with open(os.path.join(out_dir, 'fonts_data.inc'), 'w', encoding='utf-8') as o:
    o.write('// 自動生成(tools/make_fonts.py)。手で編集しないでください。\n')
    for i, (label, path, data) in enumerate(blobs):
        o.write(f'// {label}: {path}\nalignas(4) static const unsigned char kFontData{i}[] = {{\n')
        for j in range(0, len(data), 24):
            o.write(','.join(str(b) for b in data[j:j + 24]) + ',\n')
        o.write('};\n')
    o.write('static const EmbeddedFont kEmbeddedFonts[] = {\n')
    for i, (label, path, data) in enumerate(blobs):
        o.write(f'    {{ "{label}", kFontData{i}, sizeof(kFontData{i}) }},\n')
    o.write('};\n')
    o.write('static const ImWchar kGlyphRanges[] = {\n')
    for a, b in ranges(codes):
        o.write(f'    0x{a:04x}, 0x{b:04x},\n')
    o.write('    0,\n};\n')
print(f'glyphs: {len(codes)}')
