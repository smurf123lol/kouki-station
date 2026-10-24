# Kouki Station

Голосовая станция Kouki: круглая плата на столе, одна кнопка, ответ на экране.
Фраза записывается в WAV (PCM16, 16 кГц, моно) и уходит на `POST /api/voice`
сервера Kouki — дальше работает тот же конвейер, что и у Telegram-бота:
распознавание (sherpa-onnx на сервере), классификатор интента, диспетчер команд,
SQLite и журнал действий.

> **Правила работы:** инварианты прошивки (один владелец I2S, сессии, Kconfig, секреты) —
> [AGENTS.md](AGENTS.md); рабочий цикл и definition of done — [CONTRIBUTING.md](CONTRIBUTING.md).

## Железо

- **Waveshare ESP32-S3-Touch-LCD-1.85C, ревизия 2**: ESP32-S3R8, 16 МБ flash,
  8 МБ PSRAM, круглый LCD 360×360 (ST77916 по QSPI), тач CST816S, аудиокодек
  ES8311, энкодер микрофонов ES7210, усилитель NS4150B, расширитель GPIO TCA9554.
- Отличия ревизии 2 от первой и таблица пинов — в [docs/hardware.md](docs/hardware.md).

## Что нужно

- **ESP-IDF 5.5.1** или новее (ревизия 2 требует свежих драйверов I2S/LCD).
- Wi-Fi 2,4 ГГц и запущенный сервер Kouki, доступный по сети.

## Сборка

```bash
# из каталога firmware/
idf.py set-target esp32s3
idf.py menuconfig       # Kouki Station → Wi-Fi, адрес сервера, токен
idf.py build
idf.py -p COM3 flash monitor
```

Через Docker (если ESP-IDF не установлен локально):

```powershell
powershell -File tools/build.ps1 -IdfTag 5.5.1
```

## Настройки

Все — в `menuconfig` → **Kouki Station**:

| Параметр | По умолчанию | Что значит |
| --- | --- | --- |
| `KOUKI_WIFI_SSID` / `KOUKI_WIFI_PASSWORD` | пусто | домашняя сеть; вшивается в прошивку |
| `KOUKI_SERVER_URL` | `http://192.168.1.49:5300` | адрес сервера Kouki |
| `KOUKI_SERVER_TOKEN` | пусто | bearer-токен (`Kouki:Token` в `appsettings.json`) |
| `KOUKI_SOURCE` | `station` | источник в записях журнала действий |
| `KOUKI_MAX_RECORD_S` | 15 | сколько секунд пишем одну фразу |
| `KOUKI_MIC_CHANNEL` | 1 | индекс канала микрофона в сыром потоке ES7210 |
| `KOUKI_MIN_AMPLITUDE` | 120 | ниже этого уровня запись считается тишиной и не уходит на сервер |
| `KOUKI_MIC_GAIN` | 6 | цифровое усиление входа (речь на этой плате тихая) |
| `KOUKI_WAKE_ENABLED` | **n** | слушать кодовое слово в простое; включается вместе с моделью esp-sr и `srmodels.bin` |
| `KOUKI_TEST_MODE` | **n** | тестовый режим: сама запишет и отправит фразу после подключения к сети |

Wake-word по умолчанию выключен: станция работает по двум кнопкам на экране
(«Записать» и «Спросить»). Включение требует модели в отдельной flash-партиции
(~1 МБ) и подобранного порога — пошагово в `docs/build.md`.

## Устройство прошивки

| Файл | Что делает |
| --- | --- |
| `main/main.c` | порядок инициализации: NVS → EXIO → аудио-борда → LCD → LVGL → запись → UI |
| `main/app_voice.c` | запись WAV в PSRAM, отправка на `/api/voice`, разбор ответа |
| `main/app_wifi.c` | Wi-Fi в режиме станции с переподключением |
| `main/app_ui.c` | экран: кнопка, статус, текст ответа (LVGL 8.3) |
| `main/drivers/` | драйверы Waveshare (Apache-2.0), см. `main/drivers/README.md` |
| `components/audio_board/` | инициализация кодека ES8311/ES7210 и чтение микрофона |

## Документация

- [docs/hardware.md](docs/hardware.md) — плата, пины, отличия ревизий.
- [docs/protocol.md](docs/protocol.md) — что и как отправляется на сервер.
- [docs/decisions.md](docs/decisions.md) — почему ESP-IDF, почему кнопка, почему WAV.

## Безопасность

Модель угроз, что осознанно не защищено и как обращаться с токеном — [docs/security.md](docs/security.md).
