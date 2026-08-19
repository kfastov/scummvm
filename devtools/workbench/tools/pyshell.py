#!/usr/bin/env python3
"""Тёплый питон: одно состояние на много вызовов.

Между двумя вызовами Bash не переживает ничего, поэтому каждый вопрос к книге
начинался с восстановления состояния: `import struct`, `sys.path.insert`,
`d=open('games/bashnya/kntower.tbk','rb').read()`. Книга — 38,8 МБ, и за
разобранные сессии она открывалась заново 246 раз. Здесь она открывается один
раз, а дальше живёт в процессе-демоне вместе со всем, что мы про неё поняли.

    tools/pyshell.py e 'len(book)'                    выполнить
    tools/pyshell.py e 'hexat(0x1000)'                и напечатать результат
    tools/pyshell.py e 'recs = tbk.find_records(book); len(recs)'
    tools/pyshell.py e 'recs[:3]'                     recs жив с прошлого вызова
    tools/pyshell.py vars                             что сейчас в памяти
    tools/pyshell.py stop                             погасить демон

Демон поднимается сам при первом `e`. Он привязан к каталогу репозитория, так
что параллельные сессии друг другу не мешают.

Что уже лежит в пространстве имён (`vars` покажет полный список):

    book            байты games/bashnya/kntower.tbk
    tbk, osc        tools/tbk.py и tools/osc.py
    NE              класс из tools/ne16.py
    ne(path)        разобранный NE с кэшем: ne('MTB40RUN.EXE')
    dis(seg, off)   дизассемблер того же модуля
    hexat(off, n)   шестнадцатеричный дамп книги
    u8 u16 u32(off) чтение чисел из книги
    grep(pat, n)    поиск байтов или регулярного выражения по книге
    asc(off, n)     печатаемая строка по смещению
    struct re collections itertools math  — импортированы

Вывод обрезается до --max символов (по умолчанию 4000): случайный `print`
длинного списка не должен занимать пол-окна. Обрезка честно сообщает, сколько
осталось.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import io
import json
import os
import socket
import subprocess
import sys
import textwrap
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = hashlib.sha1(ROOT.encode()).hexdigest()[:10]
SOCK = '/tmp/pyshell-%s.sock' % TAG
VENV = os.path.join(ROOT, '.venv', 'bin', 'python')


# --------------------------------------------------------------- пространство

PRELUDE = r'''
import struct, re, collections, itertools, math, os, sys
sys.path.insert(0, os.path.join(ROOT, "tools"))

import tbk, osc
try:
    from ne16 import NE
except Exception as exc:          # capstone может быть не поставлен
    NE = None
    _ne_error = exc

BOOK_PATH = os.path.join(ROOT, "games/bashnya/kntower.tbk")
book = open(BOOK_PATH, "rb").read()

_ne_cache = {}
def ne(path):
    """Разобранный NE с кэшем. Путь можно короткий: 'MTB40RUN.EXE'."""
    if NE is None:
        raise RuntimeError("capstone не поставлен: %s" % _ne_error)
    if "/" not in path:
        path = os.path.join(ROOT, "games/bashnya/RUNTIME", path)
    elif not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    if path not in _ne_cache:
        _ne_cache[path] = NE(path)
    return _ne_cache[path]

def hexat(off, n=64, data=None):
    """Дамп: смещение, байты, печатаемые символы."""
    d = book if data is None else data
    out = []
    for i in range(off, min(off + n, len(d)), 16):
        chunk = d[i:i + 16]
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        out.append("%08x  %-47s  %s" % (i, chunk.hex(" "), txt))
    return "\n".join(out)

def u8(off, data=None):  return (book if data is None else data)[off]
def u16(off, data=None): return struct.unpack_from("<H", book if data is None else data, off)[0]
def u32(off, data=None): return struct.unpack_from("<I", book if data is None else data, off)[0]
def i16(off, data=None): return struct.unpack_from("<h", book if data is None else data, off)[0]

def asc(off, n=64, data=None):
    d = book if data is None else data
    s = d[off:off + n]
    return "".join(chr(c) if 32 <= c < 127 else "." for c in s)

def grep(pat, n=10, data=None, start=0):
    """Поиск по книге. pat — bytes (точное) или str (регулярное по байтам)."""
    d = book if data is None else data
    if isinstance(pat, str):
        rx = re.compile(pat.encode("latin1"))
    elif isinstance(pat, bytes):
        rx = re.compile(re.escape(pat))
    else:
        rx = pat
    out = []
    for m in rx.finditer(d, start):
        out.append((m.start(), m.group()[:32]))
        if len(out) >= n:
            break
    return out

def dis(seg, off, n=120, path="MTB40RUN.EXE"):
    """Дизассемблировать n байт сегмента seg. Возвращает строки."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_16
    m = ne(path)
    f, sz, _ = m.seg(seg)
    rel = m.relocs(seg)
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    out = []
    for i in md.disasm(m.d[f + off:f + off + n], off):
        mark = ""
        for k in range(i.address, i.address + i.size):
            if k in rel:
                mark = "   <- %s %s" % (rel[k][0], rel[k][1])
        out.append("  %04x: %-20s %s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, mark))
    return "\n".join(out)
'''


def serve() -> None:
    ns: dict = {'ROOT': ROOT, '__name__': '__pyshell__'}
    exec(compile(PRELUDE, '<prelude>', 'exec'), ns)

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK)
    srv.listen(4)
    while True:
        conn, _ = srv.accept()
        try:
            buf = b''
            while not buf.endswith(b'\n\x00'):
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
            req = json.loads(buf[:-2].decode('utf-8'))
            if req.get('op') == 'stop':
                conn.sendall(json.dumps({'out': 'демон погашен'}).encode())
                conn.close()
                break
            if req.get('op') == 'vars':
                names = sorted(k for k in ns
                               if not k.startswith('_') and k not in ('ROOT',))
                rows = []
                for k in names:
                    v = ns[k]
                    kind = type(v).__name__
                    size = ''
                    try:
                        size = ' len=%d' % len(v)
                    except Exception:
                        pass
                    rows.append('  %-16s %s%s' % (k, kind, size))
                conn.sendall(json.dumps({'out': '\n'.join(rows)}).encode())
                conn.close()
                continue
            out, err = run(req['code'], ns)
            conn.sendall(json.dumps({'out': out, 'err': err}).encode())
        except Exception:
            try:
                conn.sendall(json.dumps({'err': traceback.format_exc()}).encode())
            except Exception:
                pass
        finally:
            conn.close()
    srv.close()
    if os.path.exists(SOCK):
        os.unlink(SOCK)


def run(code: str, ns: dict) -> tuple[str, str]:
    """Выполняет код с семантикой REPL: значение последнего выражения печатается."""
    buf = io.StringIO()
    old = sys.stdout
    sys.stdout = buf
    err = ''
    try:
        tree = ast.parse(code, mode='exec')
        body = tree.body
        if body and isinstance(body[-1], ast.Expr):
            head, tail = body[:-1], body[-1]
            if head:
                exec(compile(ast.Module(body=head, type_ignores=[]), '<e>', 'exec'), ns)
            val = eval(compile(ast.Expression(tail.value), '<e>', 'eval'), ns)
            if val is not None:
                print(val if isinstance(val, str) else repr(val))
        else:
            exec(compile(tree, '<e>', 'exec'), ns)
    except Exception:
        # Кадры самого pyshell выкидываем: они одинаковы при любой ошибке и
        # только занимают место. Остаётся то, что написал вызывающий, плюс
        # строка исключения.
        exc = sys.exc_info()
        frames = [f for f in traceback.extract_tb(exc[2])
                  if os.path.basename(f.filename) != os.path.basename(__file__)]
        err = ''.join(traceback.format_list(frames)
                      + traceback.format_exception_only(exc[0], exc[1])).strip()
    finally:
        sys.stdout = old
    return buf.getvalue(), err


# ------------------------------------------------------------------- клиент

def alive() -> bool:
    if not os.path.exists(SOCK):
        return False
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(SOCK)
        return True
    except OSError:
        os.unlink(SOCK)
        return False
    finally:
        s.close()


def start() -> None:
    py = VENV if os.path.exists(VENV) else sys.executable
    subprocess.Popen([py, os.path.abspath(__file__), '--serve'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True, cwd=ROOT)
    import time
    for _ in range(150):                 # книга читается ~секунду
        if alive():
            return
        time.sleep(0.1)
    raise SystemExit('демон не поднялся; запустите вручную: tools/pyshell.py --serve')


def ask(req: dict, timeout: float) -> dict:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall(json.dumps(req).encode('utf-8') + b'\n\x00')
    buf = b''
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        return {'err': 'выполнение дольше %gс. Демон занят: tools/pyshell.py kill' % timeout}
    finally:
        s.close()
    return json.loads(buf.decode('utf-8')) if buf else {}


def clip(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    head = text[:limit]
    rest = text[limit:]
    return head + '\n… обрезано: ещё %d символов, %d строк (--max N покажет больше)' % (
        len(rest), rest.count('\n') + 1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('op', nargs='?', default='e',
                    help='e | vars | status | stop | kill')
    ap.add_argument('code', nargs='*', help='код для e')
    ap.add_argument('--max', type=int, default=4000, help='предел вывода в символах')
    ap.add_argument('--timeout', type=float, default=120.0)
    ap.add_argument('--serve', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()

    if a.serve:
        serve()
        return 0
    if a.op == 'status':
        print('демон жив (%s)' % SOCK if alive() else 'демон не запущен')
        return 0
    if a.op == 'kill':
        subprocess.run(['pkill', '-f', 'pyshell.py --serve'], capture_output=True)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        print('демон снят')
        return 0
    if a.op == 'stop':
        if alive():
            print(ask({'op': 'stop'}, 5).get('out', ''))
        else:
            print('демон не запущен')
        return 0

    if not alive():
        start()
    if a.op == 'vars':
        print(clip(ask({'op': 'vars'}, a.timeout).get('out', ''), a.max))
        return 0

    code = ' '.join(a.code) if a.code else sys.stdin.read()
    if not code.strip():
        print('нечего выполнять'); return 1
    r = ask({'op': 'e', 'code': textwrap.dedent(code)}, a.timeout)
    out = r.get('out') or ''
    err = r.get('err') or ''
    if out:
        print(clip(out.rstrip('\n'), a.max))
    if err:
        print(clip(err, 1500), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
