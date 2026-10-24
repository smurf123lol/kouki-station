# Прошивка Kouki Station на Windows без установленного ESP-IDF.
#
# Сборка делается в контейнере (tools/build.ps1), а запись в плату — здесь,
# через esptool: Docker на Windows не умеет отдавать COM-порт Linux-контейнеру.
#
#   powershell -File tools/flash-windows.ps1 -Port COM3
#
# Требует: esptool (python -m pip install --target <dir> esptool) и собранные
# бинарники в firmware/build.
[CmdletBinding()]
param(
    [string]$Port = 'COM3',
    [int]$Baud = 921600,
    [string]$EsptoolPath = 'E:\dsharness-workspaces\Kouki Workspace\_scratch\esp-flash'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'firmware\build'

$bootloader = Join-Path $build 'bootloader\bootloader.bin'
$partitions = Join-Path $build 'partition_table\partition-table.bin'
$app = Join-Path $build 'kouki_station.bin'
# Модель wake-word лежит отдельно от образа: esp-sr собирает srmodels.bin и
# ждёт его в разделе "model" (см. firmware\partitions.csv).
$srmodels = Join-Path $build 'srmodels\srmodels.bin'

foreach ($f in @($bootloader, $partitions, $app)) {
    if (-not (Test-Path $f)) {
        throw "Нет файла $f — сначала соберите прошивку: powershell -File tools/build.ps1"
    }
}

$modelArgs = @()
if (Test-Path $srmodels) {
    $modelArgs = @('0x410000', $srmodels)
} else {
    Write-Host 'srmodels.bin не найден: прошиваю без раздела моделей (wake-word не заработает)' -ForegroundColor Yellow
}

if ($EsptoolPath -and (Test-Path $EsptoolPath)) {
    $env:PYTHONPATH = $EsptoolPath
}

python -m esptool version | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw 'esptool не найден (python -m esptool). Установите: python -m pip install --target <dir> esptool'
}

Write-Host "Пишу $Port на $Baud…" -ForegroundColor Cyan
python -m esptool --chip esp32s3 -p $Port -b $Baud --before default-reset --after hard-reset write-flash `
    --flash-mode dio --flash-size 16MB --flash-freq 80m `
    0x0 $bootloader `
    0x8000 $partitions `
    0x10000 $app `
    @modelArgs

if ($LASTEXITCODE -ne 0) {
    throw "esptool вернул код $LASTEXITCODE"
}

Write-Host 'Готово. Плата перезагрузилась; монитор порта: python -m serial.tools.miniterm COM3 115200' -ForegroundColor Green
