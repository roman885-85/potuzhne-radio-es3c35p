#!/bin/bash
# ---------------------------------------------------------------------------
#  Пересборка прошивки «ПОТУЖНЕ РАДІО» для плати ES3C35P (3,5″, ST77922).
#  Версія — у firmware/VERSION; для оновлення через браузер — PotuzhneRadio-$BOARD-update.bin.
#
#  Збирає ../source/yoRadio власним набором бібліотек із ../build/sketchbook —
#  глобальні бібліотеки Arduino не чіпаються. Готові образи кладе поруч.
# ---------------------------------------------------------------------------
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
B="$ROOT/build"
SKETCH="$ROOT/source/yoRadio"

#  Це окремий проєкт для плати ES3C35P (3,5″, ST77922) — вона й збирається
#  без ключа. Код для ES3C28P лишився (зручно переносити зміни з основного
#  проєкту «powered radio»), зібрати його можна ключем «--board es3c28p».
BOARD="ES3C35P"
for a in "$@"; do
  case "$a" in
    --board=*) BOARD="$(echo "${a#--board=}" | tr '[:lower:]' '[:upper:]')" ;;
    --board) ;;
    es3c35p|ES3C35P) BOARD="ES3C35P" ;;
    es3c28p|ES3C28P) BOARD="ES3C28P" ;;
  esac
done
case "$BOARD" in
  ES3C28P) BP="$B/bp";   BO="$B/out";   BFLAGS="" ;;
  ES3C35P) BP="$B/bp35"; BO="$B/out35"; BFLAGS="-DPR_BOARD_ES3C35P=1" ;;
  *) echo "невідома плата: $BOARD (є ES3C28P, ES3C35P)"; exit 1 ;;
esac
echo ">>> плата $BOARD"

#  Беремо саме шлях, а не коротке ім'я: перевірка -x нижче дивиться на
#  файл, і коротке ім'я вона шукала б у поточній теці, а не в PATH.
CLI="$HOME/bin/arduino-cli"
_p="$(command -v arduino-cli || true)"; [ -n "$_p" ] && CLI="$_p"
[ -x "$CLI" ] || { echo "arduino-cli не знайдено (очікувався в ~/bin)"; exit 1; }
[ -d "$SKETCH" ] || { echo "не знайдено $SKETCH"; exit 1; }

#  Налаштування arduino-cli — під цей комп'ютер. У git їх немає (там абсолютні
#  шляхи), тож на новому Mac створюємо самі.
if [ ! -f "$B/arduino-cli.yaml" ]; then
  mkdir -p "$B/sketchbook"
  cat > "$B/arduino-cli.yaml" <<YAML
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json
directories:
  data: $HOME/Library/Arduino15
  downloads: $HOME/Library/Arduino15/staging
  user: $B/sketchbook
library:
  enable_unsafe_install: true
logging:
  level: warn
YAML
fi

FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,FlashMode=qio,PSRAM=opi,CPUFreq=240,PartitionScheme=huge_app,LoopCore=1,EventsCore=1,DebugLevel=none"

#  Версія й час збірки — у прошивку: сторінка «Оновлення» читає їх із файлу
#  й показує, що саме заливається і чи воно новіше за те, що на радіо.
VER="$(tr -d ' \n\r' < "$HERE/VERSION" 2>/dev/null)"; [ -n "$VER" ] || VER="1.0"
BUILD="$(date '+%d.%m.%Y %H:%M')"
printf '/* Створює firmware/rebuild.sh під час кожної збірки — вручну не правити. */\n#define PR_VERSION "%s"\n#define PR_BUILD   "%s"\n' "$VER" "$BUILD" > "$SKETCH/src/extras/yoBuild.h"
echo ">>> версія $VER, збірка $BUILD"

#  Сторінка радіо: свіжі app.js / app.css — стиснуті в образ файлової системи
if [ -f "$SKETCH/web/app.js" ]; then
  mkdir -p "$SKETCH/data/www"          #  у git теки немає (лише згенеровані .gz) — у свіжій копії її треба створити
  gzip -9 -c "$SKETCH/web/app.js"  > "$SKETCH/data/www/app.js.gz"
  gzip -9 -c "$SKETCH/web/app.css" > "$SKETCH/data/www/app.css.gz"
fi

echo ">>> компіляція (перший запуск — близько 13 хвилин)"
#  --wrap=dns_clear_cache: очистку кешу DNS із ядра Arduino ведемо в обгортку під
#  замком lwIP (src/core/network.cpp) — без неї радіо падало при появі мережі.
"$CLI" --config-file "$B/arduino-cli.yaml" compile \
  --fqbn "$FQBN" --build-path "$BP" --output-dir "$BO" \
  --build-property "compiler.cpp.extra_flags=$BFLAGS" --build-property "compiler.c.extra_flags=$BFLAGS" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=dns_clear_cache" \
  "$SKETCH"

echo ">>> образ файлової системи"
MKSPIFFS=$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/mkspiffs/*/mkspiffs | head -1)
"$MKSPIFFS" -c "$SKETCH/data" -b 4096 -p 256 -s 0x200000 "$BO/yoRadio.spiffs.bin"

echo ">>> образ ресурсів (звуки подій, заставка) — LittleFS у вільних 7,9 МБ"
MKLFS=$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs | sort -V | tail -1)
"$MKLFS" -c "$SKETCH/assets" -b 4096 -p 256 -s 0x7E0000 "$BO/yoRadio.assets.bin" > /dev/null

echo ">>> склейка одного образу"
ESPTOOL=$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool | sort -V | tail -1)
cp "$HOME"/Library/Arduino15/packages/esp32/hardware/esp32/3.3.3/tools/partitions/boot_app0.bin "$BO/"
cd "$BO"
"$ESPTOOL" --chip esp32s3 merge-bin -o PotuzhneRadio-$BOARD-full.bin \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0      yoRadio.ino.bootloader.bin \
  0x8000   yoRadio.ino.partitions.bin \
  0xe000   boot_app0.bin \
  0x10000  yoRadio.ino.bin \
  0x610000 yoRadio.spiffs.bin \
  0x820000 yoRadio.assets.bin

#  Імена — нашого проєкту: те, що бачить власник, не має нагадувати yoRadio.
cp PotuzhneRadio-$BOARD-full.bin                  "$HERE/PotuzhneRadio-$BOARD-full.bin"
cp yoRadio.ino.bin                                 "$HERE/PotuzhneRadio-$BOARD-update.bin"
cp yoRadio.ino.bootloader.bin                      "$HERE/PotuzhneRadio-$BOARD-bootloader.bin"
cp yoRadio.ino.partitions.bin                      "$HERE/PotuzhneRadio-$BOARD-partitions.bin"
cp yoRadio.spiffs.bin                              "$HERE/PotuzhneRadio-$BOARD-files.bin"
cp yoRadio.assets.bin                              "$HERE/PotuzhneRadio-$BOARD-assets.bin"
cp boot_app0.bin                                   "$HERE/boot_app0.bin"
rm -f "$HERE"/yoRadio.ino.bin "$HERE"/yoRadio.ino.bootloader.bin "$HERE"/yoRadio.ino.partitions.bin "$HERE"/yoRadio.spiffs.bin "$HERE"/yoRadio.assets.bin
mkdir -p "$HERE/web" && cp "$SKETCH"/data/www/app.*.gz "$HERE/web/" 2>/dev/null || true   # файли сторінки — поруч із прошивкою
# ELF цієї збірки — щоб потім було чим розшифрувати дамп падіння (команда
# coredump друкує адреси, а вони без ELF тієї самої збірки нічого не варті).
# Тримаємо останні 10: файл важить близько 28 МБ.
if [ -f "$BP/yoRadio.ino.elf" ]; then
  mkdir -p "$HERE/elf"
  EN="$VER"; [ "$BOARD" = "ES3C28P" ] || EN="$BOARD-$VER"
  cp "$BP/yoRadio.ino.elf" "$HERE/elf/$EN.elf"
  ls -1t "$HERE"/elf/*.elf 2>/dev/null | tail -n +11 | xargs -r rm -f
  echo ">>> ELF збережено: elf/$EN.elf"
fi
echo ">>> готово: $BOARD, версія $VER від $BUILD, образи в $HERE"
