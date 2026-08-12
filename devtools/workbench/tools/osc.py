#!/usr/bin/env python3
"""Скрипты OpenScript в книге ToolBook — поиск обработчиков и разбор кода.

Что установлено про хранение скриптов (проверено на книге «Башни знаний»,
подробности и опровергнутые версии — в ledger/0023):

* Скрипт объекта разложен по **обработчикам**: один обработчик — один блок.
  Блок устроен так::

      … 08 00 00 00 29 XX | <код> | 27 1d 66 | <u16 strOff> | <таблица строк>

  `strOff` — смещение таблицы строк от начала кода, и оно всегда равно
  `длина_кода + 7` (3 байта маркера, 2 поля, 2 байта хеша). Это и служит
  проверкой, что блок найден правильно.

* Код всех без исключения обработчиков начинается одинаково::

      26 | 0f f8 ff | 3b 03 | 0d fc ff | 3b 04 | 0d f8 ff

  Пролог найден в 988 местах книги — столько в игре обработчиков.

* **Таблица строк** — строки с нулём подряд. Перед именами-идентификаторами
  (именами сообщений и свойств) стоят два байта хеша, перед строковыми
  литералами — нет. Последняя строка таблицы — **имя обработчика**
  (`buttonClick`, `mouseEnter`, `enterPage`, `timerNotify`…).

* **Двухбайтовый операнд-ссылка** — прямое относительное смещение: цель равна
  «конец инструкции + операнд». Так адресуются строки таблицы. Проверено на
  обработчике `ResetPage`, где пятнадцать одинаковых операторов ссылаются на
  пятнадцать имён подряд, и смещения сходятся до байта.

* Исходник скрипта в книге игры **не хранится** (в системной части ToolBook —
  хранится, сжатый обратными ссылками). Значит нужен разбор байт-кода.

**Чего пока нет:** значения опкодов. Длины операндов в `OPERAND_LEN` выведены
по согласованности разбора (блок обязан разбираться ровно до маркера) и
проверены вручную на `ResetPage`; таблица неполна, на длинных обработчиках
разбор упирается в неизвестный опкод. Пока это дизассемблер структуры, а не
смысла.

Использование::

    osc.py handlers <книга>            — список обработчиков с таблицами строк
    osc.py dis <книга> [--name buttonClick] [--limit 5]  — разбор кода
    osc.py names <книга>               — какие сообщения шлют скрипты
    osc.py keywords <MTB40CMP.DLL>     — таблица имён OpenScript из среды
"""

from __future__ import annotations

import argparse
import bisect
import re
import struct
import sys

# Пролог обработчика — общий для всех 988 блоков книги.
PROLOGUE = bytes.fromhex('260ff8ff3b030dfcff3b040df8ff')
# Маркер конца кода и начала таблицы строк.
END_MARK = bytes.fromhex('271d66')

# Длины операндов. Выведены по согласованности разбора и проверены вручную на
# обработчике ResetPage; часть значений ещё под вопросом (см. модуль docstring).
OPERAND_LEN = {
    0x00: 0, 0x02: 2, 0x04: 0, 0x06: 4, 0x07: 2, 0x08: 2, 0x0d: 2, 0x0f: 2,
    0x10: 0, 0x1b: 0, 0x26: 0, 0x27: 0, 0x2b: 0, 0x2e: 0, 0x3a: 2, 0x3b: 1,
    0x3c: 0, 0x6c: 2, 0x73: 0,
}

# Как читается операнд: ссылка на таблицу строк или число.
REF_OPS = {0x07, 0x08, 0x6c}
LOCAL_OPS = {0x02, 0x0d, 0x0f}


def _is_ident(t: str) -> bool:
    return bool(t) and t[0].isascii() and (t[0].isalpha() or t[0] == '_') and \
        all(c.isascii() and (c.isalnum() or c == '_') for c in t)


def read_book(path: str) -> bytes:
    data = open(path, 'rb').read()
    if data[:4] != b'\x03JBO':
        # книга бывает зашита в .EXE
        off = data.find(b'\x03JBO')
        if off > 0:
            print(f'книга найдена внутри файла со смещения {off:#x}', file=sys.stderr)
            return data[off:]
    return data


def find_handlers(data: bytes) -> list[dict]:
    """Все обработчики книги: код и таблица строк."""
    starts = [m.start() for m in re.finditer(re.escape(PROLOGUE), data, re.S)]
    marks = [m.start() for m in re.finditer(re.escape(END_MARK), data, re.S)]
    out = []
    for s in starts:
        k = bisect.bisect_left(marks, s)
        if k >= len(marks):
            continue
        m = marks[k]
        # проверка целостности: поле указывает ровно за маркер
        if struct.unpack_from('<H', data, m + 3)[0] != (m - s) + 7:
            out.append({'code_at': s, 'broken': True, 'code': data[s:m],
                        'strings': [], 'name': None})
            continue
        p = m + 5
        strings = []
        while True:
            e = data.find(b'\x00', p)
            if e < 0 or e == p or e - p > 200:
                break
            raw = data[p:e]
            # Перед именами-идентификаторами стоит двухбайтовый хеш, перед
            # строковыми литералами — нет. Отличаем по тому, становится ли
            # строка осмысленным идентификатором после отбрасывания двух байт.
            txt = raw.decode('latin1')
            if not _is_ident(txt) and len(raw) > 2 and _is_ident(txt[2:]):
                strings.append((p - s, raw[2:], True))
            else:
                strings.append((p - s, raw, False))
            p = e + 1
        out.append({'code_at': s, 'broken': False, 'code': data[s:m],
                    'strings': strings,
                    'name': strings[-1][1].decode('latin1') if strings else None})
    return out


def string_at(h: dict, target: int) -> str | None:
    """Строка таблицы по смещению; ссылка может указывать на хеш перед ней."""
    for off, s, hashed in h['strings']:
        # у имён цель указывает на хеш, сама строка лежит через два байта
        if off == target or (hashed and off + 2 == target):
            return s.decode('cp1251', 'replace')
    return None


def disasm(h: dict) -> list[str]:
    code = h['code']
    n = len(code)
    lines = []
    i = 0
    while i < n:
        op = code[i]
        ln = OPERAND_LEN.get(op)
        if ln is None or i + 1 + ln > n:
            lines.append(f'{i:5}: {op:02x} ??? (дальше не разобрано)')
            lines.append(f'       хвост: {code[i:].hex()}')
            break
        text = f'{op:02x}'
        if ln == 1:
            text += f' {code[i + 1]}'
        elif ln == 2:
            v = struct.unpack_from('<H', code, i + 1)[0]
            sv = struct.unpack_from('<h', code, i + 1)[0]
            if op in REF_OPS:
                s = string_at(h, i + 3 + v)
                text += f' -> {s!r}' if s is not None else f' -> @{i + 3 + v}'
            elif op in LOCAL_OPS:
                text += f' слот {sv}'
            else:
                text += f' {v}'
        elif ln == 4:
            text += f' {struct.unpack_from("<i", code, i + 1)[0]}'
        lines.append(f'{i:5}: {text}')
        i += 1 + ln
    return lines


def cmd_handlers(args):
    data = read_book(args.book)
    hs = find_handlers(data)
    good = [h for h in hs if not h['broken']]
    print(f'обработчиков: {len(hs)}, с целой таблицей строк: {len(good)}')
    from collections import Counter
    names = Counter(h['name'] for h in good)
    print('\nимена обработчиков:')
    for k, v in names.most_common():
        if k and k.isascii() and k.isidentifier():
            print(f'  {k:<24} {v}')
    if args.verbose:
        print()
        for h in good[:args.limit]:
            ss = ', '.join(s.decode('cp1251', 'replace') for _, s, _h in h['strings'])
            print(f"@{h['code_at']:#x} {h['name']}: {ss}")


def cmd_dis(args):
    data = read_book(args.book)
    hs = [h for h in find_handlers(data) if not h['broken']]
    shown = 0
    for h in hs:
        if args.name and (h['name'] or '').lower() != args.name.lower():
            continue
        print(f"=== @{h['code_at']:#x}  обработчик {h['name']}  кода {len(h['code'])} байт")
        print('    строки:', [s.decode('cp1251', 'replace') for _, s, _h in h['strings']])
        for line in disasm(h):
            print('   ', line)
        shown += 1
        if shown >= args.limit:
            break


def cmd_names(args):
    """Какие сообщения и литералы встречаются в скриптах — это карта игры."""
    data = read_book(args.book)
    from collections import Counter
    msgs, lits = Counter(), Counter()
    for h in find_handlers(data):
        if h['broken']:
            continue
        for _, s, hashed in h['strings'][:-1]:
            t = s.decode('cp1251', 'replace')
            (msgs if hashed else lits)[t] += 1
    print('сообщения и идентификаторы:')
    for k, v in msgs.most_common(args.limit):
        print(f'  {k:<28} {v}')
    print('\nстроковые литералы (файлы, команды MCI, имена страниц):')
    for k, v in lits.most_common(args.limit):
        print(f'  {k:<40} {v}')


def cmd_keywords(args):
    """Имена OpenScript из среды выполнения: таблица лежит в MTB40CMP.DLL."""
    d = open(args.dll, 'rb').read()
    ident = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{1,31}\x00')
    # таблица начинается со служебных слов to/handle/end
    start = d.find(b'to\x00handle\x00end\x00if\x00')
    if start < 0:
        raise SystemExit('таблица имён не найдена')
    items, p = [], start
    while True:
        m = ident.match(d, p)
        if not m:
            break
        items.append(m.group()[:-1].decode())
        p = m.end()
    print(f'имён: {len(items)} (со смещения {start:#x})')
    for i, s in enumerate(items):
        print(f'  {i:4} {s}')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('handlers'); p.add_argument('book')
    p.add_argument('--verbose', action='store_true'); p.add_argument('--limit', type=int, default=20)
    p.set_defaults(fn=cmd_handlers)

    p = sub.add_parser('dis'); p.add_argument('book'); p.add_argument('--name')
    p.add_argument('--limit', type=int, default=3); p.set_defaults(fn=cmd_dis)

    p = sub.add_parser('names'); p.add_argument('book')
    p.add_argument('--limit', type=int, default=60); p.set_defaults(fn=cmd_names)

    p = sub.add_parser('keywords'); p.add_argument('dll'); p.set_defaults(fn=cmd_keywords)

    args = ap.parse_args()
    args.fn(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
