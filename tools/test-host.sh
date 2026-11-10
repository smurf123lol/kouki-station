#!/usr/bin/env bash
# Host-стенды станции Kouki: собирает и прогоняет все host-тесты одной командой.
#
# Без ESP-IDF и без esp-idf тулчейна: только системные gcc/g++, заглушки
# платформы лежат в firmware/test/support/. Объекты и бинарники складываются
# вне репозитория — в $WORKSPACE/_scratch/test-host-obj.
#
# Коды возврата: 0 — все PASS, 1 — падение проверки/сборки,
#                2 — неверные аргументы, 3 — нет gcc/g++.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$ROOT/firmware/main"
TEST="$ROOT/firmware/test"
SUPPORT="$TEST/support"
CJSON_DIR="$SUPPORT/cjson"
SPEECH="$SUPPORT/speech"

usage() {
    cat <<'EOF'
Использование: tools/test-host.sh [--help]

Собирает и прогоняет host-стенды прошивки (FSM, платформенное поведение,
UI-автоматы, озвучка) на хосте — без ESP-IDF, только gcc/g++.

  -h, --help   показать эту справку и выйти (код 0)

Переменные окружения:
  WORKSPACE    каталог воркспейса; объекты и бинарники пишутся в
               $WORKSPACE/_scratch/test-host-obj (по умолчанию воркспейс
               определяется автоматически: для дерева в _scratch/wt/<...>
               берётся каталог над _scratch, иначе родитель репозитория).
  CC, CXX      компиляторы C и C++ (по умолчанию gcc и g++).

Коды возврата: 0 — все проверки прошли, 1 — падение проверки или сборки,
               2 — неверные аргументы, 3 — не найден gcc/g++.
EOF
}

for arg in "$@"; do
    case "$arg" in
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "test-host: неизвестный аргумент: $arg" >&2
        echo "test-host: справка — tools/test-host.sh --help" >&2
        exit 2
        ;;
    esac
done

CC="${CC:-gcc}"
CXX="${CXX:-g++}"
for tool in "$CC" "$CXX"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-host: не найден компилятор '$tool' (нужны gcc и g++)" >&2
        exit 3
    fi
done

# Каталог для объектов вне репозитория: в самом репозитории ничего не оставляем.
if [ -z "${WORKSPACE:-}" ]; then
    case "$ROOT" in
    */_scratch/wt/*) WORKSPACE="${ROOT%%/_scratch/wt/*}" ;;
    *) WORKSPACE="$(cd "$ROOT/.." && pwd)" ;;
    esac
fi
OBJ_DIR="$WORKSPACE/_scratch/test-host-obj"
rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"

# Тесты не должны попадать в образ прошивки: main/CMakeLists.txt собирает
# только свои SRCS, каталог firmware/test/ он не перечисляет.
if grep -q 'firmware/test\|"\./test/\|"test/' "$MAIN/CMakeLists.txt"; then
    echo "test-host: firmware/test попал в SRCS сборки образа (firmware/main/CMakeLists.txt)" >&2
    exit 1
fi

FSM_CPP=(
    "$MAIN/fsm/voice_state_manager.cpp"
    "$MAIN/fsm/voice_response_parser.cpp"
    "$MAIN/fsm/states/idle_state.cpp"
    "$MAIN/fsm/states/recording_state.cpp"
    "$MAIN/fsm/states/sending_state.cpp"
    "$MAIN/fsm/states/done_state.cpp"
    "$MAIN/fsm/states/error_state.cpp"
    "$MAIN/fsm/states/followup_state.cpp"
)

UI_CPP=(
    "$MAIN/ui/ui_state_manager.cpp"
    "$MAIN/ui/states/ui_voice_substates.cpp"
    "$MAIN/ui/states/ui_history_tile.cpp"
    "$MAIN/ui/states/ui_settings_tile.cpp"
    "$MAIN/ui/states/ui_modal_screens.cpp"
    "$MAIN/ui/widgets/ui_orb_widget.cpp"
    "$MAIN/ui/widgets/ui_card_widget.cpp"
)

# run <имя> <команда...>: выполняет шаг; при ошибке печатает FAIL и выходит с кодом 1.
run() {
    local name="$1"
    shift
    if ! "$@"; then
        echo "test-host: FAIL в шаге '$name'" >&2
        exit 1
    fi
}

echo "=== Компиляция C-модулей (-Wall -Wextra) ==="
run "cJSON.c" "$CC" -std=gnu11 -O2 -I"$CJSON_DIR" -c "$CJSON_DIR/cJSON.c" -o "$OBJ_DIR/cJSON.o"

echo "=== Компиляция C++-модулей П3/П6 (-Wall -Wextra) ==="
run "app_vad.cpp" "$CXX" -std=c++17 -Wall -Wextra -I"$MAIN" -c "$MAIN/app_vad.cpp" -o "$OBJ_DIR/app_vad.o"
run "app_time.cpp" "$CXX" -std=c++17 -Wall -Wextra -I"$MAIN" -c "$MAIN/app_time.cpp" -o "$OBJ_DIR/app_time.o"
run "app_history.cpp" "$CXX" -std=c++17 -Wall -Wextra -I"$MAIN" -c "$MAIN/app_history.cpp" -o "$OBJ_DIR/app_history.o"
run "app_power.cpp" "$CXX" -std=c++17 -Wall -Wextra -I"$MAIN" -c "$MAIN/app_power.cpp" -o "$OBJ_DIR/app_power.o"

echo "=== t111: C++ Voice FSM Suite ==="
run "t111 build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$MAIN" -I"$MAIN/fsm" -I"$CJSON_DIR" \
    "$TEST/test_fsm.cpp" \
    "${FSM_CPP[@]}" \
    "$OBJ_DIR/app_history.o" "$OBJ_DIR/app_time.o" "$OBJ_DIR/app_vad.o" "$OBJ_DIR/cJSON.o" \
    -o "$OBJ_DIR/test_fsm_bin"
run "t111 run" "$OBJ_DIR/test_fsm_bin"

echo "=== t114: Platform Behavior ==="
run "t114 build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$MAIN" -I"$MAIN/fsm" -I"$CJSON_DIR" \
    "$TEST/test_platform_behavior.cpp" \
    "${FSM_CPP[@]}" \
    "$OBJ_DIR/app_history.o" "$OBJ_DIR/app_time.o" "$OBJ_DIR/app_power.o" "$OBJ_DIR/app_vad.o" "$OBJ_DIR/cJSON.o" \
    -lm \
    -o "$OBJ_DIR/test_platform_bin"
run "t114 run" "$OBJ_DIR/test_platform_bin"

echo "=== t115: Real UI C++ classes ==="
run "t115 build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$SUPPORT" -I"$MAIN" -I"$MAIN/ui" \
    "$TEST/test_ui_statemachine.cpp" \
    "${UI_CPP[@]}" \
    "$OBJ_DIR/app_history.o" "$OBJ_DIR/app_time.o" \
    -lm \
    -o "$OBJ_DIR/test_ui_bin"
run "t115 run" "$OBJ_DIR/test_ui_bin"

echo "=== t116/t118: speech hostcheck (real app_speech.c + locks + abort gen) ==="
run "speech build" "$CC" -std=gnu11 -Wall -Wextra -pthread \
    -I"$SPEECH/config" -I"$SPEECH/stub" -I"$CJSON_DIR" -I"$MAIN" \
    "$MAIN/app_speech.c" \
    "$TEST/test_speech_host.c" \
    "$SPEECH/stubs.c" \
    "$OBJ_DIR/cJSON.o" \
    -pthread \
    -o "$OBJ_DIR/test_speech_bin"
run "speech run" "$OBJ_DIR/test_speech_bin"

echo "=== t121: Config (kouki::Config + C-фасад) ==="
CFG_MOCK="$SUPPORT/config_mock"
run "config mock nvs" "$CC" -std=gnu11 -Wall -Wextra -I"$CFG_MOCK" \
    -c "$CFG_MOCK/nvs_mock.c" -o "$OBJ_DIR/nvs_mock.o"
run "config mock mbedtls" "$CC" -std=gnu11 -Wall -Wextra -I"$CFG_MOCK" \
    -c "$CFG_MOCK/mbedtls_crt_mock.c" -o "$OBJ_DIR/mbedtls_mock.o"
run "config mock stubs" "$CC" -std=gnu11 -Wall -Wextra -I"$CFG_MOCK" \
    -c "$CFG_MOCK/config_stubs.c" -o "$OBJ_DIR/config_stubs.o"
run "config mock root pem" "$CC" -std=gnu11 -Wall -Wextra -I"$CFG_MOCK" \
    -c "$CFG_MOCK/root_pem.c" -o "$OBJ_DIR/root_pem.o"
run "config build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" \
    -DKOUKI_TEST_CERT_PATH="\"$MAIN/certs/kouki-local-root.pem\"" \
    "$TEST/test_config.cpp" \
    "$MAIN/app_config.cpp" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    -o "$OBJ_DIR/test_config_bin"
run "config run" "$OBJ_DIR/test_config_bin"

echo "=== t121: Time, History, Power (классы + C-фасад) ==="
run "modules build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$MAIN" -I"$MAIN/fsm" -I"$CJSON_DIR" \
    "$TEST/test_time_history_power.cpp" \
    "${FSM_CPP[@]}" \
    "$OBJ_DIR/app_history.o" "$OBJ_DIR/app_time.o" "$OBJ_DIR/app_power.o" \
    "$OBJ_DIR/app_vad.o" "$OBJ_DIR/cJSON.o" \
    -lm \
    -o "$OBJ_DIR/test_modules_bin"
run "modules run" "$OBJ_DIR/test_modules_bin"

echo "=== t121: Vad (kouki::Vad + C-фасад, фреймы 30 мс) ==="
run "vad build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$MAIN" \
    "$TEST/test_vad.cpp" \
    "$OBJ_DIR/app_vad.o" \
    -o "$OBJ_DIR/test_vad_bin"
run "vad run" "$OBJ_DIR/test_vad_bin"

echo "=== t121: Http (kouki::Http + C-фасад, мок esp_http_client) ==="
HTTP_MOCK="$SUPPORT/http_mock"
# Стенд линкует настоящий app_config.cpp (нужен kouki::config() для pinned CA),
# поэтому моки NVS/mbedtls берём из config_mock, а esp_http_client — из http_mock.
# Собирается дважды: CONFIG_KOUKI_ALLOW_HTTP=1 (sim) и =0 (дефолт Kconfig).
run "http mock" "$CC" -std=gnu11 -Wall -Wextra -I"$HTTP_MOCK" -I"$CFG_MOCK" \
    -c "$HTTP_MOCK/http_mock.c" -o "$OBJ_DIR/http_mock.o"
run "http build (allow)" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$HTTP_MOCK" -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" \
    -DHTTP_EXPECT_ALLOW_HTTP=1 \
    "$TEST/test_http.cpp" \
    "$MAIN/app_http.cpp" \
    "$MAIN/app_config.cpp" \
    "$OBJ_DIR/http_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    -o "$OBJ_DIR/test_http_bin"
run "http run (allow)" "$OBJ_DIR/test_http_bin"

run "http build (deny)" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$HTTP_MOCK/deny" -I"$HTTP_MOCK" -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" \
    -DHTTP_EXPECT_ALLOW_HTTP=0 \
    "$TEST/test_http.cpp" \
    "$MAIN/app_http.cpp" \
    "$MAIN/app_config.cpp" \
    "$OBJ_DIR/http_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    -o "$OBJ_DIR/test_http_deny_bin"
run "http run (deny)" "$OBJ_DIR/test_http_deny_bin"

echo "=== t121: AudioOut + Tone (kouki::AudioOut/Tone + C-фасад, мок кодека) ==="
AUDIO="$SUPPORT/audio"
# Кодек заменён моком esp_audio_play: стенд видит настоящую очередь, worker и
# порядок воспроизведения. Собирается дважды: громкость сигналов 50 (обычный
# случай) и 0 (каталог quiet/) — во втором Tone обязан молчать.
run "audio mock" "$CC" -std=gnu11 -Wall -Wextra -I"$AUDIO" -I"$SUPPORT" -I"$MAIN" \
    -c "$AUDIO/audio_mock.c" -o "$OBJ_DIR/audio_mock.o"
run "audio build" "$CXX" -std=c++17 -Wall -Wextra -pthread \
    -I"$AUDIO" -I"$SUPPORT" -I"$MAIN" \
    "$TEST/test_audio_out.cpp" \
    "$MAIN/app_audio_out.cpp" "$MAIN/app_tone.cpp" \
    "$OBJ_DIR/audio_mock.o" \
    -lm -pthread \
    -o "$OBJ_DIR/test_audio_bin"
run "audio run" "$OBJ_DIR/test_audio_bin"

run "audio build (volume 0)" "$CXX" -std=c++17 -Wall -Wextra -pthread \
    -I"$AUDIO/quiet" -I"$AUDIO" -I"$SUPPORT" -I"$MAIN" \
    -DTONE_EXPECT_VOLUME_0=1 \
    "$TEST/test_audio_out.cpp" \
    "$MAIN/app_audio_out.cpp" "$MAIN/app_tone.cpp" \
    "$OBJ_DIR/audio_mock.o" \
    -lm -pthread \
    -o "$OBJ_DIR/test_audio_quiet_bin"
run "audio run (volume 0)" "$OBJ_DIR/test_audio_quiet_bin"

echo "=== ALL HOSTCHECKS PASSED ==="
