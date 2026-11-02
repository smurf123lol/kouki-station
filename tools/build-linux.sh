#!/usr/bin/env bash
# Сборка прошивки Kouki Station на Linux (сервер разработки).
#
# Зачем: на сервере нет Docker и PowerShell, но есть ESP-IDF 5.5.1 в
# /srv/kouki-workspace/_scratch/esp-idf и тулчейн в .../esp-tools. Скрипт
# активирует окружение export.sh и собирает образ в отдельный каталог, чтобы
# не мусорить в репозитории (firmware/build не трогаем).
#
#   tools/build-linux.sh
#   tools/build-linux.sh --secret        # взять Wi-Fi и адрес сервера из firmware/sdkconfig.secret
#   tools/build-linux.sh --clean         # пересобрать с нуля
#
# Переменные окружения:
#   IDF_PATH       путь к ESP-IDF  (по умолчанию <воркспейс>/_scratch/esp-idf)
#   IDF_TOOLS_PATH путь к тулчейну (по умолчанию <воркспейс>/_scratch/esp-tools)
#   BUILD_DIR      каталог сборки  (по умолчанию <воркспейс>/_scratch/build-<target>)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE="$(cd "$REPO_ROOT/.." && pwd)"
FIRMWARE="$REPO_ROOT/firmware"

IDF_PATH="${IDF_PATH:-$WORKSPACE/_scratch/esp-idf}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$WORKSPACE/_scratch/esp-tools}"
BUILD_DIR="${BUILD_DIR:-}"
USE_SECRET=0
CLEAN=0

die() { echo "Ошибка: $*" >&2; exit 1; }

usage() {
    cat <<'TXT'
Использование: tools/build-linux.sh [параметры]

Параметры:
  --secret        добавить firmware/sdkconfig.secret к sdkconfig.defaults
                  (Wi-Fi, адрес сервера, токен; файл в git не входит)
  --clean         удалить каталог сборки перед сборкой
  --build-dir DIR собрать в DIR (по умолчанию _scratch/build-<target>)
  -h, --help      эта справка

Окружение: IDF_PATH, IDF_TOOLS_PATH, BUILD_DIR.

Коды возврата: 0 — собрано, 1 — ошибка сборки, 2 — неверные аргументы.
TXT
}

while [ $# -gt 0 ]; do
    case "$1" in
        --secret) USE_SECRET=1; shift ;;
        --clean) CLEAN=1; shift ;;
        --build-dir)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            BUILD_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Ошибка: неизвестный параметр $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -f "$IDF_PATH/export.sh" ] || die "нет ESP-IDF в $IDF_PATH (ожидается export.sh). Укажите IDF_PATH."
[ -d "$IDF_TOOLS_PATH/tools" ] || die "нет тулчейна в $IDF_TOOLS_PATH. Запустите install.sh (см. T101)."
[ -f "$FIRMWARE/CMakeLists.txt" ] || die "нет $FIRMWARE/CMakeLists.txt"

TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$FIRMWARE/sdkconfig.defaults" | head -1)"
[ -n "$TARGET" ] || die "не нашёл CONFIG_IDF_TARGET в firmware/sdkconfig.defaults"
BUILD_DIR="${BUILD_DIR:-$WORKSPACE/_scratch/build-$TARGET}"

# Кэш менеджера компонентов и ccache — в воркспейс, а не в $HOME: домашний
# каталог на сервере общий с другими задачами и его не хочется засорять.
export IDF_COMPONENT_CACHE_PATH="${IDF_COMPONENT_CACHE_PATH:-$WORKSPACE/_scratch/idf-component-cache}"
export IDF_COMPONENT_STORAGE_PATH="${IDF_COMPONENT_STORAGE_PATH:-$WORKSPACE/_scratch/idf-component-data}"
export CCACHE_DIR="${CCACHE_DIR:-$WORKSPACE/_scratch/ccache}"
mkdir -p "$IDF_COMPONENT_CACHE_PATH" "$IDF_COMPONENT_STORAGE_PATH" "$CCACHE_DIR"

# export.sh читает IDF_PATH и IDF_TOOLS_PATH из окружения, поэтому переменные
# нужно именно экспортировать, а не просто присвоить: иначе idf.py не появится
# в PATH (симптом — «idf.py: command not found» после успешного source).
export IDF_PATH IDF_TOOLS_PATH
# shellcheck source=/dev/null
. "$IDF_PATH/export.sh" >/tmp/kouki-idf-export.log 2>&1 ||
    { tail -5 /tmp/kouki-idf-export.log >&2; die "export.sh завершился с ошибкой (см. /tmp/kouki-idf-export.log)"; }
command -v idf.py >/dev/null || die "export.sh прошёл, но idf.py не появился в PATH (проверьте IDF_TOOLS_PATH)"

cd "$FIRMWARE"

if [ "$CLEAN" -eq 1 ]; then
    # Каталог сборки наш (в _scratch), поэтому удаление безопасно.
    rm -rf "$BUILD_DIR"
fi

DEFAULTS="sdkconfig.defaults"
if [ "$USE_SECRET" -eq 1 ]; then
    [ -f "$FIRMWARE/sdkconfig.secret" ] || die "нет firmware/sdkconfig.secret (--secret без него бессмыслен)"
    DEFAULTS="sdkconfig.defaults;sdkconfig.secret"
fi

# SDKCONFIG_DEFAULTS читается только при создании sdkconfig: если файл уже есть
# в каталоге сборки, смена defaults (первый запуск с --secret) молча игнорируется,
# и в образ попадают дефолты «home»/старый адрес. Поэтому сверяем непустые
# значения из defaults с тем, что лежит в sdkconfig, и пересоздаём файл при
# расхождении. Сравниваем только SSID и URL: они не секреты, а пароль/токен
# читать в лог незачем.
SDKCONFIG="$BUILD_DIR/sdkconfig"
if [ -f "$SDKCONFIG" ]; then
    DEFAULTS_FILES="${DEFAULTS//;/ }"
    expected_ssid="$(sed -n 's/^CONFIG_KOUKI_WIFI_SSID="\(.*\)"/\1/p' $DEFAULTS_FILES | tail -1)"
    expected_url="$(sed -n 's/^CONFIG_KOUKI_SERVER_URL="\(.*\)"/\1/p' $DEFAULTS_FILES | tail -1)"
    stale=0
    if [ -n "$expected_ssid" ] && ! grep -qF "CONFIG_KOUKI_WIFI_SSID=\"$expected_ssid\"" "$SDKCONFIG"; then
        stale=1
    fi
    if [ -n "$expected_url" ] && ! grep -qF "CONFIG_KOUKI_SERVER_URL=\"$expected_url\"" "$SDKCONFIG"; then
        stale=1
    fi
    if [ "$stale" -eq 1 ]; then
        echo "sdkconfig в $BUILD_DIR не совпадает с defaults: пересоздаю"
        rm -f "$SDKCONFIG"
    fi
fi

echo "ESP-IDF:  $IDF_PATH"
echo "Тулчейн:  $IDF_TOOLS_PATH"
echo "Сборка:   $BUILD_DIR"
echo "Цель:     $TARGET"
echo "Defaults: $DEFAULTS"

# SDKCONFIG держим рядом со сборкой, а не в firmware/: idf.py по умолчанию
# кладёт его в каталог проекта, и тогда «сборка с секретами» и «сборка без»
# делят один файл — ровно та путаница, из-за которой образ ушёл без Wi-Fi.
idf.py -B "$BUILD_DIR" -D "SDKCONFIG=$SDKCONFIG" -D "SDKCONFIG_DEFAULTS=$DEFAULTS" build || die "idf.py build завершился с ошибкой"

IMAGE="$BUILD_DIR/kouki_station.bin"
[ -f "$IMAGE" ] || die "сборка прошла, но нет $IMAGE"
echo "Готово: $IMAGE ($(stat -c %s "$IMAGE") байт)"
echo "Прошить: tools/flash-linux.sh --port /dev/ttyACM0"
