"""Управление живым ScummVM: запуск, отладочный мост, кадры.

Мост — это TCP-порт внутри движка director (см. `scummvm-src/engines/director/
debug-bridge.cpp`). Одна команда в строке, ответ — строка JSON. Здесь клиент к
нему плюс присмотр за самим процессом: поднять, дождаться, снять кадр, убить.

Модуль общий для CLI (`tools/svmdbg`) и MCP-сервера (`tools/svmbridge/mcp_server.py`),
чтобы не разъезжались.
"""

from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCUMMVM = ROOT / "scummvm-src" / "scummvm"
RUNDIR = ROOT / "svm-run"
STATE = RUNDIR / "bridge-state.json"
DEFAULT_PORT = 3777
DEFAULT_TARGET = "sevenwitches-win-ru"


class BridgeError(RuntimeError):
    pass


# --------------------------------------------------------------- процесс


@dataclass
class RunState:
    pid: int
    port: int
    target: str
    log: str
    started: float

    def alive(self) -> bool:
        try:
            os.kill(self.pid, 0)
        except OSError:
            return False
        return True


def read_state() -> RunState | None:
    if not STATE.exists():
        return None
    try:
        data = json.loads(STATE.read_text())
    except (ValueError, OSError):
        return None
    st = RunState(**data)
    return st if st.alive() else None


def write_state(st: RunState) -> None:
    STATE.write_text(json.dumps(st.__dict__))


def clear_state() -> None:
    STATE.unlink(missing_ok=True)


def start(
    target: str = DEFAULT_TARGET,
    port: int = DEFAULT_PORT,
    debuglevel: int = 1,
    debugflags: str = "",
    framedump_ms: int = 0,
    dump_lingo: bool = False,
    audio: bool = False,
    log: str = "bridge-run.log",
    wait: float = 25.0,
    attempts: int = 3,
) -> RunState:
    """Поднимает движок вслепую и ждёт, пока мост начнёт отвечать.

    Возврат — состояние прогона. Если мост не ответил за `wait` секунд, процесс
    убивается и поднимается BridgeError с хвостом лога: молча возвращать
    «вроде запустилось» нельзя, это ровно та ошибка, на которой легко потерять час.

    Безоконный режим изредка не поднимается вовсе: лог обрывается сразу после
    инициализации звука, ошибок нет — похоже на гонку при создании поверхностей
    в драйвере dummy. Лечится повтором, поэтому `attempts`.
    """
    last = None
    for attempt in range(1, attempts + 1):
        try:
            return _start_once(target, port, debuglevel, debugflags, framedump_ms,
                               dump_lingo, audio, log, wait)
        except BridgeError as err:
            last = err
    raise BridgeError(f"движок не поднялся за {attempts} попыт(ки): {last}")


def _start_once(target, port, debuglevel, debugflags, framedump_ms,
                dump_lingo, audio, log, wait) -> RunState:
    if not SCUMMVM.exists():
        raise BridgeError(f"нет сборки ScummVM: {SCUMMVM}")

    stop()  # один прогон за раз, иначе порт занят и кадры мешаются

    RUNDIR.mkdir(exist_ok=True)
    (RUNDIR / "frames").mkdir(exist_ok=True)
    (RUNDIR / "shots").mkdir(exist_ok=True)

    ini = RUNDIR / "bridge.ini"
    ini.write_text(_build_ini(target, port, framedump_ms))

    args = [str(SCUMMVM), "-c", str(ini), "-g", "surfacesdl", "-d", str(debuglevel)]
    if debugflags:
        args.append(f"--debugflags={debugflags}")
    if dump_lingo:
        args.append("-u")
    args.append(target)

    env = dict(os.environ)
    env.update(
        SDL_VIDEODRIVER="dummy",
        SDL_RENDER_DRIVER="software",
        SDL_FRAMEBUFFER_ACCELERATION="0",
    )
    if not audio:
        env["SDL_AUDIODRIVER"] = "dummy"

    logpath = RUNDIR / log
    with open(logpath, "wb") as fh:
        proc = subprocess.Popen(
            args, cwd=str(RUNDIR), env=env, stdout=fh, stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    st = RunState(pid=proc.pid, port=port, target=target, log=str(logpath), started=time.time())
    write_state(st)

    deadline = time.time() + wait
    while time.time() < deadline:
        if proc.poll() is not None:
            clear_state()
            raise BridgeError(f"движок умер сразу же:\n{tail(logpath, 25)}")
        try:
            with Bridge(port, timeout=1.0) as br:
                br.command("ping")
            return st
        except (OSError, BridgeError):
            time.sleep(0.3)

    stop()
    raise BridgeError(f"мост не ответил за {wait} с:\n{tail(logpath, 25)}")


def _build_ini(target: str, port: int, framedump_ms: int) -> str:
    """Собирает конфиг прогона из основного, добавляя ключи моста."""
    base = (RUNDIR / "scummvm.ini").read_text()
    out, in_target = [], False
    for line in base.splitlines():
        if line.startswith("["):
            if in_target:
                out.extend(_bridge_keys(port, framedump_ms))
            in_target = line.strip() == f"[{target}]"
        # старые ключи прогона выкидываем: они задаются заново
        if re.match(r"^(debugbridge_port|framedump_ms|inputscript)\s*=", line):
            continue
        out.append(line)
    if in_target:
        out.extend(_bridge_keys(port, framedump_ms))
    return "\n".join(out) + "\n"


def _bridge_keys(port: int, framedump_ms: int) -> list[str]:
    keys = [f"debugbridge_port={port}"]
    if framedump_ms:
        keys.append(f"framedump_ms={framedump_ms}")
    return keys


def stop() -> bool:
    """Снимает прогон. Строго SIGKILL: на фатальной ошибке ScummVM уходит в свою
    консоль и обычный сигнал игнорирует — такой прогон висит часами."""
    killed = False
    st = read_state()
    if st:
        try:
            os.killpg(os.getpgid(st.pid), signal.SIGKILL)
            killed = True
        except OSError:
            try:
                os.kill(st.pid, signal.SIGKILL)
                killed = True
            except OSError:
                pass
    clear_state()
    subprocess.run(["pkill", "-9", "-f", "scummvm-src/scummvm"], capture_output=True)
    time.sleep(0.2)
    return killed


def tail(path: str | Path, lines: int = 30, grep: str = "") -> str:
    path = Path(path)
    if not path.exists():
        return f"(нет файла {path})"
    data = path.read_bytes().decode("utf-8", "replace").splitlines()
    if grep:
        rx = re.compile(grep)
        data = [ln for ln in data if rx.search(ln)]
    return "\n".join(data[-lines:])


# ------------------------------------------------------------------ мост


class Bridge:
    """Соединение с мостом. Живёт коротко: подключился, спросил, отключился."""

    def __init__(self, port: int = DEFAULT_PORT, timeout: float = 15.0):
        self.port = port
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self._buf = b""

    def __enter__(self) -> "Bridge":
        self.connect()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def connect(self) -> None:
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)

    def close(self) -> None:
        if self.sock:
            self.sock.close()
            self.sock = None

    def command(self, line: str) -> str:
        if not self.sock:
            self.connect()
        assert self.sock
        self.sock.sendall(line.strip().encode("utf-8", "replace") + b"\n")

        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise BridgeError("мост закрыл соединение")
            self._buf += chunk

        raw, self._buf = self._buf.split(b"\n", 1)
        # Движок отдаёт строки в кодировке игры; для нас важно не упасть.
        reply = json.loads(raw.decode("utf-8", "replace"))
        if not reply.get("ok", False):
            raise BridgeError(reply.get("out", "мост ответил отказом"))
        # Исходники Lingo хранятся с \r в конце строк — на терминале это
        # затирание строки поверх предыдущей.
        return reply.get("out", "").replace("\r\n", "\n").replace("\r", "\n")


def command(line: str, port: int | None = None) -> str:
    """Разовая команда: подключиться, спросить, отключиться."""
    if port is None:
        st = read_state()
        port = st.port if st else DEFAULT_PORT
    with Bridge(port) as br:
        return br.command(line)


def commands(lines: list[str], port: int | None = None) -> list[str]:
    """Несколько команд по одному соединению, по порядку."""
    if port is None:
        st = read_state()
        port = st.port if st else DEFAULT_PORT
    out = []
    with Bridge(port) as br:
        for line in lines:
            out.append(br.command(line))
    return out


def shot(name: str = "", port: int | None = None) -> Path:
    """Снимает кадр в svm-run/shots и возвращает путь."""
    (RUNDIR / "shots").mkdir(exist_ok=True)
    name = name or f"shot-{int(time.time() * 1000)}.png"
    if not name.endswith(".png"):
        name += ".png"
    path = RUNDIR / "shots" / name
    out = command(f"shot {path}", port=port)
    if not path.exists():
        raise BridgeError(f"кадр не записан: {out}")
    return path
