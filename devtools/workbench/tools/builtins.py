#!/usr/bin/env python3
"""Чем занят встроенный номер OpenScript: адрес обёртки и что она зовёт.

Каждый разбор нового барьера начинался одинаково — найти обёртку builtin'а и
посмотреть, в какую функцию среды она уходит. Имя функции решало почти всё:
так опознались `ValueNewArray` (0081), `ValueArraySet` (0082) и
`TbkTimerStartNew` (0084). Инструмент делает ровно этот шаг сразу для списка
номеров.

Обёртки лежат в `MTB40RUN.EXE` по таблице `seg34:0x1bfa` (запись — дальний
указатель на номер, ledger/0054). Дальше обёртка дизассемблируется до первого
дальнего вызова с именем: либо импорт по ординалу (`MTB40BAS.209`), и тогда имя
берётся из таблицы экспорта нужной библиотеки, либо внутренний сегмент — тогда
печатается он.

    tools/builtins.py 486 487 316         — что это за номера
    tools/builtins.py --missing           — все номера, встречающиеся в книге
                                            (список подаётся на stdin, по числу
                                            в строке)

Библиотеки ищутся рядом с `MTB40RUN.EXE`.
"""
from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ne16 import NE, BUILTIN_SEG, BUILTIN_TABLE
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
import struct

RUNTIME = 'games/bashnya/RUNTIME/MTB40RUN.EXE'
SCAN = 160          # сколько байт обёртки смотреть до первого именованного вызова

# Проверка типа аргумента стоит в начале почти каждой обёртки и уводит в
# сообщение об ошибке. Если брать первый попавшийся именованный вызов, ответом
# сплошь будет `CDBSetPlErr` — то есть «как оно ругается», а не «что оно делает».
SKIP = {('MTB40BAS', 161)}


def exports_of(directory: pathlib.Path, module: str):
    """Таблица экспорта библиотеки среды; пусто, если файла рядом нет."""
    for name in (module + '.DLL', module + '.EXE'):
        path = directory / name
        if path.exists():
            try:
                return NE(str(path)).exports()
            except Exception:
                return {}
    return {}


def describe(ne: NE, directory: pathlib.Path, cache: dict, number: int) -> str:
    entry = BUILTIN_TABLE + number * 4
    base, _, _ = ne.seg(BUILTIN_SEG)
    relocs = ne.relocs(BUILTIN_SEG)
    target = relocs.get(entry) or relocs.get(entry + 2)
    if not target:
        return 'builtin %-4d — в таблице нет' % number
    offset = struct.unpack_from('<H', ne.d, base + entry)[0]
    segment = int(target[1].split(':')[0][3:])
    where = 'RUN%d:%04x' % (segment, offset)

    # Первый дальний вызов с разрешимым именем и есть «что оно делает».
    code, size, _ = ne.seg(segment)
    inner = ne.relocs(segment)
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    for insn in md.disasm(ne.d[code + offset:code + offset + SCAN], offset):
        if insn.mnemonic != 'lcall':
            continue
        mark = None
        for at in range(insn.address, insn.address + insn.size):
            if at in inner:
                mark = inner[at][1]
        if not mark:
            continue
        if '.' not in mark:                       # внутренний сегмент — имени нет
            return '%-14s -> %s' % (where, mark)
        module, ordinal = mark.split('.', 1)
        if not ordinal.isdigit():                 # импорт уже по имени
            return '%-14s -> %s' % (where, mark)
        if (module, int(ordinal)) in SKIP:
            continue
        if module not in cache:
            cache[module] = exports_of(directory, module)
        found = cache[module].get(int(ordinal))
        name = found[1] if found and found[1] else '?'
        return '%-14s -> %s.%s  %s' % (where, module, ordinal, name)
    return '%-14s -> (именованного вызова в первых %d байтах нет)' % (where, SCAN)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    numbers = [int(a, 0) for a in args]
    if '--missing' in sys.argv or not numbers:
        numbers += [int(line.split()[-1]) for line in sys.stdin
                    if line.strip() and line.split()[-1].isdigit()]
    runtime = pathlib.Path(RUNTIME)
    ne, cache = NE(str(runtime)), {}
    for number in numbers:
        print('  builtin %-4d %s' % (number, describe(ne, runtime.parent, cache, number)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
