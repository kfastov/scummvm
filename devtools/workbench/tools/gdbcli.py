#!/usr/bin/env python3
"""Минимальный клиент удалённого протокола GDB к заглушке QEMU.

Полноценный gdb для 16-битного гостя ставить незачем: протокол простой, а нам
нужны три вещи — точка останова, продолжение и чтение памяти и регистров.
Так проще, чем воевать с чужим отладчиком, и всё видно.

    gdbcli.py break 0x8068b840        — поставить точку и ждать срабатывания
    gdbcli.py regs                    — регистры
    gdbcli.py mem 0x1234 64           — прочитать память

Заглушка поднимается ключом -s (порт 1234). Адреса — линейные.
"""
from __future__ import annotations

import socket
import struct
import sys
import time

PORT = 1234
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi', 'eip',
        'eflags', 'cs', 'ss', 'ds', 'es', 'fs', 'gs']


class Gdb:
    def __init__(self, host='127.0.0.1', port=PORT):
        self.s = socket.create_connection((host, port), timeout=5)
        self.buf = b''

    def _send(self, body: str):
        csum = sum(body.encode()) & 0xff
        self.s.sendall(b'$' + body.encode() + b'#%02x' % csum)

    def _recv_packet(self, timeout=5.0):
        self.s.settimeout(timeout)
        while True:
            if b'$' in self.buf and b'#' in self.buf.split(b'$', 1)[1]:
                _, rest = self.buf.split(b'$', 1)
                body, tail = rest.split(b'#', 1)
                if len(tail) >= 2:
                    self.buf = tail[2:]
                    self.s.sendall(b'+')
                    return body.decode('latin1')
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk.replace(b'+', b'', 1) if self.buf == b'' and chunk.startswith(b'+') else chunk

    def cmd(self, body: str, timeout=5.0):
        self._send(body)
        return self._recv_packet(timeout)

    def regs(self):
        r = self.cmd('g')
        if not r or r.startswith('E'):
            return None
        vals = {}
        for i, name in enumerate(REGS):
            hexs = r[i * 8:(i + 1) * 8]
            if len(hexs) < 8:
                break
            vals[name] = struct.unpack('<I', bytes.fromhex(hexs))[0]
        return vals

    def mem(self, addr: int, length: int):
        r = self.cmd('m%x,%x' % (addr, length))
        if not r or r.startswith('E'):
            return None
        return bytes.fromhex(r)

    def brk(self, addr: int, kind=0, size=1):
        return self.cmd('Z%d,%x,%x' % (kind, addr, size))

    def unbrk(self, addr: int, kind=0, size=1):
        return self.cmd('z%d,%x,%x' % (kind, addr, size))

    def cont(self, timeout=120.0):
        self._send('c')
        return self._recv_packet(timeout)


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    g = Gdb()
    what = sys.argv[1]
    if what == 'regs':
        r = g.regs()
        print(' '.join('%s=%08x' % (k, v) for k, v in r.items()) if r else 'нет ответа')
    elif what == 'mem':
        addr = int(sys.argv[2], 0)
        n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
        d = g.mem(addr, n)
        print(d.hex() if d else 'нет ответа')
    elif what == 'break':
        addr = int(sys.argv[2], 0)
        print('точка останова:', g.brk(addr))
        print('ждём срабатывания…')
        stop = g.cont(timeout=float(sys.argv[3]) if len(sys.argv) > 3 else 120.0)
        print('останов:', stop)
        r = g.regs()
        if r:
            print(' '.join('%s=%08x' % (k, v) for k, v in r.items()))
    else:
        raise SystemExit(__doc__)
    return 0


if __name__ == '__main__':
    sys.exit(main())
