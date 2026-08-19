#!/usr/bin/env python3
"""Сводка на старт сессии: собирает STATE.md из того, что и так есть.

Каждая сессия начиналась с переоткрытия одного и того же: `ls`, хвост
`LEDGER.md`, чтение `NOTES.md` целиком (27 КБ за один вызов). Ответы на эти
вопросы выводятся из репозитория, значит их надо выводить машиной, а не
перечитывать.

    tools/brief.py            напечатать сводку
    tools/brief.py --write    записать её в STATE.md

STATE.md — производный файл, его не редактируют руками: он пересобирается.
Постоянное знание живёт в NOTES.md и ledger/, и туда же дописываются выводы.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd: str, cwd: str = ROOT) -> str:
    try:
        return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True,
                              text=True, timeout=20).stdout.strip()
    except Exception:
        return ''


def ledger_tail(n: int = 6) -> list[str]:
    """Последние строки таблицы LEDGER.md — по строке на запись."""
    path = os.path.join(ROOT, 'LEDGER.md')
    if not os.path.exists(path):
        return []
    rows = [ln.strip() for ln in open(path, encoding='utf-8')
            if ln.startswith('| 20')]
    out = []
    for r in rows[-n:]:
        cells = [c.strip() for c in r.strip('|').split('|')]
        if len(cells) >= 4:
            ident = re.sub(r'\[(\d+)\].*', r'\1', cells[1])
            out.append('%s — %s — %s %s' % (cells[0], ident, cells[2], cells[3]))
    return out


def barrier() -> list[str]:
    """Где сейчас упирается книга — из лога последнего прогона."""
    path = os.path.join(ROOT, 'svm-run', 'run.log')
    if not os.path.exists(path):
        return ['прогонов не было (svm-run/run.log нет)']
    pat = re.compile(r'пока не реализован|пока не разобран|неожиданная форма|не попало в стек')
    lines = [ln.rstrip('\n') for ln in open(path, encoding='utf-8', errors='replace')]
    hits = [l for l in lines if pat.search(l)]
    page = [l for l in lines if 'страница' in l]
    out = []
    if hits:
        out.append('барьер: %s' % hits[-1].strip()[:150])
        out.append('всего барьеров в прогоне: %d' % len(hits))
    else:
        out.append('барьеров в последнем прогоне нет')
    if page:
        out.append('последняя страница: %s' % page[-1].strip()[:150])
    return out


def tools_map() -> list[str]:
    """Однострочники по каждому инструменту — первая строка его docstring."""
    out = []
    tdir = os.path.join(ROOT, 'tools')
    for name in sorted(os.listdir(tdir)):
        if not name.endswith('.py') or name.startswith('_'):
            continue
        first = ''
        with open(os.path.join(tdir, name), encoding='utf-8', errors='replace') as f:
            body = f.read(1200)
        m = re.search(r'"""(.+)', body)
        if m:
            first = m.group(1).strip().rstrip('"').strip()
        out.append('  tools/%-14s %s' % (name, first[:88]))
    return out


def build_state() -> str:
    svm = os.path.join(ROOT, 'scummvm-src', 'scummvm')
    if not os.path.exists(svm):
        return 'своей сборки нет — собрать: ./dev.sh (BUILD_ONLY=1)'
    import datetime
    ts = datetime.datetime.fromtimestamp(os.path.getmtime(svm))
    newer = sh("find scummvm-src/engines/toolbook -newer scummvm-src/scummvm -name '*.cpp' -o "
               "-newer scummvm-src/scummvm -name '*.h' 2>/dev/null | head -3")
    tail = '  (исходники движка новее сборки — пересобрать)' if newer else ''
    return 'сборка от %s%s' % (ts.strftime('%Y-%m-%d %H:%M'), tail)


def render() -> str:
    L = []
    L.append('# Состояние работ (файл производный, пересобирается tools/brief.py)')
    L.append('')
    L.append('## Где сейчас')
    for b in barrier():
        L.append('* %s' % b)
    L.append('* %s' % build_state())
    L.append('* ветка %s, последний коммит: %s'
             % (sh('git rev-parse --abbrev-ref HEAD'), sh('git log -1 --format=%s')))
    dirty = sh('git status --porcelain | head -8')
    if dirty:
        L.append('* незакоммиченное:')
        for ln in dirty.split('\n'):
            L.append('  * %s' % ln.strip())
    L.append('')
    L.append('## Последние записи леджера')
    for r in ledger_tail():
        L.append('* %s' % r)
    L.append('')
    L.append('## Инструменты')
    L.append('```')
    L += tools_map()
    L.append('```')
    L.append('')
    L.append('Подробности — `CLAUDE.md` (метод и ловушки стенда) и `NOTES.md` (форматы).')
    return '\n'.join(L) + '\n'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--write', action='store_true', help='записать в STATE.md')
    a = ap.parse_args()
    text = render()
    if a.write:
        with open(os.path.join(ROOT, 'STATE.md'), 'w', encoding='utf-8') as f:
            f.write(text)
        print('STATE.md: %d символов' % len(text))
    else:
        print(text, end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
