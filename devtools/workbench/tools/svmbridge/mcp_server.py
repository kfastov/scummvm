#!/usr/bin/env python3
"""MCP-сервер к отладочному мосту ScummVM.

Даёт агенту то же, что и `tools/svmdbg`, но инструментами: поднять прогон,
расспросить живой движок, ткнуть мышью, забрать кадр картинкой, снять прогон.
Общий код с командной строкой — в `core.py`, чтобы поведение не разъезжалось.

Транспорт — JSON-RPC 2.0 по stdio, без сторонних зависимостей: сервер должен
подниматься на голом Python, иначе он сам станет источником отказов.

Подключение (в `.mcp.json` проекта):

    {"mcpServers": {"scummvm": {"command": "python3",
     "args": ["tools/svmbridge/mcp_server.py"]}}}
"""

from __future__ import annotations

import base64
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from svmbridge import core  # noqa: E402

PROTOCOL = "2024-11-05"

TOOLS = [
    {
        "name": "svm_start",
        "description": (
            "Поднять ScummVM вслепую с отладочным мостом и дождаться его ответа. "
            "Снимает предыдущий прогон. Возвращает состояние движка."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {"type": "string", "description": "цель в scummvm.ini"},
                "level": {"type": "integer", "description": "уровень отладочного лога"},
                "flags": {"type": "string", "description": "--debugflags, например lingoexec"},
                "framedump_ms": {"type": "integer", "description": "сброс кадров в PNG раз в N мс"},
                "dump_lingo": {"type": "boolean", "description": "выгрузить декомпилированный Lingo"},
                "audio": {"type": "boolean", "description": "со звуком (по умолчанию нет)"},
            },
        },
    },
    {
        "name": "svm_stop",
        "description": "Снять прогон (SIGKILL: на фатальной ошибке движок игнорирует обычный сигнал).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "svm_cmd",
        "description": (
            "Выполнить команды моста по порядку и вернуть их вывод. Свои команды: "
            "state, sprites [all], hit X Y, behaviors CH, castinfo MEM, event CH ИМЯ, "
            "click X Y [мс], down/up/move X Y, key КОД, shot ПУТЬ. Всё остальное уходит "
            "в консоль ScummVM: channels [кадр], cast, var, funcs, bt, disasm, markers, "
            "bpset, debugflag_enable."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "commands": {"type": "array", "items": {"type": "string"}},
            },
            "required": ["commands"],
        },
    },
    {
        "name": "svm_screenshot",
        "description": "Снять текущий кадр и вернуть его картинкой.",
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string", "description": "имя файла в svm-run/shots"}},
        },
    },
    {
        "name": "svm_click",
        "description": "Клик по экрану: перемещение, нажатие, отпускание через hold мс.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "hold": {"type": "integer", "description": "мс между нажатием и отпусканием, по умолчанию 250"},
                "settle": {"type": "number", "description": "сколько секунд подождать после клика"},
                "screenshot": {"type": "boolean", "description": "вернуть кадр после клика"},
            },
            "required": ["x", "y"],
        },
    },
    {
        "name": "svm_wait",
        "description": (
            "Подождать. Если задан until — ждать, пока вывод `state` не совпадёт с этим "
            "регулярным выражением (например 'movie: Datas/MAIN.DIR' или 'label: Fahne')."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "seconds": {"type": "number"},
                "until": {"type": "string"},
                "timeout": {"type": "number"},
            },
        },
    },
    {
        "name": "svm_log",
        "description": "Хвост лога прогона, при желании отфильтрованный регулярным выражением.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "lines": {"type": "integer"},
                "grep": {"type": "string"},
            },
        },
    },
]


# ------------------------------------------------------------ инструменты


def tool_svm_start(args: dict) -> list[dict]:
    st = core.start(
        target=args.get("target", core.DEFAULT_TARGET),
        debuglevel=args.get("level", 1),
        debugflags=args.get("flags", ""),
        framedump_ms=args.get("framedump_ms", 0),
        dump_lingo=args.get("dump_lingo", False),
        audio=args.get("audio", False),
    )
    return [text(f"pid {st.pid}, порт {st.port}, цель {st.target}\n\n{core.command('state')}")]


def tool_svm_stop(args: dict) -> list[dict]:
    return [text("снят" if core.stop() else "нечего снимать")]


def tool_svm_cmd(args: dict) -> list[dict]:
    lines = args["commands"]
    outs = core.commands(lines)
    return [text("\n".join(f"--- {c}\n{o}" for c, o in zip(lines, outs)))]


def tool_svm_screenshot(args: dict) -> list[dict]:
    path = core.shot(args.get("name", ""))
    data = base64.b64encode(path.read_bytes()).decode()
    return [
        {"type": "image", "data": data, "mimeType": "image/png"},
        text(str(path)),
    ]


def tool_svm_click(args: dict) -> list[dict]:
    out = core.command(f"click {args['x']} {args['y']} {args.get('hold', 250)}")
    time.sleep(max(0.0, float(args.get("settle", 1.5))))
    result = [text(f"{out}\n\n{core.command('state')}")]
    if args.get("screenshot"):
        result = tool_svm_screenshot({}) + result
    return result


def tool_svm_wait(args: dict) -> list[dict]:
    until = args.get("until", "")
    if not until:
        time.sleep(float(args.get("seconds", 1.0)))
        return [text(core.command("state"))]

    import re

    rx = re.compile(until)
    deadline = time.time() + float(args.get("timeout", 60.0))
    while time.time() < deadline:
        state = core.command("state")
        if rx.search(state):
            return [text(f"дождались «{until}»:\n{state}")]
        time.sleep(1.0)
    return [text(f"не дождались «{until}» за {args.get('timeout', 60.0)} с:\n{core.command('state')}")]


def tool_svm_log(args: dict) -> list[dict]:
    st = core.read_state()
    path = st.log if st else core.RUNDIR / "bridge-run.log"
    return [text(core.tail(path, args.get("lines", 40), args.get("grep", "")))]


HANDLERS = {
    "svm_start": tool_svm_start,
    "svm_stop": tool_svm_stop,
    "svm_cmd": tool_svm_cmd,
    "svm_screenshot": tool_svm_screenshot,
    "svm_click": tool_svm_click,
    "svm_wait": tool_svm_wait,
    "svm_log": tool_svm_log,
}


def text(s: str) -> dict:
    return {"type": "text", "text": s}


# ------------------------------------------------------------- транспорт


def handle(req: dict) -> dict | None:
    method = req.get("method", "")
    rid = req.get("id")

    if method == "initialize":
        return reply(rid, {
            "protocolVersion": PROTOCOL,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "scummvm-bridge", "version": "1.0.0"},
        })

    if method in ("notifications/initialized", "notifications/cancelled"):
        return None

    if method == "tools/list":
        return reply(rid, {"tools": TOOLS})

    if method == "tools/call":
        name = req["params"]["name"]
        args = req["params"].get("arguments") or {}
        if name not in HANDLERS:
            return error(rid, -32601, f"нет инструмента {name}")
        try:
            return reply(rid, {"content": HANDLERS[name](args)})
        except (core.BridgeError, OSError, ValueError) as err:
            # Ошибку возвращаем содержимым, а не отказом протокола: агенту нужен
            # текст неудачи, а не «инструмент сломался».
            return reply(rid, {"content": [text(f"ошибка: {err}")], "isError": True})

    if method == "ping":
        return reply(rid, {})

    return error(rid, -32601, f"нет метода {method}")


def reply(rid, result: dict) -> dict:
    return {"jsonrpc": "2.0", "id": rid, "result": result}


def error(rid, code: int, message: str) -> dict:
    return {"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}}


def main() -> int:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        resp = handle(req)
        if resp is not None:
            sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
