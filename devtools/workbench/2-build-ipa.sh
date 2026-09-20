#!/bin/bash
# Собирает ScummVM.ipa с вшитой Nancy Drew: The Secret of Shadow Ranch.
#
# На выходе два файла:
#   ScummVM-nancy10.ipa        — основной: игра появляется в списке сразу после установки
#   ScummVM-nancy10-manual.ipa — запасной: без правки бинаря, конфиг кладётся вручную
#
# Как работает основной вариант:
#   1. Данные игры лежат ВНУТРИ бандла, а ссылка на них в конфиге записана как
#      appbundle:/games/nancy10 — это виртуальный диск, который iOS-порт вешает
#      на .app поверх chroot'а на Documents (ios7_osys_main.cpp). Абсолютный путь
#      не подошёл бы: UUID контейнера iOS выдаёт заново при каждой установке.
#   2. Сам конфиг ScummVM читает из Documents/Preferences, а IPA туда писать не
#      может — контейнер создаётся пустым. Поэтому в бандл добавлен libseed.dylib:
#      dyld вызывает его конструктор до main(), и тот копирует заготовку конфига
#      в Documents при первом запуске.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$ROOT/game"
PRISTINE="$ROOT/build/ScummVM.app"
DYLIB="$ROOT/build/libseed.dylib"
STAGE="$ROOT/build/stage"
APP="$STAGE/Payload/ScummVM.app"
TARGET="nancy10"

[ -d "$PRISTINE" ] || { echo "Нет $PRISTINE"; exit 1; }
[ -f "$DYLIB" ]    || { echo "Нет $DYLIB — соберите его (см. tools/seed.m)"; exit 1; }
plutil -lint "$PRISTINE/Info.plist" >/dev/null || { echo "Info.plist битый"; exit 1; }

# --- 1. Данные игры -----------------------------------------------------------
SRC="$(dirname "$(find "$GAMEDIR" -iname 'ciftree.dat' -print -quit)")"
SRC="$(dirname "$SRC")"   # ciftree.dat лежит в подкаталоге Ciftree/, движку нужен корень
[ -d "$SRC" ] || { echo "ciftree.dat не найден в $GAMEDIR"; exit 1; }
echo "==> Данные игры: $SRC"

rm -rf "$STAGE"
mkdir -p "$APP" "$ROOT/out"
echo "==> Копирую бандл ScummVM"
cp -R "$PRISTINE"/ "$APP"/

echo "==> Копирую игру в бандл"
mkdir -p "$APP/games/$TARGET"
# steamapps — служебные метаданные Steam; .exe/.dll на iOS бесполезны.
rsync -a --exclude='steamapps/' --exclude='*.exe' --exclude='*.dll' \
      "$SRC"/ "$APP/games/$TARGET"/
du -sh "$APP/games/$TARGET"

# Старая ad-hoc подпись после правки бандла недействительна;
# AltStore всё равно переподписывает всё вашим Apple ID.
rm -rf "$APP/_CodeSignature"
find "$STAGE" -name '.DS_Store' -delete

# --- 2. Конфиг ----------------------------------------------------------------
# На iOS конфиг называется Preferences (OSystem_iOS7::getDefaultConfigFileName).
CONFIG="[scummvm]
gui_browser_show_hidden=false

[$TARGET]
engineid=nancy
gameid=$TARGET
description=Nancy Drew: The Secret of Shadow Ranch
language=en
platform=windows
path=appbundle:/games/$TARGET"

echo "$CONFIG" > "$ROOT/out/Preferences"

# --- 3. Запасной IPA (бинарь не тронут) ---------------------------------------
echo "==> Собираю запасной IPA"
rm -f "$ROOT/out/ScummVM-$TARGET-manual.ipa"
(cd "$STAGE" && zip -qryX "$ROOT/out/ScummVM-$TARGET-manual.ipa" Payload)

# --- 4. Основной IPA: заготовка конфига + инъекция dylib ----------------------
echo "==> Добавляю автонастройку"
echo "$CONFIG" > "$APP/Preferences.default"
mkdir -p "$APP/Frameworks"
cp "$DYLIB" "$APP/Frameworks/libseed.dylib"

python3 "$ROOT/tools/machopatch.py" \
	"$APP/ScummVM" "$APP/ScummVM.patched" \
	"@executable_path/Frameworks/libseed.dylib"
mv "$APP/ScummVM.patched" "$APP/ScummVM"
chmod +x "$APP/ScummVM"

# Проверяем, что Apple'овские инструменты читают результат и видят зависимость.
otool -L "$APP/ScummVM" | grep -q 'libseed.dylib' || { echo "инъекция не удалась"; exit 1; }

echo "==> Собираю основной IPA"
rm -f "$ROOT/out/ScummVM-$TARGET.ipa"
(cd "$STAGE" && zip -qryX "$ROOT/out/ScummVM-$TARGET.ipa" Payload)

rm -rf "$STAGE"
echo
echo "==> Готово:"
ls -lh "$ROOT/out/"
