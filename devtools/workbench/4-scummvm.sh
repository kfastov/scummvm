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
[ "$TARGET" = "bashnya-toolbook" ] && ARGS+=(--extrapath="$ROOT/scummvm-src/dists/engine-data")
[ "${DUMP:-0}" = "1" ] && ARGS+=(-u)

echo "==> $TARGET, вслепую, ${RUNTIME}s"

# Безоконный режим изредка не поднимается: лог обрывается сразу после инициализации
# звука, кадров ноль, ошибок в логе нет — похоже на гонку при создании поверхностей
# в драйвере dummy. Гоняться за ней ради стенда незачем, достаточно повтора.
#
# Признак «поднялся» — НЕ первый кадр. У ToolBook разбор книги занимает секунды,
# и проверка «есть ли кадры за 8 секунд» срабатывала ложно, отстреливая здоровый
# движок посреди работы. Настоящая гонка выглядит иначе: лог замирает насовсем.
# Поэтому ждём любого из двух признаков жизни — кадра или движения в логе — и
# перезапускаем, только когда не растёт ни то, ни другое (или процесс умер).
started=$SECONDS
for attempt in 1 2 3; do
	find "$RUNDIR/frames" -name '*.png' -delete
	SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_FRAMEBUFFER_ACCELERATION=0 \
		SDL_AUDIODRIVER=dummy "$SVM" "${ARGS[@]}" "$TARGET" > run.log 2>&1 &
	PID=$!
	alive=1 frozen=0 prev=-1
	for _ in $(seq 1 "${STARTUP_WAIT:-40}"); do
		sleep 1
		kill -0 "$PID" 2>/dev/null || { alive=0; break; }
		[ "$(find "$RUNDIR/frames" -name '*.png' | wc -l)" -gt 0 ] && break
		size=$(wc -c < run.log)
		if [ "$size" -eq "$prev" ]; then
			# Лог стоит. Шесть секунд подряд без единой строки и без кадров —
			# это та самая гонка, а не долгий разбор книги.
			frozen=$((frozen + 1))
			[ "$frozen" -ge 6 ] && { alive=0; break; }
		else
			frozen=0 prev=$size
		fi
	done
	[ "$alive" = 1 ] && break
	echo "==> попытка $attempt: движок не поднялся, перезапускаю"
	kill -9 "$PID" 2>/dev/null || true
	wait "$PID" 2>/dev/null || true
done

# RUNTIME отсчитывается от запуска, а не от конца ожидания: иначе долгий разбор
# книги молча удлинял бы каждый прогон.
remaining=$((RUNTIME - (SECONDS - started)))
[ "$remaining" -gt 0 ] && sleep "$remaining"
# Строго -9: на фатальной ошибке ScummVM уходит в свою отладочную консоль и
# обычный сигнал завершения игнорирует. Один такой прогон висел семь часов и
# успел записать 27 тысяч кадров на 5 гигабайт.
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
pkill -9 -f "scummvm-src/scummvm" 2>/dev/null || true

echo "==> кадров снято: $(ls "$RUNDIR/frames" | wc -l | tr -d ' ')  ($RUNDIR/frames)"
echo "==> лог: $RUNDIR/run.log"
# -a обязательно: в логе CP1251, без него grep считает файл двоичным и молча
# ничего не показывает.
grep -aviE "hardwareinput|antialias|joy_|Checking SDL|Saved screenshot" run.log | tail -12

# Барьер с его операндами — единственное, ради чего обычно читают лог целиком.
# Печатаем сразу, чтобы не открывать файл ради четырёх строк.
barrier=$(grep -an "пока не реализован\|пока не разобран\|неожиданная форма\|не попало в стек" run.log | tail -1)
if [ -n "$barrier" ]; then
	echo "==> барьер:"
	sed -n "${barrier%%:*},+10p" run.log | grep -a "операнд\|ToolBook" | head -11 | sed 's/^/    /'
fi
