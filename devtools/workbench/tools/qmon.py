#!/usr/bin/env python3
"""Управление виртуалкой через монитор QEMU (unix-сокет).

Нужно, чтобы ставить и отлаживать гостя без человека за клавиатурой: команды
идут в монитор, картинка забирается через screendump.

    qmon.py cmd "info block"          — произвольная команда монитора
    qmon.py key ret                   — нажать клавишу (синтаксис sendkey)
    qmon.py type "fdisk"              — напечатать строку посимвольно
    qmon.py enter "format c: /s"      — напечатать строку и нажать Enter
    qmon.py shot vm/shots/01.png      — снимок экрана (ppm конвертируется в png)
    qmon.py look [путь]               — снимок И описание словами вместо картинки
    qmon.py await [секунды]           — ждать, пока экран изменится

`await` заменяет цепочки «щёлкнуть, поспать сорок пять секунд, снять экран,
повторить»: в разобранных сессиях такие цепочки доходили до четырнадцати
одинаковых снимков подряд, и каждый снимок стоил около полутора тысяч токенов.
Здесь ожидание идёт внутри, а наружу выходит одна строка. `look` печатает
описание кадра (размеры, цвета, карта яркости) — этого почти всегда хватает,
чтобы понять, что на экране, не открывая картинку.
"""

import os
import hashlib
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


# Windows домножает относительные сдвиги на скорость указателя. В снимке
# clean-desktop она стоит по умолчанию, и каждый сдвиг удваивается: запрос
# (300,197) уводил курсор в (610,400). Проверено по кадру с видимым курсором.
# Если применить guest-tools/NOACCEL.REG (MouseSpeed=0), коэффициент станет 1.
MOUSE_SCALE = float(os.environ.get("QMON_MOUSE_SCALE", "2"))


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
    x = int(round(x / MOUSE_SCALE))
    y = int(round(y / MOUSE_SCALE))
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


def raw_screen() -> bytes:
    """Сырой ppm с экрана. Для сравнения кадров png не нужен — sips только тратит время."""
    ppm = "/tmp/qmon-await.ppm"
    send(f"screendump {ppm}", wait=0.6)
    with open(ppm, "rb") as f:
        return f.read()


def diff_ratio(a: bytes, b: bytes) -> float:
    """Доля различающихся байтов ppm. Сравниваем с шагом — попиксельно незачем."""
    if len(a) != len(b):
        return 1.0
    step = max(1, len(a) // 20000)
    n = d = 0
    for i in range(0, len(a), step):
        n += 1
        if a[i] != b[i]:
            d += 1
    return d / max(n, 1)


def wait_change(timeout: float = 60.0, path: str = "", settle: float = 1.5,
                thresh: float = 0.005) -> None:
    """Ждать изменения экрана; вернуть одну строку вместо череды снимков.

    После первого изменения ждём `settle` секунд тишины: гость перерисовывает
    окно в несколько заходов, и снимок, взятый на первом же отличии, ловит
    картинку наполовину нарисованной.

    Порог `thresh` обязателен: на экране гостя мигает курсор, и сравнение по
    хешу срабатывало бы каждые полсекунды, ничего не сообщая. Считается доля
    изменившихся точек, а не факт изменения.
    """
    base = raw_screen()
    start = time.time()
    changed_at = None
    last = base
    while time.time() - start < timeout:
        time.sleep(0.7)
        cur = raw_screen()
        if changed_at is None:
            if diff_ratio(base, cur) >= thresh:
                changed_at = time.time()
                last = cur
            continue
        if diff_ratio(last, cur) >= thresh:   # ещё рисует — сдвигаем отсчёт тишины
            changed_at, last = time.time(), cur
        elif time.time() - changed_at >= settle:
            break
    took = time.time() - start
    if changed_at is None:
        print("экран не изменился за %.0fс" % took)
        return
    print("экран изменился через %.0fс (на %.1f%% точек)"
          % (took, 100 * diff_ratio(base, last)))
    if path:
        shot(path)
        look(path, take=False)


def look(path: str = "", take: bool = True) -> None:
    """Снимок и его описание словами."""
    path = path or "vm/shots/look.png"
    if take:
        shot(path)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import frames
        print(frames.describe(path))
    except ImportError as exc:
        print("нет Pillow (%s): .venv/bin/pip install Pillow" % exc)


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
    elif action == "look":
        look(arg)
    elif action == "await":
        thresh = float(sys.argv[3]) / 100 if len(sys.argv) > 3 else 0.005
        wait_change(float(arg or 60), path="vm/shots/await.png", thresh=thresh)
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
