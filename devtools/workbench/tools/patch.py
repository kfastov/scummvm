#!/usr/bin/env python3
"""Наложение unified diff на файлы репозитория.

Зачем отдельный инструмент, когда есть `patch(1)` и штатный Edit:

  * `Edit` требует, чтобы файл сперва был прочитан целиком. В `scummvm-src`
    файлы по 30–60 КБ, и чтение ради двух правок стоит дороже самой правки.
    Здесь якорем служат контекстные строки диффа, читать файл незачем.
  * Один вызов накладывает несколько ханков и несколько файлов сразу. В
    прошлых сессиях правки шли пачками по 2–7 (медиана 2), и каждая пачка
    превращалась в отдельный heredoc с полным текстом «до» и «после».
  * Наложение атомарно: если хоть один ханк не лёг, на диск не пишется
    ничего. `patch(1)` в этом месте оставляет файл наполовину правленым и
    сыплет `.rej`, а исходник движка потом чинить дороже, чем переписать диff.
  * Ответ короткий и одинаковой формы: строка на ханк. При промахе печатается
    не весь файл, а то место, где ожидался контекст.

Формат — обычный unified diff, тот же, что выдаёт `git diff`:

    --- a/engines/toolbook/toolbook.cpp
    +++ b/engines/toolbook/toolbook.cpp
    @@ -180,7 +180,9 @@
     контекст
    -убрать
    +добавить
     контекст

Номера строк в `@@` не обязаны быть точными: они лишь подсказка, откуда
начинать поиск контекста. Это важно, когда дифф пишется по памяти или по
выводу `grep -n`, а файл с тех пор сдвинулся.

Использование:

    tools/patch.py < d.diff              наложить
    tools/patch.py -                     то же, дифф со stdin
    tools/patch.py --check < d.diff      только проверить, не писать
    tools/patch.py --fuzz ws < d.diff    не считать разницей отступы
    tools/patch.py --root scummvm-src < d.diff   пути a/… от другого корня

Коды возврата: 0 — всё легло, 1 — хоть один ханк не лёг (файлы не тронуты).
"""
from __future__ import annotations

import argparse
import os
import re
import sys

HUNK_RE = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


class Hunk:
    __slots__ = ('old_start', 'lines', 'idx')

    def __init__(self, old_start: int, idx: int):
        self.old_start = old_start
        self.lines: list[tuple[str, str]] = []   # (' '|'-'|'+', текст без \n)
        self.idx = idx

    @property
    def before(self) -> list[str]:
        return [t for k, t in self.lines if k in ' -']

    @property
    def after(self) -> list[str]:
        return [t for k, t in self.lines if k in ' +']


def parse(diff: str) -> list[tuple[str, list[Hunk]]]:
    """Разбирает дифф в список (путь, ханки). Мусор между файлами пропускаем."""
    files: list[tuple[str, list[Hunk]]] = []
    cur_path: str | None = None
    hunks: list[Hunk] = []
    hunk: Hunk | None = None
    n = 0

    def flush():
        nonlocal cur_path, hunks
        if cur_path is not None and hunks:
            files.append((cur_path, hunks))
        cur_path, hunks = None, []

    for raw in diff.splitlines():
        if raw.startswith('--- '):
            flush()
            hunk = None
            continue
        if raw.startswith('+++ '):
            p = raw[4:].strip().split('\t')[0]
            # git пишет b/путь; голый путь тоже принимаем
            cur_path = p[2:] if p.startswith(('a/', 'b/')) else p
            hunks, hunk = [], None
            continue
        m = HUNK_RE.match(raw)
        if m:
            n += 1
            hunk = Hunk(int(m.group(1)), n)
            hunks.append(hunk)
            continue
        if hunk is None:
            continue                      # шапка diff --git, index, similarity…
        if raw.startswith('\\'):          # \ No newline at end of file
            continue
        if not raw:
            hunk.lines.append((' ', ''))  # пустая строка контекста
        elif raw[0] in ' -+':
            hunk.lines.append((raw[0], raw[1:]))
        else:
            hunk = None                   # вышли из ханка — дальше не наш текст
    flush()
    return files


def norm(s: str, fuzz: str) -> str:
    if fuzz == 'ws':
        return re.sub(r'\s+', ' ', s).strip()
    return s


def short(path: str) -> str:
    """Путь покороче: отчёт читает модель, лишние 60 символов на строку — расход."""
    rel = os.path.relpath(path)
    return rel if len(rel) < len(path) else path


def find(hay: list[str], need: list[str], hint: int, fuzz: str) -> int:
    """Ищет need в hay, начиная поиск от hint и расходясь в обе стороны.

    Возвращает индекс или -1. Расхождение от подсказки — чтобы номера строк в
    `@@` могли врать, но при этом ближайшее совпадение выигрывало у дальнего:
    в исходниках движка одинаковые куски (`return false;` и пустые строки)
    встречаются десятками.
    """
    if not need:
        return max(0, min(hint, len(hay)))
    nh = [norm(x, fuzz) for x in hay]
    nn = [norm(x, fuzz) for x in need]
    limit = len(hay) - len(nn)
    if limit < 0:
        return -1
    hint = max(0, min(hint, limit))
    for d in range(0, len(hay) + 1):
        for pos in ({hint - d, hint + d} if d else {hint}):
            if 0 <= pos <= limit and nh[pos:pos + len(nn)] == nn:
                return pos
    return -1


def apply_file(path: str, hunks: list[Hunk], fuzz: str) -> tuple[list[str] | None, list[str]]:
    """Возвращает (новые строки | None, отчёт по ханкам)."""
    report: list[str] = []
    if not os.path.exists(path):
        return None, ['ханк %d: нет файла %s' % (hunks[0].idx, short(path))]
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')

    # Ханки накладываем от конца к началу: тогда наложенные правки не сдвигают
    # подсказки для ещё не наложенных.
    ok = True
    for h in sorted(hunks, key=lambda x: -x.old_start):
        need = h.before
        pos = find(lines, need, h.old_start - 1, fuzz)
        if pos < 0:
            ok = False
            report.append('ханк %d: контекст не найден в %s около строки %d'
                          % (h.idx, short(path), h.old_start))
            first = next((t for k, t in h.lines if k != '+'), '')
            report.append('    ждали: %s' % first.strip()[:90])
            near = lines[max(0, h.old_start - 3):h.old_start + 2]
            for i, t in enumerate(near, start=max(1, h.old_start - 2)):
                report.append('    %5d: %s' % (i, t.strip()[:90]))
            continue
        lines[pos:pos + len(need)] = h.after
        report.append('ханк %d: %s:%d  −%d +%d'
                      % (h.idx, short(path), pos + 1,
                         sum(1 for k, _ in h.lines if k == '-'),
                         sum(1 for k, _ in h.lines if k == '+')))
    return (lines if ok else None), report


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('diff', nargs='?', default='-', help='файл с диффом или - (stdin)')
    ap.add_argument('--root', default='.', help='корень для путей из диффа')
    ap.add_argument('--check', action='store_true', help='проверить, не записывая')
    ap.add_argument('--fuzz', choices=['none', 'ws'], default='none',
                    help='ws — не считать разницей пробелы и табуляции')
    a = ap.parse_args()

    text = sys.stdin.read() if a.diff == '-' else open(a.diff, encoding='utf-8').read()
    files = parse(text)
    if not files:
        print('дифф пуст или не разобран: нет ни одного @@ с путём (+++)')
        return 1

    results, failed = [], False
    for rel, hunks in files:
        path = os.path.join(a.root, rel)
        if not os.path.exists(path) and os.path.exists(rel):
            path = rel               # дифф уже относительно текущего каталога
        new, report = apply_file(path, hunks, a.fuzz)
        results.append((path, new, report))
        if new is None:
            failed = True

    # Атомарность: пишем, только когда все файлы собрались целиком.
    for path, new, report in results:
        for line in report:
            print(line)
    if failed:
        print('НЕ НАЛОЖЕНО: ни один файл не изменён')
        return 1
    if a.check:
        print('проверка пройдена, файлы не тронуты')
        return 0
    for path, new, _ in results:
        with open(path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(new))
    print('готово: файлов %d, ханков %d' % (len(results), sum(len(h) for _, h in files)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
