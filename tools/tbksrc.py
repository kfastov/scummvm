#!/usr/bin/env python3
"""Исходный текст скриптов, который лежит в книге ToolBook.

Книга хранит рядом с байт-кодом **исходник обработчиков — простым текстом в
CP1251**. Никакого сжатия нет: прежняя версия этого инструмента (ledger/0060)
принимала русские буквы за управляющие байты — `0xC0..0xCF` это «А»..«П», а
`0xE0` это «а», — и разбирала их как обратные ссылки и конец строки, отчего
текст выглядел изъеденным. Отменено записью ledger/0064.

    tools/tbksrc.py find "подстрока"     — где встречается (в CP1251)
    tools/tbksrc.py at 0xf3b400 600      — показать кусок как текст
    tools/tbksrc.py handlers             — заголовки `to handle` / `to get`
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


def text(book: bytes, start: int, count: int) -> str:
    """Кусок книги как текст CP1251."""
    return book[start:start + count].decode('cp1251', errors='replace')


def find(book: bytes, needle: str) -> list[int]:
    return [m.start() for m in re.finditer(re.escape(needle.encode('cp1251')), book)]


def handlers(book: bytes) -> list[tuple[int, str]]:
    """Заголовки обработчиков в исходниках."""
    out = []
    for m in re.finditer(rb'to (handle|get) [A-Za-z_][A-Za-z0-9_]*', book):
        out.append((m.start(), m.group().decode('latin1')))
    return out


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    book = load()
    cmd = sys.argv[1]
    if cmd == 'find':
        for off in find(book, sys.argv[2]):
            print('0x%06x  %s' % (off, text(book, max(0, off - 60), 200).replace('\n', ' ')))
    elif cmd == 'at':
        start = int(sys.argv[2], 0)
        count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 400
        print(text(book, start, count))
    elif cmd == 'handlers':
        for off, head in handlers(book):
            print('0x%06x  %s' % (off, head))
    else:
        raise SystemExit(__doc__)
    return 0


if __name__ == '__main__':
    sys.exit(main())
