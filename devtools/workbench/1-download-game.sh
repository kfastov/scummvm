#!/bin/bash
# Скачивает Nancy Drew: The Secret of Shadow Ranch (Steam AppID 572740) из вашей библиотеки.
#
# Игра существует только под Windows, поэтому обычный Steam-клиент на macOS её не покажет.
# steamcmd умеет принудительно качать windows-депоты — файлы данных нам и нужны,
# запускать .exe мы не собираемся, их будет читать ScummVM.
#
# ЗАПУСКАЕТЕ ЭТОТ СКРИПТ ВЫ САМИ: steamcmd спросит пароль от Steam и код Steam Guard.
# Пароль вводите только в это окно терминала.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
STEAM_USER="kfastov"
APPID=572740
GAMEDIR="$ROOT/game"

mkdir -p "$GAMEDIR"

echo "==> Качаю AppID $APPID в $GAMEDIR (платформа: windows)"
echo "==> Сейчас steamcmd попросит пароль и код Steam Guard."
echo

"$ROOT/tools/steamcmd/steamcmd.sh" \
	+@sSteamCmdForcePlatformType windows \
	+force_install_dir "$GAMEDIR" \
	+login "$STEAM_USER" \
	+app_update $APPID validate \
	+quit

echo
echo "==> Готово. Содержимое:"
find "$GAMEDIR" -maxdepth 2 | head -40
echo
echo "==> Теперь скажите Claude «игра скачана» — он соберёт IPA."
