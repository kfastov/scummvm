#!/usr/bin/env python3
"""Разборщик книги Asymetrix ToolBook (.TBK) — «Башня знаний».

Что установлено про формат (каждый пункт проверен на самой книге, см. ledger/0015):

* Книга начинается сигнатурой ``\\x03JBO``. В игре она зашита внутрь
  ``KNTOWER.EXE`` со смещения ``0x29e0``; отдельный ``RESOURCE.TBK`` — маленькая
  ресурсная книга и содержимого игры не несёт.

* **Записи.** Заголовок ``2a 00 01 02 <тип> 00 <размер32>``, дальше два
  четырёхбайтовых поля-связки, вложенный заголовок ``01 02 <тип> 00 <размер32>``
  и данные. Всего 289 записей, 227 из них типа ``0x01``; в 69 записях сразу за
  вложенным заголовком лежит готовый Windows-битмап (сигнатура ``BM``).

* **Сегменты.** Двухбайтовое значение перед заголовком записи — смещение самой
  записи от начала сегмента. Отсюда база сегмента вычисляется вычитанием, и
  записи разбиваются на 25 сегментов. Внутрисегментные ссылки шестнадцатибитные —
  наследство 16-битного адресного пространства.

* **Имена.** Строка с нуль-терминатором, перед ней — четырёхбайтовое смещение
  этой строки внутри сегмента (с точностью до выравнивания). Так записаны и
  имена сегментов, и имена классов, и свойства объектов.

* **Страницы.** Якорь — свойство ``ASYM_TpID``; за ним в пределах ~150 байт
  лежит полноэкранный DIB 640×480 — фон страницы, а перед ним имена страницы и
  её обработчиков. Якорей 51, полноэкранных фонов 58.

* **Картинки.** В книге 2484 заголовка DIB (``BITMAPINFOHEADER``), из них 2320
  восьмибитных. Свой декодер не нужен — это обычные DIB.

* **Текст** — в CP1251, лежит внутри страничных записей открытым текстом.

* **Скрипты скомпилированы.** Строковые операнды вкраплены в поток команд
  группами; группе часто предшествует ``27 1d 66``. Опкоды не разобраны.

Опровергнуто по ходу: «617 именованных сегментов» — артефакт поиска по
регулярному выражению ``\\*[A-Za-z0-9_]{2,20}\\*`` по всему файлу: пиксельные
данные дают тысячи ложных совпадений вида ``*NN*``. Настоящих именованных
сегментов двенадцать.

Использование:

    tbk.py extract <exe> [--offset 0x29e0]  — вынуть книгу из .EXE
    tbk.py map <книга>                      — общая карта: записи, сегменты, картинки
    tbk.py segments <книга>                 — именованные сегменты и классы
    tbk.py records <книга>                  — записи с типами и размерами
    tbk.py pages <книга>                    — страницы: имя, фон, обработчики
    tbk.py images <книга> <каталог> [--min-w 64]  — выгрузка DIB в .bmp
    tbk.py text <книга> [--min 20]          — текст CP1251
    tbk.py scripts <книга> [--limit 20]     — группы строковых операндов скриптов
    tbk.py objects <книга>                  — объекты страниц с прямоугольниками
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys

BOOK_SIGNATURE = b"\x03JBO"
REC_TAG = b"\x2a\x00\x01\x02"
PAGE_ANCHOR = b"ASYM_TpID\x00"

# Имена сегментов ToolBook. Ищем только их, а не всё подряд «между звёздочек»:
# по всему файлу шаблон со звёздочками даёт тысячи ложных совпадений в пикселях.
SEGMENT_NAMES = [
    "*ClassTbl*", "*ClassEntry*", "*PTABLE*", "*WINDOWSEG*", "*ICONRESTAB*",
    "*ICONRESSEG*", "*ICONRES*", "*OBJTABLE*", "*IDTABLE*", "*NAMETAB*",
    "*RHOTWORD*", "*TbxBase*",
]

IDENT = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{2,30}\x00")
VOWELS = set("aeiouyAEIOUY")

DIB_DEPTHS = (1, 4, 8, 16, 24, 32)


def read_book(path: str) -> bytes:
    data = open(path, "rb").read()
    if not data.startswith(BOOK_SIGNATURE):
        print(f"предупреждение: нет сигнатуры {BOOK_SIGNATURE!r}, "
              f"файл начинается с {data[:4]!r}", file=sys.stderr)
    return data


# ------------------------------------------------------------------ разбор


def find_records(data: bytes) -> list[dict]:
    """Записи книги. Заголовок 2a 00 01 02 <тип> 00 <размер32>."""
    out, i, n = [], 0, len(data)
    while i < n - 10:
        if data[i:i + 4] == REC_TAG and data[i + 5] == 0:
            rtype = data[i + 4]
            size = struct.unpack_from("<I", data, i + 6)[0]
            if 0 < size < min(n - i, 0x400000):
                own = struct.unpack_from("<H", data, i - 2)[0] if i >= 2 else 0
                body = i + 10
                out.append({
                    "offset": i, "type": rtype, "size": size,
                    "own": own, "base": i - own,
                    "body": body,
                    "bitmap": data[body + 16:body + 18] == b"BM",
                })
                i += 10 + size
                continue
        i += 1
    return out


def group_segments(records: list[dict]) -> list[dict]:
    """Соседние записи с общей базой — один сегмент."""
    segs = []
    for r in records:
        if segs and segs[-1]["base"] == r["base"]:
            segs[-1]["records"].append(r)
        else:
            segs.append({"base": r["base"], "records": [r]})
    return segs


def find_dibs(data: bytes) -> list[dict]:
    """Заголовки BITMAPINFOHEADER с правдоподобными полями."""
    out, i = [], 0
    while True:
        i = data.find(b"\x28\x00\x00\x00", i)
        if i < 0:
            break
        try:
            size, w, h, planes, bpp, comp = struct.unpack_from("<IiiHHI", data, i)
        except struct.error:
            break
        if (size == 40 and planes == 1 and bpp in DIB_DEPTHS
                and 0 < w <= 2048 and 0 < abs(h) <= 2048 and comp in (0, 1, 2)):
            colors = struct.unpack_from("<I", data, i + 32)[0]
            if colors == 0 and bpp <= 8:
                colors = 1 << bpp
            stride = ((w * bpp + 31) // 32) * 4
            out.append({
                "offset": i, "w": w, "h": abs(h), "bpp": bpp, "comp": comp,
                "colors": colors, "palette": i + 40,
                "pixels": i + 40 + colors * 4,
                "bytes": stride * abs(h),
            })
            i += 40
        else:
            i += 1
    return out


def identifiers(data: bytes, lo: int, hi: int) -> list[str]:
    """Осмысленные идентификаторы в куске. Фильтр по гласным отсеивает
    пиксельный мусор, который формально проходит как строка с нулём."""
    out = []
    for m in IDENT.finditer(data[lo:hi]):
        s = m.group()[:-1].decode("latin1")
        if any(c in VOWELS for c in s) and len(set(s)) > 2:
            out.append(s)
    return out


def find_pages(data: bytes, dibs: list[dict]) -> list[dict]:
    """Страницы книги: якорь ASYM_TpID, следом фон, перед ним — имена."""
    pages, i = [], 0
    anchors = []
    while True:
        i = data.find(PAGE_ANCHOR, i)
        if i < 0:
            break
        anchors.append(i)
        i += 1

    for a in anchors:
        # Фон — ближайший следующий DIB; полноэкранный ищем в пределах 4 КБ.
        bg = None
        for d in dibs:
            if d["offset"] <= a:
                continue
            if d["offset"] - a > 8192:
                break
            if bg is None or (d["w"] >= 320 and d["h"] >= 240 and bg["w"] < 320):
                bg = d
            if d["w"] >= 600 and d["h"] >= 440:
                bg = d
                break
        names = identifiers(data, max(0, a - 600), a)
        pages.append({"anchor": a, "background": bg, "names": names})
    return pages


def find_string_groups(data: bytes, limit: int = 0) -> list[dict]:
    """Группы строковых операндов в скомпилированных скриптах.

    Границы ищутся по содержимому: подряд идущие осмысленные строки с нулями.
    Маркер `27 1d 66` предваряет лишь часть групп, резать по нему нельзя —
    проверено, операнды вроде `wav\\click.wav` идут без него.
    """
    groups, i, n = [], 0, len(data)
    while i < n:
        m = IDENT.search(data, i)
        if not m:
            break
        start = m.start()
        items, p = [], start
        while True:
            mm = IDENT.match(data, p)
            if not mm:
                break
            s = mm.group()[:-1].decode("latin1")
            if not any(c in VOWELS for c in s) or len(set(s)) <= 2:
                break
            items.append(s)
            p = mm.end()
        if len(items) >= 3:
            groups.append({"offset": start, "items": items,
                           "marker": data[start - 5:start - 2] == b"\x27\x1d\x66"})
            if limit and len(groups) >= limit:
                break
        i = max(p, start + 1)
    return groups


# ------------------------------------------------------------------ команды


def cmd_extract(args):
    data = open(args.book, "rb").read()
    off = args.offset
    if data[off:off + 4] != BOOK_SIGNATURE:
        raise SystemExit(f"по смещению {off:#x} нет сигнатуры книги")
    dst = args.out or os.path.splitext(args.book)[0] + ".tbk"
    open(dst, "wb").write(data[off:])
    print(f"{dst}: {len(data) - off} байт")


def cmd_map(args):
    data = read_book(args.book)
    recs = find_records(data)
    segs = group_segments(recs)
    dibs = find_dibs(data)
    pages = find_pages(data, dibs)
    types = {}
    for r in recs:
        types.setdefault(r["type"], 0)
        types[r["type"]] += 1

    print(f"размер книги: {len(data)} байт")
    print(f"записей: {len(recs)}, из них с битмапом: {sum(r['bitmap'] for r in recs)}")
    print(f"  типы: {', '.join(f'0x{t:02x}×{c}' for t, c in sorted(types.items()))}")
    print(f"сегментов (по общей базе): {len(segs)}")
    print(f"заголовков DIB: {len(dibs)}"
          f", полноэкранных 640×480: {sum(1 for d in dibs if d['w'] == 640 and d['h'] == 480)}")
    depth = {}
    for d in dibs:
        depth[d["bpp"]] = depth.get(d["bpp"], 0) + 1
    print(f"  глубины: {', '.join(f'{k} бит×{v}' for k, v in sorted(depth.items()))}")
    print(f"страниц (якорь ASYM_TpID): {len(pages)}")
    named = [n for n in SEGMENT_NAMES if data.find(n.encode()) >= 0]
    print(f"именованных сегментов найдено: {len(named)} из {len(SEGMENT_NAMES)}")


def cmd_segments(args):
    data = read_book(args.book)
    print("именованные сегменты:")
    for name in SEGMENT_NAMES:
        pos = data.find(name.encode())
        if pos < 0:
            print(f"  {name:<14} не найден")
            continue
        val = struct.unpack_from("<I", data, pos - 4)[0]
        print(f"  {name:<14} имя @0x{pos:07x}, поле перед именем 0x{val:x}")

    # Классы объектов лежат рядом, в той же таблице имён.
    lo = min((data.find(n.encode()) for n in SEGMENT_NAMES if data.find(n.encode()) >= 0), default=0)
    print("\nклассы объектов рядом с таблицей:")
    for s in identifiers(data, lo, lo + 0x1000):
        if s[0].isupper() and len(s) > 3:
            print(f"  {s}")


def cmd_records(args):
    data = read_book(args.book)
    recs = find_records(data)
    segs = group_segments(recs)
    for seg in segs:
        print(f"сегмент, база 0x{seg['base']:07x}, записей {len(seg['records'])}")
        for r in seg["records"][:args.limit]:
            kind = "битмап" if r["bitmap"] else "данные"
            print(f"   @0x{r['offset']:07x} тип 0x{r['type']:02x} размер 0x{r['size']:<6x} "
                  f"смещение в сегменте 0x{r['own']:04x}  {kind}")
        if len(seg["records"]) > args.limit:
            print(f"   … ещё {len(seg['records']) - args.limit}")


def cmd_pages(args):
    data = read_book(args.book)
    dibs = find_dibs(data)
    pages = find_pages(data, dibs)
    print(f"страниц: {len(pages)}\n")
    for p in pages:
        bg = p["background"]
        bgs = f"{bg['w']}×{bg['h']} {bg['bpp']} бит @0x{bg['offset']:07x}" if bg else "нет"
        # Имя страницы — последнее «содержательное» имя перед якорем.
        names = [n for n in p["names"] if n not in ("true", "false", "script", "ASYM_BeenHere")]
        print(f"@0x{p['anchor']:07x}  фон: {bgs}")
        print(f"    имена: {', '.join(names[-8:]) if names else '—'}")


def cmd_images(args):
    data = read_book(args.book)
    dibs = find_dibs(data)
    os.makedirs(args.outdir, exist_ok=True)
    written = 0
    for k, d in enumerate(dibs):
        if d["w"] < args.min_w or d["h"] < args.min_h:
            continue
        end = d["pixels"] + d["bytes"]
        if end > len(data):
            continue
        # Собираем файл BMP: заголовок + DIB как есть.
        dib = data[d["offset"]:end]
        head = struct.pack("<2sIHHI", b"BM", 14 + len(dib), 0, 0, 14 + 40 + d["colors"] * 4)
        name = f"{k:04d}_{d['w']}x{d['h']}_{d['bpp']}bit_0x{d['offset']:07x}.bmp"
        open(os.path.join(args.outdir, name), "wb").write(head + dib)
        written += 1
        if args.limit and written >= args.limit:
            break
    print(f"записано {written} файлов в {args.outdir} (всего DIB {len(dibs)})")


def cmd_text(args):
    data = read_book(args.book)
    # Русский текст в CP1251: байты 0xC0..0xFF плюс пробелы и знаки.
    rx = re.compile(rb"[\xc0-\xff][\xc0-\xff \-,.!?:;()\r\n0-9]{%d,}" % args.min)
    seen = set()
    for m in rx.finditer(data):
        s = m.group().decode("cp1251").strip()
        s = " ".join(s.split())
        if len(s) < args.min or s in seen:
            continue
        seen.add(s)
        print(f"0x{m.start():07x}: {s}")


def cmd_scripts(args):
    data = read_book(args.book)
    groups = find_string_groups(data, args.limit)
    print(f"групп строковых операндов: {len(groups)}"
          f" (с маркером 27 1d 66: {sum(g['marker'] for g in groups)})\n")
    for g in groups:
        print(f"@0x{g['offset']:07x} {'[27 1d 66] ' if g['marker'] else ''}{', '.join(g['items'])}")




# ------------------------------------------------------- объекты страниц

# Координаты в книге — в 1/1440 дюйма. Книга 640×480 точек при 96 точках на
# дюйм, значит 9600×7200 единиц, и **все координаты кратны 15**. Это и есть
# точный фильтр, отличающий настоящий прямоугольник от случайных байт.
UNIT = 15
PAGE_W, PAGE_H = 640, 480

OBJ_NAME = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{1,24}\x00")
FULLSCREEN_DIB = bytes.fromhex('280000008002000' + '0e0010000' + '01000800')


def find_objects(data: bytes) -> list[dict]:
    """Объекты книги: имя и прямоугольник в точках экрана.

    Запись объекта заканчивается на ``<u16 next><u32 self><имя\0>``; прямоугольник
    из четырёх ``u16`` лежит недалеко перед ней, но на разном расстоянии — оно
    зависит от класса объекта (у многоугольника между ними ещё число вершин).
    Поэтому прямоугольник ищется перебором назад с проверкой кратности 15.
    """
    out = []
    for m in OBJ_NAME.finditer(data):
        p = m.start()
        if p < 64:
            continue
        self_off = struct.unpack_from('<I', data, p - 4)[0]
        if not 0 < self_off < 0x10000:
            continue
        for back in range(6, 48):
            q = p - 4 - back
            l, t, r, b = struct.unpack_from('<4H', data, q)
            if l % UNIT or t % UNIT or r % UNIT or b % UNIT:
                continue
            if not (0 <= l < r <= PAGE_W * UNIT and 0 <= t < b <= PAGE_H * UNIT):
                continue
            if r - l < 2 * UNIT or b - t < 2 * UNIT:
                continue
            out.append({'offset': p, 'name': m.group()[:-1].decode('cp1251', 'replace'),
                        'rect': (l // UNIT, t // UNIT, r // UNIT, b // UNIT),
                        'rect_at': q})
            break
    return out


def find_backgrounds(data: bytes) -> list[int]:
    """Смещения полноэкранных фонов 640×480×8."""
    pat = struct.pack('<IiiHH', 40, PAGE_W, PAGE_H, 1, 8)
    return [m.start() for m in re.finditer(re.escape(pat), data)]


def cmd_objects(args):
    data = read_book(args.book)
    objs = find_objects(data)
    bgs = find_backgrounds(data)
    print(f'объектов с прямоугольником: {len(objs)}, полноэкранных фонов: {len(bgs)}')
    import bisect
    starts = [o['offset'] for o in objs]
    shown = 0
    for k, f in enumerate(bgs):
        nxt = bgs[k + 1] if k + 1 < len(bgs) else len(data)
        i, j = bisect.bisect_left(starts, f), bisect.bisect_left(starts, nxt)
        grp = objs[i:j]
        if args.min_objects and len(grp) < args.min_objects:
            continue
        print(f"\nфон @0x{f:07x}: объектов {len(grp)}")
        for o in grp[:args.limit]:
            l, t, r, b = o['rect']
            print(f"   {o['name']:<20} ({l:3},{t:3})-({r:3},{b:3})")
        shown += 1
        if args.pages and shown >= args.pages:
            break


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract"); p.add_argument("book"); p.add_argument("--offset", type=lambda s: int(s, 0), default=0x29e0); p.add_argument("--out")
    p.set_defaults(fn=cmd_extract)
    p = sub.add_parser("map"); p.add_argument("book"); p.set_defaults(fn=cmd_map)
    p = sub.add_parser("segments"); p.add_argument("book"); p.set_defaults(fn=cmd_segments)
    p = sub.add_parser("records"); p.add_argument("book"); p.add_argument("--limit", type=int, default=6); p.set_defaults(fn=cmd_records)
    p = sub.add_parser("pages"); p.add_argument("book"); p.set_defaults(fn=cmd_pages)
    p = sub.add_parser("images"); p.add_argument("book"); p.add_argument("outdir")
    p.add_argument("--min-w", type=int, default=32); p.add_argument("--min-h", type=int, default=32)
    p.add_argument("--limit", type=int, default=0); p.set_defaults(fn=cmd_images)
    p = sub.add_parser("text"); p.add_argument("book"); p.add_argument("--min", type=int, default=20); p.set_defaults(fn=cmd_text)
    p = sub.add_parser("scripts"); p.add_argument("book"); p.add_argument("--limit", type=int, default=40); p.set_defaults(fn=cmd_scripts)
    p = sub.add_parser("objects"); p.add_argument("book")
    p.add_argument("--limit", type=int, default=20); p.add_argument("--pages", type=int, default=6)
    p.add_argument("--min-objects", type=int, default=3); p.set_defaults(fn=cmd_objects)

    args = ap.parse_args()
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
