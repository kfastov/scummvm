"""Разбор 16-битного NE: дизассемблер сегмента и разрешение перемещений."""
import re, struct, sys, pathlib
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


# ------------------------------------------------------- границы функций
#
# Раньше каждая функция читалась вслепую: `dis 67 0x0cfe 150`, потом
# `dis 67 0x0d90 200` — длина угадывалась, и один и тот же код
# дизассемблировался по десятку раз (адрес 0x29e0 встретился в 66 командах
# разобранных сессий). Границу можно вычислить, а не угадывать.

TERMINALS = ('ret', 'retf', 'iret')


def _md():
    return Cs(CS_ARCH_X86, CS_MODE_16)


def func_end(ne, seg, start, limit=0x4000):
    """Конец функции линейным проходом.

    Останов на ret/retf, но только когда мы уже прошли все места, куда
    прыгали вперёд: у компилятора 16-битного кода несколько точек выхода —
    ранний `ret` в середине не означает конца тела. Возвращает адрес за
    последней командой.
    """
    f, sz, _ = ne.seg(seg)
    end = min(start + limit, sz)
    code = ne.d[f + start:f + end]
    reach = start
    last = start
    for i in _md().disasm(code, start):
        last = i.address + i.size
        if i.mnemonic.startswith('j') or i.mnemonic == 'loop':
            try:
                t = int(i.op_str, 0)
                if start <= t < end:
                    reach = max(reach, t)
            except ValueError:
                pass
        if i.mnemonic in TERMINALS and i.address >= reach:
            return i.address + i.size
    return last


def prologues(ne, seg):
    """Начала функций: прологи плюс цели ближних call.

    Дальняя точка входа выглядит как `mov ax,ds; nop; inc bp; push bp;
    mov bp,sp` — внутри неё сидит и короткий пролог, на четыре байта дальше.
    Считать их двумя функциями нельзя, поэтому вложенный отбрасываем.
    """
    f, sz, _ = ne.seg(seg)
    body = ne.d[f:f + sz]
    starts = set()
    far = {m.start() for m in re.finditer(rb'\x8c\xd8\x90\x45\x55\x8b\xec', body)}
    starts |= far
    for m in re.finditer(rb'\x55\x8b\xec', body):        # push bp; mov bp,sp
        if m.start() - 4 not in far:
            starts.add(m.start())
    for i in _md().disasm(body, 0):                       # цели ближних call
        if i.mnemonic == 'call':
            try:
                t = int(i.op_str, 0)
                if 0 <= t < sz:
                    starts.add(t)
            except ValueError:
                pass
    return sorted(starts)


def cmd_index(ne, argv):
    """Таблица функций сегмента: адрес, размер, имя из таблицы вывода."""
    seg = int(argv[0])
    names = {}
    for num, (where, name) in ne.exports().items():
        m = re.match(r'seg(\d+):([0-9a-fA-F]+)', str(where))
        if m and int(m.group(1)) == seg:
            names[int(m.group(2), 16)] = '%s (#%d)' % (name, num)
    starts = prologues(ne, seg)
    print('  сегмент %d: функций %d' % (seg, len(starts)))
    for s in starts:
        e = func_end(ne, seg, s)
        print('  %04x..%04x  %5d  %s' % (s, e, e - s, names.get(s, '')))


def cmd_fn(ne, argv):
    """Ровно одна функция — от адреса до её конца, без угадывания длины."""
    seg = int(argv[0]); start = int(argv[1], 0)
    end = func_end(ne, seg, start)
    f, _, _ = ne.seg(seg)
    rel = ne.relocs(seg)
    print('  %d:%04x..%04x  (%d байт)' % (seg, start, end, end - start))
    for i in _md().disasm(ne.d[f + start:f + end], start):
        mark = ''
        for k in range(i.address, i.address + i.size):
            if k in rel:
                mark = '   <- %s %s' % (rel[k][0], rel[k][1])
        print('  %04x: %-20s %s %s%s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, mark))


def cmd_xref(ne, argv):
    """Кто зовёт этот адрес: ближние call внутри сегмента и дальние через фиксапы.

    У дальнего вызова фиксап типа 2 правит только слово сегмента, а смещение
    лежит в самом образе — по слову перед фиксапом (ledger/0054). Поэтому
    сравнивать надо не описание цели (там смещение всегда 0000), а то, что
    реально записано в байтах.
    """
    seg = int(argv[0]); target = int(argv[1], 0)
    found = 0
    f, sz, _ = ne.seg(seg)
    for i in _md().disasm(ne.d[f:f + sz], 0):
        if i.mnemonic in ('call', 'jmp') and i.op_str.startswith('0x'):
            try:
                if int(i.op_str, 0) == target:
                    print('  %d:%04x  %s %s' % (seg, i.address, i.mnemonic, i.op_str))
                    found += 1
            except ValueError:
                pass
    for s in range(1, ne.nseg + 1):
        try:
            rel = ne.relocs(s)
            sf, ssz, _ = ne.seg(s)
        except Exception:
            continue
        for off, v in sorted(rel.items()):
            m = re.match(r'seg(\d+):', str(v[1]))
            if not m or int(m.group(1)) != seg:
                continue
            # слово-смещение дальнего адреса стоит перед словом-сегментом
            if off < 2 or off + 2 > ssz:
                continue
            imm = struct.unpack_from('<H', ne.d, sf + off - 2)[0]
            if imm == target:
                # 9A перед парой смещение:сегмент — это именно дальний вызов;
                # без него мы видим просто дальний указатель (или звено цепочки
                # фиксапов, которое к вызову отношения не имеет).
                call = ne.d[sf + off - 3] == 0x9a
                print('  %s %d:%04x -> %d:%04x'
                      % ('дальний вызов' if call else 'дальний указатель',
                         s, off - 3 if call else off - 2, seg, target))
                found += 1
    if not found:
        print('  ссылок на %d:%04x не найдено' % (seg, target))


def main():
    if len(sys.argv) < 3:
        print("""tools/ne16.py МОДУЛЬ ПОДКОМАНДА …

  index СЕГ            таблица функций сегмента: адрес, размер, имя
  fn СЕГ АДРЕС         одна функция целиком — конец вычисляется, не угадывается
  xref СЕГ АДРЕС       кто зовёт: ближние call и дальние через фиксапы
  dis СЕГ АДРЕС [N]    N байт вслепую (когда границы не нужны)
  rel СЕГ [АДРЕС]      перемещения сегмента
  bt НОМЕР…            адрес встроенной функции по номеру
  exp [НОМЕР…]         таблица вывода

Пример: tools/ne16.py games/bashnya/RUNTIME/MTB40RUN.EXE fn 67 0x0cfe""")
        return
    ne = NE(sys.argv[1]); cmd = sys.argv[2]
    if cmd == 'index':
        return cmd_index(ne, sys.argv[3:])
    if cmd == 'fn':
        return cmd_fn(ne, sys.argv[3:])
    if cmd == 'xref':
        return cmd_xref(ne, sys.argv[3:])
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
