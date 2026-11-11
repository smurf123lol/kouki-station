# Host-стенды прошивки

Проверки логики станции на обычном Linux-хосте — без ESP-IDF и без платы.
Собираются системными `gcc`/`g++` против реальных исходников `firmware/main/`
с заглушками платформы (ESP-IDF, FreeRTOS, LVGL) из `support/`.

## Запуск

```bash
tools/test-host.sh             # все стенды, код 0 при успехе
tools/test-host.sh --asan      # то же под AddressSanitizer + UBSan
tools/test-host.sh --help      # справка
```

Объекты и бинарники пишутся вне репозитория — в `$WORKSPACE/_scratch/test-host-obj`
(`--asan` — в `test-host-obj-asan`, чтобы обычный прогон не пересобирался).
В самом репозитории артефактов не остаётся. Каталог `firmware/test/` в сборку
образа (`firmware/main/CMakeLists.txt`) не входит.

`--asan` нужен не «на всякий случай»: часть проверок видит только санитайзер —
выход за границу `m_fifo` в Vad (П6), утечка PCM при `ESP_ERR_TIMEOUT` в
AudioOut (П5), подтверждение известной утечки повторного `Voice::init` (П12).
Режим подставляет `-fsanitize=address,undefined -fno-omit-frame-pointer -g`
и задаёт `ASAN_OPTIONS=detect_stack_use_after_return=1:detect_leaks=1`,
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`,
`LSAN_OPTIONS=suppressions=firmware/test/lsan.supp`.

## Стенды

| Стенд в `test-host.sh` | Файл | Проверок | Что проверяет |
| --- | --- | --- | --- |
| `t111: C++ Voice FSM Suite` | `test_fsm.cpp` | 159 | `kouki::voice` FSM: переходы и guard'ы, прерывания, VAD-отсечки, follow-up, парсер ответа, история, settle-задержка IDLE |
| `t114: Platform Behavior` | `test_platform_behavior.cpp` | 59 | SNTP и форматирование времени, снимок VAD-метрик (`VoiceStateManager`), диспетчер питания/димминг, кольцо истории в PSRAM |
| `t115: Real UI C++ classes` | `test_ui_statemachine.cpp` | 80 | настоящие классы `kouki::ui`: тач-зоны и свайпы, VAD-дуга и интерполяция, карточка ответа, мини-сфера против тела карточки, история и OOM-повтор, настройки с опросом Wi-Fi, модалки OTA/Setup |
| `t121: Config` | `test_config.cpp` | 157 | `kouki::Config` на моке NVS: ключи 1:1, миграция `wake_mig`, `save`/`erase` (включая `ca_pem`), `is_complete`, маскирование секретов, совпадение вшитого CA с `certs/kouki-local-root.pem` |
| `t116/t118: speech hostcheck` | `test_speech_host.c` | 92 | сценарии озвучки через C-фасад: WAV/стерео, нарезка по фразам, межфразовая пауза, блокировка записи во время игры, abort/поколения, EAGAIN заголовков и тела, границы буфера ответа |
| `T121 П10: kouki::Speech` | `test_speech.cpp` | 45 | границы `wav_parse`/`stereo_expand`, `busy`/`is_playing`/`worker_active`, равенство фасада и методов класса |
| `t121: Time, History, Power` | `test_time_history_power.cpp` | 88 | `kouki::Time` (TZ, форматирование, отказ при несинхронизированном времени), `kouki::History` (кольцо, версия, фильтры, повторный `init`), `kouki::Power` (димминг, насыщение, активность) |
| `t121: Vad` | `test_vad.cpp` | 96 | `kouki::Vad`: смешанный чанк, частичный FIFO и чанк больше фрейма, streak, метрики |
| `t121: Wifi` | `test_wifi.cpp` | 160 | `kouki::Wifi`: отказы стека (netif/init/регистрация/режим/конфиг/старт), открытая сеть, backoff и его сброс по GOT_IP, `inactive_time`, `ip_len=0`, C-фасад |
| `t121: Http` (allow) | `test_http.cpp` | 50 | `kouki::Http`: запрос и разбор ответа, `diagnose_error` (TLS/DNS/нет связи), логирование только https, guard пустых аргументов |
| `t121: Http` (deny) | `test_http.cpp` + `-DHTTP_EXPECT_ALLOW_HTTP=0` | 51 | то же с запрещённым HTTP: отказ на `http://`, разрешён только `https://` |
| `t121: AudioOut + Tone` | `test_audio_out.cpp` | 142 | `kouki::AudioOut` (очередь, владение буфером, leaf-lock, `abort`, переполнение) и `kouki::Tone` (синтез, fade на краях, сигналы) |
| `t121: AudioOut + Tone` (quiet) | `test_audio_out.cpp` + `-DTONE_EXPECT_VOLUME_0` | 123 | то же при нулевой громкости сигналов: кодек не должен получать ни одного блока |
| `t121: Ota` | `test_ota.cpp` | 161 | `kouki::Ota`: проверка sha (верхний и нижний регистр), 401 и прочие статусы, порог verify-таймаута 60 с, `mark_valid`, `cert_pem`, отказ `xTaskCreate` |
| `t121: Wake + WakeVerify` | `test_wake.cpp` | 216 | `kouki::Wake` (два хита подряд, окно mute в мкс, энергопорог) и `kouki::WakeVerify` (WAV-заголовок: `RIFF`/`data`, отправка на сервер) |
| `t121: Setup` | `test_setup.cpp` | 220 | `kouki::Setup`: имя SoftAP из MAC, регистрация маршрутов, валидации `/save` (SSID/URL/токен/PEM), отказ записи в NVS, консольные команды и их тексты, подъём USB-Serial-JTAG |
| `t121: Voice` | `test_voice.cpp` | 173 | `kouki::Voice`: фасад и события, `start_recording` из SENDING, тексты `print_state` и логов, известная утечка повторного `init`, инвариант одного владельца I2S |
| `t121: Station` | `test_station.cpp` | 170 | `kouki_api`: порядок инициализации 1:1 с прежним `app_main`, состав объектного графа, `main.c` как C-точка входа, все 14 текстов логов, C-фасад |

Число проверок в таблице — фактический вывод прогона; оно растёт вместе с
пунктами покрытия и не является «нормой» само по себе.

## Заглушки (`support/`)

| Каталог | Что подменяет |
| --- | --- |
| `support/esp_err.h`, `esp_log.h`, `esp_app_desc.h`, `esp_board_init.h`, `lvgl_mock.hpp` | общие заглушки ESP-IDF и LVGL для всех стендов |
| `support/log_macro.h`, `log_capture.h/.c` | перехват `ESP_LOG*`: с `-DKOUKI_TEST_LOG_CAPTURE` строки складываются в буфер 8 КБ (`kouki_test_log_count/contains/text/clear`), без флага — прежние «выключенные» макросы |
| `support/speech/` | `esp_http_client`, heap и FreeRTOS для озвучки, `stubs.c` (кодек/HTTP с хуками отказов), `speech_factories.cpp` (фабрика `kouki::audio_out()` без worker'а), отдельный `sdkconfig.h` |
| `support/setup/` | httpd (`esp_http_server.h`), SoftAP/MAC, USB-Serial-JTAG, heap и задачи для стенда портала настройки |
| `support/config_mock/` | NVS (с хуками отказа open/get/set/commit), заглушки `nvs.h`/`nvs_flash.h`/`mbedtls`, `sdkconfig.h` и символ вшитого CA |
| `support/wifi/` | `esp_wifi`, `esp_netif`, `esp_event`, `esp_timer` + хуки отказа каждого шага стека; `wifi_ap_config_t` описан целиком, как в IDF 5.5.1 |
| `support/http_mock/` | `esp_http_client` (allow и `deny/` для запрещённого HTTP), `esp_tls_errors.h` |
| `support/audio/` | кодек `esp_audio_play` с записью блоков, пика и краёв (для проверки fade), плюс `quiet/` для нулевой громкости |
| `support/ota/` | `esp_ota_ops`/`esp_https_ota`, sha256, состояния образа и отказы `xTaskCreate` |
| `support/wake/` | esp-dl `rfft`, heap, таймер и счётчики прогонов FFT |
| `support/cjson/` | cJSON 1.7.19 (MIT) — копия зависимости образа, чтобы стенд не требовал ESP-IDF |

У каждого набора заглушек свой `sdkconfig.h`; все они защищены общим гвардом
`KOUKI_TEST_SDKCONFIG_SEEN`, поэтому действует первый подключённый набор, и
`CONFIG_KOUKI_*` не могут разойтись между модулями одного стенда. Значения
в стабах обязаны совпадать с реальной сборкой (`Kconfig.projbuild`): сверено
для всех `CONFIG_KOUKI_*`, включая `WIFI_TIMEOUT_S = 25`.

## Ограничения стендов

Стенд проверяет то, что наблюдаемо на хосте. Ниже — что именно НЕ проверяется
и почему; это не «дыры, которые забыли», а сознательные границы.

- **I2S и микрофон.** Чтение микрофона (`esp_get_feed_data`, `esp_codec_dev_read`,
  `i2s_channel_read`) живёт под `#ifdef ESP_PLATFORM` и на хосте недостижимо.
  Вместо поведения проверяется инвариант: во всём `firmware/main/**` (кроме
  `drivers/` и `test/`) эти функции вызываются только из `app_voice.cpp`
  (`test_i2s_owner_invariant` в `test_voice.cpp`). Реальный путь доказуем
  только на плате.
- **mbedtls.** `mbedtls_x509_crt_parse` в моке проверяет лишь маркеры PEM и
  непустое base64-тело — подделку ASN.1 стенд не поймает. В образе разбор
  настоящий.
- **httpd и SoftAP.** Портал не поднимается: маршруты вызываются напрямую,
  тело формы стенд кладёт в `httpd_req_t.body` (в IDF тела в структуре нет).
  Проверяется логика обработчиков, не сетевой стек.
- **NVS.** «Флеш» — массивы в памяти процесса; переполнение разделов, износ и
  настоящие ошибки флеша не моделируются.
- **FreeRTOS.** Задачи не запускаются планировщиком: мок запоминает
  `xTaskCreate`, а тест сам решает, когда «прогнать» задачу. Гонки планировщика
  и приоритеты на хосте не проверяются.
- **Аудио.** Кодек — счётчик блоков: сохранённые сэмплы позволяют проверить
  форму сигнала (fade, тишина, стереопара), но не качество звука на железе.
- **Модель wake-word** — сгенерированный `wake_model_ey_gin.h` (static inline,
  без счётчика инференсов). Стенд проверяет наблюдаемые эффекты (хиты, окно
  mute, энергопорог), а не число прогонов модели.
- **Файловые проверки.** `test_station.cpp` и часть `test_setup.cpp` читают
  исходники `firmware/main/**` (`-DKOUKI_MAIN_DIR`) и сверяют тексты и порядок
  вызовов. Это ловит подмену строк, но не доказывает поведение — поэтому там,
  где эффект наблюдаем (моки, счётчики, перехват логов), проверяется эффект.
- **Известная утечка.** Повторный `Voice::init()` не освобождает прежний буфер
  (~480 КБ) — поведение master 1:1, спека T121 §3 запрещает чинить его в
  миграции. Стенд это фиксирует (`test_repeated_init_keeps_old_buffer`), а
  LeakSanitizer получает подавление в `lsan.supp`; правило снимается вместе с
  отдельной задачей на освобождение буфера.

## Правила

- Перенос стендов — 1:1: `TEST_ASSERT` не ослаблять, кейсы не удалять, имена
  и тексты проверок сохранять.
- Новый модуль — новый стенд рядом; тест не должен требовать ESP-IDF.
- Пункт покрытия закрывается мутационным прогоном: мутация в модуле должна
  ронять тест, откат — возвращать PASS. Выживший мутант — незакрытый пункт,
  а не «и так сойдёт».
- cJSON в `support/cjson/` обновляется вместе с `espressif__cjson` в образе
  (сейчас 1.7.19; в ESP-IDF 5.5.1 лежит 1.7.18 — на поведение стенда не влияет).
