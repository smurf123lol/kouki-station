#!/usr/bin/env bash
# Окружение стенда esp32sim для станции Kouki (T120).
#
# Зачем: все пути и дефолты стенда — в одном файле, чтобы sim-run.sh не
# повторял их и чтобы человек мог прогнать сценарий, поправив одну переменную.
#
# Использование (файл только подключают, сам он ничего не запускает):
#   . tools/sim-env.sh
#   "$SIM_BIN" ... "$SIM_BOARD" ...
#
# Любая переменная переопределяется окружением:
#   SIM_BIN=... SIM_NET=nat,epoch=1700000000 bash tools/sim-run.sh --scenario boot

# Не `set -e`: файл подключают, и внезапный выход из чужого скрипта — сюрприз.
set -u

SIM_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_REPO_ROOT="$(cd "$SIM_ENV_DIR/.." && pwd)"

# Дерево задачи (git worktree) лежит в _scratch/wt/<имя>, поэтому воркспейс
# считаем от общего каталога git — как в tools/build-linux.sh.
SIM_COMMON_GIT="$(git -C "$SIM_REPO_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
if [ -n "$SIM_COMMON_GIT" ]; then
    SIM_WORKSPACE="$(cd "$(dirname "$(dirname "$SIM_COMMON_GIT")")" && pwd)"
else
    SIM_WORKSPACE="$(cd "$SIM_REPO_ROOT/.." && pwd)"
fi

export SIM_WORKSPACE SIM_REPO_ROOT

# --- Что и чем запускать ---------------------------------------------------
# esp32sim 0.3.0 (собран из _scratch/esp32sim, git 6275250).
export SIM_BIN="${SIM_BIN:-$SIM_WORKSPACE/_scratch/esp32sim/target/release/esp32sim}"
# ROM загрузчика ESP32-S3 rev0 — без него эмулятор не стартует.
export SIM_ROM="${SIM_ROM:-$SIM_WORKSPACE/_scratch/esp-tools/tools/esp-rom-elfs/20241011/esp32s3_rev0_rom.elf}"
# waveshare-lcd185c = железо станции (ST77916 360x360, CST816S, ES8311/ES7210).
export SIM_BOARD="${SIM_BOARD:-waveshare-lcd185c}"

# --- Состояние -------------------------------------------------------------
# Каталог сборки sim-образа; по умолчанию отдельный, чтобы не смешать с боевой.
export SIM_BUILD="${SIM_BUILD:-$SIM_WORKSPACE/_scratch/build-kouki-station-sim}"
# Сеть эмулятора: NAT + фиксированные часы (иначе время в NVS/логах скачет).
export SIM_NET="${SIM_NET:-nat,epoch=1794200000}"
# Куда станция ходит за /api/voice — локальный сервер Kouki. Эти два значения
# sim-run.sh подставляет в NVS-образ (srv_url/srv_token), сам CSV в репозитории
# остаётся демо-шаблоном; рабочая копия — в _scratch.
export SIM_SERVER_URL="${SIM_SERVER_URL:-http://192.168.1.157:5300}"
export SIM_SERVER_TOKEN="${SIM_SERVER_TOKEN:-kouki-demo}"
# Wi-Fi виртуальной точки доступа эмулятора (см. --wifi в docs/cli.md).
export SIM_WIFI_SSID="${SIM_WIFI_SSID:-kouki-sim}"
export SIM_WIFI_PASS="${SIM_WIFI_PASS:-kouki-pass}"

# --- Артефакты и NVS -------------------------------------------------------
# Все прогоны — в _scratch/sim/<таймстамп>-<сценарий>/, ничего в корнях репо.
export SIM_OUT_ROOT="${SIM_OUT_ROOT:-$SIM_WORKSPACE/_scratch/sim}"
# Каталог сценариев для --all (по умолчанию репозиторный). Переменная нужна,
# чтобы прогнать набор из другого каталога (например, заведомо сломанный
# сценарий во временной копии) и не трогать файлы в git.
export SIM_SCENARIOS="${SIM_SCENARIOS:-$SIM_REPO_ROOT/sim/scenarios}"
# Образ NVS с конфигурацией станции (namespace kouki_config). Если файла нет,
# sim-run.sh сгенерирует его из kouki-station/sim/nvs/net.csv.
export KOUKI_SIM_NVS="${KOUKI_SIM_NVS:-$SIM_OUT_ROOT/nvs/net.bin}"
export SIM_NVS_CSV="${SIM_NVS_CSV:-$SIM_REPO_ROOT/sim/nvs/net.csv}"
# Генератор NVS из ESP-IDF 5.5.1 (тот же python, что у idf.py).
export SIM_NVS_PY="${SIM_NVS_PY:-$SIM_WORKSPACE/_scratch/esp-tools/python_env/idf5.5_py3.14_env/bin/python}"
export SIM_NVS_GEN="${SIM_NVS_GEN:-$SIM_WORKSPACE/_scratch/esp-idf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py}"

# Размер раздела nvs из partitions.csv станции: 0x6000.
export SIM_NVS_SIZE="${SIM_NVS_SIZE:-0x6000}"
# Сколько секунд эмуляционного времени гонять сценарий, если он не задал своё.
export SIM_MAX_SECONDS="${SIM_MAX_SECONDS:-30}"
# Предел настенных секунд: зависший эмулятор не должен держать прогон вечно.
export SIM_TIMEOUT="${SIM_TIMEOUT:-240}"
