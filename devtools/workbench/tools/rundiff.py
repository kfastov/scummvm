#!/usr/bin/env python3
"""Что изменилось с прошлого прогона.

Раньше после каждого прогона лог читался хвостом, а кадры — глазами: 465
обращений к `svm-run` за разобранные сессии. Но интересна почти всегда
*разница*: какие строки в логе новые и какие кадры перестали совпадать. Хвост
одного и того же лога, прочитанный десять раз, — это десять раз одно и то же в
окне.

    tools/rundiff.py            отчёт против прошлого прогона и запомнить этот
    tools/rundiff.py --keep     отчёт, но снимок не обновлять
    tools/rundiff.py --lines 40 больше новых строк лога

Снимок лежит в `svm-run/prev/`. Первый запуск сравнивать не с чем — тогда
печатается обычная сводка.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'svm-run')
PREV = os.path.join(RUN, 'prev')

# Строки-барьеры: ради них лог обычно и открывают.
BARRIER = re.compile(r'пока не реализован|пока не разобран|неожиданная форма|'
                     r'не попало в стек|не поддерж|WARNING|ERROR|assert', re.I)
NOISE = re.compile(r'hardwareinput|antialias|joy_|Checking SDL|Saved screenshot', re.I)


def norm(line: str) -> str:
    """Огрубление для сравнения: адреса и числа гуляют от прогона к прогону."""
    return re.sub(r'0x[0-9a-f]+|\b\d+\b', '#', line.strip(), flags=re.I)


def read_log(path: str) -> list[str]:
    if not os.path.exists(path):
        return []
    with open(path, encoding='utf-8', errors='replace') as f:
        return [ln.rstrip('\n') for ln in f]


def frame_hashes(d: str) -> list[tuple[str, str]]:
    if not os.path.isdir(d):
        return []
    out = []
    for n in sorted(f for f in os.listdir(d) if f.endswith('.png')):
        with open(os.path.join(d, n), 'rb') as f:
            out.append((n, hashlib.sha1(f.read()).hexdigest()[:12]))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep', action='store_true', help='не обновлять снимок')
    ap.add_argument('--lines', type=int, default=20, help='сколько новых строк лога печатать')
    a = ap.parse_args()

    cur_log = read_log(os.path.join(RUN, 'run.log'))
    old_log = read_log(os.path.join(PREV, 'run.log'))
    cur_fr = frame_hashes(os.path.join(RUN, 'frames'))
    old_fr = {}
    prev_fr_file = os.path.join(PREV, 'frames.txt')
    if os.path.exists(prev_fr_file):
        for ln in open(prev_fr_file, encoding='utf-8'):
            n, _, h = ln.strip().partition(' ')
            old_fr[n] = h

    print('== кадров %d (было %d), различных %d'
          % (len(cur_fr), len(old_fr), len({h for _, h in cur_fr})))
    if cur_fr:
        same = sum(1 for n, h in cur_fr if old_fr.get(n) == h)
        if old_fr:
            print('   совпало с прошлым прогоном: %d из %d' % (same, len(cur_fr)))
        print('   последний кадр: %s' % cur_fr[-1][0])

    seen = {norm(l) for l in old_log}
    new = [l for l in cur_log if norm(l) not in seen and not NOISE.search(l)]
    # Повторы внутри самого прогона тоже сворачиваем: цикл движка легко даёт
    # тысячу одинаковых по смыслу строк.
    folded, cnt = [], {}
    for l in new:
        k = norm(l)
        cnt[k] = cnt.get(k, 0) + 1
        if cnt[k] == 1:
            folded.append(l)
    print('== строк в логе %d, новых по смыслу %d (уникальных %d)'
          % (len(cur_log), len(new), len(folded)))
    for l in folded[:a.lines]:
        k = norm(l)
        mult = ' [x%d]' % cnt[k] if cnt[k] > 1 else ''
        print('   %s%s' % (l[:160], mult))
    if len(folded) > a.lines:
        print('   … ещё %d видов строк (--lines N)' % (len(folded) - a.lines))

    bar = [l for l in cur_log if BARRIER.search(l)]
    if bar:
        print('== барьеры (%d), последний:' % len(bar))
        i = cur_log.index(bar[-1])
        for l in cur_log[i:i + 8]:
            if 'ToolBook' in l or 'операнд' in l or BARRIER.search(l):
                print('   %s' % l[:160])

    if not a.keep:
        os.makedirs(PREV, exist_ok=True)
        if os.path.exists(os.path.join(RUN, 'run.log')):
            shutil.copy(os.path.join(RUN, 'run.log'), os.path.join(PREV, 'run.log'))
        with open(prev_fr_file, 'w', encoding='utf-8') as f:
            for n, h in cur_fr:
                f.write('%s %s\n' % (n, h))
    return 0


if __name__ == '__main__':
    sys.exit(main())
