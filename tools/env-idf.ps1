<#
    Готовит окружение ESP-IDF в текущей сессии PowerShell без export.bat.
    export.bat рассчитан на запуск из cmd и ломается на путях с пробелами
    (а наш каталог проекта как раз с пробелом), поэтому переменные берём
    из idf_tools.py export --format key-value.
#>
param(
    [string]$IdfPath = 'E:\kouki-esp\idf',
    [string]$ToolsPath = 'E:\kouki-esp\tools'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path (Join-Path $IdfPath 'tools\idf.py'))) {
    throw "Не найден ESP-IDF в $IdfPath. Укажите -IdfPath."
}

$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$python = Join-Path $ToolsPath 'python_env\idf5.5_py3.12_env\Scripts\python.exe'
if (-not (Test-Path $python)) { throw "Не найдено окружение Python: $python. Запустите install.bat esp32s3." }

# idf_tools.py ругается в stderr на системные cmake/ninja — это предупреждения,
# а не ошибка, поэтому на время вызова понижаем строгость.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$raw = & $python (Join-Path $IdfPath 'tools\idf_tools.py') export --format key-value 2>&1 |
    Where-Object { $_ -is [string] }
$ErrorActionPreference = $prevEap

$toolPath = ''

foreach ($line in $raw) {
    if ($line -match '^([A-Z_]+)=(.*)$') {
        $name = $Matches[1]
        $value = $Matches[2]
        if ($name -eq 'PATH') {
            $toolPath = ($value -replace '%PATH%', $env:PATH)
            continue
        }
        if ($name -eq 'IDF_DEACTIVATE_FILE_PATH') { continue }
        Set-Item -Path "Env:$name" -Value $value
    }
}

if (-not $toolPath) { throw 'idf_tools.py не вернул PATH — окружение ESP-IDF не собрано.' }
$env:PATH = $toolPath
$env:ESP_IDF_VERSION = if ($env:ESP_IDF_VERSION) { $env:ESP_IDF_VERSION } else { '5.5' }

# cc1 сам подставляет относительный путь ядра Xtensa и спорит с -dynconfig,
# если текущий каталог не совпадает с lib тулчейна. Явный абсолютный путь
# снимает конфликт «Both 'XTENSA_GNU_CONFIG' and "-dynconfig=" specified».
$elfRoot = Get-ChildItem (Join-Path $ToolsPath 'tools\xtensa-esp-elf') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if ($elfRoot) {
    $core = Join-Path $elfRoot.FullName 'xtensa-esp-elf\lib\xtensa_esp32s3.so'
    if (Test-Path $core) { $env:XTENSA_GNU_CONFIG = $core }
}

# Менеджер компонентов по умолчанию пишет кэш в %LOCALAPPDATA%\Espressif, а он
# в песочнице недоступен. Кэш и менеджер компонентов переносим в рабочую папку.
$workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$componentCache = Join-Path $workspace '_scratch\idf-component-cache'
$componentData = Join-Path $workspace '_scratch\idf-component-data'
New-Item -ItemType Directory -Path $componentCache, $componentData -Force | Out-Null
$env:CCACHE_DIR = Join-Path $workspace '_scratch\ccache'
New-Item -ItemType Directory -Path $env:CCACHE_DIR -Force | Out-Null
$env:IDF_COMPONENT_CACHE_PATH = $componentCache
$env:IDF_COMPONENT_STORAGE_PATH = $componentData
Write-Host "ESP-IDF: $env:IDF_PATH"
Write-Host "Python:  $python"