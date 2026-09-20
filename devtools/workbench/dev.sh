#!/bin/bash
# Один цикл работы над движком: собрать → прогнать → показать разницу.
#
# Раньше это была составная команда символов на четыреста, которую агент
# набирал заново каждый раз: путь, `make -j8`, фильтр ошибок, `cd`, TARGET,
# `4-scummvm.sh`, `grep`, `ls`, хвост лога. За разобранные сессии на такие
# компаунды ушло примерно 430 тысяч символов только на набор.
#
# Здесь форма ответа постоянная и короткая: ошибки сборки (первые пять),
# кадры, НОВЫЕ строки лога относительно прошлого прогона, барьер.
#
#   ./dev.sh              собрать и прогнать 40 секунд
#   ./dev.sh 90           то же, 90 секунд
#   SKIP_BUILD=1 ./dev.sh только прогнать
#   BUILD_ONLY=1 ./dev.sh только собрать
#   TARGET=sevenwitches-win-ru ./dev.sh   другая игра
#
# По умолчанию цель — bashnya-toolbook. Это важно: у `4-scummvm.sh` умолчание
# другое (director-win-fallback), и запуск книги без TARGET трижды «перезапускал
# движок», прежде чем сдаться (ledger/0086).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
SECS="${1:-40}"
export TARGET="${TARGET:-bashnya-toolbook}"
PY="$ROOT/.venv/bin/python"
[ -x "$PY" ] || PY=python3

if [ "${SKIP_BUILD:-0}" != "1" ]; then
	echo "== сборка"
	cd "$ROOT/scummvm-src"
	if ! make -j8 > /tmp/dev-build.log 2>&1; then
		echo "   СБОРКА УПАЛА, первые ошибки:"
		grep -aE "error:|Error [0-9]" /tmp/dev-build.log | head -5 | sed 's/^/     /'
		echo "   полный лог: /tmp/dev-build.log"
		exit 1
	fi
	# Предупреждения своего движка показываем: чужие не наши, их тысячи.
	warn=$(grep -aE "warning:" /tmp/dev-build.log | grep -a "engines/toolbook" | head -3)
	[ -n "$warn" ] && { echo "   предупреждения в engines/toolbook:"; echo "$warn" | sed 's/^/     /'; }
	echo "   собрано: $(ls -la "$ROOT/scummvm-src/scummvm" | awk '{print $5" байт, "$6" "$7" "$8}')"
fi

[ "${BUILD_ONLY:-0}" = "1" ] && exit 0

echo "== прогон ${SECS}s, цель $TARGET"
cd "$ROOT"
# Из вывода скрипта нужны только его собственные отметки: остальное скажет rundiff.
./4-scummvm.sh "$SECS" 2>&1 | grep -aE "^==>" | sed 's/^==> /   /'

echo "== разница с прошлым прогоном"
"$PY" "$ROOT/tools/rundiff.py" "${@:2}"
