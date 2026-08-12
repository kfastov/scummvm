#!/bin/bash
# Виртуалка Windows 98 SE на QEMU — отладочный стенд для обеих игр.
#
# На Apple Silicon x86-гость идёт через TCG (без ускорения), но для игр
# 1996-2002 года это с запасом: эмулируемый Pentium получается быстрее,
# чем железо, под которое они писались.
#
# ВАЖНО: гостя нельзя перезагружать изнутри (Пуск → Restart), а также командами
# system_reset и loadvm — Windows 98 после сброса внутри одного процесса QEMU
# намертво виснет в реальном режиме на заставке. Перезапускать надо процесс:
# quit в мониторе, затем этот скрипт заново. Свежий процесс грузится всегда.
#
#   ./3-vm.sh install   — первая установка: грузимся с дистрибутива Windows 98
#   ./3-vm.sh run       — обычный запуск (грузимся с диска)
#   ./3-vm.sh run tivola   / bashnya   — запуск с примонтированным CD игры
#   ./3-vm.sh reset     — снести диск виртуалки и начать заново
#
# Дистрибутив Windows 98 SE положите в vm/win98se.iso — его мы не качаем.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VM="$ROOT/vm"
DISK="$VM/win98.qcow2"
INSTALL_ISO="$VM/win98se.iso"
DISK_SIZE=4G
RAM=256          # Windows 98 плохо переносит >512 МБ без правок system.ini

# CD-образы игр: имя -> путь (сконвертированные в plain ISO 2048)
iso_for() {
	case "${1:-}" in
		tivola)  echo "$ROOT/downloaded/iso/tivola.iso" ;;
		bashnya) echo "$ROOT/downloaded/iso/bashnya.iso" ;;
		win98)   echo "$INSTALL_ISO" ;;
		tools)   echo "$VM/tools.iso" ;;
		"")      echo "" ;;
		*)       echo "неизвестный диск: $1 (ожидалось tivola или bashnya)" >&2; exit 1 ;;
	esac
}

mkdir -p "$VM"

case "${1:-run}" in
install)
	if [ ! -f "$INSTALL_ISO" ]; then
		echo "Нет дистрибутива: $INSTALL_ISO"
		echo "Положите туда образ Windows 98 SE и повторите."
		exit 1
	fi
	if [ ! -f "$DISK" ]; then
		echo "==> Создаю диск $DISK на $DISK_SIZE"
		qemu-img create -f qcow2 "$DISK" "$DISK_SIZE" >/dev/null
	fi
	BOOT="order=d"
	CD="$INSTALL_ISO"
	echo "==> Установка Windows 98. Диск виртуалки: 4 ГБ (FAT32, разметить fdisk-ом из установщика)."
	;;
run)
	if [ ! -f "$DISK" ]; then
		echo "Диска виртуалки нет — сначала ./3-vm.sh install"
		exit 1
	fi
	BOOT="order=c"
	CD="$(iso_for "${2:-}")"
	[ -n "$CD" ] && echo "==> CD в приводе: $CD"
	;;
reset)
	rm -f "$DISK"
	echo "==> Диск удалён, можно ставить заново: ./3-vm.sh install"
	exit 0
	;;
*)
	sed -n '2,14p' "$0"
	exit 1
	;;
esac

# -cpu pentium3 — Windows 98 спотыкается на слишком «новых» CPUID.
# -vga cirrus   — под Cirrus GD5446 у Windows 98 есть родные драйверы, DirectDraw заводится.
# -device sb16  — то, что игры 90-х ожидают увидеть как звуковую карту.
# -device pcnet — AMD PCnet, драйвер тоже есть в коробке (нужен, чтобы таскать файлы по сети).
#
# usb-tablet намеренно НЕ подключаем: Windows 98 отдаёт ему указатель, драйвера USB HID
# в чистой системе нет, и мышь перестаёт слушаться вовсе — включая ввод через монитор.
# Остаётся штатная PS/2-мышь, ей и рулит tools/qmon.py.
ARGS=(
	-machine pc,accel=tcg
	-cpu pentium3
	-m "$RAM"
	-vga cirrus
	-audiodev coreaudio,id=snd0
	-device sb16,audiodev=snd0
	-netdev user,id=net0
	-device pcnet,netdev=net0
	-drive "file=$DISK,if=ide,index=0,media=disk,format=qcow2"
	-boot "$BOOT"
	-rtc base=localtime
	-name "Win98 — стенд для старых игр"
	# монитор в unix-сокете: через него tools/qmon.py шлёт клавиши и снимает экран
	-monitor "unix:$VM/monitor.sock,server,nowait"
	# QMP — для ввода: HMP-команды мыши гость игнорирует, работает только input-send-event
	-qmp "unix:$VM/qmp.sock,server,nowait"
	# планшет даёт абсолютные координаты: с ним не нужно угадывать акселерацию указателя
	-usb
	-device usb-tablet
)

# HEADLESS=1 — без окна, только через монитор (удобно для скриптованной установки)
if [ "${HEADLESS:-0}" = "1" ]; then
	ARGS+=(-display none)
fi

# LOADVM=имя — подняться из снимка состояния (savevm в мониторе), а не с нуля.
# Так можно передавать машину «из рук в руки» между headless и окном, не теряя прогресс.
if [ -n "${LOADVM:-}" ]; then
	ARGS+=(-loadvm "$LOADVM")
fi

# Привод создаём всегда, даже пустым: тогда диск можно менять на ходу из монитора
# (change ide1-cd0 /путь/к.iso), не перезапуская гостя.
if [ -n "$CD" ]; then
	ARGS+=(-drive "file=$CD,if=ide,index=2,media=cdrom,readonly=on")
else
	ARGS+=(-drive "if=ide,index=2,media=cdrom")
fi

echo "==> qemu-system-i386 ${ARGS[*]}"
exec qemu-system-i386 "${ARGS[@]}"
