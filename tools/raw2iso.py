#!/usr/bin/env python3
"""Конвертер raw CD-образов (2352 байта на сектор) в plain ISO 9660 (2048).

MDF от Alcohol/Daemon Tools и часть «ISO» с трекеров на самом деле хранят сырые
сектора вместе с sync/header/ECC. hdiutil такое не монтирует. Скрипт определяет
режим сектора по байту 15 заголовка и вырезает пользовательские 2048 байт:

    Mode 1        : 16 байт sync+header, 2048 данных, 288 ECC/EDC
    Mode 2 Form 1 : 16 байт sync+header + 8 байт subheader, 2048 данных, 280 ECC

Использование: raw2iso.py <вход.mdf|iso> <выход.iso>
"""

import sys
import os

RAW = 2352
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def detect_offset(first_sector: bytes) -> int:
    if not first_sector.startswith(SYNC):
        raise SystemExit("не raw-сектор: нет sync-паттерна в начале файла")
    mode = first_sector[15]
    if mode == 1:
        return 16
    if mode == 2:
        return 24
    raise SystemExit(f"неизвестный режим сектора: {mode}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]

    size = os.path.getsize(src)
    if size % RAW:
        raise SystemExit(f"размер {size} не кратен {RAW} — образ не raw либо повреждён")
    sectors = size // RAW

    with open(src, "rb") as f, open(dst, "wb") as out:
        off = detect_offset(f.read(RAW))
        f.seek(0)
        print(f"{sectors} секторов, режим {'1' if off == 16 else '2 form 1'} (offset {off})")
        for i in range(sectors):
            out.write(f.read(RAW)[off:off + 2048])
            if i % 20000 == 0:
                print(f"  {i}/{sectors}", flush=True)

    print(f"готово: {dst} ({os.path.getsize(dst)} байт)")


if __name__ == "__main__":
    main()
