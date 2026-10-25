#!/bin/bash
# ---------------------------------------------------------------------------
#  Прошивка «ПОТУЖНЕ РАДІО» в плату ES3C35P (ESP32-S3, 16 МБ flash) по USB.
#
#  У плати немає мікросхеми USB-UART: Type-C іде прямо в ESP32-S3, у режим
#  завантаження вона входить сама (кнопку BOOT тримати не треба).
#  Після запису плата лишається в завантажувачі (на цьому Mac піднятий DTR
#  засувує GPIO0) — тому перезапуск робимо самі: RTS 0→1→0 при DTR=0
#  (tools/es3c35p/con.py --reset).
#
#  Використання:
#     ./flash.sh                 — увесь образ (налаштування й станції — заводські!)
#     ./flash.sh --app-only      — лише програма: мережі, станції й налаштування лишаються
#     ./flash.sh --app-only --assets — те саме й розділ ресурсів (звуки, заставка)
#     ./flash.sh … /dev/cu.usbmodemXXXX — інший порт
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")"
B=ES3C35P

APP_ONLY=0; ASSETS=0; PORT=""
for a in "$@"; do
  case "$a" in
    --app-only) APP_ONLY=1 ;;
    --assets)   ASSETS=1 ;;
    *)          PORT="$a" ;;
  esac
done

ESPTOOL=$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | sort -V | tail -1)
[ -x "$ESPTOOL" ] || { echo "esptool не знайдено (ядро Arduino ESP32)"; exit 1; }
[ -n "$PORT" ] || PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "Порт /dev/cu.usbmodem* не знайдено — плата підключена?"; exit 1; }
echo "esptool: $ESPTOOL"; echo "порт:    $PORT"

#  Мітка прошивки мусить бути для ЦІЄЇ плати: прошивка ES3C28P на ES3C35P
#  керувала б не тими виводами.
F="PotuzhneRadio-$B-update.bin"
[ -f "$F" ] || { echo "немає $F — спершу ./rebuild.sh"; exit 1; }
grep -a -q "POTUZHNE-RADIO-FW|$B|" "$F" || { echo "$F зібрано не для $B — не шию"; exit 1; }

COMMON=(--chip esp32s3 --port "$PORT" --baud 921600 --before default-reset --after no-reset write-flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB)
if [ "$APP_ONLY" = "1" ]; then
  EXTRA=()
  [ "$ASSETS" = "1" ] && EXTRA=(0x8000 "PotuzhneRadio-$B-partitions.bin" 0x820000 "PotuzhneRadio-$B-assets.bin")
  "$ESPTOOL" "${COMMON[@]}" 0xe000 boot_app0.bin 0x10000 "$F" "${EXTRA[@]}"
else
  "$ESPTOOL" "${COMMON[@]}" 0x0 "PotuzhneRadio-$B-full.bin"
fi

echo ">>> перезапуск"
python3 "$(dirname "$0")/../tools/es3c35p/con.py" --reset --wait 3 > /dev/null 2>&1 || true
echo "Готово. Якщо Wi-Fi ще не задано — торкніться екрана радіо й виберіть мережу зі списку."
