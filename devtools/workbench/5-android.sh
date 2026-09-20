#!/bin/bash
# Ставит нашу сборку ScummVM на телефон и заливает данные обеих игр.
#
# Телефон нужно подключить кабелем и включить в нём отладку по USB
# (Настройки → О телефоне → семь раз по «Номер сборки», затем
#  Настройки → Система → Для разработчиков → Отладка по USB).
# При первом подключении телефон спросит разрешение — надо согласиться.
#
#   ./5-android.sh            — всё сразу: установка, данные, конфиг
#   ./5-android.sh install    — только установить APK
#   ./5-android.sh games      — только залить данные игр
#   ./5-android.sh config     — только положить scummvm.ini
#   ./5-android.sh status     — что уже стоит и лежит на телефоне
#
# APK собирается скриптом 6-build-apk.sh.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APK="$ROOT/build-android/src/ScummVM-debug.apk"

# Отладочная сборка ScummVM ставится под своим именем пакета: так она не
# конфликтует с версией из Play Store, и так работает `run-as` — единственный
# способ без root положить конфиг во внутренний каталог приложения.
PKG="org.scummvm.scummvm.debug"

# Каталог приложения на «карте памяти». Именно сюда можно писать через adb и
# именно отсюда ScummVM читает файлы обычными путями, без возни с SAF:
# всё, что лежит в собственном каталоге приложения, доступно ему напрямую.
EXT="/sdcard/Android/data/$PKG/files"

WITCHES_SRC="$ROOT/games/tivola"
NANCY_SRC="$ROOT/game"

step() { echo; echo "==> $*"; }

need_device() {
	local state
	state="$(adb get-state 2>/dev/null || true)"
	if [ "$state" != "device" ]; then
		echo "Телефон не виден (adb get-state: ${state:-нет ответа})."
		echo "Проверьте кабель, отладку по USB и разрешение на телефоне."
		adb devices -l
		exit 1
	fi
	echo "Телефон: $(adb shell getprop ro.product.model | tr -d '\r') "\
"(Android $(adb shell getprop ro.build.version.release | tr -d '\r'), "\
"$(adb shell getprop ro.product.cpu.abi | tr -d '\r'))"
}

do_install() {
	[ -f "$APK" ] || { echo "Нет APK: $APK — соберите его ./6-build-apk.sh"; exit 1; }
	step "Ставлю $(basename "$APK") ($(du -h "$APK" | cut -f1))"
	# -r: поверх имеющейся; -g: сразу выдать запрошенные разрешения.
	adb install -r -g "$APK"

	# Ловушка, стоившая одного пустого прогона: каталог
	# /sdcard/Android/data/<пакет> должен завести САМО приложение. Если его
	# первым создаёт adb, каталог достаётся пользователю shell, и приложение
	# потом не видит собственную папку — ScummVM отвечает «Game data not found».
	# Поэтому один раз запускаем приложение и ждём, пока каталог появится.
	# Внутренние подкаталоги уже можно набивать через adb: там важен только
	# владелец верхнего каталога.
	step "Запускаю приложение, чтобы оно завело свой каталог"
	adb shell am start -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
		-n "$PKG/org.scummvm.scummvm.SplashActivity" > /dev/null
	local i
	for i in 1 2 3 4 5 6 7 8 9 10; do
		adb shell "test -d $EXT" && break
		sleep 2
	done
	adb shell "test -d $EXT" || { echo "Приложение не завело $EXT — запустите ScummVM руками и повторите"; exit 1; }
	# saves/ не заводим: ScummVM создаст его сам при первом сохранении, и тогда
	# каталог будет принадлежать приложению, а не shell.
	adb shell mkdir -p "$EXT/games"
}

# Данные игр кладём в собственный каталог приложения. Альтернатива — общая
# память телефона, но тогда из-за scoped storage ScummVM пришлось бы каждый раз
# показывать системный выбор папки (SAF), и путь в конфиге стал бы saf://...
push_game() {
	local src="$1" dst="$2" name="$3"
	[ -d "$src" ] || { echo "Нет данных игры: $src"; exit 1; }
	step "Заливаю $name ($(du -sh "$src" | cut -f1)) в $dst"
	adb shell mkdir -p "$dst"
	# Точка в конце: копируем содержимое каталога, а не сам каталог.
	adb push "$src/." "$dst/"
}

do_games() {
	push_game "$WITCHES_SRC" "$EXT/games/tivola"  "Семь ведьм"
	push_game "$NANCY_SRC"   "$EXT/games/nancy10" "Nancy Drew"
}

# Конфиг ScummVM на Android лежит во внутренней памяти приложения
# (getFilesDir()/scummvm.ini), куда adb напрямую не пускает. Отладочная сборка
# позволяет обойти это через run-as: команда выполняется от имени приложения.
do_config() {
	step "Кладу scummvm.ini"
	local tmp="$ROOT/build-android/scummvm.ini"
	# Названия — латиницей не из вредности: собрано без FreeType, а встроенный
	# шрифт списка игр кириллицы не знает и рисует её квадратами. На текст
	# внутри самой игры это не влияет — его рисует движок своими шрифтами.
	cat > "$tmp" <<EOF
[scummvm]
gui_browser_show_hidden=false
savepath=$EXT/saves
autosave_period=300
android_saf_dialog_shown=true

[sevenwitches]
engineid=director
gameid=sevenwitches
description=7 Vedm i zakoldovannyy prints (RU)
path=$EXT/games/tivola
language=ru
platform=windows
enable_unsupported_game_warning=false

[nancy10]
engineid=nancy
gameid=nancy10
description=Nancy Drew: The Secret of Shadow Ranch
path=$EXT/games/nancy10
language=en
platform=windows
enable_unsupported_game_warning=false
EOF
	adb push "$tmp" /data/local/tmp/scummvm.ini
	# Каталог files/ заводится при первом обращении приложения к getFilesDir(),
	# то есть после первого запуска; мы приходим раньше — создаём сами.
	adb shell "run-as $PKG mkdir -p files"
	adb shell "run-as $PKG cp /data/local/tmp/scummvm.ini files/scummvm.ini"
	adb shell rm /data/local/tmp/scummvm.ini
	echo "Готово. Обе игры появятся в списке при запуске."
}

do_status() {
	step "Что на телефоне"
	echo "Пакет: $(adb shell pm list packages | grep -c "$PKG" | tr -d '\r') (1 — стоит, 0 — нет)"
	adb shell "ls -d $EXT/games/* 2>/dev/null" || echo "данных игр нет"
	adb shell "du -sh $EXT/games/* 2>/dev/null" || true
	adb shell "run-as $PKG ls -l files/scummvm.ini" 2>/dev/null || echo "конфига нет"
}

case "${1:-all}" in
	install) need_device; do_install ;;
	games)   need_device; do_games ;;
	config)  need_device; do_config ;;
	status)  need_device; do_status ;;
	all)     need_device; do_install; do_games; do_config; do_status ;;
	*)       echo "Не знаю команду «$1». Есть: install, games, config, status, all"; exit 1 ;;
esac
