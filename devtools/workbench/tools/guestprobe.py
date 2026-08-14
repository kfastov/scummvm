#!/usr/bin/env python3
"""Наблюдение за живой средой ToolBook в госте: перехват вызовов и аргументов.

Готовый обвес для метода из ledger/0044, применённый к свойствам (ledger/0066).
Что важно помнить:

* стенд надо поднимать с заглушкой GDB: `GDB=1 MACHINE=pc-i440fx-10.1
  LOADVM=bashnya-installed HEADLESS=1 ./3-vm.sh run bashnya`;
* гость в защищённом режиме, `SS` — селектор, а не параграф. База берётся из
  монитора (`info registers`, колонка после селектора) и подставляется в
  `SS_BASE`; для процесса игры она постоянна;
* пока гость стоит на точке останова, QMP-ввод не работает вовсе
  («VM not running»), и монитор молчит — сначала `cont`;
* среда сама по кругу читает свойство `0x040c` объекта `(1, 0x7c80)`, поэтому
  фильтр по номеру обязателен, иначе интересные вызовы утонут.

Адрес функции в памяти ищется так: `pmemsave` всей памяти, поиск сигнатуры
пролога из файла (`tools/ne16.py dis`), затем обход таблиц страниц по `CR3`.
Проверка: `CS.base + смещение_в_сегменте` должно совпасть с найденным адресом.
"""
import sys, struct, threading, time, json, socket, os
sys.path.insert(0, 'tools')
from gdbcli import Gdb

ADDR = 0x818d625a          # builtin 152 в памяти гостя
SS_BASE = 0x809f17a0       # база стека процесса игры

def qmp_click(x, y):
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join('vm', 'qmp.sock'))
    time.sleep(0.3); s.recv(65536)
    s.sendall(json.dumps({"execute": "qmp_capabilities"}).encode() + b'\n')
    time.sleep(0.3); s.recv(65536)
    ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 640)}},
          {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 480)}},
          {"type": "btn", "data": {"down": True, "button": "left"}},
          {"type": "btn", "data": {"down": False, "button": "left"}}]
    s.sendall(json.dumps({"execute": "input-send-event", "arguments": {"events": ev}}).encode() + b'\n')
    time.sleep(0.3)
    return s.recv(65536).decode()[:120]

g = Gdb()
g.unbrk(ADDR)
print('точка:', g.brk(ADDR))

def poke():
    time.sleep(2.0)
    print('клик:', qmp_click(237, 292))

threading.Thread(target=poke, daemon=True).start()

seen = []
for n in range(60):
    stop = g.cont(timeout=25)
    if not stop:
        print('остановов больше нет'); break
    r = g.regs()
    bp = r['ebp'] & 0xffff
    d = g.mem(SS_BASE + bp, 20)
    if not d:
        print('память не читается'); break
    w = [struct.unpack_from('<H', d, i)[0] for i in range(0, 20, 2)]
    pid, obj = w[3], (w[6], w[7])
    seen.append((pid, obj))
    if n < 12 or pid >= 0x4000:
        print('  вызов %2d: свойство 0x%04x, объект %04x:%04x, вызвано из cs=%04x' %
              (n, pid, obj[0], obj[1], w[2]))
g.unbrk(ADDR)
g.cont(timeout=2)
print('всего перехвачено:', len(seen))
