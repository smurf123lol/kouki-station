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
