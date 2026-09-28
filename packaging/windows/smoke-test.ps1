# Tries the packaged program on a Windows machine that has no Qt, no
# GStreamer and no build tools: the installation check with a test song,
# several starts and clean quits, a second copy started while one runs, and
# proof that the Test Songs folder was not touched.
param(
    [Parameter(Mandatory)] [string] $Package,
    # The machine has a sound output (in CI, a virtual sound card).
    [switch] $ExpectSound
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Package = (Resolve-Path $Package).Path
$exe = Join-Path $Package 'Program\FrankiesKaraokeStudio.exe'
$dataFolder = Join-Path $env:LOCALAPPDATA 'Granda\FrankiesKaraokeStudio'
$log = Join-Path $dataFolder 'frankies-karaoke-studio.log'

# Nothing but Windows itself may be found outside the package.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
foreach ($name in @(Get-ChildItem env: | Where-Object { $_.Name -like 'GST*' -or $_.Name -like 'GSTREAMER*' -or $_.Name -like 'QT_*' } | ForEach-Object Name)) {
    Remove-Item "env:$name"
}
if (Test-Path $dataFolder) { Remove-Item -Recurse -Force $dataFolder }

function Fail([string] $message) {
    Write-Host "::error::$message"
    if (Test-Path $log) {
        Write-Host '--- End of the program log ---'
        Get-Content $log | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
    }
    throw $message
}

# --- 1. Installation check, with the MP3 decoder, Key and Tempo ---------------
$report = Join-Path $env:RUNNER_TEMP 'installation-check.txt'
$song = Join-Path $Package 'Test Songs\FKT01-01 - Test Singer - Clicks In Time.mp3'
$check = Start-Process -FilePath $exe -ArgumentList "--self-check `"$report`" `"$song`"" -Wait -PassThru
Get-Content $report
if ($check.ExitCode -ne 0) { Fail "Installation check failed (exit $($check.ExitCode))" }
if (-not (Select-String -Path $report -Pattern 'Packaged copy: yes' -Quiet)) { Fail 'Not recognised as a packaged copy' }
if ($ExpectSound) {
    $outputs = Select-String -Path $report -Pattern 'Sound outputs found: (\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
    if (-not $outputs -or $outputs -lt 1) { Fail 'No sound output was found' }
    Write-Host "Sound outputs found: $outputs"
}
# The self-check also rejects any audio component loaded from outside the package.
$unicodeSong = Get-ChildItem -LiteralPath (Join-Path $Package 'Test Songs') -Recurse -Filter '*.mp3' |
    Where-Object { $_.FullName -match '[^\x00-\x7F]' } | Select-Object -First 1
$check = Start-Process -FilePath $exe -ArgumentList "--self-check `"$report`" `"$($unicodeSong.FullName)`"" -Wait -PassThru
Get-Content $report | Select-String 'Test song|MP3|RESULT'
if ($check.ExitCode -ne 0) { Fail "A song in a folder with accented letters did not play (exit $($check.ExitCode))" }

# --- 2. Start and quit, several times ----------------------------------------
function Start-Program([string[]] $arguments = @()) {
    if ($arguments) { return Start-Process -FilePath $exe -ArgumentList $arguments -PassThru }
    return Start-Process -FilePath $exe -PassThru
}
function Wait-Window($process, [int] $seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { Fail "The program ended by itself (exit $($process.ExitCode))" }
        if ($process.MainWindowHandle -ne 0) { return }
        Start-Sleep -Milliseconds 250
    }
    Fail 'The program window did not appear'
}
function Stop-Program($process, [string] $what) {
    if (-not $process.CloseMainWindow()) { Fail "Could not ask the program to close ($what)" }
    if (-not $process.WaitForExit(20000)) { $process.Kill(); Fail "The program did not quit within 20 s ($what)" }
    if ($process.ExitCode -ne 0) { Fail "The program quit with exit code $($process.ExitCode) ($what)" }
}

# The first start chooses the Test Songs as the music folder (as Settings
# would); every start then scans it, and each quit comes during that work.
$songs = Join-Path $Package 'Test Songs'
for ($round = 1; $round -le 3; $round++) {
    $started = Get-Date
    $process = if ($round -eq 1) { Start-Program @('--music-folder', "`"$songs`"") } else { Start-Program }
    Wait-Window $process 30
    Start-Sleep -Seconds (2 + $round)   # through the splash, then a little longer each time
    Stop-Program $process "start $round"
    Write-Host ("Start/quit {0}: window after launch, clean quit after {1:N1} s" -f $round, ((Get-Date) - $started).TotalSeconds)
    if (-not (Select-String -Path $log -Pattern 'Exiting with code 0' -Quiet)) { Fail "No clean exit in the log (start $round)" }
    if (Select-String -Path $log -Pattern 'Music folder refused' -Quiet) { Fail 'The Test Songs folder was refused' }
}
if (-not (Select-String -Path $log -Pattern 'Music folder:.*Test Songs' -Quiet)) { Fail 'The Test Songs folder was not the music folder' }

# Windows display scaling at 125% and 150%, as on many laptops: the window
# must come up, keep its interface size within the screen, and quit cleanly.
foreach ($scale in '1.25', '1.5') {
    $env:QT_SCALE_FACTOR = $scale
    $process = Start-Program
    Remove-Item env:QT_SCALE_FACTOR
    Wait-Window $process 30
    Start-Sleep -Seconds 4
    Stop-Program $process "display scaling $scale"
    $screenLine = Select-String -Path $log -Pattern 'Primary screen' | Select-Object -First 1
    $limitLine = Select-String -Path $log -Pattern 'Interface size .* so the window fits' | Select-Object -First 1
    Write-Host "Display scaling ${scale}: $($screenLine.Line.Substring(24))"
    if ($limitLine) { Write-Host "  $($limitLine.Line.Substring(24))" } else { Write-Host '  interface size fits without a limit' }
    if (-not (Select-String -Path $log -Pattern "device pixel ratio $scale" -Quiet)) { Fail "Display scaling $scale was not in effect" }
}

# While a song is playing (opened from the command line).
$process = Start-Program @("`"$song`"")
Wait-Window $process 30
Start-Sleep -Seconds 6
Stop-Program $process 'while a song plays'
Write-Host 'Quit while a song was playing: clean'

# --- 3. A second copy while one is open -------------------------------------
# --single-instance-probe exits at once: 3 if a copy is running, 0 if not.
function Invoke-Probe {
    $probe = Start-Process -FilePath $exe -ArgumentList '--single-instance-probe' -PassThru
    if (-not $probe.WaitForExit(10000)) { $probe.Kill(); Fail 'The single-instance probe started a full copy' }
    return $probe.ExitCode
}
if ((Invoke-Probe) -ne 0) { Fail 'The probe saw a running copy when none was open' }
$first = Start-Program
Wait-Window $first 30
Start-Sleep -Seconds 2
$code = Invoke-Probe
if ($code -ne 3) { Fail "A second copy was not refused while one was open (probe exit $code)" }
Write-Host 'A second copy is refused while one is open'
# A real second start shows "already open" and must leave the first alone.
$second = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 4
$second.Refresh()
if ($second.HasExited -and $second.ExitCode -ne 0) { Fail "The second copy failed (exit $($second.ExitCode))" }
if (-not $second.HasExited) { Stop-Process -Id $second.Id -Force }
$first.Refresh()
if ($first.HasExited) { Fail 'The first copy ended when a second was started' }
Stop-Program $first 'first copy after a second was started'
if ((Invoke-Probe) -ne 0) { Fail 'The lock was not released when the program quit' }
Write-Host 'The running copy was unaffected and released the lock when it quit'

# --- 4. What was written, and where -----------------------------------------
Write-Host "Program data folder ($dataFolder):"
Get-ChildItem -LiteralPath $dataFolder -Recurse -File | ForEach-Object { '  ' + $_.FullName.Substring($dataFolder.Length + 1) }
$programFiles = Get-ChildItem -LiteralPath (Join-Path $Package 'Program') -Recurse -File -Include *.sqlite, *.sqlite-*, *.log, *.json, *.bin, *.lock
if ($programFiles) { $programFiles | ForEach-Object { Write-Host "  $($_.FullName)" }; Fail 'The program wrote into its own folder' }
& (Join-Path $Package 'checks\test-songs-manifest.ps1') -Folder (Join-Path $Package 'Test Songs') -Verify (Join-Path $Package 'checks\test-songs-manifest.txt')
if ($LASTEXITCODE -ne 0) { Fail 'The Test Songs folder changed' }

Write-Host ''
Write-Host '--- Log of the last run ---'
Get-Content $log | Select-Object -First 40
Write-Host 'Smoke test passed.'
