# Сборка прошивки Kouki Station в контейнере ESP-IDF.
#
# Зачем: не держать ESP-IDF и xtensa-тулчейн на хосте. Требует запущенный
# движок Docker (проверка ниже скажет прямо, если он не отвечает).
#
#   powershell -File tools/build.ps1
#   powershell -File tools/build.ps1 -Secret        # взять Wi-Fi и адрес сервера из sdkconfig.secret
#   powershell -File tools/build.ps1 -Clean -Menuconfig
[CmdletBinding()]
param(
    [string]$IdfTag = 'v5.5.1',
    [switch]$Secret,
    [switch]$Clean,
    [switch]$Menuconfig
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$firmware = Join-Path $repo 'firmware'
$image = "espressif/idf:$IdfTag"

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker не найден. Установите Docker Desktop или соберите локальным ESP-IDF: idf.py build'
}

docker version --format '{{.Server.Version}}' 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw @'
Движок Docker не отвечает. Что проверить:
  1) Docker Desktop запущен и «Engine running»;
  2) WSL2 на месте: wsl -l -v (дистрибутив docker-desktop должен быть Running).
Альтернатива без Docker: установить ESP-IDF 5.5.1 и выполнить idf.py build в firmware/.
'@
}

# Секреты (Wi-Fi, адрес сервера, токен) не лежат в репозитории: передаём их
# вторым файлом defaults, который подхватывается при сборке.
$defaults = 'sdkconfig.defaults'
if ($Secret) {
    $secretFile = Join-Path $firmware 'sdkconfig.secret'
    if (-not (Test-Path $secretFile)) {
        throw 'Нет firmware/sdkconfig.secret. Создайте его по образцу docs/README.md или уберите -Secret.'
    }
    $defaults = 'sdkconfig.defaults;sdkconfig.secret'
}

$script = if ($Menuconfig) { 'idf.py menuconfig' } else { "idf.py -D SDKCONFIG_DEFAULTS='$defaults' build" }
if ($Clean) { $script = "idf.py fullclean; $script" }

Write-Host "Собираю $image (первый запуск скачивает образ ~4 ГБ)…" -ForegroundColor Cyan

docker run --rm -v "${firmware}:/project" -w /project $image bash -lc ". `$IDF_PATH/export.sh >/dev/null 2>&1; $script"

if ($LASTEXITCODE -ne 0) {
    throw "Сборка упала с кодом $LASTEXITCODE."
}

$bin = Join-Path $firmware 'build\kouki_station.bin'
if (Test-Path $bin) {
    $info = Get-Item $bin
    Write-Host ("Готово: {0} ({1:N0} КБ)" -f $bin, ($info.Length / 1KB)) -ForegroundColor Green
    Write-Host 'Прошить: powershell -File tools/flash-windows.ps1 -Port COM3' -ForegroundColor Green
} else {
    throw 'Сборка прошла, но kouki_station.bin не найден.'
}
