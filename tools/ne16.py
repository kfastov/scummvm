"""Разбор 16-битного NE: дизассемблер сегмента и разрешение перемещений."""
import struct, sys, pathlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

class NE:
    def __init__(self, path):
        self.d = d = pathlib.Path(path).read_bytes()
        self.ne = ne = struct.unpack_from('<H', d, 0x3c)[0]
        self.segtab = ne + struct.unpack_from('<H', d, ne+0x22)[0]
        self.nseg = struct.unpack_from('<H', d, ne+0x1c)[0]
        self.shift = 1 << struct.unpack_from('<H', d, ne+0x32)[0]
        self.modref = ne + struct.unpack_from('<H', d, ne+0x28)[0]
        self.nmod = struct.unpack_from('<H', d, ne+0x1e)[0]
        self.impnames = ne + struct.unpack_from('<H', d, ne+0x2a)[0]
        self.enttab = ne + struct.unpack_from('<H', d, ne+0x04)[0]
        self.resnames = ne + struct.unpack_from('<H', d, ne+0x26)[0]
        self.nonres = struct.unpack_from('<I', d, ne+0x2c)[0]  # уже файловое смещение

    def exports(self):
        """{ординал: ('segN:offset', имя)} — таблица входов плюс имена.

        Таблица входов лежит пачками: `[сколько][тип]`, где тип 0 — дыра
        (ординалы просто пропускаются), 0xff — подвижная запись по 6 байт
        (флаги, `int 3Fh`, сегмент, смещение), иначе — неподвижная по 3 байта
        (флаги, смещение), а сам тип и есть номер сегмента.
        """
        out, p, ordinal = {}, self.enttab, 1
        while p < len(self.d):
            count, kind = self.d[p], self.d[p+1]
            if count == 0:
                break
            p += 2
            for _ in range(count):
                if kind == 0:
                    pass
                elif kind == 0xff:
                    seg, off = self.d[p+3], struct.unpack_from('<H', self.d, p+4)[0]
                    out[ordinal] = ['seg%d:%04x' % (seg, off), '']
                    p += 6
                else:
                    off = struct.unpack_from('<H', self.d, p+1)[0]
                    out[ordinal] = ['seg%d:%04x' % (kind, off), '']
                    p += 3
                ordinal += 1
        # Имена: резидентная и нерезидентная таблицы устроены одинаково —
        # `[длина][имя][u16 ординал]`, пустое имя закрывает таблицу.
        for base in (self.resnames, self.nonres):
            q = base
            while q < len(self.d) and self.d[q]:
                n = self.d[q]
                name = self.d[q+1:q+1+n].decode('latin1')
                num = struct.unpack_from('<H', self.d, q+1+n)[0]
                if num in out:
                    out[num][1] = name
                q += n + 3
        return out

    def seg(self, n):
        off, sz, fl, _ = struct.unpack_from('<HHHH', self.d, self.segtab+(n-1)*8)
        return off*self.shift, (sz or 0x10000), fl

    def impname(self, off):
        n = self.d[self.impnames+off]
        return self.d[self.impnames+off+1:self.impnames+off+1+n].decode('latin1')

    def module(self, idx):
        off = struct.unpack_from('<H', self.d, self.modref + (idx-1)*2)[0]
        return self.impname(off)

    def relocs(self, n):
        """{смещение в сегменте: описание цели}"""
        o, sz, fl = self.seg(n)
        if not (fl & 0x0100):
            return {}
        cnt = struct.unpack_from('<H', self.d, o+sz)[0]
        p, out = o+sz+2, {}
        for _ in range(cnt):
            typ, flags, src = self.d[p], self.d[p+1], struct.unpack_from('<H', self.d, p+2)[0]
            a, b = struct.unpack_from('<HH', self.d, p+4)
            kind = flags & 3
            if kind == 0:      # внутренняя ссылка
                what = 'MOVABLE ordinal %d' % b if a == 0xff else 'seg%d:%04x' % (a, b)
            elif kind == 1:    # импорт по ординалу
                what = '%s.%d' % (self.module(a), b)
            elif kind == 2:    # импорт по имени
                what = '%s.%s' % (self.module(a), self.impname(b))
            else:
                what = 'OSFIXUP %d/%d' % (a, b)
            # Аддитивный фиксап правит одно место; обычный — связан в цепочку:
            # в самом месте лежит смещение следующего использования, 0xffff — конец.
            if flags & 4:
                out[src] = ('тип%d' % typ, what, True)
            else:
                seen, cur = set(), src
                while cur != 0xffff and cur not in seen and o + cur + 2 <= o + sz:
                    seen.add(cur)
                    out[cur] = ('тип%d' % typ, what, False)
                    cur = struct.unpack_from('<H', self.d, o + cur)[0]
            p += 8
        return out

def main():
    ne = NE(sys.argv[1]); cmd = sys.argv[2]
    if cmd == 'dis':
        s, off = int(sys.argv[3]), int(sys.argv[4], 0)
        n = int(sys.argv[5]) if len(sys.argv) > 5 else 120
        f, sz, _ = ne.seg(s); rel = ne.relocs(s)
        md = Cs(CS_ARCH_X86, CS_MODE_16)
        for i in md.disasm(ne.d[f+off:f+off+n], off):
            mark = ''
            for k in range(i.address, i.address + i.size):
                if k in rel:
                    mark = '   ← %s %s' % (rel[k][0], rel[k][1])
            print('  %04x: %-20s %s %s%s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, mark))
    elif cmd == 'rel':
        s = int(sys.argv[3]); want = int(sys.argv[4], 0) if len(sys.argv) > 4 else None
        for k, v in sorted(ne.relocs(s).items()):
            if want is None or abs(k - want) <= 8:
                print('  %04x: %s %s%s' % (k, v[0], v[1], ' (цепочка)' if not v[2] else ''))
    elif cmd == 'bt':
        # Адрес встроенной функции по её номеру. Таблица дальних указателей
        # лежит в сегменте 34 по 0x1bfa, запись — четыре байта на номер
        # (найдена по `lcall cs:[bx+0x1bfa]` в обработчике 0x2c, ledger/0054).
        # Смещение берётся из образа, сегмент — из перемещения: фиксап типа 2
        # правит только слово сегмента, а на месте слова-сегмента в образе
        # лежит звено цепочки, а не адрес.
        f, _, _ = ne.seg(BUILTIN_SEG); rel = ne.relocs(BUILTIN_SEG)
        for arg in sys.argv[3:]:
            bid = int(arg, 0)
            e = BUILTIN_TABLE + bid * 4
            off = struct.unpack_from('<H', ne.d, f + e)[0]
            target = rel.get(e) or rel.get(e + 2)
            if not target:
                print('  builtin %-4d @%04x: перемещения нет — номера нет в таблице' % (bid, e))
                continue
            seg = target[1].split(':')[0]
            print('  builtin %-4d -> %s:%04x' % (bid, seg.upper().replace('SEG', 'RUN'), off))
    elif cmd == 'exp':
        exp = ne.exports()
        want = [int(a, 0) for a in sys.argv[3:]]
        for num in (want or sorted(exp)):
            where, name = exp.get(num, ('—', '—'))
            print('  %-5d %-14s %s' % (num, where, name))

BUILTIN_SEG = 34
BUILTIN_TABLE = 0x1bfa

if __name__ == '__main__':
    main()
