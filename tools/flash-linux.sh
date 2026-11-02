#!/usr/bin/env bash
# Прошивка Kouki Station с Linux (сервер разработки).
#
# Зачем: на сервере плата видна как /dev/ttyACM0, но у процесса агента может
# не быть группы dialout (она появляется только у нового логина). Скрипт сам
# перезапускает себя через `sg dialout`, если группы нет в текущем процессе.
#
#   tools/flash-linux.sh                          # /dev/ttyACM0, сборка из build-<target>
#   tools/flash-linux.sh --port /dev/ttyACM1
#   tools/flash-linux.sh --monitor                # после прошивки смотреть лог
#
# Переменные окружения: IDF_PATH, IDF_TOOLS_PATH, BUILD_DIR (как в build-linux.sh).

set -euo pipefail

# Аргументы нужны целиком для перезапуска через sg: после разбора "$@" уже
# сдвинут, и без копии повторный запуск терял бы --port (однажды так прошили
# не тот порт с дефолтным конфигом).
ORIGINAL_ARGS=("$@")

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
PORT="/dev/ttyACM0"
BAUD=460800
MONITOR=0
BUILD_DIR="${BUILD_DIR:-}"

die() { echo "Ошибка: $*" >&2; exit 1; }

usage() {
    cat <<'TXT'
Использование: tools/flash-linux.sh [параметры]

Параметры:
  -p, --port DEV    последовательный порт (по умолчанию /dev/ttyACM0)
  -b, --baud N      скорость прошивки (по умолчанию 460800)
  --monitor         после прошивки открыть монитор (Ctrl+] — выход)
  -h, --help        эта справка

Окружение: IDF_PATH, IDF_TOOLS_PATH, BUILD_DIR.

Коды возврата: 0 — прошито, 1 — ошибка, 2 — неверные аргументы.
TXT
}

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            PORT="$2"; shift 2 ;;
        -b|--baud)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            BAUD="$2"; shift 2 ;;
        --monitor) MONITOR=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Ошибка: неизвестный параметр $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Группа dialout нужна и на прошивку, и на монитор: без неё open() даёт EACCES.
if ! id -nG | tr ' ' '\n' | grep -qx dialout; then
    if getent group dialout | grep -qE "(^|[:,])$(id -un)(,|$)"; then
        # Пользователь в группе по /etc/group, но текущий процесс её не унаследовал
        # (например, usermod сделали после старта агента) — перезапускаемся через sg.
        echo "Группы dialout нет в процессе, перезапуск через sg dialout…"
        exec sg dialout -c "$(printf '%q ' "$0" "${ORIGINAL_ARGS[@]}")"
    fi
    die "порт $PORT недоступен: нет группы dialout. Выполните: sudo usermod -aG dialout $(id -un) и войдите заново"
fi

[ -e "$PORT" ] || die "нет устройства $PORT (плата подключена? порт можно задать --port)"
[ -f "$IDF_PATH/export.sh" ] || die "нет ESP-IDF в $IDF_PATH"

TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$FIRMWARE/sdkconfig.defaults" | head -1)"
BUILD_DIR="${BUILD_DIR:-$WORKSPACE/_scratch/build-${TARGET:-esp32s3}}"
[ -f "$BUILD_DIR/kouki_station.bin" ] || die "нет сборки в $BUILD_DIR — сначала tools/build-linux.sh"

export IDF_COMPONENT_CACHE_PATH="${IDF_COMPONENT_CACHE_PATH:-$WORKSPACE/_scratch/idf-component-cache}"
export IDF_COMPONENT_STORAGE_PATH="${IDF_COMPONENT_STORAGE_PATH:-$WORKSPACE/_scratch/idf-component-data}"
export CCACHE_DIR="${CCACHE_DIR:-$WORKSPACE/_scratch/ccache}"
export IDF_PATH IDF_TOOLS_PATH
# shellcheck source=/dev/null
. "$IDF_PATH/export.sh" >/tmp/kouki-idf-export.log 2>&1 ||
    { tail -5 /tmp/kouki-idf-export.log >&2; die "export.sh завершился с ошибкой (см. /tmp/kouki-idf-export.log)"; }
command -v idf.py >/dev/null || die "export.sh прошёл, но idf.py не появился в PATH (проверьте IDF_TOOLS_PATH)"

cd "$FIRMWARE"
echo "Порт:   $PORT ($BAUD)"
echo "Сборка: $BUILD_DIR"
# --no-stub отключён намеренно: со stub прошивка идёт в разы быстрее.
idf.py -B "$BUILD_DIR" -p "$PORT" -b "$BAUD" flash || die "flash завершился с ошибкой"

if [ "$MONITOR" -eq 1 ]; then
    echo "Монитор (Ctrl+] — выход)…"
    idf.py -B "$BUILD_DIR" -p "$PORT" monitor || die "monitor завершился с ошибкой"
fi
