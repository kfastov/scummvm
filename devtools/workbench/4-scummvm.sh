#!/bin/bash
# Прогон игры в ScummVM без окна, с выгрузкой кадров на диск.
#
# Окно на переднем плане мешает работать, поэтому запускаем вслепую. Рецепт
# собран по кусочкам, каждый из них обязателен:
#
#   SDL_VIDEODRIVER=dummy          — окна нет вовсе
#   SDL_RENDER_DRIVER=software     — иначе SDL не создаёт рендерер поверх dummy
#   SDL_FRAMEBUFFER_ACCELERATION=0   и ScummVM падает на «Parameter 'renderer' is invalid»
#   -g surfacesdl                  — OpenGL в этом режиме недоступен
#   framedump_ms в конфиге         — наша правка в backends/modular-backend.cpp:
#                                    сбрасывает кадр в PNG раз в N миллисекунд
#
# Нужна своя сборка: scummvm-src/scummvm (в кассовой сборке правки нет).
#
#   ./4-scummvm.sh            — прогон 40 секунд
#   ./4-scummvm.sh 90         — прогон 90 секунд
#   DUMP=1 ./4-scummvm.sh     — плюс выгрузка декомпилированного Lingo в svm-run/dumps
#   DEBUG=3 ./4-scummvm.sh    — подробнее лог

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SVM="$ROOT/scummvm-src/scummvm"
TARGET="${TARGET:-director-win-fallback}"
RUNDIR="$ROOT/svm-run"
RUNTIME="${1:-40}"

[ -x "$SVM" ] || { echo "нет своей сборки: $SVM (см. NOTES.md)"; exit 1; }

mkdir -p "$RUNDIR/frames" "$RUNDIR/dumps"
# find, а не rm по маске: подвисший прогон однажды накопил 27 тысяч кадров,
# и список аргументов перестал влезать в командную строку.
find "$RUNDIR/frames" -name '*.png' -delete
cd "$RUNDIR"

ARGS=(-c scummvm.ini -g surfacesdl -d "${DEBUG:-1}")
[ "${DUMP:-0}" = "1" ] && ARGS+=(-u)

echo "==> $TARGET, вслепую, ${RUNTIME}s"

# Безоконный режим изредка не поднимается: лог обрывается сразу после инициализации
# звука, кадров ноль, ошибок в логе нет — похоже на гонку при создании поверхностей
# в драйвере dummy. Гоняться за ней ради стенда незачем, достаточно повтора.
for attempt in 1 2 3; do
	find "$RUNDIR/frames" -name '*.png' -delete
	SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_FRAMEBUFFER_ACCELERATION=0 \
		SDL_AUDIODRIVER=dummy "$SVM" "${ARGS[@]}" "$TARGET" > run.log 2>&1 &
	PID=$!
	sleep 8
	if [ "$(find "$RUNDIR/frames" -name '*.png' | wc -l)" -gt 0 ]; then
		break
	fi
	echo "==> попытка $attempt: движок не поднялся, перезапускаю"
	kill -9 "$PID" 2>/dev/null || true
	wait "$PID" 2>/dev/null || true
done

sleep "$RUNTIME"
# Строго -9: на фатальной ошибке ScummVM уходит в свою отладочную консоль и
# обычный сигнал завершения игнорирует. Один такой прогон висел семь часов и
# успел записать 27 тысяч кадров на 5 гигабайт.
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
pkill -9 -f "scummvm-src/scummvm" 2>/dev/null || true

echo "==> кадров снято: $(ls "$RUNDIR/frames" | wc -l | tr -d ' ')  ($RUNDIR/frames)"
echo "==> лог: $RUNDIR/run.log"
grep -viE "hardwareinput|antialias|joy_|Checking SDL|Saved screenshot" run.log | tail -12
