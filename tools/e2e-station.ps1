<#
.SYNOPSIS
    Сквозная проверка станции без платы: синтез → POST /api/voice → задача в Kouki.

.DESCRIPTION
    Станция — это «записать WAV и отправить». Ровно это и проверяется здесь, только
    вместо микрофона — синтезированная фраза, а вместо платы — curl. Скрипт отвечает
    на вопрос «контракт станция ↔ сервер цел?» без человека с кнопкой и без железа.

    Проверяются обе стороны: удачный путь (фраза → задача/команда) и отказы
    (нет токена, не WAV, неизвестный intent) — именно они чаще всего ломаются
    при правках сервера.

    Для синтеза нужен Kouki.SpeechLab, собранный с -p:KoukiEnableSherpa=true.

.EXAMPLE
    .\tools\e2e-station.ps1 -Server http://127.0.0.1:5300 -Token kouki-demo

.EXAMPLE
    .\tools\e2e-station.ps1 -Phrase "Сколько минут варить пельмени?" -Intent ask
#>
[CmdletBinding()]
param(
    [string] $Server = 'http://127.0.0.1:5300',
    [string] $Token = $env:KOUKI_TOKEN,
    [string] $Phrase = 'Запиши задачу проверить станцию без платы',
    [ValidateSet('add_task', 'ask', 'none')] [string] $Intent = 'add_task',
    [string] $Source = 'station',
    [string] $Wav,
    [string] $SpeechLab,
    [switch] $KeepWav
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- вспомогательное -------------------------------------------------------

$isWindowsHost = $true
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) { $isWindowsHost = $IsWindows }
$curl = if ($isWindowsHost) { 'curl.exe' } else { 'curl' }

function Test-Writable {
    param([string] $dir)
    try {
        [void](New-Item -ItemType Directory -Force -Path $dir)
        $probe = Join-Path $dir '.probe'
        [IO.File]::WriteAllText($probe, 'ok')
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        return $true
    } catch { return $false }
}

function Get-WorkDir {
    # $env:TEMP в ограниченном окружении бывает недоступен, поэтому пробуем по порядку:
    # системный temp, затем _scratch/tmp рабочей папки (там точно можно писать).
    $repoRoot = Split-Path -Parent $PSScriptRoot
    # Порядок важен: в ограниченном окружении дочерние процессы не могут писать в %TEMP%,
    # а _scratch/tmp рабочей папки доступен всем (там же лежат логи сборки).
    $candidates = @(
        (Join-Path $repoRoot '..\_scratch\tmp\kouki-e2e'),
        (Join-Path $repoRoot '..\..\_scratch\tmp\kouki-e2e'),
        (Join-Path ([IO.Path]::GetTempPath()) 'kouki-e2e'),
        '/tmp/kouki-e2e'
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Writable -dir $candidate)) { return $candidate }
    }

    throw 'Нет каталога для временных файлов: проверьте TEMP или создайте _scratch/tmp'
}

# curl печатает тело в файл, а код ответа — в stdout: так одно и то же работает
# и в Windows PowerShell 5.1, и в pwsh 7.
function Invoke-Http {
    param(
        [string] $Method,
        [string] $Url,
        [string] $BodyFile,
        [string] $AuthToken,
        [string] $ContentType,
        [hashtable] $Extra
    )

    $bodyOut = Join-Path $script:work "body.json"
    $args = @('-s', '-S', '-X', $Method, '-o', $bodyOut, '-w', '%{http_code}')
    if ($BodyFile) { $args += @('--data-binary', "@$BodyFile") }
    if ($AuthToken) { $args += @('-H', "Authorization: Bearer $AuthToken") }
    if ($ContentType) { $args += @('-H', "Content-Type: $ContentType") }
    $args += $Url

    $codeText = & $script:curl @args 2>&1 | Out-String
    $code = 0
    [void][int]::TryParse($codeText.Trim(), [ref]$code)

    $text = ''
    if (Test-Path -LiteralPath $bodyOut) { $text = [IO.File]::ReadAllText($bodyOut) }

    return [pscustomobject]@{ Status = $code; Body = $text }
}

$script:work = Get-WorkDir
$script:curl = $curl
$failures = New-Object System.Collections.Generic.List[string]
$rows = New-Object System.Collections.Generic.List[object]

function Add-Check {
    param([string] $Name, [bool] $Ok, [string] $Detail = '')
    $rows.Add([pscustomobject]@{ Проверка = $Name; Итог = $(if ($Ok) { 'ок' } else { 'ПРОВАЛ' }); Что = $Detail })
    if (-not $Ok) { $failures.Add($Name) }
}

function Find-SpeechLab {
    param([string] $Hint)
    if ($Hint) {
        if (Test-Path -LiteralPath $Hint) { return (Resolve-Path -LiteralPath $Hint).Path }
        throw "SpeechLab не найден: $Hint"
    }

    # Скрипт лежит в kouki-station/tools, а сервер — в соседней папке Kouki.
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $repoRoot '..\Kouki\tools\Kouki.SpeechLab\bin\Debug\net10.0\Kouki.SpeechLab.exe'),
        (Join-Path $repoRoot '..\Kouki\tools\Kouki.SpeechLab\bin\Debug\net10.0\Kouki.SpeechLab'),
        (Join-Path $repoRoot '..\Kouki\tools\Kouki.SpeechLab\bin\Release\net10.0\Kouki.SpeechLab'),
        '/opt/kouki/tools/Kouki.SpeechLab/Kouki.SpeechLab'
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw 'Kouki.SpeechLab не найден: соберите Kouki/tools/Kouki.SpeechLab с -p:KoukiEnableSherpa=true или укажите -SpeechLab'
}

function New-PhraseWav {
    param([string] $Text, [string] $Target)

    $lab = $script:speechLab
    & $lab 'say' $Text '--out' $Target | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Target)) {
        throw "Синтез не удался (код $LASTEXITCODE): $Target"
    }
    return (Resolve-Path -LiteralPath $Target).Path
}

# --- проверки --------------------------------------------------------------

Write-Host "станция-без-платы: $Server, источник '$Source', интент '$Intent'" -ForegroundColor Cyan

# 0. Сервер жив?
$health = Invoke-Http -Method 'GET' -Url "$Server/api/health"
Add-Check 'сервер отвечает /api/health' ($health.Status -eq 200) "HTTP $($health.Status)"

if ($health.Status -ne 200) {
    $rows | Format-Table -AutoSize | Out-String | Write-Host
    throw "Сервер $Server недоступен: запустите Kouki\tools\run-server.ps1 (с -p:KoukiEnableSherpa=true)"
}

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw 'Не задан токен: передайте -Token или переменную окружения KOUKI_TOKEN'
}

# 1. WAV: готовый или синтезированный
if ($Wav) {
    if (-not (Test-Path -LiteralPath $Wav)) { throw "WAV не найден: $Wav" }
    $wavPath = (Resolve-Path -LiteralPath $Wav).Path
    Add-Check 'WAV-файл найден' $true $wavPath
}
else {
    $script:speechLab = Find-SpeechLab -Hint $SpeechLab
    $wavPath = New-PhraseWav -Text $Phrase -Target (Join-Path $script:work 'phrase.wav')
    $seconds = [math]::Round(((Get-Item -LiteralPath $wavPath).Length - 44) / 32000.0, 2)
    Add-Check 'фраза синтезирована' $true "$seconds с"
}

# 2. Основной путь: как со станции
$intentQuery = if ($Intent -eq 'none') { '' } else { "&intent=$Intent" }
$url = "$Server/api/voice?source=$Source$intentQuery"
$voice = Invoke-Http -Method 'POST' -Url $url -BodyFile $wavPath -AuthToken $Token -ContentType 'audio/wav'

Add-Check 'сервер принял WAV' ($voice.Status -eq 200) "HTTP $($voice.Status)"

$parsed = $null
if ($voice.Body) {
    try { $parsed = $voice.Body | ConvertFrom-Json } catch { $parsed = $null }
}
Add-Check 'ответ — JSON' ($null -ne $parsed) ''

$transcript = if ($parsed) { $parsed.transcript } else { $null }
Add-Check 'речь распознана' (-not [string]::IsNullOrWhiteSpace($transcript)) "«$transcript»"

$intake = if ($parsed) { $parsed.intake } else { $null }
$outcome = if ($intake) { $intake.outcome } else { $null }
Add-Check 'приём отработал' ($outcome -in @('created', 'executed', 'needs_review')) "outcome=$outcome"

if ($Intent -ne 'none' -and $intake) {
    $actualIntent = $intake.intent.intent
    Add-Check 'интент сессии применён' ($actualIntent -eq $Intent) "intent=$actualIntent, model=$($intake.intent.model)"
}

# 3. Данные в Kouki: задача и действие приёма
$task = if ($intake) { $intake.task } else { $null }
if ($task) {
    $byId = Invoke-Http -Method 'GET' -Url "$Server/api/tasks/$($task.id)" -AuthToken $Token
    Add-Check 'задача читается по id' ($byId.Status -eq 200) "HTTP $($byId.Status)"

    $actions = Invoke-Http -Method 'GET' -Url "$Server/api/actions?taskId=$($task.id)&limit=10" -AuthToken $Token
    $actionCount = 0
    try { $actionCount = @(($actions.Body | ConvertFrom-Json)).Count } catch { $actionCount = 0 }
    Add-Check 'действие приёма записано' ($actionCount -ge 1) "actions=$actionCount"

    Add-Check 'источник задачи — станция' ($task.source -eq $Source) "source=$($task.source)"
    Add-Check 'заголовок не обрезан' (-not [string]::IsNullOrWhiteSpace($task.title)) "«$($task.title)»"
}
elseif ($Intent -eq 'ask') {
    # В режиме вопроса задачи может не быть, если команда ответила без записи.
    Add-Check 'режим вопроса: задача не обязательна' $true "outcome=$outcome"
}

# 4. Отказы: их чаще всего ломают, а замечают поздно
$noToken = Invoke-Http -Method 'POST' -Url $url -BodyFile $wavPath -ContentType 'audio/wav'
Add-Check 'без токена — 401' ($noToken.Status -eq 401) "HTTP $($noToken.Status)"

$junk = Join-Path $script:work 'junk.bin'
[IO.File]::WriteAllBytes($junk, [byte[]](1..200))
$badWav = Invoke-Http -Method 'POST' -Url $url -BodyFile $junk -AuthToken $Token -ContentType 'audio/wav'
Add-Check 'не-WAV — 415' ($badWav.Status -eq 415) "HTTP $($badWav.Status)"

$badIntent = Invoke-Http -Method 'POST' -Url "$Server/api/voice?source=$Source&intent=patch_task" -BodyFile $wavPath -AuthToken $Token -ContentType 'audio/wav'
Add-Check 'неизвестный intent — 400' ($badIntent.Status -eq 400) "HTTP $($badIntent.Status)"

if (-not $KeepWav -and -not $Wav) {
    Remove-Item -LiteralPath $wavPath -ErrorAction SilentlyContinue
}

# --- итог ------------------------------------------------------------------

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

if ($failures.Count -gt 0) {
    Write-Host ("ПРОВАЛЕНО проверок: {0}" -f $failures.Count) -ForegroundColor Red
    foreach ($name in $failures) { Write-Host "  - $name" -ForegroundColor Red }
    exit 1
}

Write-Host ("Все проверки прошли: {0}" -f $rows.Count) -ForegroundColor Green
exit 0
