# Host-стенды прошивки

Проверки логики станции на обычном Linux-хосте — без ESP-IDF и без платы.
Собираются системными `gcc`/`g++` против реальных исходников `firmware/main/`
с заглушками платформы (ESP-IDF, FreeRTOS, LVGL) из `support/`.

## Запуск

```bash
tools/test-host.sh          # все стенды, код 0 при успехе
tools/test-host.sh --help   # справка
```

Объекты и бинарники пишутся вне репозитория — в `$WORKSPACE/_scratch/test-host-obj`;
в самом репозитории артефактов не остаётся. Каталог `firmware/test/` в сборку
образа (`firmware/main/CMakeLists.txt`) не входит.

## Что где

| Файл | Что проверяет |
| --- | --- |
| `test_fsm.cpp` | `kouki::voice` FSM: переходы и guard'ы, прерывания, VAD-отсечки, follow-up, парсер ответа, история |
| `test_platform_behavior.cpp` | `app_time`, `app_power` (дисплей/димминг), `app_history` (кольцо истории), VAD-метрики snapshot |
| `test_ui_statemachine.cpp` | реальные классы `kouki::ui`: тач-зоны, свайпы, VAD-дуга, карточка ответа, история, настройки, модалки OTA/Setup |
| `test_speech_host.c` | реальный `app_speech.cpp` (t116, C-стенд): WAV/стерео, нарезка по фразам, блокировки аудио, abort/поколения, EAGAIN |
| `test_speech.cpp` | `kouki::Speech` (t121 П10): границы `wav_parse`/`stereo_expand`, состояние busy/playing/worker, равенство фасада и методов класса |
| `test_setup.cpp` | `kouki::Setup` (t121 П11): имя SoftAP из MAC, регистрация маршрутов, валидации формы `/save`, консольные команды и подъём USB-Serial-JTAG консоли |
| `test_station.cpp` | `kouki_api` (t121 П13): порядок инициализации 1:1 с прежним `app_main`, состав объектного графа и порядок зависимостей, `main.c` как C-точка входа, тексты логов и C-фасад |
| `test_config.cpp` | реальный `app_config.cpp`: `kouki::Config` на моке NVS (ключи 1:1, миграция `wake_mig`, `save`/`erase`, `is_complete`, маскирование секретов) |
| `support/` | заглушки `esp_err.h`, `esp_log.h`, `esp_app_desc.h`, `esp_board_init.h`, `lvgl_mock.hpp` |
| `support/speech/` | заглушки esp_http_client/heap/freertos, `sdkconfig.h`, `stubs.c` (кодек/HTTP) и `speech_factories.cpp` (фабрика `kouki::audio_out()` без worker'а) для стенда озвучки |
| `support/setup/` | моки httpd (`esp_http_server.h`), SoftAP/MAC, USB-Serial-JTAG, heap и задач для стенда портала настройки |
| `support/cjson/` | cJSON 1.7.19 (MIT) — копия зависимости образа, чтобы стенд не требовал ESP-IDF |
| `support/config_mock/` | мок NVS, заглушки `nvs.h`/`nvs_flash.h`/`mbedtls`, `sdkconfig.h` и символ вшитого CA для стенда конфигурации |

## Правила

- Перенос стендов — 1:1: `TEST_ASSERT` не ослаблять, кейсы не удалять, имена
  и тексты проверок сохранять.
- Новый модуль — новый стенд рядом; тест не должен требовать ESP-IDF.
- cJSON в `support/cjson/` обновляется вместе с `espressif__cjson` в образе
  (сейчас 1.7.19; в ESP-IDF 5.5.1 лежит 1.7.18 — на поведение стенда не влияет).
