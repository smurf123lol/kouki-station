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
    [string] = 'COM3',
    [int] = 921600,
    [string] = 'E:\dsharness-workspaces\Kouki Workspace\_scratch\esp-flash'
)

Continue = 'Stop'

 = Split-Path -Parent 
 = Join-Path  'firmware\build'

 = Join-Path  'bootloader\bootloader.bin'
 = Join-Path  'partition_table\partition-table.bin'
 = Join-Path  'ota_data_initial.bin'
 = Join-Path  'kouki_station.bin'
# Модель wake-word лежит отдельно от образа: esp-sr собирает srmodels.bin и
# ждёт его в разделе " model\ (см. firmware\partitions.csv, смещение 0x820000).
 = Join-Path 'srmodels\srmodels.bin'

foreach ( in @(, , )) {
 if (-not (Test-Path )) {
 throw \Нет файла  — сначала соберите прошивку: powershell -File tools/build.ps1\
 }
}

 = @()
if ((Test-Path ) -and ((Get-Item ).Length -gt 1024)) {
 = @('0x820000', )
} else {
 Write-Host 'srmodels.bin нет (wake-word выключен): прошиваю без раздела моделей' -ForegroundColor Yellow
}

 = @()
if (Test-Path ) {
 = @('0xf000', )
}

if ( -and (Test-Path )) {
 = 
}

python -m esptool version | Out-Null
if ( -ne 0) {
 throw 'esptool не найден (python -m esptool). Установите: python -m pip install --target <dir> esptool'
}

Write-Host \Пишу  на …\ -ForegroundColor Cyan
python -m esptool --chip esp32s3 -p -b --before default-reset --after hard-reset write-flash 
 --flash-mode dio --flash-size 16MB --flash-freq 80m 
 0x0 
 0x8000 
 @otaArgs 
 0x20000 
 @modelArgs

if ( -ne 0) {
 throw \esptool вернул код \
}

Write-Host 'Готово. Плата перезагрузилась; монитор порта: python -m serial.tools.miniterm COM3 115200' -ForegroundColor Green
