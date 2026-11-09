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
# Дерево задачи (git worktree) лежит в _scratch/wt/<имя>, поэтому воркспейс
# считаем от общего каталога git: иначе _scratch, ESP-IDF и тулчейн искались бы
# внутри wt/ и сборка падала на «нет ESP-IDF».
COMMON_GIT="$(git -C "$REPO_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
if [ -n "$COMMON_GIT" ]; then
    WORKSPACE="$(cd "$(dirname "$(dirname "$COMMON_GIT")")" && pwd)"
else
    WORKSPACE="$(cd "$REPO_ROOT/.." && pwd)"
fi
FIRMWARE="$REPO_ROOT/firmware"

IDF_PATH="${IDF_PATH:-$WORKSPACE/_scratch/esp-idf}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$WORKSPACE/_scratch/esp-tools}"
BUILD_DIR="${BUILD_DIR:-}"
USE_SECRET=0
CLEAN=0
# Дополнительные defaults копим списком: --sim и --extra-defaults (и любой
# --sim-mic в будущем) не должны затирать друг друга. Раньше оба писали в одну
# переменную, и второй флаг молча выбрасывал первый — образ получался либо без
# тестовых команд консоли, либо без sim-микрофона, без единой ошибки в выводе.
EXTRA_DEFAULTS=()
add_defaults() {
    # Одинаковый файл дважды не добавляем: idf.py такого не любит, а пользы нет.
    local f
    for f in "${EXTRA_DEFAULTS[@]+"${EXTRA_DEFAULTS[@]}"}"; do
        [ "$f" = "$1" ] && return 0
    done
    EXTRA_DEFAULTS+=("$1")
}

die() { echo "Ошибка: $*" >&2; exit 1; }

usage() {
    cat <<'TXT'
Использование: tools/build-linux.sh [параметры]

Параметры:
  --secret        добавить firmware/sdkconfig.secret к sdkconfig.defaults
                  (Wi-Fi, адрес сервера, токен; файл в git не входит)
  --clean         удалить каталог сборки перед сборкой
  --build-dir DIR собрать в DIR (по умолчанию _scratch/build-<target>)
  --sim           добавить sim/sdkconfig.sim.defaults (тестовые команды консоли;
                  образ для стенда esp32sim, см. docs/sim.md)
  --sim-mic       добавить sim/sdkconfig.sim-mic.defaults (подмена микрофона из
                  I2S-WAV; нужен сценариям с голосом, см. docs/sim.md).
                  Флаги --sim, --sim-mic и --extra-defaults складываются: все
                  файлы попадают в SDKCONFIG_DEFAULTS в порядке указания,
                  побеждает более поздний.
  --extra-defaults FILE  добавить произвольный файл к sdkconfig.defaults
  -h, --help      эта справка

Окружение: IDF_PATH, IDF_TOOLS_PATH, BUILD_DIR.

Коды возврата: 0 — собрано, 1 — ошибка сборки, 2 — неверные аргументы.
TXT
}

while [ $# -gt 0 ]; do
    case "$1" in
        --secret) USE_SECRET=1; shift ;;
        --clean) CLEAN=1; shift ;;
        --sim) add_defaults "$REPO_ROOT/sim/sdkconfig.sim.defaults"; shift ;;
        --sim-mic) add_defaults "$REPO_ROOT/sim/sdkconfig.sim-mic.defaults"; shift ;;
        --extra-defaults)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            add_defaults "$2"; shift 2 ;;
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
IDF_EXPORT_LOG="${TMPDIR:-/tmp}/kouki-idf-export-$$.log"
. "$IDF_PATH/export.sh" >"$IDF_EXPORT_LOG" 2>&1 ||
    { tail -5 "$IDF_EXPORT_LOG" >&2; die "export.sh завершился с ошибкой (см. $IDF_EXPORT_LOG)"; }
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
# Дополнительные файлы defaults (--sim / --sim-mic / --extra-defaults) идут
# последними, в порядке указания флагов: в SDKCONFIG_DEFAULTS побеждает более
# позднее значение, поэтому sim-настройки (CONFIG_KOUKI_TEST_CONSOLE_CMDS,
# CONFIG_KOUKI_SIM_MIC) перекрывают базовые, если разойдутся.
for f in "${EXTRA_DEFAULTS[@]+"${EXTRA_DEFAULTS[@]}"}"; do
    # Пути к sim/sdkconfig.*.defaults абсолютные (вне firmware/), idf.py
    # принимает такие пути в списке defaults как есть.
    [ -f "$f" ] || die "нет файла defaults: $f"
    DEFAULTS="$DEFAULTS;$f"
done

# SDKCONFIG_DEFAULTS читается только при создании sdkconfig: если файл уже есть
# в каталоге сборки, смена defaults (первый запуск с --secret) молча игнорируется,
# и в образ попадают дефолты «home»/старый адрес. Поэтому сверяем непустые
# значения из defaults с тем, что лежит в sdkconfig, и пересоздаём файл при
# расхождении. Сравниваем только SSID и URL: они не секреты, а пароль/токен
# читать в лог незачем.
SDKCONFIG="$BUILD_DIR/sdkconfig"
if [ -f "$SDKCONFIG" ]; then
    IFS=';' read -r -a DEFAULTS_ARRAY <<< "$DEFAULTS"
    expected_ssid="$(sed -n 's/^CONFIG_KOUKI_WIFI_SSID="\(.*\)"/\1/p' "${DEFAULTS_ARRAY[@]}" | tail -1)"
    expected_url="$(sed -n 's/^CONFIG_KOUKI_SERVER_URL="\(.*\)"/\1/p' "${DEFAULTS_ARRAY[@]}" | tail -1)"
    stale=0
    if [ -n "$expected_ssid" ] && ! grep -qF "CONFIG_KOUKI_WIFI_SSID=\"$expected_ssid\"" "$SDKCONFIG"; then
        stale=1
    fi
    if [ -n "$expected_url" ] && ! grep -qF "CONFIG_KOUKI_SERVER_URL=\"$expected_url\"" "$SDKCONFIG"; then
        stale=1
    fi
    # Флаги стенда тоже сверяем (T120): каталог, собранный с --sim, нельзя
    # молча переиспользовать для боевого образа (в нём останутся
    # test_seed_history/test_mock_done), и наоборот — сценарии стенда сломаются
    # без этого флага. То же с --sim-mic: образ без sim-микрофона должен
    # пересобраться, когда флаг добавили (и наоборот), иначе кривой sdkconfig
    # молча переживает смену флагов — ровно тот дефект, из-за которого флаги
    # defaults раньше терялись. Значение берём из defaults: есть строка —
    # ожидаем y, нет — ожидаем, что в sdkconfig не y.
    for flag in CONFIG_KOUKI_TEST_CONSOLE_CMDS CONFIG_KOUKI_SIM_MIC; do
        expected_flag=""
        for f in "${DEFAULTS_ARRAY[@]}"; do
            v="$(sed -n "s/^$flag=\(.*\)/\1/p" "$f" 2>/dev/null | tail -1)"
            [ -n "$v" ] && expected_flag="$v"
        done
        actual_flag="$(sed -n "s/^$flag=\(.*\)/\1/p" "$SDKCONFIG" 2>/dev/null | tail -1)"
        if [ "${expected_flag:-n}" != "${actual_flag:-n}" ]; then
            stale=1
        fi
    done
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
