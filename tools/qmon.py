#!/usr/bin/env python3
"""Управление виртуалкой через монитор QEMU (unix-сокет).

Нужно, чтобы ставить и отлаживать гостя без человека за клавиатурой: команды
идут в монитор, картинка забирается через screendump.

    qmon.py cmd "info block"          — произвольная команда монитора
    qmon.py key ret                   — нажать клавишу (синтаксис sendkey)
    qmon.py type "fdisk"              — напечатать строку посимвольно
    qmon.py enter "format c: /s"      — напечатать строку и нажать Enter
    qmon.py shot vm/shots/01.png      — снимок экрана (ppm конвертируется в png)
"""

import os
import socket
import subprocess
import sys
import time

SOCK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "vm", "monitor.sock")

# sendkey принимает не символы, а имена клавиш
KEYMAP = {
    " ": "spc", "\\": "backslash", "/": "slash", ".": "dot", ",": "comma",
    ";": "semicolon", "'": "apostrophe", "-": "minus", "=": "equal",
    "[": "bracket_left", "]": "bracket_right", "\n": "ret", "\t": "tab",
    ":": "shift-semicolon", "_": "shift-minus", "?": "shift-slash",
    "*": "shift-8", "(": "shift-9", ")": "shift-0", "!": "shift-1",
    "+": "shift-equal", '"': "shift-apostrophe", ">": "shift-dot", "<": "shift-comma",
}


def send(cmd, wait: float = 0.35) -> str:
    """Одна команда или список команд — списком они уходят в одно соединение."""
    cmds = [cmd] if isinstance(cmd, str) else list(cmd)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(SOCK)
    time.sleep(0.15)
    try:
        s.recv(65536)          # приветствие монитора
    except socket.timeout:
        pass
    for c in cmds:
        s.sendall((c + "\n").encode())
        time.sleep(0.012 if len(cmds) > 1 else 0)
    # Закрыть сокет раньше, чем монитор разберёт очередь, — значит потерять хвост команд.
    time.sleep(max(wait, 0.03 * len(cmds)))
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
            if out.rstrip().endswith(b"(qemu)"):
                break
    except socket.timeout:
        pass
    s.close()
    return out.decode(errors="replace")


def key(name: str) -> None:
    send(f"sendkey {name}", wait=0.08)


def type_text(text: str) -> None:
    for ch in text:
        if ch in KEYMAP:
            key(KEYMAP[ch])
        elif ch.isupper():
            key(f"shift-{ch.lower()}")
        else:
            key(ch)
        time.sleep(0.02)


QMP_SOCK = os.path.join(os.path.dirname(SOCK), "qmp.sock")
SCREEN_W, SCREEN_H = 640, 480      # текущий видеорежим гостя
ABS_MAX = 32767                    # диапазон абсолютных координат в input-send-event


def qmp(events: list) -> None:
    """Отправить события ввода через QMP.

    HMP-команды mouse_move/mouse_button до гостя не доходят; поддерживаемый путь —
    input-send-event, он же единственный, который умеет абсолютные координаты.
    """
    import json

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(QMP_SOCK)
    s.recv(65536)                                        # greeting
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    time.sleep(0.2)
    s.recv(65536)
    for ev in events:
        s.sendall((json.dumps({"execute": "input-send-event",
                               "arguments": {"events": ev}}) + "\n").encode())
        time.sleep(0.05)
        try:
            s.recv(65536)
        except socket.timeout:
            pass
    s.close()


def abs_event(x: int, y: int) -> list:
    return [
        {"type": "abs", "data": {"axis": "x", "value": int(x * ABS_MAX / SCREEN_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * ABS_MAX / SCREEN_H)}},
    ]


def qmp_click(x: int, y: int, double: bool = False, button: str = "left") -> None:
    press = [{"type": "btn", "data": {"down": True, "button": button}}]
    release = [{"type": "btn", "data": {"down": False, "button": button}}]
    events = [abs_event(x, y), press, release]
    if double:
        events += [press, release]
    qmp(events)


def mouse_to(x: int, y: int) -> None:
    """Поставить курсор в (x, y) экрана гостя.

    Гость слушает PS/2-мышь, а она относительная: абсолютных координат нет.
    Поэтому сначала упираем курсор в левый верхний угол (гигантский отрицательный
    сдвиг обрезается по краю экрана), а дальше идём шагами по 4 пикселя.

    Работает только при выключенной акселерации указателя (guest-tools/NOACCEL.REG,
    MouseSpeed=0) — иначе Windows домножает каждый сдвиг и курсор улетает в угол.
    """
    # В пакет PS/2 влезает сдвиг в пределах ±255, поэтому в угол прижимаемся
    # не одним рывком, а серией — с запасом на 1024 пикселя по каждой оси.
    step = 4
    cmds = ["mouse_set 2"] + ["mouse_move -200 -200"] * 12
    for _ in range(x // step):
        cmds.append(f"mouse_move {step} 0")
    if x % step:
        cmds.append(f"mouse_move {x % step} 0")
    for _ in range(y // step):
        cmds.append(f"mouse_move 0 {step}")
    if y % step:
        cmds.append(f"mouse_move 0 {y % step}")
    send(cmds, wait=0.4)


def click(x: int, y: int, double: bool = False) -> None:
    mouse_to(x, y)
    time.sleep(0.3)
    seq = ["mouse_button 1", "mouse_button 0"]
    if double:
        seq += ["mouse_button 1", "mouse_button 0"]
    send(seq, wait=0.3)


def shot(path: str) -> None:
    ppm = "/tmp/qmon-shot.ppm"
    send(f"screendump {ppm}", wait=0.8)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", path],
                   check=True, capture_output=True)
    print(path)


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    action, arg = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else ""
    if action == "cmd":
        print(send(arg))
    elif action == "key":
        key(arg)
    elif action == "type":
        type_text(arg)
    elif action == "enter":
        type_text(arg)
        key("ret")
    elif action == "shot":
        shot(arg or "vm/shots/shot.png")
    elif action in ("click", "dblclick"):
        x, y = (int(v) for v in arg.split(","))
        click(x, y, double=(action == "dblclick"))
    elif action == "move":
        x, y = (int(v) for v in arg.split(","))
        mouse_to(x, y)
    elif action in ("tabclick", "tabmove"):    # планшет: заработает, когда в госте будет USB
        x, y = (int(v) for v in arg.split(","))
        qmp_click(x, y) if action == "tabclick" else qmp([abs_event(x, y)])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
