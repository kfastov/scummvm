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

* Код обработчика начинается парой ``26 0f`` (вход в обработчик и изменение
  стека). Дальнейшая часть пролога зависит от числа локальных переменных и
  аргументов: например, наряду с обычным ``0f f8 ff ...`` у ``buttonClick``
  кнопки ``Yes`` встречается ``0f f4 ff 07 ...``. Поэтому граница
  подтверждается полем ``strOff``, а не одним шаблоном пролога.

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

Диспетчер байт-кода найден в `MTB40RUN.EXE`: сегмент NE 34, чтение опкода по
смещению `0x3448`, таблица переходов `SS:0x0b88`. Поэтому длины ниже взяты из
настоящих обработчиков опкодов, а не подобраны по книге. Все 1805 обработчиков
с целой таблицей строк разбираются ровно до маркера. Смысл сложных операций
вызова объектов ещё восстанавливается; это по-прежнему дизассемблер, а не
готовый интерпретатор.

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

# Общий двухбайтовый префикс. Размер кадра и раскладка аргументов после него
# различаются, поэтому полный пролог нельзя использовать как сигнатуру.
HANDLER_PREFIX = bytes.fromhex('260f')
# Маркер конца кода и начала таблицы строк.
END_MARK = bytes.fromhex('271d66')

# Длины операндов из диспетчера MTB40RUN.EXE. В книге «Башни знаний» реально
# эта таблица разбирает 1805/1805 целых обработчиков.
OPERAND_LEN = {
    0x00: 2, 0x01: 2, 0x02: 2, 0x03: 2, 0x04: 1, 0x05: 2, 0x06: 4,
    0x07: 2, 0x08: 2, 0x09: 2, 0x0a: 2, 0x0b: 2, 0x0c: 2, 0x0d: 2,
    0x0f: 2, 0x10: 1, 0x12: 2, 0x13: 2, 0x14: 2, 0x15: 2,
    0x19: 0, 0x1a: 0, 0x1b: 0, 0x1e: 3, 0x1f: 3,
    0x20: 3, 0x21: 2, 0x22: 2, 0x23: 2, 0x24: 0, 0x25: 1,
    0x26: 0, 0x27: 0, 0x29: 1, 0x2a: 0, 0x2b: 1, 0x2c: 1,
    0x2d: 1, 0x2e: 1, 0x2f: 0, 0x30: 0, 0x31: 0, 0x32: 0,
    0x33: 1, 0x34: 1, 0x35: 1, 0x36: 1, 0x37: 1, 0x38: 1,
    0x39: 1, 0x3a: 3, 0x3b: 1,
    0x3c: 0, 0x3e: 4, 0x3f: 4, 0x40: 2, 0x41: 0, 0x42: 0,
    0x44: 2, 0x45: 2, 0x46: 2, 0x48: 0, 0x49: 0, 0x4a: 1,
    0x4b: 2, 0x53: 2, 0x56: 0, 0x59: 2, 0x64: 2, 0x68: 2,
    0x6c: 3, 0x6d: 5, 0x71: 2, 0x72: 1, 0x73: 0, 0x76: 3,
    0x77: 0,
}

# Как читается операнд: ссылка на таблицу строк или число.
REF_OPS = {0x07, 0x08, 0x6c}
LOCAL_OPS = {0x00, 0x01, 0x02, 0x03, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
             0x0f, 0x45, 0x46, 0x59, 0x68}

OP_NAME = {
    0x00: 'loadLocalU8', 0x01: 'loadLocalW', 0x02: 'loadLocalD',
    0x03: 'loadLocalQ', 0x04: 'pushU8', 0x05: 'pushU16',
    0x06: 'pushU32', 0x07: 'pushNearRef', 0x08: 'pushFarRef',
    0x09: 'pushLocalPtr', 0x0a: 'pushLocalFarPtr',
    0x0b: 'storeLocalU8', 0x0c: 'storeLocalW', 0x0d: 'storeLocalD',
    0x0f: 'adjustStack', 0x10: 'dupBytes', 0x12: 'jump',
    0x13: 'jumpIfTrueW', 0x14: 'jumpIfFalseW',
    0x15: 'jumpIfFalseD', 0x19: 'restoreCodePtr',
    0x21: 'callBuiltinVoid', 0x22: 'callBuiltinW',
    0x23: 'callBuiltinD', 0x26: 'enterHandler',
}

# Опкод 0x23 берёт номер класса/встроенного имени. Номера свои, с таблицей
# имён компилятора не совпадают (ledger/0024); значения ниже выведены по тому,
# какая строка лежит на стеке перед инструкцией, на сотнях обработчиков.
CLASS_ID = {
    177: 'background',   # consumes a background name from bytecode
    180: 'page',         # consumes a page name from bytecode
    482: 'callMCI',      # play mySound, close mySound wait, stop myMIDI wait
}


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
    starts = [m.start() for m in re.finditer(re.escape(HANDLER_PREFIX), data, re.S)]
    marks = [m.start() for m in re.finditer(re.escape(END_MARK), data, re.S)]
    out = []
    for s in starts:
        k = bisect.bisect_left(marks, s)
        if k >= len(marks):
            continue
        m = marks[k]
        # проверка целостности: поле указывает ровно за маркер
        if m + 5 > len(data) or struct.unpack_from('<H', data, m + 3)[0] != (m - s) + 7:
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
        text = f'{op:02x} {OP_NAME.get(op, "")}'.rstrip()
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
        elif ln == 3:
            a = code[i + 1]
            v = struct.unpack_from('<H', code, i + 2)[0]
            if op == 0x3a:
                s = string_at(h, i + 4 + v)
                text += f' {a} -> {s!r}' if s is not None else f' {a} -> @{i + 4 + v}'
            elif op == 0x6c:
                # У 6c сначала относительная ссылка, затем однобайтовый флаг.
                v = struct.unpack_from('<H', code, i + 1)[0]
                s = string_at(h, i + 3 + v)
                text += f' -> {s!r} {code[i + 3]}' if s is not None else \
                        f' -> @{i + 3 + v} {code[i + 3]}'
            else:
                text += f' {a} {v}'
        elif ln == 5:
            rel = struct.unpack_from('<H', code, i + 1)[0]
            s = string_at(h, i + 3 + rel)
            text += f' -> {s!r}' if s is not None else f' -> @{i + 3 + rel}'
            text += f' {code[i + 3]} {struct.unpack_from("<H", code, i + 4)[0]}'
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
