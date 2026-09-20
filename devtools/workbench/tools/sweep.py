#!/usr/bin/env python3
"""Сплошной перебор с якорями — по правилу из ledger/0080.

Правило: если ответ можно получить тактами, надо получать его тактами. От
человека нужны две вещи — **условие валидности** (якоря, установленные раньше
и другим способом) и **генератор попыток** (сплошной обход конечного
пространства). Тогда ответ звучит как «единственное» или «доказано, что нет»,
и версия закрывается насовсем.

Разница с перебором, который в этом проекте сжёг дни (0029, 0034, 0023), не в
переборе, а в независимости критерия. Поэтому инструмент всегда печатает
**ожидаемое число ложных совпадений**: сколько кандидатов прошло бы проверку
на случайных данных. Если оно не близко к нулю, «нашлось единственное» ничего
не значит, и об этом сказано прямо, а не оставлено на догадку.

## Режим field: где в записи лежит известное значение

Классический случай 0079. Есть записи с известными базовыми смещениями и
независимо установленное значение для каждой. Ищем такое (смещение, ширина,
порядок байт, знаковость), при котором совпадают ВСЕ записи.

    tools/sweep.py field --cases c.txt --span 48

`c.txt` — по строке на запись: `<база> <ожидаемое>`, числа в 0x… или как есть.

## Режим grid: произвольное пространство и своё условие

    tools/sweep.py grid --space 'off=0..64' --space 'w=1,2,4' \
                        --cases c.txt --check 'val(base+off, w) == expect'

В выражении доступны: `data`, `base`, `expect`, все имена из --space, и
помощники `val(off, w, signed=False, be=False)`, `u8/u16/u32`, `raw(off, n)`.

## Из тёплого питона

    tools/pyshell.py e 'import sweep; sweep.field(cases, span=48)'
"""
from __future__ import annotations

import argparse
import itertools
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOK = os.path.join(ROOT, 'games/bashnya/kntower.tbk')


def read_value(data: bytes, off: int, width: int, be: bool, signed: bool):
    if off < 0 or off + width > len(data):
        return None
    fmt = {1: 'b', 2: 'h', 4: 'i'}[width]
    if not signed:
        fmt = fmt.upper()
    return struct.unpack_from(('>' if be else '<') + fmt, data, off)[0]


def expected_false(space_size: int, cases: int, width: int) -> float:
    """Сколько кандидатов прошло бы проверку на случайных данных.

    Грубая, но честная оценка: значение шириной width байт совпадает случайно
    с вероятностью 2^(-8*width); независимых записей — cases штук.
    """
    return space_size * (2.0 ** (-8 * width * cases))


def field(cases, span=48, widths=(1, 2, 4), data=None, quiet=False):
    """Все (off, width, endian, signed), при которых совпадают все записи."""
    if data is None:
        data = open(BOOK, 'rb').read()
    hits = []
    space = 0
    for width, be, signed in itertools.product(widths, (False, True), (False, True)):
        if width == 1 and be:
            continue                      # у одного байта порядка нет
        for off in range(0, span - width + 1):
            space += 1
            for base, want in cases:
                got = read_value(data, base + off, width, be, signed)
                if got != want:
                    break
            else:
                hits.append({'off': off, 'width': width,
                             'endian': 'be' if be else 'le',
                             'signed': signed})
    hits = collapse(hits)
    if not quiet:
        report(hits, space, len(cases), min(widths))
    return hits


def collapse(hits):
    """Схлопывает знаковый и беззнаковый варианты одного поля в один ответ.

    Если все значения якорей положительные и влезают без старшего бита, оба
    варианта совпадут. Это не два разных ответа, а один с неопределённым
    знаком — так и надо печатать, иначе «единственное» никогда не выпадает.
    """
    by_key = {}
    for h in hits:
        key = (h['off'], h['width'], h['endian'])
        by_key.setdefault(key, []).append(h)
    out = []
    for (off, width, endian), group in sorted(by_key.items()):
        signs = {g['signed'] for g in group}
        out.append({'off': off, 'width': width, 'endian': endian,
                    'signed': (None if len(signs) > 1 else signs.pop())})
    return out


def report(hits, space, n_cases, width) -> None:
    ghost = expected_false(space, n_cases, width)
    if not hits:
        print('НЕТ: ни один вариант не проходит все %d записей.' % n_cases)
        print('Пространство исчерпано (%d вариантов) — версия закрыта.' % space)
    elif len(hits) == 1:
        h = hits[0]
        print('ЕДИНСТВЕННОЕ: %s' % fmt_hit(h))
    else:
        print('КАНДИДАТОВ %d из %d вариантов:' % (len(hits), space))
        for h in hits[:20]:
            print('   %s' % fmt_hit(h))
        if len(hits) > 20:
            print('   … ещё %d' % (len(hits) - 20))
    print('якорей: %d; ожидаемых ложных совпадений: %s'
          % (n_cases, ('%.3g' % ghost) if ghost >= 1e-4 else '<0.0001'))
    if ghost > 0.01:
        print('ВНИМАНИЕ: критерий слаб — на случайных данных нашлось бы примерно')
        print('столько же. Нужны ещё якори либо шире значения (см. ledger/0029).')


def fmt_hit(h) -> str:
    sign = {True: ', со знаком', False: ', без знака',
            None: ', знак не определён (все якори положительные)'}[h.get('signed')]
    order = '' if h['width'] == 1 else ', %s' % h['endian']
    return 'смещение +%d (0x%x), %d байт%s%s' % (
        h['off'], h['off'], h['width'], order, sign)


def parse_cases(path: str):
    out = []
    for ln in open(path, encoding='utf-8'):
        ln = ln.split('#')[0].strip()
        if not ln:
            continue
        parts = re.split(r'[\s,;]+', ln)
        if len(parts) < 2:
            raise SystemExit('строка «%s»: нужно два числа — база и ожидаемое' % ln)
        out.append((int(parts[0], 0), int(parts[1], 0)))
    return out


def parse_space(specs):
    """'off=0..64' | 'w=1,2,4' | 'x=0..32:4' (шаг)."""
    space = {}
    for s in specs:
        name, _, body = s.partition('=')
        name = name.strip()
        if '..' in body:
            rng, _, step = body.partition(':')
            lo, _, hi = rng.partition('..')
            space[name] = list(range(int(lo, 0), int(hi, 0) + 1, int(step, 0) if step else 1))
        else:
            space[name] = [int(v, 0) for v in re.split(r'[\s,]+', body) if v]
    return space


def grid(space: dict, cases, check: str, data=None, quiet=False):
    if data is None:
        data = open(BOOK, 'rb').read()

    def val(off, w=2, signed=False, be=False):
        return read_value(data, off, w, be, signed)

    helpers = {
        'data': data, 'val': val, 'raw': lambda o, n: data[o:o + n],
        'u8': lambda o: val(o, 1), 'u16': lambda o: val(o, 2), 'u32': lambda o: val(o, 4),
        'len': len, 'abs': abs, 'all': all, 'any': any, 'range': range,
    }
    code = compile(check, '<check>', 'eval')
    names = sorted(space)
    hits = []
    total = 0
    for combo in itertools.product(*(space[n] for n in names)):
        total += 1
        env = dict(helpers)
        env.update(dict(zip(names, combo)))
        for base, want in cases:
            env['base'], env['expect'] = base, want
            try:
                if not eval(code, env):
                    break
            except Exception:
                break
        else:
            hits.append(dict(zip(names, combo)))
    if not quiet:
        if not hits:
            print('НЕТ: ни одна точка пространства (%d) не проходит все %d якорей.'
                  % (total, len(cases)))
        elif len(hits) == 1:
            print('ЕДИНСТВЕННОЕ: %s' % hits[0])
        else:
            print('КАНДИДАТОВ %d из %d:' % (len(hits), total))
            for h in hits[:20]:
                print('   %s' % h)
            if len(hits) > 20:
                print('   … ещё %d' % (len(hits) - 20))
        print('якорей: %d' % len(cases))
        if hits and len(hits) > 1:
            print('Множественность — не «почти нашли», а «критерий не различает».')
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('field', help='где в записи лежит известное значение')
    p.add_argument('--cases', required=True)
    p.add_argument('--span', type=int, default=48, help='размер записи в байтах')
    p.add_argument('--widths', default='1,2,4')
    p.add_argument('--data', default=BOOK)

    p = sub.add_parser('grid', help='произвольное пространство и своё условие')
    p.add_argument('--space', action='append', required=True)
    p.add_argument('--cases', required=True)
    p.add_argument('--check', required=True)
    p.add_argument('--data', default=BOOK)

    a = ap.parse_args()
    data = open(a.data, 'rb').read()
    cases = parse_cases(a.cases)
    if len(cases) < 2:
        print('якорей меньше двух: перебор даст мусор. См. ledger/0080.')
        return 1
    if a.cmd == 'field':
        field(cases, span=a.span,
              widths=tuple(int(w) for w in a.widths.split(',')), data=data)
    else:
        grid(parse_space(a.space), cases, a.check, data=data)
    return 0


if __name__ == '__main__':
    sys.exit(main())
