#!/usr/bin/env python3
"""Проводник по «Семи ведьмам»: довести игру до нужного места и там оставить.

Прогон каждый раз начинается с заставки, а до интересного места идти минуты.
Чтобы не выклацывать этот путь заново при каждой правке движка, он записан
здесь по шагам: «дождаться признака — сделать действие».

    tools/witches.py main            — до главной сцены (город)
    tools/witches.py slots           — до полки с ботинками
    tools/witches.py main --dump     — то же, но с выгрузкой Lingo и картинок
    tools/witches.py main --flags lingoexec --level 3

Признак ожидания — регулярное выражение по выводу `state` моста. Ждать по
признаку, а не по секундам: время до кадра плавает в разы.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from svmbridge import core  # noqa: E402


def wait_for(pattern: str, timeout: float = 180.0, poll: float = 1.5) -> str:
    """Ждёт признака в выводе `state`. Возвращает состояние или падает."""
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            last = core.command("state")
        except (core.BridgeError, OSError) as err:
            last = f"мост не отвечает: {err}"
            time.sleep(poll)
            continue
        if rx.search(last):
            return last
        time.sleep(poll)
    raise core.BridgeError(f"не дождались «{pattern}» за {timeout} с. Состояние:\n{last}")


# Путь по игре. Каждый шаг — (описание, признак ожидания, действие).
# Действие «click X Y» шлётся мосту как есть.
WAYPOINTS = {
    "flags": [
        ("экран выбора языка", r"label: Fahne", None),
    ],
    "slots": [
        ("экран выбора языка", r"label: Fahne", "click 232 215"),
        ("полка с ботинками", r"movie: Datas/LogIn\.DIR", None),
    ],
    "main": [
        ("экран выбора языка", r"label: Fahne", "click 232 215"),
        ("полка с ботинками", r"movie: Datas/LogIn\.DIR", None),
        ("ботинки ожили", None, "click 160 300"),          # первый слот сохранения
        ("заставка города", r"movie: Datas/intro\.DIR", None),
        ("главная сцена", r"frame: 9[0-9][0-9]", None),
    ],
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("where", choices=sorted(WAYPOINTS), help="куда довести")
    ap.add_argument("--dump", action="store_true", help="выгрузить Lingo и картинки")
    ap.add_argument("--flags", default="", help="--debugflags движка")
    ap.add_argument("--level", type=int, default=1)
    ap.add_argument("--framedump", type=int, default=0)
    ap.add_argument("--audio", action="store_true")
    ap.add_argument("--keep", action="store_true", help="не перезапускать, идти от текущего состояния")
    args = ap.parse_args()

    try:
        if not args.keep:
            st = core.start(debuglevel=args.level, debugflags=args.flags,
                            framedump_ms=args.framedump, dump_lingo=args.dump,
                            audio=args.audio)
            print(f"pid {st.pid}, порт {st.port}, лог {st.log}")

        for title, pattern, action in WAYPOINTS[args.where]:
            if pattern:
                wait_for(pattern)
            print(f"  ✓ {title}")
            if action:
                # Поведения кадра создаются не мгновенно после смены кадра;
                # клик впритык к переходу уходит в пустоту.
                time.sleep(2.5)
                core.command(action)

        print(core.command("state"))
    except (core.BridgeError, OSError) as err:
        print(f"ошибка: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
