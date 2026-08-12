#!/usr/bin/env python3
"""Разборщик книги Asymetrix ToolBook (формат .TBK) — «Башня знаний».

Что известно о формате на сегодня (подробности в NOTES.md):

* Книга начинается сигнатурой ``\\x03JBO``. В игре она зашита внутрь
  ``KNTOWER.EXE`` со смещения ``0x29e0``, отдельный ``RESOURCE.TBK`` — лишь
  ресурсная книга и почти не содержит кода.
* Ближе к началу лежит каталог именованных сегментов вида ``*OBJTABLE*``,
  ``*NAMETAB*``, ``*ClassTbl*``; в игровой книге таких имён 617.
* Записи разделены маркером ``2a 00 01 02 01 00``.
* Ресурсы хранятся готовыми Windows-битмапами: сразу за маркером встречается
  сигнатура ``BM``, и такие куски вынимаются как есть.
* Скрипты хранятся скомпилированными: сначала таблица идентификаторов строками
  с нуль-терминаторами, следом байт-код. Текстовых форм OpenScript в книге нет.

Использование:

    tbk.py extract <книга> [--offset 0x29e0]   — вынуть книгу из .EXE
    tbk.py segments <книга>                    — каталог именованных сегментов
    tbk.py records <книга>                     — записи по маркеру
    tbk.py bitmaps <книга> <каталог>           — извлечь ресурсы-битмапы
    tbk.py strings <книга> [--min 4]           — таблицы идентификаторов
"""

import os
import re
import struct
import sys

BOOK_SIGNATURE = b"\x03JBO"
RECORD_MARKER = b"\x2a\x00\x01\x02\x01\x00"
SEGMENT_NAME = re.compile(rb"\*[A-Za-z0-9_]{2,20}\*")
# Идентификаторы в таблицах имён: печатаемая ASCII-строка с нуль-терминатором.
IDENTIFIER = re.compile(rb"[A-Za-z_][A-Za-z0-9_ ]{2,63}\x00")

BMP_HEADER_SIZES = (12, 40, 64, 108, 124)
BMP_DEPTHS = (1, 4, 8, 16, 24, 32)


def read_book(path):
    data = open(path, "rb").read()
    if not data.startswith(BOOK_SIGNATURE):
        print(f"предупреждение: нет сигнатуры {BOOK_SIGNATURE!r}, "
              f"файл начинается с {data[:4]!r}", file=sys.stderr)
    return data


def cmd_extract(args):
    """Вынуть книгу из исполняемого файла: она просто лежит там куском."""
    src = args[0]
    offset = int(args[1], 0) if len(args) > 1 else 0x29e0
    data = open(src, "rb").read()
    if data[offset:offset + 4] != BOOK_SIGNATURE:
        raise SystemExit(f"по смещению {offset:#x} нет сигнатуры книги")
    dst = os.path.splitext(src)[0] + ".tbk"
    open(dst, "wb").write(data[offset:])
    print(f"{dst}: {len(data) - offset} байт")


def cmd_segments(args):
    data = read_book(args[0])
    names = sorted((m.start(), m.group().decode("latin1"))
                   for m in SEGMENT_NAME.finditer(data))
    print(f"именованных сегментов: {len(names)}")
    for off, name in names:
        # Перед именем лежит четырёхбайтовое возрастающее число — похоже на
        # смещение внутри каталога; печатаем его для дальнейшего разбора.
        tag = struct.unpack_from("<I", data, off - 4)[0] if off >= 4 else 0
        print(f"  {off:#010x}  {tag:#08x}  {name}")


def find_records(data):
    return [m.start() for m in re.finditer(re.escape(RECORD_MARKER), data)]


def cmd_records(args):
    data = read_book(args[0])
    offsets = find_records(data)
    print(f"записей по маркеру {RECORD_MARKER.hex(' ')}: {len(offsets)}")
    for i, off in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
        payload = data[off:off + 64]
        kind = "bitmap" if b"BM" in payload else "?"
        print(f"  {off:#010x}  длина {end - off:8d}  {kind}")


def valid_bitmaps(data):
    """Найти куски, которые действительно являются Windows-битмапами.

    Одной сигнатуры `BM` мало: она встречается в данных случайно. Проверяем
    заголовок целиком — резервные поля, размер, разумные габариты и глубину.
    """
    found = []
    for m in re.finditer(rb"BM", data):
        off = m.start()
        if off + 54 > len(data):
            continue
        size, res1, res2, _ = struct.unpack_from("<IHHI", data, off + 2)
        header = struct.unpack_from("<I", data, off + 14)[0]
        if res1 or res2 or header not in BMP_HEADER_SIZES:
            continue
        if not (100 < size < 20_000_000) or off + size > len(data):
            continue
        width, height, planes, depth = struct.unpack_from("<iiHH", data, off + 18)
        if planes != 1 or depth not in BMP_DEPTHS:
            continue
        if not (0 < width <= 4096 and 0 < abs(height) <= 4096):
            continue
        found.append((off, size, width, abs(height), depth))
    return found


def cmd_bitmaps(args):
    data = read_book(args[0])
    outdir = args[1] if len(args) > 1 else "bitmaps"
    os.makedirs(outdir, exist_ok=True)
    found = valid_bitmaps(data)
    for i, (off, size, w, h, depth) in enumerate(found):
        name = f"{i:04d}_{off:08x}_{w}x{h}_{depth}bit.bmp"
        open(os.path.join(outdir, name), "wb").write(data[off:off + size])
    print(f"извлечено битмапов: {len(found)} -> {outdir}")
    for off, size, w, h, depth in found[:10]:
        print(f"  {off:#010x}: {w}x{h}, {depth} бит, {size} байт")


def looks_like_identifier(name):
    """Отсеять пиксельный мусор, похожий на строку.

    Данные битмапов дают длинные цепочки повторяющихся символов («dddddd…»),
    которые проходят по формальному признаку «печатаемая ASCII с нулём».
    Настоящее имя разнообразнее по составу и содержит гласные.
    """
    if len(set(name)) < 4:
        return False
    letters = [c for c in name.lower() if c.isalpha()]
    if not letters:
        return False
    if not set("aeiouy") & set(letters):
        return False
    # доля самого частого символа — у мусора она близка к единице
    return max(name.count(c) for c in set(name)) / len(name) < 0.5


def cmd_strings(args):
    data = read_book(args[0])
    minlen = int(args[1]) if len(args) > 1 else 4
    offsets = find_records(data)
    bitmap_starts = {off for off, *_ in valid_bitmaps(data)}
    total = 0
    for i, off in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
        # Записи с ресурсом пропускаем целиком: там только пиксели.
        if any(off <= b < end for b in bitmap_starts):
            continue
        names = [m.group()[:-1].decode("latin1")
                 for m in IDENTIFIER.finditer(data, off, end)]
        names = [n for n in names if len(n) >= minlen and looks_like_identifier(n)]
        if not names:
            continue
        total += len(names)
        print(f"{off:#010x} ({len(names)}): {', '.join(names[:12])}"
              + (" …" if len(names) > 12 else ""))
    print(f"\nвсего идентификаторов: {total}")


STRING_GROUP_MARKER = b"\x27\x1d\x66"


def split_record(rec):
    """Разрезать запись на куски кода и строковые операнды.

    Строки не собраны в отдельную таблицу — они вкраплены в поток команд.
    Резать по маркеру ``27 1d 66`` не получается: он предваряет лишь часть
    групп, а такие операнды, как ``workPath`` или ``openPopka``, идут без него.
    Поэтому границы ищем по содержимому: строка — это последовательность
    печатаемых символов с нуль-терминатором, достаточно длинная и похожая на
    осмысленную (иначе в неё попадают пиксели и случайные байты).

    Возвращает список пар ``(вид, данные)``, где вид — ``"code"`` или
    ``"string"``.
    """
    parts = []
    code_start = 0
    pos = 0
    while pos < len(rec):
        if 32 <= rec[pos] < 127:
            end = pos
            while end < len(rec) and 32 <= rec[end] < 127:
                end += 1
            token = rec[pos:end]
            if end < len(rec) and rec[end] == 0 and len(token) >= 3 \
                    and looks_like_identifier(token.decode("latin1")):
                if pos > code_start:
                    parts.append(("code", rec[code_start:pos]))
                parts.append(("string", token.decode("latin1")))
                pos = end + 1
                code_start = pos
                continue
            pos = end
            continue
        pos += 1
    if code_start < len(rec):
        parts.append(("code", rec[code_start:]))
    return parts


def cmd_disasm(args):
    """Показать строение одной записи: код и строковые операнды вперемешку."""
    data = read_book(args[0])
    start = int(args[1], 0)
    offsets = find_records(data)
    later = [o for o in offsets if o > start]
    end = later[0] if later else len(data)
    rec = data[start:end]

    print(f"запись {start:#010x}..{end:#010x}, {len(rec)} байт\n")
    for kind, payload in split_record(rec):
        if kind == "string":
            print(f"  строка: {payload!r}")
        else:
            # Операнды кадра стека: отрицательные, кратные четырём.
            slots = [struct.unpack_from("<h", payload, i)[0]
                     for i in range(len(payload) - 1)
                     if payload[i + 1] == 0xff and payload[i] >= 0x80]
            note = f"  слоты кадра: {sorted(set(slots))[:8]}" if slots else ""
            print(f"  код, {len(payload)} байт: {payload[:24].hex(' ')}"
                  + ("…" if len(payload) > 24 else ""))
            if note:
                print(note)


COMMANDS = {
    "extract": cmd_extract,
    "segments": cmd_segments,
    "records": cmd_records,
    "bitmaps": cmd_bitmaps,
    "strings": cmd_strings,
    "disasm": cmd_disasm,
}


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in COMMANDS:
        raise SystemExit(__doc__)
    COMMANDS[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    main()
