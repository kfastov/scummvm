#!/usr/bin/env python3
"""Кадры прогона: описание словами, склейка одинаковых, сравнение.

Скриншот в контексте стоит около полутора тысяч токенов. За разобранные
сессии их набралось 259 — примерно 0,4 миллиона токенов, и добрая половина
была снимками ожидания, отличавшимися от предыдущего нулём пикселей.

Здесь кадр описывается словами. Этого хватает, чтобы понять «чёрный экран»,
«меню на месте», «поменялась только нижняя треть» — а картинку смотреть уже
осознанно, когда описание говорит, что смотреть есть на что.

    tools/frames.py describe svm-run/frames/…-00002.png
    tools/frames.py uniq svm-run/frames          какие кадры вообще разные
    tools/frames.py diff a.png b.png             что изменилось между двумя

Карта яркости — это и есть «картинка словами»: сетка 32×16 по кадру, символ
на ячейку от тёмного к светлому « .:-=+*#%@». По ней видно рамки, текстовые
блоки и пустоты, а стоит она полтысячи символов вместо полутора тысяч токенов.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys

RAMP = ' .:-=+*#%@'


def load(path):
    from PIL import Image
    return Image.open(path).convert('RGB')


def digest(path: str) -> str:
    with open(path, 'rb') as f:
        return hashlib.sha1(f.read()).hexdigest()[:12]


def luma_grid(img, cols=32, rows=16):
    small = img.resize((cols, rows))
    px = small.load()
    out = []
    for y in range(rows):
        line = ''
        for x in range(cols):
            r, g, b = px[x, y]
            lum = (r * 299 + g * 587 + b * 114) // 1000
            line += RAMP[min(len(RAMP) - 1, lum * len(RAMP) // 256)]
        out.append(line)
    return out


def describe(path: str, grid: bool = True) -> str:
    img = load(path)
    w, h = img.size
    colors = img.getcolors(maxcolors=1 << 20) or []
    colors.sort(reverse=True)
    total = w * h
    lines = ['%s  %dx%d, различных цветов %d' % (os.path.basename(path), w, h, len(colors))]
    top = []
    for count, rgb in colors[:4]:
        top.append('#%02x%02x%02x %d%%' % (rgb[0], rgb[1], rgb[2], round(100 * count / total)))
    lines.append('  преобладают: ' + ', '.join(top))

    # Непустая область: всё, что отличается от самого частого цвета (фона).
    bg = colors[0][1] if colors else (0, 0, 0)
    px = img.load()
    x0, y0, x1, y1 = w, h, -1, -1
    step = max(1, min(w, h) // 200)
    for y in range(0, h, step):
        for x in range(0, w, step):
            if px[x, y] != bg:
                x0, y0 = min(x0, x), min(y0, y)
                x1, y1 = max(x1, x), max(y1, y)
    if x1 < 0:
        lines.append('  кадр одноцветный — скорее всего пусто')
    else:
        lines.append('  непустая область: (%d,%d)-(%d,%d)' % (x0, y0, x1, y1))
    lines.append('  хеш: %s' % digest(path))
    if grid:
        lines.append('  карта яркости 32x16:')
        lines += ['    ' + r for r in luma_grid(img)]
    return '\n'.join(lines)


def uniq(dirpath: str, limit: int = 40) -> str:
    """Список различных кадров: одинаковые подряд идущие склеиваются."""
    names = sorted(f for f in os.listdir(dirpath) if f.endswith('.png'))
    if not names:
        return 'кадров нет: %s' % dirpath
    runs = []
    prev = None
    for n in names:
        h = digest(os.path.join(dirpath, n))
        if h == prev:
            runs[-1][2] = n
            runs[-1][3] += 1
        else:
            runs.append([h, n, n, 1])
            prev = h
    out = ['кадров %d, различных подряд %d' % (len(names), len(runs))]
    for h, first, last, cnt in runs[:limit]:
        span = first if cnt == 1 else '%s … %s' % (first, last)
        out.append('  %s  x%-4d %s' % (h, cnt, span))
    if len(runs) > limit:
        out.append('  … ещё %d' % (len(runs) - limit))
    return '\n'.join(out)


def diff(a: str, b: str) -> str:
    ia, ib = load(a), load(b)
    if ia.size != ib.size:
        return 'размеры разные: %s против %s' % (ia.size, ib.size)
    w, h = ia.size
    pa, pb = ia.load(), ib.load()
    changed = 0
    x0, y0, x1, y1 = w, h, -1, -1
    step = max(1, min(w, h) // 240)
    for y in range(0, h, step):
        for x in range(0, w, step):
            if pa[x, y] != pb[x, y]:
                changed += 1
                x0, y0 = min(x0, x), min(y0, y)
                x1, y1 = max(x1, x), max(y1, y)
    probed = len(range(0, h, step)) * len(range(0, w, step))
    if not changed:
        return 'кадры совпадают (проверено %d точек)' % probed
    return ('изменилось %d%% точек, область (%d,%d)-(%d,%d)'
            % (round(100 * changed / probed), x0, y0, x1, y1))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('describe'); p.add_argument('path'); p.add_argument('--no-grid', action='store_true')
    p = sub.add_parser('uniq'); p.add_argument('dir'); p.add_argument('--limit', type=int, default=40)
    p = sub.add_parser('diff'); p.add_argument('a'); p.add_argument('b')
    a = ap.parse_args()
    try:
        if a.cmd == 'describe':
            print(describe(a.path, grid=not a.no_grid))
        elif a.cmd == 'uniq':
            print(uniq(a.dir, a.limit))
        else:
            print(diff(a.a, a.b))
    except FileNotFoundError as exc:
        print('нет файла: %s' % exc.filename); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
