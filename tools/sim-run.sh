#!/usr/bin/env bash
# Стенд esp32sim: один сценарий прошивки станции одной командой (T120).
#
# Зачем: проверить FSM/wake/UI без платы. Скрипт запускает эмулятор с образом
# из каталога сборки, раскладывает артефакты в _scratch/sim/<ts>-<сценарий>/
# и сам решает, прошёл ли сценарий (verdict.txt).
#
#   tools/sim-run.sh --scenario boot
#   tools/sim-run.sh --build _scratch/build-t120-sim --scenario boot
#   tools/sim-run.sh --scenario boot -- --realtime
#
# Сценарий — файл kouki-station/sim/scenarios/<имя>.scenario из трёх частей:
#   * заголовок (необязательный), по строке на ключ:
#       `max-seconds: N`         сколько эмуляционных секунд гонять;
#       `nvs: имя`               NVS-образ из sim/nvs/имя.csv (по умолчанию net);
#       `audio: nonsilent`       требовать непустой out.wav (пик >= 1000);
#       `png-min-bytes: N`       порог «экран отрисовался» (по умолчанию 2000);
#       `esp32sim: <аргументы>`  доп. флаги эмулятора для этого сценария
#                                (например `--realtime`); работают и при --all;
#   * тело скрипта эмулятора (формат _scratch/esp32sim/docs/cli.md, «Action
#     scripts»): `3.0 touch 180 180 1`, `5.0 serial state`, `6.0 stop`;
#     относительные пути в `i2s_wav` считаются от корня репозитория (а не от
#     каталога запуска), отсутствующий файл — ошибка стенда, код 3;
#   * блок `expect:` — по строке на проверку:
#       `+ шаблон` — grep -F обязан найти шаблон в run.log;
#       `- шаблон` — обязан НЕ найтись (паника, abort, assert).
# Пустые строки и строки, начинающиеся с `#`, в теле игнорируются.
# verdict.txt начинается с итога, дальше по строке на каждое правило (ok/FAIL).
#
# Параметры:
#   --build DIR        каталог сборки (по умолчанию SIM_BUILD)
#   --scenario NAME|FILE  имя сценария или путь к .scenario (по умолчанию boot)
#   --all              прогнать все сценарии из sim/scenarios/ и свести итог
#   --out DIR          каталог артефактов (по умолчанию _scratch/sim/<ts>-<имя>/)
#   --timeout SEC      предел настенных секунд (по умолчанию SIM_TIMEOUT)
#   -- passthrough     всё после `--` уходит в esp32sim как есть
#
# Коды возврата: 0 — PASS, 1 — FAIL, 2 — неверные аргументы, 3 — нет окружения
# (эмулятор/ROM/сборка/NVS) — это ошибка стенда, а не прошивки.
#
# NVS: образ генерируется из sim/nvs/<имя>.csv, но srv_url и srv_token в нём
# берутся из SIM_SERVER_URL / SIM_SERVER_TOKEN — подставленный CSV пишется в
# _scratch/sim/nvs/ (в git остаётся только демо-шаблон, токен в лог не идёт).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sim-env.sh
. "$SCRIPT_DIR/sim-env.sh"

die() { echo "Ошибка: $*" >&2; exit 2; }

usage() {
    cat <<'TXT'
Использование: tools/sim-run.sh [параметры] [-- аргументы esp32sim]

Параметры:
  --build DIR        каталог сборки прошивки (по умолчанию SIM_BUILD)
  --scenario NAME|FILE  сценарий из sim/scenarios или путь к файлу (по умолчанию boot)
  --all              все сценарии из sim/scenarios/ (или SIM_SCENARIOS): по
                     подкаталогу на сценарий + сводный verdict.txt; exit 0
                     только если все PASS
  --out DIR          каталог артефактов (по умолчанию _scratch/sim/<ts>-<имя>/,
                     для --all — _scratch/sim/<ts>-all/)
  --timeout SEC      предел настенных секунд на сценарий (по умолчанию SIM_TIMEOUT=240)
  -h, --help         эта справка

--all и --scenario взаимоисключающие.

Артефакты в --out: run.log, screen.png, out.wav, verdict.txt (PASS/FAIL: причина).
Заголовок сценария `esp32sim: <аргументы>` передаёт доп. флаги эмулятору только
для этого сценария (работает и под --all); аргументы после `--` главнее.
Окружение: SIM_BIN, SIM_ROM, SIM_BOARD, SIM_BUILD, SIM_NET, KOUKI_SIM_NVS,
SIM_SERVER_URL, SIM_SERVER_TOKEN, SIM_SCENARIOS.

Коды возврата: 0 — PASS, 1 — FAIL, 2 — аргументы, 3 — нет окружения стенда.
TXT
}

BUILD_DIR=""
SCENARIO="boot"
SCENARIO_SET=0
OUT_DIR=""
TIMEOUT_SEC="$SIM_TIMEOUT"
ALL=0
PASSTHROUGH=()

while [ $# -gt 0 ]; do
    case "$1" in
        --build)      [ $# -ge 2 ] || die "--build без значения"; BUILD_DIR="$2"; shift 2 ;;
        --build=*)    BUILD_DIR="${1#*=}"; shift ;;
        --scenario)   [ $# -ge 2 ] || die "--scenario без значения"; SCENARIO="$2"; SCENARIO_SET=1; shift 2 ;;
        --scenario=*) SCENARIO="${1#*=}"; SCENARIO_SET=1; shift ;;
        --all)        ALL=1; shift ;;
        --out)        [ $# -ge 2 ] || die "--out без значения"; OUT_DIR="$2"; shift 2 ;;
        --out=*)      OUT_DIR="${1#*=}"; shift ;;
        --timeout)    [ $# -ge 2 ] || die "--timeout без значения"; TIMEOUT_SEC="$2"; shift 2 ;;
        --timeout=*)  TIMEOUT_SEC="${1#*=}"; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; PASSTHROUGH=("$@"); break ;;
        *)            echo "Ошибка: неизвестный параметр $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$ALL" -eq 1 ] && [ "$SCENARIO_SET" -eq 1 ]; then
    die "--all и --scenario вместе бессмысленны: --all гоняет все сценарии. Выберите одно."
fi

BUILD_DIR="${BUILD_DIR:-$SIM_BUILD}"

# --- --all: все сценарии одним вызовом -------------------------------------
# Каждый сценарий гоняется отдельным процессом этого же скрипта в свой
# подкаталог, поэтому артефакты и вердикты не смешиваются. Сводный verdict.txt
# лежит в корне --out: первая строка — общий итог, дальше по строке на сценарий.
# Код возврата 0 только если PASS-ов столько же, сколько сценариев; 3 — если
# хотя бы один сценарий не смог стартовать из-за неподготовленного стенда.
if [ "$ALL" -eq 1 ]; then
    OUT_ROOT="${OUT_DIR:-$SIM_OUT_ROOT/$(date +%Y%m%d-%H%M%S)-all}"
    mkdir -p "$OUT_ROOT"
    SCEN_DIR="${SIM_SCENARIOS:-$SIM_REPO_ROOT/sim/scenarios}"
    SHOWN=()
    RC_ALL=()
    for f in "$SCEN_DIR"/*.scenario; do
        [ -f "$f" ] || die "нет сценариев в $SCEN_DIR"
        name="$(basename "$f" .scenario)"
        echo "=== $name ==="
        set +e
        bash "${BASH_SOURCE[0]}" --build "$BUILD_DIR" --scenario "$f" \
            --out "$OUT_ROOT/$name" --timeout "$TIMEOUT_SEC" \
            ${PASSTHROUGH[@]+-- "${PASSTHROUGH[@]}"}
        rc=$?
        set -e
        RC_ALL+=("$rc")
        verdict="$(head -1 "$OUT_ROOT/$name/verdict.txt" 2>/dev/null || echo 'FAIL: нет verdict.txt')"
        case "$rc" in
            0) SHOWN+=("ok   $name: PASS") ;;
            3) SHOWN+=("ОШИБКА СТЕНДА $name: $verdict") ;;
            *) SHOWN+=("FAIL $name: $verdict") ;;
        esac
    done
    FAILED=0
    ENVERR=0
    for rc in "${RC_ALL[@]}"; do
        case "$rc" in
            0) ;;
            3) ENVERR=$((ENVERR + 1)); FAILED=$((FAILED + 1)) ;;
            *) FAILED=$((FAILED + 1)) ;;
        esac
    done
    {
        if [ "$FAILED" -eq 0 ]; then
            printf 'PASS\n'
        elif [ "$ENVERR" -ne 0 ]; then
            printf 'FAIL: стенд не готов — сценариев с ошибкой окружения: %s\n' "$ENVERR"
        else
            printf 'FAIL: не прошли сценарии: %s\n' "$FAILED"
        fi
        for line in "${SHOWN[@]}"; do printf '%s\n' "$line"; done
    } > "$OUT_ROOT/verdict.txt"
    echo
    echo "Сводка: $OUT_ROOT/verdict.txt"
    if [ "$FAILED" -eq 0 ]; then
        echo "PASS: все сценарии ($OUT_ROOT)"
        exit 0
    fi
    if [ "$ENVERR" -ne 0 ]; then
        echo "Ошибка стенда: не готовы к запуску сценарии: $ENVERR (см. $OUT_ROOT/verdict.txt)" >&2
        exit 3
    fi
    echo "FAIL: не прошли сценарии: $FAILED (см. $OUT_ROOT/verdict.txt)" >&2
    exit 1
fi

# --- Сценарий --------------------------------------------------------------
if [ -f "$SCENARIO" ]; then
    SCENARIO_FILE="$SCENARIO"
elif [ -f "$SIM_REPO_ROOT/sim/scenarios/$SCENARIO.scenario" ]; then
    SCENARIO_FILE="$SIM_REPO_ROOT/sim/scenarios/$SCENARIO.scenario"
else
    echo "Ошибка: нет сценария «$SCENARIO» ($SIM_REPO_ROOT/sim/scenarios/$SCENARIO.scenario)" >&2
    echo "Есть:" >&2
    ls -1 "$SIM_REPO_ROOT/sim/scenarios"/*.scenario 2>/dev/null | sed 's|.*/|  |; s|\.scenario$||' >&2 || true
    exit 2
fi
SCENARIO_NAME="$(basename "$SCENARIO_FILE")"
SCENARIO_NAME="${SCENARIO_NAME%.scenario}"

# --- Артефакты -------------------------------------------------------------
if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$SIM_OUT_ROOT/$(date +%Y%m%d-%H%M%S)-$SCENARIO_NAME"
fi
mkdir -p "$OUT_DIR"
RUN_LOG="$OUT_DIR/run.log"
VERDICT="$OUT_DIR/verdict.txt"
SCRIPT_TMP="$OUT_DIR/scenario.script"
# Удаляем всё, что этот прогон должен произвести сам: иначе файл от прошлого
# запуска в том же --out будет принят за свежий, и сценарий даст ложный PASS
# (например, эмулятор не смог записать PNG, а проверка нашла старый).
rm -f "$VERDICT" "$OUT_DIR/screen.png" "$OUT_DIR/out.wav"

# --- Разбор сценария -------------------------------------------------------
MAX_SECONDS="$SIM_MAX_SECONDS"
NVS_NAME=""
AUDIO_EXPECT=""
PNG_MIN_BYTES="2000"
SIM_EXTRA_ARGS=()
EXPECT_PLUS=()
EXPECT_MINUS=()
: > "$SCRIPT_TMP"
IN_EXPECT=0
while IFS= read -r line || [ -n "$line" ]; do
    # CR из Windows-файла; grep -F по run.log всё равно получит чистый шаблон.
    line="${line%$'\r'}"
    case "$line" in
        ""|\#*) continue ;;
    esac
    if [ "$IN_EXPECT" -eq 0 ]; then
        case "$line" in
            expect:*) IN_EXPECT=1; continue ;;
        esac
        case "$line" in
            max-seconds:*) MAX_SECONDS="$(echo "${line#max-seconds:}" | tr -d '[:space:]')"; continue ;;
            nvs:*)         NVS_NAME="$(echo "${line#nvs:}" | tr -d '[:space:]')"; continue ;;
            audio:*)       AUDIO_EXPECT="$(echo "${line#audio:}" | tr -d '[:space:]')"; continue ;;
            png-min-bytes:*) PNG_MIN_BYTES="$(echo "${line#png-min-bytes:}" | tr -d '[:space:]')"; continue ;;
            # Доп. флаги эмулятора именно для этого сценария. Нужны затем, что
            # `--` прокидывает аргументы только в один прогон, а под --all
            # сценарию со своим темпом (--realtime) иначе не помочь.
            esp32sim:*)
                read -r -a _extra <<< "${line#esp32sim:}"
                SIM_EXTRA_ARGS+=("${_extra[@]}")
                continue ;;
        esac
        printf '%s\n' "$line" >> "$SCRIPT_TMP"
    else
        case "$line" in
            +*) EXPECT_PLUS+=("${line#+}") ;;
            -*) EXPECT_MINUS+=("${line#-}") ;;
            *)  echo "Ошибка: в блоке expect: строка без «+» или «-»: $line" >&2; exit 2 ;;
        esac
    fi
done < "$SCENARIO_FILE"

if [ "$IN_EXPECT" -eq 1 ]; then
    # Шаблоны начинаются с пробела после «+»: убираем ровно один.
    for i in "${!EXPECT_PLUS[@]}"; do
        EXPECT_PLUS[$i]="${EXPECT_PLUS[$i]# }"
    done
    for i in "${!EXPECT_MINUS[@]}"; do
        EXPECT_MINUS[$i]="${EXPECT_MINUS[$i]# }"
    done
fi

# --- Фикстуры в теле сценария ----------------------------------------------
# Тело сценария лежит в git, поэтому пути в нём относительные (например
# `sim/fixtures/mic/ey_gin.wav`). Эмулятор резолвит их от своего cwd, а сам
# sim-run.sh зовут из любого каталога — отсюда ложный FAIL «эмулятор завершился
# с кодом 2 … No such file or directory», хотя файл на месте (task-25). Поэтому
# каждый путь из `i2s_wav` раскрывается здесь в абсолютный: сначала от корня
# репозитория (так его подразумевают авторы сценариев), затем от каталога
# запуска. Отсутствующий файл — неподготовленный стенд (код 3), а не дефект
# прошивки: иначе ошибка стенда маскируется под FAIL сценария.
SCRIPT_RESOLVED="$SCRIPT_TMP.resolved"
FIXTURE_MISSING=0
: > "$SCRIPT_RESOLVED"
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        *i2s_wav*|*i2s-wav*) ;;
        *) printf '%s\n' "$line" >> "$SCRIPT_RESOLVED"; continue ;;
    esac
    read -r _when _action _port _file _rest <<< "$line" || true
    case "${_action:-}" in
        i2s_wav|i2s-wav) ;;
        *) printf '%s\n' "$line" >> "$SCRIPT_RESOLVED"; continue ;;
    esac
    if [ -z "${_file:-}" ]; then
        printf '%s\n' "$line" >> "$SCRIPT_RESOLVED"; continue
    fi
    _src=""
    case "$_file" in
        /*) [ -f "$_file" ] && _src="$_file" ;;
        *)
            if [ -f "$SIM_REPO_ROOT/$_file" ]; then
                _src="$SIM_REPO_ROOT/$_file"
            elif [ -f "$_file" ]; then
                _src="$(cd "$(dirname "$_file")" && pwd)/$(basename "$_file")"
            fi ;;
    esac
    if [ -z "$_src" ]; then
        echo "Ошибка: нет файла фикстуры «$_file» (искали $SIM_REPO_ROOT/$_file)" >&2
        echo "  Сценарий: $SCENARIO_FILE" >&2
        echo "  Это неподготовленный стенд (код 3), а не FAIL прошивки." >&2
        # verdict.txt пишем и здесь: под --all сводка берёт причину из него, и
        # «FAIL: нет verdict.txt» скрыла бы настоящую причину.
        printf 'ОШИБКА СТЕНДА: нет файла фикстуры «%s» (искали %s/%s)\n' \
            "$_file" "$SIM_REPO_ROOT" "$_file" > "$VERDICT"
        FIXTURE_MISSING=1
        printf '%s\n' "$line" >> "$SCRIPT_RESOLVED"
        continue
    fi
    printf '%s\n' "${line%%"$_file"*}${_src}${line#*"$_file"}" >> "$SCRIPT_RESOLVED"
done < "$SCRIPT_TMP"
if [ "$FIXTURE_MISSING" -ne 0 ]; then
    rm -f "$SCRIPT_RESOLVED"
    exit 3
fi
mv "$SCRIPT_RESOLVED" "$SCRIPT_TMP"

# --- NVS -------------------------------------------------------------------
# Имя из заголовка сценария (`nvs: wake`) выбирает CSV из sim/nvs/; иначе —
# SIM_NVS_CSV (по умолчанию net.csv). Готовый образ можно подсунуть через
# KOUKI_SIM_NVS.
NVS_CSV="$SIM_NVS_CSV"
if [ -n "$NVS_NAME" ]; then
    NVS_CSV="$SIM_REPO_ROOT/sim/nvs/$NVS_NAME.csv"
    KOUKI_SIM_NVS="$SIM_OUT_ROOT/nvs/$NVS_NAME.bin"
fi
# В репозитории CSV — демо-шаблон; адрес и токен сервера берём из окружения
# (SIM_SERVER_URL / SIM_SERVER_TOKEN). Рабочая копия CSV со подставленными
# значениями живёт в _scratch: секреты в git не попадают, а токен не печатаем.
# Файл перезаписываем только при изменении содержимого — иначе mtime дёргался
# бы каждый прогон и NVS пересобирался бы зря.
NVS_CSV_EFFECTIVE="$SIM_OUT_ROOT/nvs/$(basename "$NVS_CSV" .csv).csv"
if [ -f "$NVS_CSV" ]; then
    mkdir -p "$(dirname "$NVS_CSV_EFFECTIVE")"
    awk -F, -v OFS=, -v url="$SIM_SERVER_URL" -v tok="$SIM_SERVER_TOKEN" '
        $1 == "srv_url"   { print "srv_url,data,string," url; next }
        $1 == "srv_token" { print "srv_token,data,string," tok; next }
        { print }
    ' "$NVS_CSV" > "$NVS_CSV_EFFECTIVE.tmp"
    if [ -f "$NVS_CSV_EFFECTIVE" ] && cmp -s "$NVS_CSV_EFFECTIVE.tmp" "$NVS_CSV_EFFECTIVE"; then
        rm -f "$NVS_CSV_EFFECTIVE.tmp"
    else
        mv "$NVS_CSV_EFFECTIVE.tmp" "$NVS_CSV_EFFECTIVE"
    fi
fi
# Образ NVS перегенерируем не только когда его нет или он старее CSV, но и
# когда он негодный: 0 байт (так остаётся после неудачного ручного запуска
# генератора) или размер не кратен сектору 4096. Иначе эмулятор молча
# подсовывает прошивке пустой раздел, сценарий падает «нет адрес получен»,
# и ошибка стенда выглядит как дефект прошивки — коды 3 и 1 путаются.
nvs_bad=0
if [ -f "$KOUKI_SIM_NVS" ]; then
    nvs_size="$(stat -c %s "$KOUKI_SIM_NVS" 2>/dev/null || echo 0)"
    if [ "$nvs_size" -lt 4096 ] || [ $((nvs_size % 4096)) -ne 0 ]; then
        nvs_bad=1
        echo "NVS: образ $KOUKI_SIM_NVS негодный ($nvs_size байт, нужен кратный 4096) — пересоздаю"
        rm -f "$KOUKI_SIM_NVS"
    fi
fi
if [ ! -f "$KOUKI_SIM_NVS" ] || [ "$NVS_CSV_EFFECTIVE" -nt "$KOUKI_SIM_NVS" ]; then
    [ -f "$NVS_CSV_EFFECTIVE" ] || { echo "Ошибка: нет CSV для NVS: $NVS_CSV" >&2; exit 3; }
    [ -x "$SIM_NVS_PY" ] || { echo "Ошибка: нет python генератора NVS: $SIM_NVS_PY" >&2; exit 3; }
    [ -f "$SIM_NVS_GEN" ] || { echo "Ошибка: нет nvs_partition_gen.py: $SIM_NVS_GEN" >&2; exit 3; }
    mkdir -p "$(dirname "$KOUKI_SIM_NVS")"
    echo "NVS: генерирую $KOUKI_SIM_NVS (srv_url=$SIM_SERVER_URL)"
    if ! "$SIM_NVS_PY" "$SIM_NVS_GEN" generate "$NVS_CSV_EFFECTIVE" "$KOUKI_SIM_NVS" "$SIM_NVS_SIZE" >/dev/null; then
        echo "Ошибка: генерация NVS не удалась ($SIM_NVS_PY $SIM_NVS_GEN)" >&2
        exit 3
    fi
fi

# --- Проверка окружения ----------------------------------------------------
# Отдельный код 3: это не дефект прошивки, а неподготовленный стенд, и путать
# это с FAIL сценария нельзя.
missing=0
if [ ! -x "$SIM_BIN" ]; then
    echo "Ошибка: нет эмулятора $SIM_BIN" >&2
    echo "  Пересоберите: cd _scratch/esp32sim && cargo build --release" >&2
    missing=1
fi
if [ ! -f "$SIM_ROM" ]; then
    echo "Ошибка: нет ROM загрузчика $SIM_ROM (SIM_ROM)." >&2
    missing=1
fi
# srmodels.bin тоже обязателен: он уходит в --flash-at 0x820000, и без него
# эмулятор падает с невнятным кодом 2 уже после старта.
for f in "bootloader/bootloader.bin" "partition_table/partition-table.bin" \
         "ota_data_initial.bin" "kouki_station.bin" "kouki_station.elf" \
         "bootloader/bootloader.elf" "srmodels/srmodels.bin"; do
    if [ ! -f "$BUILD_DIR/$f" ]; then
        echo "Ошибка: в сборке нет $BUILD_DIR/$f" >&2
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    echo "  Пересоберите образ: bash tools/build-linux.sh --sim --sim-mic --build-dir $BUILD_DIR" >&2
    exit 3
fi

# --- Запуск ----------------------------------------------------------------
SIM_ARGS=(
    --board "$SIM_BOARD"
    --boot rom
    --rom "$SIM_ROM"
    --bootloader "$BUILD_DIR/bootloader/bootloader.bin"
    --ptable "$BUILD_DIR/partition_table/partition-table.bin"
    --flash-at "0x9000=$KOUKI_SIM_NVS"
    --flash-at "0xf000=$BUILD_DIR/ota_data_initial.bin"
    --flash-at "0x20000=$BUILD_DIR/kouki_station.bin"
    --flash-at "0x820000=$BUILD_DIR/srmodels/srmodels.bin"
    --elf "$BUILD_DIR/kouki_station.elf"
    --elf "$BUILD_DIR/bootloader/bootloader.elf"
    --wifi "ssid=$SIM_WIFI_SSID,psk=$SIM_WIFI_PASS"
    --net "$SIM_NET"
    --console usb
    --no-dump
    --max-seconds "$MAX_SECONDS"
    --script "$SCRIPT_TMP"
    --tft-png "$OUT_DIR/screen.png"
    --wav "$OUT_DIR/out.wav"
)
# Флаги из заголовка сценария (`esp32sim: --realtime`) идут до аргументов из
# `--`: явный запуск из командной строки главнее настройки файла.
if [ ${#SIM_EXTRA_ARGS[@]} -gt 0 ]; then
    SIM_ARGS+=("${SIM_EXTRA_ARGS[@]}")
fi
if [ ${#PASSTHROUGH[@]} -gt 0 ]; then
    SIM_ARGS+=("${PASSTHROUGH[@]}")
fi

echo "Сценарий: $SCENARIO_FILE"
echo "Сборка:   $BUILD_DIR"
echo "NVS:      $KOUKI_SIM_NVS"
echo "Артефакты: $OUT_DIR"

# Таймаут снаружи: зависший эмулятор иначе держит прогон до --max-seconds в
# лучшем случае, а в худшем — вечно (например, если прошивка ушла в цикл).
set +e
timeout --signal=TERM --kill-after=10 "$TIMEOUT_SEC" "$SIM_BIN" "${SIM_ARGS[@]}" > "$RUN_LOG" 2>&1
EMU_EXIT=$?
set -e
if [ "$EMU_EXIT" -eq 124 ] || [ "$EMU_EXIT" -eq 137 ]; then
    echo "[sim-run] эмулятор убит по таймауту ${TIMEOUT_SEC}s" >> "$RUN_LOG"
fi

# --- Вердикт ---------------------------------------------------------------
# В verdict.txt первая строка — итог (PASS или FAIL: причина), дальше по
# строке на каждое правило: что искали и чем кончилось. Так FAIL разбирается
# без чтения run.log целиком.
FAIL_REASONS=()
RULE_LINES=()
if [ "$EMU_EXIT" -eq 124 ] || [ "$EMU_EXIT" -eq 137 ]; then
    FAIL_REASONS+=("таймаут ${TIMEOUT_SEC}s: эмулятор не завершился")
elif [ "$EMU_EXIT" -ne 0 ]; then
    FAIL_REASONS+=("эмулятор завершился с кодом $EMU_EXIT")
fi

if [ "$IN_EXPECT" -eq 0 ]; then
    FAIL_REASONS+=("сценарий не содержит блок expect:")
else
    for pattern in "${EXPECT_PLUS[@]}"; do
        if grep -qF -- "$pattern" "$RUN_LOG"; then
            RULE_LINES+=("ok   есть в логе: $pattern")
        else
            FAIL_REASONS+=("в логе нет «$pattern»")
            RULE_LINES+=("FAIL нет в логе: $pattern")
        fi
    done
    for pattern in "${EXPECT_MINUS[@]}"; do
        if grep -qF -- "$pattern" "$RUN_LOG"; then
            FAIL_REASONS+=("в логе есть запрещённое «$pattern»")
            RULE_LINES+=("FAIL есть в логе (запрещено): $pattern")
        else
            RULE_LINES+=("ok   нет в логе: $pattern")
        fi
    done
fi

# Экран и звук — часть контракта стенда (§2): пустой PNG означает, что UI не
# отрисовался, и молча PASS-ить это нельзя. Порог размера задаётся сценарием
# (png-min-bytes:), дефолт 2000 байт: кадр 360x360 даже пустого экрана больше.
PNG_BYTES=0
if [ -f "$OUT_DIR/screen.png" ]; then
    PNG_BYTES="$(stat -c %s "$OUT_DIR/screen.png" 2>/dev/null || echo 0)"
fi
if [ "$PNG_BYTES" -lt "$PNG_MIN_BYTES" ]; then
    FAIL_REASONS+=("screen.png меньше $PNG_MIN_BYTES байт ($PNG_BYTES) — UI не отрисовался")
    RULE_LINES+=("FAIL screen.png: $PNG_BYTES байт < $PNG_MIN_BYTES")
else
    RULE_LINES+=("ok   screen.png: $PNG_BYTES байт")
fi

# Звук проверяем только если сценарий этого ждёт (`audio: nonsilent`): в
# прогонах без TTS законная тишина, и заваливать их нельзя. Амплитуду считает
# python3 — он есть на сервере, отдельной зависимости не тянем.
if [ "$AUDIO_EXPECT" = "nonsilent" ]; then
    PEAK="$(python3 - "$OUT_DIR/out.wav" <<'PY' 2>/dev/null || echo "err"
import sys, wave
try:
    with wave.open(sys.argv[1], 'rb') as w:
        if w.getsampwidth() != 2:
            print('err'); sys.exit(0)
        data = w.readframes(w.getnframes())
except Exception:
    print('err'); sys.exit(0)
peak = 0
for i in range(0, len(data) - 1, 2):
    v = int.from_bytes(data[i:i+2], 'little', signed=True)
    if v < 0: v = -v
    if v > peak: peak = v
print(peak)
PY
)"
    if [ "$PEAK" = "err" ]; then
        FAIL_REASONS+=("out.wav не разобрать (нет файла или не PCM16)")
        RULE_LINES+=("FAIL out.wav: не разобрать")
    elif [ "$PEAK" -lt 1000 ]; then
        FAIL_REASONS+=("out.wav почти тишина (пик $PEAK)")
        RULE_LINES+=("FAIL out.wav: пик $PEAK < 1000")
    else
        RULE_LINES+=("ok   out.wav: пик $PEAK")
    fi
fi

if [ ${#FAIL_REASONS[@]} -eq 0 ]; then
    {
        printf 'PASS\n'
        for line in "${RULE_LINES[@]}"; do printf '%s\n' "$line"; done
    } > "$VERDICT"
    echo "PASS: $SCENARIO_NAME"
    echo "  лог:     $RUN_LOG"
    echo "  экран:   $OUT_DIR/screen.png"
    echo "  звук:    $OUT_DIR/out.wav"
    exit 0
fi

REASON=""
for r in "${FAIL_REASONS[@]}"; do
    [ -n "$REASON" ] && REASON="$REASON; "
    REASON="$REASON$r"
done
{
    printf 'FAIL: %s\n' "$REASON"
    for line in "${RULE_LINES[@]}"; do printf '%s\n' "$line"; done
} > "$VERDICT"
echo "FAIL: $REASON" >&2
echo "  лог:     $RUN_LOG" >&2
exit 1
