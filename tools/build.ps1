# Сборка прошивки Kouki Station в контейнере ESP-IDF.
#
# Зачем: не держать ESP-IDF и xtensa-тулчейн на хосте. Требует Docker Desktop
# (движок должен быть запущен — проверка ниже это скажет прямо).
#
#   powershell -File tools/build.ps1
#   powershell -File tools/build.ps1 -IdfTag 5.5.1 -Clean
[CmdletBinding()]
param(
    [string]$IdfTag = '5.5.1',
    [switch]$Clean,
    [switch]$Menuconfig
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$firmware = Join-Path $repo 'firmware'
$image = "espressif/idf:$IdfTag"

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker не найден. Установите Docker Desktop или соберите прошивку локальным ESP-IDF: idf.py build'
}

docker version --format '{{.Server.Version}}' 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw @'
Движок Docker не отвечает. Что проверить:
  1) Docker Desktop запущен и «Engine running»;
  2) WSL2 на месте: wsl -l -v (дистрибутив docker-desktop должен быть Running);
  3) если WSL сломан (ERROR_FILE_NOT_FOUND на ext4.vhdx), помогает
     «Docker Desktop → Troubleshoot → Clean / Purge data» или wsl --install.
Альтернатива без Docker: установить ESP-IDF 5.5.1 и выполнить idf.py build в firmware/.
'@
}

$script = if ($Menuconfig) { 'idf.py menuconfig' } else { 'idf.py build' }
if ($Clean) { $script = 'idf.py fullclean; ' + $script }

Write-Host "Собираю $image (это ~5–10 минут при первом запуске: качаются компоненты)…" -ForegroundColor Cyan

docker run --rm -v "${firmware}:/project" -w /project $image bash -lc ". `$IDF_PATH/export.sh >/dev/null 2>&1; $script"

if ($LASTEXITCODE -ne 0) {
    throw "Сборка упала с кодом $LASTEXITCODE."
}

$bin = Join-Path $firmware 'build\kouki_station.bin'
if (Test-Path $bin) {
    $info = Get-Item $bin
    Write-Host ("Готово: {0} ({1:N0} КБ)" -f $bin, ($info.Length / 1KB)) -ForegroundColor Green
} else {
    throw 'Сборка прошла, но kouki_station.bin не найден.'
}
