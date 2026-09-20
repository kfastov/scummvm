#!/usr/bin/env python3
"""Карта свойств OpenScript: что среда делает с каждым номером.

Имён свойств в файлах среды найти не удалось (ledger/0058), поэтому карта
строится по действию: в `MTB40RUN.EXE` сегмент 66 лежит разбор номеров для
чтения свойства (builtin 140), и для каждого номера видно, в какую ветку он
уходит и что там вызывается.

    tools/propmap.py [путь к MTB40RUN.EXE]

Номер свойства в байт-коде — `0x4000 + индекс` (это подтверждает сама
`CDBGetValueEx`, ledger/0057). Номера, которых в разборе нет, среда хранит в
общей базе объектов CDB.
"""
import sys
sys.path.insert(0, 'tools')
from ne16 import NE
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

import sys as _s
ne = NE(_s.argv[1] if len(_s.argv) > 1 else 'games/bashnya/RUNTIME/MTB40RUN.EXE')
SEG = 66
f, sz, _ = ne.seg(SEG)
rel = ne.relocs(SEG)
md = Cs(CS_ARCH_X86, CS_MODE_16)
code = ne.d[f:f+sz]
ins = list(md.disasm(code, 0))
by_addr = {i.address: k for k, i in enumerate(ins)}

def window(addr, n=7):
    k = by_addr.get(addr)
    if k is None:
        return ['(нет разбора)']
    out = []
    for i in ins[k:k+n]:
        mark = ''
        for a in range(i.address, i.address + i.size):
            if a in rel:
                mark = ' → ' + rel[a][1]
        out.append('%s %s%s' % (i.mnemonic, i.op_str, mark))
    return out

acc = 0
found = []
for k, i in enumerate(ins):
    v = None
    if i.mnemonic in ('cmp', 'sub') and i.op_str.startswith('ax, 0x'):
        v = int(i.op_str.split('0x')[1], 16)
        if 0x4000 <= v <= 0x42ff:
            acc = v
        elif i.mnemonic == 'sub' and acc and v < 0x400:
            acc += v
        else:
            continue
        nxt = ins[k+1] if k + 1 < len(ins) else None
        if nxt and nxt.mnemonic in ('je', 'jz'):
            found.append((acc, int(nxt.op_str, 16)))
seen = {}
for pid, tgt in found:
    seen.setdefault(pid, tgt)
for pid in sorted(seen):
    print('0x%04x → seg66:%04x   %s' % (pid, seen[pid], ' ; '.join(window(seen[pid]))))
