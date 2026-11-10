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

echo "=== t116/t118: speech hostcheck (real kouki::Speech + locks + abort gen) ==="
# Стенд t116 остаётся C-файлом (test_speech_host.c): моки речи — C (atomic_*),
# поэтому stubs.c и сам стенд собирает gcc, а линкуются они с C++-модулем речи.
# kouki::Speech берёт адрес/токен из настоящего kouki::Config, поэтому в
# линковку входит app_config.cpp с моками config_mock (NVS + mbedtls);
# kouki::audio_out() подменён support/speech/speech_factories.cpp (объект в
# статике, без worker'а: звук стенд считает через stubs.c), Tone — настоящий.
run "speech stubs" "$CC" -std=gnu11 -Wall -Wextra -pthread \
    -I"$SPEECH/config" -I"$SPEECH/stub" -I"$CJSON_DIR" -I"$MAIN" \
    -c "$SPEECH/stubs.c" -o "$OBJ_DIR/speech_stubs.o"
run "speech test obj" "$CC" -std=gnu11 -Wall -Wextra -pthread \
    -I"$SPEECH/config" -I"$SPEECH/stub" -I"$CJSON_DIR" -I"$MAIN" \
    -c "$TEST/test_speech_host.c" -o "$OBJ_DIR/test_speech_host.o"
run "speech build" "$CXX" -std=c++17 -Wall -Wextra -pthread \
    -I"$SPEECH/config" -I"$CFG_MOCK" -I"$SPEECH/stub" -I"$SUPPORT" -I"$CJSON_DIR" -I"$MAIN" \
    "$MAIN/app_speech.cpp" "$MAIN/app_config.cpp" "$MAIN/app_tone.cpp" \
    "$SPEECH/speech_factories.cpp" \
    "$OBJ_DIR/test_speech_host.o" "$OBJ_DIR/speech_stubs.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    "$OBJ_DIR/cJSON.o" \
    -pthread \
    -o "$OBJ_DIR/test_speech_bin"
run "speech run" "$OBJ_DIR/test_speech_bin"

# Тот же модуль и те же моки, но проверки уровня класса: границы wav_parse,
# вырожденные входы stereo_expand, состояние busy/playing/worker и равенство
# фасада и static-методов класса.
run "speech class build" "$CXX" -std=c++17 -Wall -Wextra -pthread \
    -I"$SPEECH/config" -I"$CFG_MOCK" -I"$SPEECH/stub" -I"$SUPPORT" -I"$CJSON_DIR" -I"$MAIN" \
    "$TEST/test_speech.cpp" \
    "$MAIN/app_speech.cpp" "$MAIN/app_config.cpp" "$MAIN/app_tone.cpp" \
    "$SPEECH/speech_factories.cpp" \
    "$OBJ_DIR/speech_stubs.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    "$OBJ_DIR/cJSON.o" \
    -pthread \
    -o "$OBJ_DIR/test_speech_class_bin"
run "speech class run" "$OBJ_DIR/test_speech_class_bin"

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

echo "=== t121: Wifi (kouki::Wifi + C-фасад, мок esp_wifi/esp_netif/esp_event) ==="
# Сеть не поднимается: мок запоминает вызовы и сам доставляет события
# (STA_START, GOT_IP), поэтому стенд видит настоящую логику backoff.
WIFI="$SUPPORT/wifi"
run "wifi mock" "$CC" -std=gnu11 -Wall -Wextra -I"$WIFI" -I"$SUPPORT" \
    -c "$WIFI/wifi_mock.c" -o "$OBJ_DIR/wifi_mock.o"
# app_config.cpp тоже участвует: SSID/пароль Wi-Fi берёт из kouki::config().
run "wifi build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$WIFI" -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" \
    "$TEST/test_wifi.cpp" \
    "$MAIN/app_wifi.cpp" "$MAIN/app_config.cpp" "$MAIN/app_time.cpp" \
    "$OBJ_DIR/wifi_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    -lm \
    -o "$OBJ_DIR/test_wifi_bin"
run "wifi run" "$OBJ_DIR/test_wifi_bin"

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

echo "=== t121: Ota (kouki::Ota + C-фасад, мок esp_ota/esp_https_ota) ==="
OTA="$SUPPORT/ota"
# Разделы и HTTPS-OTA замоканы (support/ota), HTTP-клиент проверки обновлений
# и группа событий — общие моки http_mock/wifi: стенд линкует настоящий
# app_wifi.cpp ради kouki::wifi() в конструкторе Ota. sha256 считается
# по-настоящему (sha256_mock.c), поэтому сверка образа проверяется как в образе.
run "ota mock" "$CC" -std=gnu11 -Wall -Wextra -I"$OTA" -I"$HTTP_MOCK" -I"$WIFI" -I"$SUPPORT" -I"$CFG_MOCK" \
    -c "$OTA/ota_mock.c" -o "$OBJ_DIR/ota_mock.o"
run "ota sha256" "$CC" -std=gnu11 -Wall -Wextra -I"$OTA" -I"$SUPPORT" \
    -c "$OTA/sha256_mock.c" -o "$OBJ_DIR/sha256_mock.o"
run "ota stubs" "$CC" -std=gnu11 -Wall -Wextra -I"$OTA" -I"$SUPPORT" -I"$MAIN" \
    -c "$OTA/ota_stubs.c" -o "$OBJ_DIR/ota_stubs.o"
run "ota build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$OTA" -I"$HTTP_MOCK" -I"$WIFI" -I"$CFG_MOCK" -I"$SUPPORT" \
    -I"$MAIN" -I"$CJSON_DIR" \
    "$TEST/test_ota.cpp" \
    "$MAIN/app_ota.cpp" "$MAIN/app_wifi.cpp" "$MAIN/app_config.cpp" \
    "$MAIN/app_http.cpp" "$MAIN/app_time.cpp" \
    "$OBJ_DIR/ota_mock.o" "$OBJ_DIR/sha256_mock.o" "$OBJ_DIR/ota_stubs.o" \
    "$OBJ_DIR/http_mock.o" "$OBJ_DIR/wifi_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    "$OBJ_DIR/cJSON.o" \
    -lm \
    -o "$OBJ_DIR/test_ota_bin"
run "ota run" "$OBJ_DIR/test_ota_bin"

echo "=== t121: Wake + WakeVerify (kouki::Wake/kouki::WakeVerify, мок esp-dl rfft) ==="
WAKE="$SUPPORT/wake"
# esp-dl rfft заменён наивным DFT с тем же форматом выхода, время и heap_caps —
# моками: детектор на стенде — настоящая модель microWakeWord, гоняемая по
# реальным фикстурам sim/fixtures/mic/*.wav. HTTP-клиент ASR-проверки — общий
# мок http_mock (добавлен только esp_http_client_write для POST-тела).
run "wake mock" "$CC" -std=gnu11 -Wall -Wextra -I"$WAKE" -I"$CFG_MOCK" -I"$SUPPORT" \
    -c "$WAKE/wake_mock.c" -o "$OBJ_DIR/wake_mock.o"
run "wake build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$WAKE" -I"$HTTP_MOCK" -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" \
    "$TEST/test_wake.cpp" \
    "$MAIN/app_wake.cpp" "$MAIN/app_wake_verify.cpp" \
    "$MAIN/app_config.cpp" "$MAIN/app_http.cpp" \
    "$OBJ_DIR/wake_mock.o" "$OBJ_DIR/http_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    -lm \
    -o "$OBJ_DIR/test_wake_bin"
run "wake run" "$OBJ_DIR/test_wake_bin"

echo "=== t121: Setup (kouki::Setup, моки httpd/SoftAP/USB-Serial-JTAG) ==="
SETUP="$SUPPORT/setup"
run "setup mock" "$CC" -std=gnu11 -Wall -Wextra -I"$SETUP" -I"$WIFI" -I"$SUPPORT" \
    -c "$SETUP/setup_mock.c" -o "$OBJ_DIR/setup_mock.o"
run "setup stubs" "$CC" -std=gnu11 -Wall -Wextra -I"$SETUP" -I"$WIFI" -I"$SUPPORT" -I"$MAIN" \
    -c "$SETUP/setup_stubs.c" -o "$OBJ_DIR/setup_stubs.o"
run "setup build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$SETUP" -I"$WIFI" -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" \
    "$TEST/test_setup.cpp" \
    "$MAIN/app_setup.cpp" "$MAIN/app_wifi.cpp" "$MAIN/app_config.cpp" "$MAIN/app_time.cpp" \
    "$OBJ_DIR/setup_mock.o" "$OBJ_DIR/setup_stubs.o" \
    "$OBJ_DIR/wifi_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    -lm \
    -o "$OBJ_DIR/test_setup_bin"
run "setup run" "$OBJ_DIR/test_setup_bin"

echo "=== t121: Voice (kouki::Voice + C-фасад, инвариант одного владельца I2S) ==="
# Голосовое ядро без платы: на хосте I2S-путь под #ifdef ESP_PLATFORM не
# собирается, поэтому стенд проверяет инвариант по исходникам firmware/main
# (KOUKI_MAIN_DIR), сессии, колбэк событий и равенство фасада классу.
# В линковку входят настоящие Config, Vad и Wake: фабрика kouki::voice_engine()
# берёт их ссылками (того требует спека §2), а Wake поднимает esp-dl-мок.
run "voice build" "$CXX" -std=c++17 -Wall -Wextra \
    -I"$WAKE" -I"$HTTP_MOCK" -I"$CFG_MOCK" -I"$SUPPORT" -I"$MAIN" -I"$MAIN/fsm" \
    -DKOUKI_MAIN_DIR="\"$MAIN\"" \
    "$TEST/test_voice.cpp" \
    "$MAIN/app_voice.cpp" "$MAIN/app_wake.cpp" "$MAIN/app_vad.cpp" \
    "$MAIN/app_config.cpp" "$MAIN/app_http.cpp" \
    "${FSM_CPP[@]}" \
    "$OBJ_DIR/app_history.o" "$OBJ_DIR/app_time.o" "$OBJ_DIR/app_power.o" \
    "$OBJ_DIR/wake_mock.o" "$OBJ_DIR/http_mock.o" \
    "$OBJ_DIR/nvs_mock.o" "$OBJ_DIR/mbedtls_mock.o" "$OBJ_DIR/config_stubs.o" "$OBJ_DIR/root_pem.o" \
    "$OBJ_DIR/cJSON.o" \
    -lm \
    -o "$OBJ_DIR/test_voice_bin"
run "voice run" "$OBJ_DIR/test_voice_bin"

echo "=== ALL HOSTCHECKS PASSED ==="
