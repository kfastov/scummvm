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
    # Регистр не фиксируем: путь фильма приходит из файловой системы, где имена
    # набраны заглавными, и совпадать с ним по регистру — ловушка.
    rx = re.compile(pattern, re.IGNORECASE)
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
        ("главная сцена", r"frame: (9[0-9][0-9]|7[0-5][0-9])", None),
    ],
}


# Точки наведения на главной сцене. Кликать надо по спрайту-подсветке (2..8 + 7),
# который скрипт кадра подводит под курсор при наведении, поэтому перед кликом
# обязательно движение мыши и пауза на один кадр.
LOCATIONS = [
    ("Turm", 51, 297),
    ("Kneipe", 597, 252),
    ("Markt", 297, 293),
    # Точка выбирается не по центру прямоугольника: попадание считается по
    # маске картинки-подсветки, и в середине Ecke она прозрачна.
    ("Ecke", 350, 120),
    ("Salon", 144, 221),
    ("Laden", 459, 139),
    ("Spielpl", 73, 136),
]

# Кнопка «обратно в город» на нижней панели (спрайт 75, поведение 27).
# Первое нажатие на любую кнопку панели только произносит подсказку и взводит
# gErstschwatz — действие происходит со второго раза.
BACK_BUTTON = (50, 444)


# Главная сцена (город) живёт в двух фильмах: первый раз до неё доводит
# заставка intro.DIR, дальше игра возвращается в main.DIR. Спрайты-подсветки
# в обоих фильмах стоят на одних и тех же местах.
MAIN_MOVIES = re.compile(r"movie: Datas/(intro|main)\.DIR", re.IGNORECASE)


def at_main() -> bool:
    return bool(MAIN_MOVIES.search(core.command("state")))


def goto_main() -> None:
    """Возвращает игру на главную сцену, откуда бы она ни была."""
    if not at_main():
        # Из локации — кнопкой панели. Первое нажатие только произносит
        # подсказку, действие происходит со второго.
        for _ in range(3):
            core.command(f"click {BACK_BUTTON[0]} {BACK_BUTTON[1]}")
            time.sleep(4)
            if at_main():
                break
    wait_for(MAIN_MOVIES.pattern)
    # Дать городу доиграть свою анимацию: спрайты-подсветки появляются не сразу.
    time.sleep(6)


def tour() -> int:
    """Обходит все семь локаций, снимая кадр в каждой. Возвращает число неудач."""
    failures = 0
    goto_main()

    for name, x, y in LOCATIONS:
        print(f"— {name}: навожу на {x},{y}")
        core.command(f"move {x} {y}")
        time.sleep(1.5)
        core.command(f"click {x} {y}")
        try:
            # Путь фильма локации — Datas\<Имя>\<Имя>.DIR; регистр и
            # наличие каталога не фиксируем.
            state = wait_for(rf"movie: (Datas/)?{name}/", timeout=60)
        except core.BridgeError as err:
            print(f"  ✗ не открылась: {err}")
            failures += 1
            goto_main()
            continue

        time.sleep(6)  # дать доиграть переходу и вступительной анимации
        shot = core.shot(f"tour-{name}.png")
        sound = core.command("sound").strip().splitlines()[0]
        alive = len([ln for ln in core.command("sprites").splitlines()
                     if ln.startswith("ch ") and ln.rstrip().endswith("mouse 1")])
        print(f"  ✓ {state.splitlines()[2]}, интерактивных спрайтов {alive}, {sound}")
        print(f"    кадр: {shot}")

        # Возврат: первое нажатие — подсказка, второе — переход.
        for _ in range(2):
            core.command(f"click {BACK_BUTTON[0]} {BACK_BUTTON[1]}")
            time.sleep(4)
        try:
            goto_main()
        except core.BridgeError:
            print("  ✗ не вернулись в город")
            failures += 1
            return failures

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("where", choices=sorted(WAYPOINTS) + ["tour"] + [n for n, _, _ in LOCATIONS],
                    help="куда довести")
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

        if args.where in [n for n, _, _ in LOCATIONS]:
            if not args.keep:
                for title, pattern, action in WAYPOINTS["main"]:
                    if pattern:
                        wait_for(pattern)
                    print(f"  ✓ {title}")
                    if action:
                        time.sleep(2.5)
                        core.command(action)
            goto_main()
            name, x, y = next(t for t in LOCATIONS if t[0] == args.where)
            core.command(f"move {x} {y}")
            time.sleep(2)
            core.command(f"click {x} {y}")
            print(wait_for(rf"movie: (Datas/)?{name}/", timeout=90))
            time.sleep(6)
            return 0

        if args.where == "tour":
            if not args.keep:
                for title, pattern, action in WAYPOINTS["main"]:
                    if pattern:
                        wait_for(pattern)
                    print(f"  ✓ {title}")
                    if action:
                        time.sleep(2.5)
                        core.command(action)
            failures = tour()
            print("обход закончен без сбоев" if not failures else f"сбоев: {failures}")
            return 1 if failures else 0

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
