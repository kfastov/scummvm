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

if __name__ == '__main__':
    main()
