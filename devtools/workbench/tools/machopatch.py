#!/usr/bin/env python3
"""Добавляет LC_LOAD_DYLIB в 64-битный Mach-O (thin, little-endian).

Нужно, чтобы ScummVM подгружал наш libseed.dylib до main() — dyld грузит
зависимости и вызывает их конструкторы раньше точки входа исполняемого файла.

Место под новую команду берём из паддинга между концом load-команд и началом
данных первой секции. Заодно выкидываем LC_CODE_SIGNATURE: правка всё равно
делает старую подпись невалидной, а AltStore подписывает бандл заново.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_LOAD_DYLIB = 0x0C
LC_CODE_SIGNATURE = 0x1D
LC_SEGMENT_64 = 0x19


def parse(data):
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<8I", data, 0)
    if magic != MH_MAGIC_64:
        sys.exit(f"не thin 64-битный Mach-O (magic={magic:#x}); fat-бинарь надо сначала разрезать lipo")
    cmds, off = [], 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        cmds.append((cmd, cmdsize, off))
        off += cmdsize
    return dict(ncmds=ncmds, sizeofcmds=sizeofcmds, cmds=cmds)


def min_section_offset(data, cmds):
    """Наименьшее смещение реальных данных — за него load-команды заезжать не должны."""
    best = None
    for cmd, cmdsize, off in cmds:
        if cmd != LC_SEGMENT_64:
            continue
        segname = data[off + 8:off + 24].rstrip(b"\0").decode()
        nsects = struct.unpack_from("<I", data, off + 64)[0]
        soff = off + 72
        for _ in range(nsects):
            # struct section_64: sectname[16] segname[16] addr(8) size(8) offset(4) ...
            fileoff = struct.unpack_from("<I", data, soff + 48)[0]
            if segname == "__TEXT" and fileoff > 0:
                best = fileoff if best is None else min(best, fileoff)
            soff += 80
    return best


def build_load_dylib(path):
    raw = path.encode() + b"\0"
    pad = (-(24 + len(raw))) % 8
    payload = raw + b"\0" * pad
    cmdsize = 24 + len(payload)
    # cmd, cmdsize, name_offset, timestamp, current_version, compatibility_version
    return struct.pack("<6I", LC_LOAD_DYLIB, cmdsize, 24, 0, 0x10000, 0x10000) + payload


def main():
    src, dst, dylib = sys.argv[1], sys.argv[2], sys.argv[3]
    data = bytearray(open(src, "rb").read())
    info = parse(data)

    limit = min_section_offset(data, info["cmds"])
    used = 32 + info["sizeofcmds"]
    print(f"load-команд: {info['ncmds']}, занято {used} байт, данные __TEXT с {limit} -> запас {limit - used}")

    # Вырезаем LC_CODE_SIGNATURE, если есть.
    ncmds, sizeofcmds = info["ncmds"], info["sizeofcmds"]
    sig = [(c, s, o) for c, s, o in info["cmds"] if c == LC_CODE_SIGNATURE]
    if sig:
        _, cmdsize, off = sig[0]
        end = 32 + sizeofcmds
        data[off:end - cmdsize] = data[off + cmdsize:end]
        data[end - cmdsize:end] = b"\0" * cmdsize
        ncmds -= 1
        sizeofcmds -= cmdsize
        print(f"убрал LC_CODE_SIGNATURE ({cmdsize} байт)")

    new = build_load_dylib(dylib)
    if 32 + sizeofcmds + len(new) > limit:
        sys.exit(f"не хватает паддинга: нужно {len(new)}, доступно {limit - 32 - sizeofcmds}")

    at = 32 + sizeofcmds
    data[at:at + len(new)] = new
    ncmds += 1
    sizeofcmds += len(new)
    struct.pack_into("<2I", data, 16, ncmds, sizeofcmds)

    open(dst, "wb").write(data)
    print(f"добавил LC_LOAD_DYLIB -> {dylib} ({len(new)} байт); теперь {ncmds} команд, {sizeofcmds} байт")


if __name__ == "__main__":
    main()
