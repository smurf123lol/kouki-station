# Сборка прошивки

Прошивка собирается ESP-IDF **не ниже 5.5.1** (в проекте проверено на 5.5.1).

Канон — **сервер разработки** (`/srv/kouki-workspace`), где ESP-IDF и тулчейн уже
лежат в `_scratch`, сборка и прошивка идут Linux-скриптами `tools/build-linux.sh` и
`tools/flash-linux.sh`. Windows-путь (Docker, `*.ps1`, `COM3`) оставлен ниже как
**вспомогательный и исторический**: те же действия, но не канон.

## Вариант 1 (канон): сервер, Linux-скрипты

На сервере разработки нет Docker и PowerShell, но есть ESP-IDF 5.5.1 в `_scratch/esp-idf`
и тулчейн в `_scratch/esp-tools`.

```bash
# из каталога kouki-station
tools/build-linux.sh --secret                     # сборка в _scratch/build-esp32s3
tools/build-linux.sh --secret --build-dir _scratch/build-master   # свой каталог сборки

tools/flash-linux.sh                               # прошивка в /dev/ttyACM0
tools/flash-linux.sh -p /dev/ttyACM0 --monitor     # плюс лог станции (Ctrl+] — выход)
```

- Окружение: `IDF_PATH`, `IDF_TOOLS_PATH`, `BUILD_DIR`; по умолчанию — `_scratch/esp-idf`,
  `_scratch/esp-tools`, `_scratch/build-<target>`.
- `sdkconfig` живёт **в каталоге сборки**, а не в `firmware/`: иначе сборка с секретами и без
  делят один файл. Скрипт сверяет `KOUKI_WIFI_SSID`/`KOUKI_SERVER_URL` из defaults с тем, что
  лежит в `sdkconfig`, и пересоздаёт его при расхождении — иначе `SDKCONFIG_DEFAULTS` молча
  игнорируется и в образ уходит «home».
- **Сначала стенд, потом плата.** Правки FSM, аудио-тракта, wake-word и UI проверяются
  в эмуляторе esp32sim до прошивки: `tools/build-linux.sh --sim` и `tools/sim-run.sh --all`.
  Стенд гоняет настоящий образ и сам ставит вердикт по логу, экрану и звуку; прошивка на
  плату и живой диалог остаются **финальной** проверкой (эмулятор не воспроизводит
  аналоговый микрофон). Подробности — `docs/sim.md`.
- Группа `dialout`: у процесса агента её может не быть, даже если `usermod` уже сделан (группы
  наследуются при входе). `flash-linux.sh` сам перезапускается через `sg dialout`, аргументы
  сохраняются копией — иначе `exec` теряет `--port` и прошивает дефолтный порт.
- Кэш менеджера компонентов и ccache — в `_scratch/idf-component-cache`, `_scratch/ccache`.
- Python 3.14 на сервере: ESP-IDF 5.5.1 собрал проект без ошибок (предупреждение о
  неподдерживаемой версии не помешало), но официально заявлены 3.8–3.11.

### Дерево задачи на сервере

`git worktree` живёт в `_scratch/wt/<имя>`, поэтому каталог сборки надо задать
явно и отдельно от основного дерева — иначе `idf.py` увидит, что build-dir
принадлежит другому пути проекта, и откажется собирать/прошивать:

```bash
export IDF_PATH=/srv/kouki-workspace/_scratch/esp-idf
export IDF_TOOLS_PATH=/srv/kouki-workspace/_scratch/esp-tools
export BUILD_DIR=/srv/kouki-workspace/_scratch/build-<задача>

bash tools/build-linux.sh --secret
bash tools/flash-linux.sh --port /dev/ttyACM0
```

`WORKSPACE` скрипты считают от общего каталога git (`rev-parse --git-common-dir`),
поэтому `_scratch`, ESP-IDF и тулчейн находятся и в дереве задачи. `firmware/sdkconfig.secret`
в дерево не копируется (он в `.gitignore`) — его надо взять из основного дерева.

### Что должно получиться

```
<каталог сборки>/kouki_station.bin
```

Прошивка адресами: bootloader `0x0`, partitions `0x8000`, приложение `0x20000 [ota_0],
otadata 0xf000`; `srmodels.bin` (если собран и больше 1 КБ) — на адрес раздела модели
(`0x820000`). `flash-linux.sh` пишет всё это сам.

## Вариант 2 (вспомогательно): Windows / PowerShell

Этот путь актуален только если собираешь на Windows-машине; на сервере его нет (`pwsh` не
установлен). Скрипты-двойники оставлены для совместимости.

### Контейнер

```powershell
docker run --rm -v "${PWD}/firmware:/project" -w /project espressif/idf:v5.5.1 `
    idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" build
```

Тег именно `v5.5.1` — в Docker Hub образа с тегом без `v` нет.

Конфиг Wi-Fi и адрес сервера в контейнере передаются так же, как локально: вторым файлом defaults
(`sdkconfig.defaults;sdkconfig.secret`).

### Локальный ESP-IDF

```powershell
# 1. клонировать IDF нужной версии
git clone --depth 1 -b v5.5.1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git

# 2. поставить тулчейн и python-зависимости
./install.bat esp32s3

# 3. собрать (скрипт сам выставит окружение и секреты)
./tools/build-native.ps1 -Secret

# 4. прошить
./tools/flash-windows.ps1 -Port COM3
```

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

**BOM у скриптов.** Скрипты `tools/*.ps1` лежат в UTF-8 **с BOM**: Windows PowerShell 5.1 иначе
читает кириллицу как ANSI и падает `ParserError` ещё до первой строки логики.

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

## Грабли, на которые уже наступили (после сборки)

| симптом | причина | лечение |
| --- | --- | --- |
| `EXIO_Init: Add new I2C device failed`, «I2C bus is not initialized» | `EXIO_Init()` вызван раньше `esp_board_init()` | I2C-шину создаёт аудио-борда: сначала `esp_board_init(16000, 2, 16)`, потом `EXIO_Init()` |
| `esp_codec_dev.h: No such file or directory` | managed-компонент не попал в `REQUIRES` | `espressif__esp_codec_dev` в `main/CMakeLists.txt` и `components/audio_board/CMakeLists.txt` |
| самопроверка `KOUKI_SELFTEST` не срабатывает | `-DSDKCONFIG_DEFAULTS` игнорируется, если `firmware/sdkconfig` уже существует | удалить `sdkconfig` (или `idf.py fullclean`) |
| на экране пустые прямоугольники вместо русских букв | в `lv_font_montserrat_*` нет кириллицы, LVGL молча пропускает глифы | `firmware/main/ui/font_ru_{14,16,20}.c` (команда перегенерации — `docs/decisions.md`) |
| прошивка не шьётся из контейнера | Docker на Windows не отдаёт COM-порт | `tools/flash-windows.ps1 -Port COM3` из Windows (или Linux-путь: `tools/flash-linux.sh -p /dev/ttyACM0`) |

## Секреты и настройка (T46)

Секреты (Wi-Fi, токен сервера, URL и источник) больше не зашиваются в образ прошивки и живут в NVS (namespace `kouki_config`).
При первом включении (или сбросе) станция поднимает точку доступа `Kouki-Setup-XXXX` (веб-форма на `http://192.168.4.1`)
или настраивается через USB-консоль (команды `set_wifi`, `set_token`, `set_url`, `save`).

Файл `firmware/sdkconfig.secret` (в `.gitignore`) остаётся только для локальных флагов сборки (например, включение wake-word):

```
CONFIG_KOUKI_MAX_RECORD_S=15
CONFIG_KOUKI_MIC_CHANNEL=1
```

## Wake-word выключен по умолчанию (25.10.2026)

`CONFIG_KOUKI_WAKE_ENABLED` не выставлен: обычная сборка — станция с двумя кнопками, без
детектора. Причины: модель занимает раздел ~1 МБ, детектор без правильно подобранного порога
срабатывает на чужое «Hi ESP», а включать это на каждой плате по умолчанию — сюрприз для
владельца. Поэтому код детектора собирается только под `#if CONFIG_KOUKI_WAKE_ENABLED`, а в
остальных случаях `app_wake.c` даёт заглушки.

Включить:

```ini
# firmware/sdkconfig.secret (UTF-8 без BOM, файл не в git)
CONFIG_KOUKI_WAKE_ENABLED=y
CONFIG_MODEL_IN_FLASH=y
CONFIG_SR_WN_WN10_HIESP=y
```

затем удалить `firmware/sdkconfig`, собрать и прошить вместе с `srmodels.bin` (flash-скрипт
сам пишет его на `0x820000`, если файл больше 1 КБ — пустая заглушка от упаковщика теперь не
прошивается).

## Wake-word и раздел моделей

Модель кодового слова — не файл в приложении, а содержимое flash-партиции `model`
(`partitions.csv`, 2 МБ). `esp_srmodel_init("model")` мапит раздел, поэтому одной `idf.py flash`
мало: рядом с `kouki_station.bin` нужен `build/srmodels/srmodels.bin` на адрес раздела
(`0x820000`). `tools/flash-linux.sh`/`tools/flash-windows.ps1` пишут оба файла сами; если
`srmodels.bin` не собрался — предупредят и прошьют без моделей (станция поднимется, но
кодовое слово будет недоступно).

Что ломалось:

1. `E Conv: no Conv kernel for packed key 0x0202` — esp-dl собирает только ядра,
   перечисленные в `compile_req`-файлах компонентов, а esp-sr не указал
   `dl_tie728_w8a16_conv2d_11cn`. Лечение: свой `firmware/main/kouki-dl-req.yml`
   (ops esp-sr + это ядро) и регистрация через `esp_dl_compile_add_yml`.
2. `UnicodeEncodeError: 'charmap'` при упаковке моделей — скрипт печатает китайские
   имена. Лечение: `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8` в окружении сборки
   (`build-native.ps1` на Windows, `build-linux.sh` на сервере).
3. `sdkconfig.secret` с BOM: kconfgen пишет `ignoring malformed line` и теряет
   Wi-Fi/токен. Файл должен быть UTF-8 **без BOM**.
4. Новая опция `Kconfig.projbuild` не появляется в уже собранном `sdkconfig` —
   удалить `firmware/sdkconfig` (иначе `CONFIG_KOUKI_WAKE_PARTITION` не определён).

## Тестовый режим

`KOUKI_TEST_MODE` (+ `KOUKI_TEST_MODE_DELAY_S`, `KOUKI_TEST_MODE_SECONDS`,
выбор «как задачу / как вопрос») — станция после подключения к сети сама пишет
и отправляет фразу тем же путём, что и кнопка. Нужен для проверки тракта без
человека: видно, доходит ли запись до сервера и что он отвечает. По умолчанию
выключен, иначе лишняя фраза уходила бы при каждом включении.

## Микрофон и усиление

Микрофоны платы тихие: средняя амплитуда речи — 130–310 из 32767 даже с PGA 30 дБ.
Поэтому в прошивке есть цифровое усиление `CONFIG_KOUKI_MIC_GAIN` (по умолчанию 6,
мягкий клиппинг) и порог «есть ли речь» `CONFIG_KOUKI_MIN_AMPLITUDE` (120).
Проверить уровни на месте: включить `CONFIG_KOUKI_DEBUG_MIC` — при загрузке и
дальше раз в 6 секунд печатаются средние уровни всех каналов входа.

## Сервер для станции должен уметь распознавать речь

Если станция показывает «распознавание речи ещё не подключено», это ответ сервера
`503` из `UnavailableSpeechTranscriber`: сервер собран без `sherpa-onnx`.

```bash
# на сервере, из каталога Kouki/
dotnet build Kouki.Kagedama.Api -p:KoukiEnableSherpa=true
export Kouki__SpeechModelDirectory=/srv/kouki-workspace/Kouki/models/asr/sherpa-onnx-whisper-small
export Kouki__SpeechLanguage=ru
```

Обычный `dotnet build` (и `dotnet test`) собирает без флага намеренно: пакет
`sherpa-onnx` тянет нативные библиотеки, и без модели приложение должно
оставаться работоспособным, просто без голоса.

## Сквозная проверка тракта без платы

**Канон на сервере — стенд esp32sim** (настоящий образ в эмуляторе, без платы):

```bash
tools/build-linux.sh --sim
tools/sim-run.sh --all                          # все сценарии и вердикты
tools/sim-run.sh --scenario network-roundtrip   # путь «команда → DONE → озвучка» + «сеть готова»
```

Артефакты прогона: `run.log`, `screen.png`, `out.wav`, `verdict.txt` (`docs/sim.md`).
Сценарий `network-roundtrip` проверяет голосовое ядро и звук, а сеть — только строкой
«сеть готова» (реальный `/api/voice` на стенде не вызывается).

**Реальный серверный путь без станции — синтез + `curl`** (сервер собран с `sherpa`):

```bash
# 1. синтезировать фразу в WAV 16 кГц моно
dotnet run --project Kouki/tools/Kouki.SpeechLab -- say "Запиши задачу проверить станцию" \
  --out /tmp/check.wav
# (Kouki.SpeechLab собирается только с -p:KoukiEnableSherpa=true)

# 2. отправить как станция и посмотреть ответ
curl -s -X POST "http://127.0.0.1:5300/api/voice?source=station&intent=add_task" \
  -H "Authorization: Bearer kouki-demo" -H "Content-Type: audio/wav" \
  --data-binary @/tmp/check.wav
```

**Windows-вариант (история):** `tools/e2e-station.ps1` делает то же через PowerShell —
синтезирует фразу TTS из `Kouki.SpeechLab`, отправляет WAV на `/api/voice` и проверяет
14 пунктов (сервер жив, WAV принят, JSON-ответ, речь распознана, `outcome` из ожидаемого
набора, интент сессии применён, задача читается по id, действие приёма записано, источник
`station`, заголовок не пустой; отказы: без токена — 401, не-WAV — 415, неизвестный
`intent` — 400).

Чего это **не** заменяет: проверку микрофона, усиления и порога громкости, работу кнопок и
экрана. Это только тракт «сервер ↔ станция».
