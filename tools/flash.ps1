# Прошивка Kouki Station.
#
#   powershell -File tools/flash.ps1 -Port COM3
#
# Требует локальный ESP-IDF (idf.py). Если его нет — соберите образ через
# tools/build.ps1 и прошейте esptool-ом вручную (см. docs/README.md).
[CmdletBinding()]
param(
    [string]$Port = 'COM3',
    [int]$Baud = 460800,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$firmware = Join-Path $repo 'firmware'

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw 'idf.py не найден. Активируйте ESP-IDF (export.bat / idf.py) или соберите через tools/build.ps1.'
}

Push-Location $firmware
try {
    Write-Host "Прошиваю $Port на $Baud…" -ForegroundColor Cyan
    idf.py -p $Port -b $Baud flash
    if ($LASTEXITCODE -ne 0) { throw "flash вернул код $LASTEXITCODE" }

    if ($Monitor) {
        Write-Host 'Монитор порта (Ctrl+] — выход)…' -ForegroundColor Cyan
        idf.py -p $Port monitor
    }
} finally {
    Pop-Location
}
