#!/usr/bin/env python3
"""Разжатие исходного текста скриптов, который лежит в книге ToolBook.

Книга хранит не только байт-код, но и исходник каждого обработчика — сжатый
обратными ссылками. Схема проверена на четырёх обработчиках подряд
(ledger/0060):

    байт 0xC0..0xCF  — ссылка: длина = (байт and 0x0f) + 3,
                       следующий байт = расстояние − 3;
                       **расстояние отсчитывается по сжатому потоку**, а не по
                       разжатому — копируются сырые байты с позиции i − dist;
    байт 0xE0        — конец строки;
    прочее           — как есть (текст в CP1251).

Байты 0x80..0xBF пока не расшифрованы: это, судя по всему, лексемы встроенных
слов языка. Литеральные имена (обработчиков, объектов) видны как есть.

    tools/tbksrc.py 0x134575          — разжать с этого смещения книги
    tools/tbksrc.py 0x134575 400      — столько байт входа
    tools/tbksrc.py --find            — где в книге лежат несжатые «TO HANDLE»
"""
from __future__ import annotations

import pathlib
import re
import sys

BOOK_IN_EXE = 0x29e0
DEFAULT_BOOK = 'games/bashnya/KNTOWER.EXE'


def load(path: str = DEFAULT_BOOK) -> bytes:
    data = pathlib.Path(path).read_bytes()
    return data[BOOK_IN_EXE:] if data[:2] == b'MZ' else data


def unpack(book: bytes, start: int, count: int) -> bytes:
    """Разжать `count` байт входа, начиная со `start`."""
    out = bytearray()
    i, end = start, min(start + count, len(book))
    while i < end:
        b = book[i]
        if 0xc0 <= b <= 0xcf and i + 1 < len(book):
            length = (b & 0x0f) + 3
            source = i - (book[i + 1] + 3)
            if source >= 0:
                out += book[source:source + length]
                i += 2
                continue
        out.append(b)
        i += 1
    return bytes(out)


def render(raw: bytes) -> str:
    """Показать разжатое: конец строки — 0xE0, неизвестные лексемы — <XX>."""
    parts = []
    for b in raw:
        if b == 0xe0:
            parts.append('\n')
        elif b in (0x09, 0x20) or 32 <= b < 127:
            parts.append(chr(b))
        elif 0x80 <= b <= 0xbf:
            parts.append('<%02x>' % b)
        else:
            parts.append(bytes([b]).decode('cp1251', errors='replace'))
    return ''.join(parts)


def main() -> int:
    book = load()
    if len(sys.argv) > 1 and sys.argv[1] == '--find':
        for m in re.finditer(rb'TO HANDLE ', book):
            print('0x%06x' % m.start())
        return 0
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    start = int(sys.argv[1], 0)
    count = int(sys.argv[2], 0) if len(sys.argv) > 2 else 300
    print(render(unpack(book, start, count)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
