<#
    Собирает прошивку станции локальным ESP-IDF (без Docker).

    ESP-IDF и инструменты подключаются через junction без пробелов (E:\kouki-esp\idf и
        .\tools\build-native.ps1 -Secret -SetTarget
    E:\kouki-esp\tools): путь с пробелом ломает 8.3-имена в тулчейне Xtensa.
#>
param(
    [string]$IdfPath = 'E:\kouki-esp\idf',
    [string]$ToolsPath = 'E:\kouki-esp\tools',
    [switch]$Secret,
    [switch]$Clean,
    [switch]$SetTarget,
    [string[]]$IdfArgs = @()
)

$ErrorActionPreference = 'Stop'
$fw = Join-Path $PSScriptRoot '..\firmware' | Resolve-Path
$secretFile = Join-Path $fw 'sdkconfig.secret'

& (Join-Path $PSScriptRoot 'env-idf.ps1') -IdfPath $IdfPath -ToolsPath $ToolsPath

$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'

# Песочница закрывает %LOCALAPPDATA% и системный %TEMP%, поэтому кеши инструментов
# и временные файлы живут внутри рабочей папки.
$workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tmp = Join-Path $workspace '_scratch\tmp'
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
$env:TEMP = $tmp
$env:TMP = $tmp
$env:CCACHE_DIR = Join-Path $workspace '_scratch\ccache'
New-Item -ItemType Directory -Path $env:CCACHE_DIR -Force | Out-Null

# esp-sr упаковывает модели скриптом на Python, который печатает имена моделей
# по-китайски: в консоли cp1251 это UnicodeEncodeError. Заставляем Python писать UTF-8.
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
Push-Location $fw
try {
    $common = @()
    if ($Secret) {
        if (-not (Test-Path $secretFile)) { throw "Нет $secretFile — Wi-Fi и токен не заданы." }
        $common += "--project-dir"; $common += $fw
        $common += "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secret"
    }
    if ($Clean) { & $python $idfPy @common fullclean; if ($LASTEXITCODE -ne 0) { throw "fullclean упал: $LASTEXITCODE" } }
    if ($SetTarget) {
        & $python $idfPy @common set-target esp32s3
        if ($LASTEXITCODE -ne 0) { throw "set-target упал: $LASTEXITCODE" }
    }
    $buildArgs = @()
    if ($Secret) { $buildArgs += "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secret" }
    $buildArgs += 'build'
    $buildArgs += $IdfArgs
    Write-Host ("idf.py " + ($buildArgs -join ' '))
    & $python $idfPy @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Сборка упала с кодом $LASTEXITCODE." }
    Write-Host "Готово: build\kouki_station.bin"
}
finally { Pop-Location }