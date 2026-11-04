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

## HTTPS и корневой сертификат сервера (pinned CA)

Станция обращается к серверу Kouki по HTTPS (`https://kouki.local`) и валидирует сертификат сервера по корневому CA (`firmware/main/certs/kouki-local-root.pem`), вшитому в образ прошивки.

### Экспорт корневого CA с сервера Caddy
Корневой сертификат генерируется Caddy при использовании `tls internal`:
```bash
# Путь на сервере:
/var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt

# Копирование в дерево сборки станции:
cp /var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt \
   firmware/main/certs/kouki-local-root.pem
```

### Ротация CA без перепрошивки
1. **Через SoftAP-портал (`http://192.168.4.1`)**: в поле «Сертификат CA (PEM)» ввести новый сертификат. Он сохранится в NVS (`ca_pem`) и имеет приоритет над вшитым в образ.
2. **Через сетевое OTA (T16)**: новый образ прошивки с обновлённым `kouki-local-root.pem` доставляется по сети.

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

## Секреты и настройка (T46)

Секреты (Wi-Fi, токен сервера, URL и источник) больше не зашиваются в образ прошивки и живут в NVS (namespace `kouki_config`).
При первом включении (или сбросе) станция поднимает точку доступа `Kouki-Setup-XXXX` (веб-форма на `http://192.168.4.1`)
или настраивается через USB-консоль (команды `set_wifi`, `set_token`, `set_url`, `save`).

Файл `firmware/sdkconfig.secret` (в `.gitignore`) остаётся только для локальных флагов сборки (например, включение wake-word):

```
CONFIG_KOUKI_MAX_RECORD_S=15
CONFIG_KOUKI_MIC_CHANNEL=1
```

## Что должно получиться

```
firmware/build/kouki_station.bin
```

Прошивка: `tools/flash-windows.ps1 -Port COM3` (bootloader `0x0`, partitions `0x8000`, приложение `0x10000`).

### Дерево задачи на сервере

`git worktree` живёт в `_scratch/wt/<имя>`, поэтому каталог сборки надо задать
явно и отдельно от основного дерева — иначе `idf.py` увидит, что build-dir
принадлежит другому пути проекта, и откажется собирать/прошивать:

```bash
export IDF_PATH=/srv/kouki-workspace/_scratch/esp-idf
export IDF_TOOLS_PATH=/srv/kouki-workspace/_scratch/esp-tools
export BUILD_DIR=/srv/kouki-workspace/_scratch/build-esp32s3-t103

bash tools/build-linux.sh --secret
bash tools/flash-linux.sh --port /dev/ttyACM0
```

`WORKSPACE` скрипты считают от общего каталога git (`rev-parse --git-common-dir`),
поэтому `_scratch`, ESP-IDF и тулчейн находятся и в дереве задачи. `firmware/sdkconfig.secret`
в дерево не копируется (он в `.gitignore`) — его надо взять из основного дерева.

## Wake-word выключен по умолчанию (25.10.2026)

KOUKI_WAKE_ENABLED стоит в 
: обычная сборка — станция с двумя кнопками, без
детектора. Причины: модель занимает раздел ~1 МБ, детектор без правильно
подобранного порога срабатывает на чужое «Hi ESP», а включать это на каждой
плате по умолчанию — сюрприз для владельца. Поэтому код детектора собирается
только под #if CONFIG_KOUKI_WAKE_ENABLED, а в остальных случаях
app_wake.c даёт заглушки.

Включить:

`ini
# firmware/sdkconfig.secret (UTF-8 без BOM, файл не в git)
CONFIG_KOUKI_WAKE_ENABLED=y
CONFIG_MODEL_IN_FLASH=y
CONFIG_SR_WN_WN10_HIESP=y
`

затем удалить firmware/sdkconfig, собрать и прошить вместе с srmodels.bin
(`tools/flash-windows.ps1` сам пишет его на 0x410000, если файл больше 1 КБ —
пустая заглушка от упаковщика теперь не прошивается).

## Тестовый режим

KOUKI_TEST_MODE (+ KOUKI_TEST_MODE_DELAY_S, KOUKI_TEST_MODE_SECONDS,
выбор «как задачу / как вопрос») — станция после подключения к сети сама пишет
и отправляет фразу тем же путём, что и кнопка. Нужен для проверки тракта без
человека: видно, доходит ли запись до сервера и что он отвечает. По умолчанию
выключен, иначе лишняя фраза уходила бы при каждом включении.

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
## Сквозная проверка без платы (T30)

Когда железа под рукой нет (или нужно проверить сервер после правок), есть
`tools/e2e-station.ps1`: он делает то же, что станция, но без микрофона — синтезирует фразу
локальным TTS из `Kouki.SpeechLab` и отправляет WAV на `/api/voice`.

```powershell
# сервер должен быть запущен с распознаванием
Kouki\tools\run-server.ps1 -Port 5300 -Token kouki-demo `
  -DatabasePath ..\kouki-station\station-db\kouki.sqlite `
  -AudioDirectory ..\kouki-station\station-audio

# сессия записи
kouki-station\tools\e2e-station.ps1 -Server http://127.0.0.1:5300 -Token kouki-demo `
  -Phrase "Запиши задачу проверить станцию" -Intent add_task

# сессия вопроса
kouki-station\tools\e2e-station.ps1 -Server http://127.0.0.1:5300 -Token kouki-demo `
  -Phrase "Сколько минут варить пельмени?" -Intent ask
```

Скрипт проверяет 14 пунктов: сервер жив, фраза синтезирована, WAV принят (200), ответ — JSON,
речь распознана, `outcome` из ожидаемого набора, интент сессии применён, задача читается по id,
действие приёма записано, источник `station`, заголовок не пустой, а также отказы: без токена —
401, не-WAV — 415, неизвестный `intent` — 400. Любой провал — `exit 1` и список, что не сошлось.

Чего он **не** заменяет: проверку микрофона, усиления и порога громкости, работу кнопок и
экрана. Это только тракт «сервер ↔ станция».

## Вариант 3: Linux-сервер (build-linux.sh / flash-linux.sh)

На сервере разработки (`/srv/kouki-workspace`) нет Docker и PowerShell, но есть ESP-IDF 5.5.1
в `_scratch/esp-idf` и тулчейн в `_scratch/esp-tools`. Скрипты делают то же, что PS-двойники:

```bash
tools/build-linux.sh --secret          # сборка в _scratch/build-esp32s3
tools/flash-linux.sh                   # /dev/ttyACM0, группа dialout поднимается сама
tools/flash-linux.sh --monitor         # плюс лог станции
```

- Окружение: `IDF_PATH`, `IDF_TOOLS_PATH`, `BUILD_DIR`; по умолчанию — `_scratch/esp-idf`,
  `_scratch/esp-tools`, `_scratch/build-<target>`.
- `sdkconfig` живёт **в каталоге сборки**, а не в `firmware/`: иначе сборка с секретами и без
  делят один файл. Скрипт сверяет `KOUKI_WIFI_SSID`/`KOUKI_SERVER_URL` из defaults с тем, что
  лежит в `sdkconfig`, и пересоздаёт его при расхождении — иначе `SDKCONFIG_DEFAULTS` молча
  игнорируется и в образ уходит «home».
- Группа `dialout`: у процесса агента её может не быть, даже если `usermod` уже сделан (группы
  наследуются при входе). `flash-linux.sh` сам перезапускается через `sg dialout`, аргументы
  сохраняются копией — иначе `exec` теряет `--port` и прошивает дефолтный порт.
- Кэш менеджера компонентов и ccache — в `_scratch/idf-component-cache`, `_scratch/ccache`.
- Python 3.14 на сервере: ESP-IDF 5.5.1 собрал проект без ошибок (предупреждение о
  неподдерживаемой версии не помешало), но официально заявлены 3.8–3.11.
