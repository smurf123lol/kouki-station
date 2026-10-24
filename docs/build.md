# Сборка прошивки

Прошивка собирается ESP-IDF **не ниже 5.5.1** (в проекте проверено на 5.5.1). Два способа: в контейнере и локально.

## Вариант 1: контейнер

```powershell
docker run --rm -v "${PWD}/firmware:/project" -w /project espressif/idf:v5.5.1 `
    idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" build
```

Тег именно `v5.5.1` — в Docker Hub образа с тегом без `v` нет.

Конфиг Wi-Fi и адрес сервера в контейнере передаются так же, как локально: вторым файлом defaults
(`sdkconfig.defaults;sdkconfig.secret`).

## Вариант 2: локальный ESP-IDF

```powershell
# 1. клонировать IDF нужной версии
git clone --depth 1 -b v5.5.1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git

# 2. поставить тулчейн и python-зависимости
./install.bat esp32s3

# 3. собрать (скрипт сам выставит окружение и секреты)
./tools/build-native.ps1 -Secret
```

## Грабли, на которые уже наступили

**Путь с пробелом.** Компилятор Xtensa падает с

```
cc1.exe: fatal error: Both 'XTENSA_GNU_CONFIG' and "-dynconfig=" specified but pointed different files:
    '...\lib\XTENSA~3.SO' 'xtensa_esp32s3.so'
```

Это 8.3-имена ломаются на пробеле в пути. Лечится junction'ами без пробелов:

```cmd
mklink /J E:\kouki-esp\idf   <путь>\esp-idf
mklink /J E:\kouki-esp\tools <путь>\esp-tools
```

`tools/build-native.ps1` по умолчанию ждёт их именно там (`-IdfPath`, `-ToolsPath`).

**Недоступные каталоги.** Если `%TEMP%`, `%LOCALAPPDATA%` или `%USERPROFILE%` недоступны на запись,
падают три разные вещи:

| симптом | причина | лечение |
| --- | --- | --- |
| `pip` молча висит | нет каталога для временных файлов | `TEMP`/`TMP` внутри рабочей папки |
| `ccache: error: Permission denied` | кэш по умолчанию вне рабочей папки | `CCACHE_DIR` внутри рабочей папки |
| `Failed to create cache directory: …\Espressif\ComponentManager\Cache` | кэш менеджера компонентов | `IDF_COMPONENT_CACHE_PATH` и `IDF_COMPONENT_STORAGE_PATH` |

`tools/build-native.ps1` выставляет всё это сам.

**`pip` и версии пакетов.** Без constraint-файла `idf.py` ставит свежие `esptool`, `click`, `esp-idf-kconfig`
и падает на проверке требований. Правильно:

```powershell
uv pip install --python <env>\Scripts\python.exe `
    -r esp-idf\tools\requirements\requirements.core.txt `
    -c esp-tools\espidf.constraints.v5.5.txt
```

**`export.bat`.** На путях с пробелами и на `XTENSA_GNU_CONFIG` он не работает — в проекте вместо него
`tools/env-idf.ps1`, который берёт переменные из `idf_tools.py export --format key-value`.

## Что ломалось уже после сборки

| симптом | причина | лечение |
| --- | --- | --- |
| `EXIO_Init: Add new I2C device failed`, «I2C bus is not initialized» | `EXIO_Init()` вызван раньше `esp_board_init()` | I2C-шину создаёт аудио-борда: сначала `esp_board_init(16000, 2, 16)`, потом `EXIO_Init()` |
| `esp_codec_dev.h: No such file or directory` | managed-компонент не попал в `REQUIRES` | `espressif__esp_codec_dev` в `main/CMakeLists.txt` и `components/audio_board/CMakeLists.txt` |
| самопроверка `KOUKI_SELFTEST` не срабатывает | `-DSDKCONFIG_DEFAULTS` игнорируется, если `firmware/sdkconfig` уже существует | удалить `sdkconfig` (или `idf.py fullclean`) |
| на экране пустые прямоугольники вместо русских букв | в `lv_font_montserrat_*` нет кириллицы, LVGL молча пропускает глифы | `firmware/main/ui/font_ru_{14,16,20}.c` (команда перегенерации — `docs/decisions.md`) |
| прошивка не шьётся из контейнера | Docker на Windows не отдаёт COM-порт | `tools/flash-windows.ps1 -Port COM3` из Windows |

Скрипты `tools/*.ps1` лежат в UTF-8 **с BOM**: Windows PowerShell 5.1 иначе читает
кириллицу как ANSI и падает `ParserError` ещё до первой строки логики.

## Секреты

`firmware/sdkconfig.secret` (в `.gitignore`):

```
CONFIG_KOUKI_WIFI_SSID="..."
CONFIG_KOUKI_WIFI_PASSWORD="..."
CONFIG_KOUKI_SERVER_URL="http://<ip>:5300"
CONFIG_KOUKI_SERVER_TOKEN="..."
CONFIG_KOUKI_SOURCE="station"
CONFIG_KOUKI_MAX_RECORD_S=15
CONFIG_KOUKI_MIC_CHANNEL=1
```

## Что должно получиться

```
firmware/build/kouki_station.bin
```

Прошивка: `tools/flash-windows.ps1 -Port COM3` (bootloader `0x0`, partitions `0x8000`, приложение `0x10000`).

## Wake-word и раздел моделей

Модель кодового слова — не файл в приложении, а содержимое flash-партиции
`model` (`partitions.csv`, 2 МБ). `esp_srmodel_init("model")` мапит раздел,
поэтому одной `idf.py flash` мало: рядом с `kouki_station.bin` нужен
`build/srmodels/srmodels.bin` на адрес раздела (`0x410000`). `tools/flash-windows.ps1`
пишет оба файла сам; если `srmodels.bin` не собрался — предупредит и прошьёт без
моделей (станция поднимется, но кодовое слово будет недоступно).

Что ломалось:

1. `E Conv: no Conv kernel for packed key 0x0202` — esp-dl собирает только ядра,
   перечисленные в `compile_req`-файлах компонентов, а esp-sr не указал
   `dl_tie728_w8a16_conv2d_11cn`. Лечение: свой `firmware/main/kouki-dl-req.yml`
   (ops esp-sr + это ядро) и регистрация через `esp_dl_compile_add_yml`.
2. `UnicodeEncodeError: 'charmap'` при упаковке моделей — скрипт печатает китайские
   имена. Лечение: `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8` в `build-native.ps1`.
3. `sdkconfig.secret` с BOM: kconfgen пишет `ignoring malformed line` и теряет
   Wi-Fi/токен. Файл должен быть UTF-8 **без BOM**.
4. Новая опция `Kconfig.projbuild` не появляется в уже собранном `sdkconfig` —
   удалить `firmware/sdkconfig` (иначе `CONFIG_KOUKI_WAKE_PARTITION` не определён).
## Сервер для станции должен уметь распознавать речь

Если станция показывает «распознавание речи ещё не подключено», это ответ сервера
`503` из `UnavailableSpeechTranscriber`: сервер собран без `sherpa-onnx`.

```powershell
dotnet build Kagedama.api -p:KoukiEnableSherpa=true
$env:Kouki__SpeechModelDirectory='…\models\asr\sherpa-onnx-whisper-small'
$env:Kouki__SpeechLanguage='ru'
```

Обычный `dotnet build` (и `dotnet test`) собирает без флага намеренно: пакет
`sherpa-onnx` тянет нативные библиотеки, и без модели приложение должно
оставаться работоспособным, просто без голоса.

## Микрофон и усиление

Микрофоны платы тихие: средняя амплитуда речи — 130–310 из 32767 даже с PGA 30 дБ.
Поэтому в прошивке есть цифровое усиление `CONFIG_KOUKI_MIC_GAIN` (по умолчанию 6,
мягкий клиппинг) и порог «есть ли речь» `CONFIG_KOUKI_MIN_AMPLITUDE` (120).
Проверить уровни на месте: включить `CONFIG_KOUKI_DEBUG_MIC` — при загрузке и
дальше раз в 6 секунд печатаются средние уровни всех каналов входа.