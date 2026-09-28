# Builds the portable Windows x64 package of Frankie's Karaoke Studio:
#
#   <Output>/
#     README-FIRST.txt
#     Check Installation.cmd          runs the program's --self-check
#     Check Test Songs Unchanged.cmd  proves nothing was written into Test Songs
#     Test Songs/                     synthetic MP3+CDG songs (no real media)
#     checks/                         the Test Songs manifest and its checker
#     Program/                        the program, Qt, GStreamer, VC++ runtime
#
# Run from a Visual Studio x64 developer shell (dumpbin, VCToolsRedistDir).
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $QtPrefix,
    [Parameter(Mandatory)] [string] $GStreamerRoot,
    [Parameter(Mandatory)] [string] $Output
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$program = Join-Path $Output 'Program'
if (Test-Path $Output) { Remove-Item -Recurse -Force $Output }
New-Item -ItemType Directory -Force -Path $program | Out-Null

# --- The program -------------------------------------------------------------
Copy-Item (Join-Path $BuildDir 'FrankiesKaraokeStudio.exe') $program

# --- Qt ----------------------------------------------------------------------
# windeployqt copies the Qt DLLs and the plugin folders Qt needs
# (platforms\qwindows.dll, sqldrivers\qsqlite.dll, styles, image formats).
& (Join-Path $QtPrefix 'bin\windeployqt.exe') --release --no-translations `
    --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime `
    --exclude-plugins qsqlodbc,qsqlpsql,qsqlmimer,qsqlibase,qsqloci,qsqldb2 `
    (Join-Path $program 'FrankiesKaraokeStudio.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
foreach ($required in 'platforms\qwindows.dll', 'sqldrivers\qsqlite.dll', 'Qt6Core.dll', 'Qt6Widgets.dll', 'Qt6Sql.dll') {
    if (-not (Test-Path (Join-Path $program $required))) { throw "Qt deployment is missing $required" }
}

# --- Visual C++ runtime (app-local, so no separate installer is needed) --------
$crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Select-Object -First 1
if (-not $crt) { throw "Visual C++ runtime not found under $env:VCToolsRedistDir" }
Copy-Item (Join-Path $crt.FullName '*.dll') $program

# --- GStreamer -----------------------------------------------------------------
# Only the plugins a karaoke song needs (see KaraokePlayer::requiredElements),
# plus the other Windows sound systems as fallbacks for autoaudiosink.
$plugins = @(
    'coreelements', 'playback', 'typefindfunctions', 'pbtypes',
    'audioconvert', 'audioresample', 'volume', 'audiotestsrc', 'autodetect',
    'wasapi2', 'wasapi', 'directsound',
    'soundtouch', 'audiofx',
    'mpg123', 'audioparsers', 'id3demux', 'apetag'
)
$gstBin = Join-Path $GStreamerRoot 'bin'
$pluginDir = Join-Path $program 'lib\gstreamer-1.0'
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
foreach ($plugin in $plugins) {
    $file = Join-Path $GStreamerRoot "lib\gstreamer-1.0\gst$plugin.dll"
    if (-not (Test-Path $file)) { throw "GStreamer plugin missing from the SDK: $plugin" }
    Copy-Item $file $pluginDir
}
$scanner = Join-Path $GStreamerRoot 'libexec\gstreamer-1.0\gst-plugin-scanner.exe'
if (-not (Test-Path $scanner)) { throw "gst-plugin-scanner.exe not found" }
Copy-Item $scanner $program

# Every DLL any packaged file needs, found in the GStreamer or Qt SDK, is
# copied beside the program, repeatedly until nothing new is needed.
function Get-Imports([string] $file) {
    $lines = & dumpbin /nologo /dependents $file
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $file" }
    $lines | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[^\s]+\.dll$' }
}
$present = @{}
Get-ChildItem $program -Recurse -Include *.dll, *.exe | ForEach-Object { $present[$_.Name.ToLowerInvariant()] = $true }
$queue = [System.Collections.Generic.Queue[string]]::new()
Get-ChildItem $program -Recurse -Include *.dll, *.exe | ForEach-Object { $queue.Enqueue($_.FullName) }
$external = @{}
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    foreach ($import in Get-Imports $file) {
        $key = $import.ToLowerInvariant()
        if ($present.ContainsKey($key)) { continue }
        $source = @((Join-Path $gstBin $import), (Join-Path $QtPrefix "bin\$import")) |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($source) {
            Copy-Item $source $program
            $present[$key] = $true
            $queue.Enqueue((Join-Path $program (Split-Path -Leaf $source)))
        } else {
            $external[$key] = (Split-Path -Leaf $file)
        }
    }
}

# Anything not packaged must be part of Windows itself. The Visual C++
# runtime is not: some computers do not have it, so it must be packaged.
$problems = @()
foreach ($name in $external.Keys) {
    if ($name -like 'api-ms-win-*' -or $name -like 'ext-ms-*') { continue }
    if ($name -match '^(msvcp|vcruntime|concrt|vccorlib)\d') {
        $problems += "$name (needed by $($external[$name])) is the Visual C++ runtime but is not packaged"
    } elseif (-not (Test-Path (Join-Path $env:SystemRoot "System32\$name"))) {
        $problems += "$name (needed by $($external[$name])) is neither packaged nor part of Windows"
    }
}
if ($problems) { $problems | ForEach-Object { Write-Host "::error::$_" }; throw "Package is incomplete" }
Write-Host "Windows system DLLs used:" (($external.Keys | Sort-Object) -join ', ')

# --- Test Songs, their manifest, and the helper scripts -----------------------
$songs = Join-Path $Output 'Test Songs'
& (Join-Path $BuildDir 'tests\fks-make-test-songs.exe') $songs
if ($LASTEXITCODE -ne 0) { throw "Could not make the test songs" }
$checks = Join-Path $Output 'checks'
New-Item -ItemType Directory -Force -Path $checks | Out-Null
& (Join-Path $here 'test-songs-manifest.ps1') -Folder $songs -Write (Join-Path $checks 'test-songs-manifest.txt')
Copy-Item (Join-Path $here 'test-songs-manifest.ps1') $checks
Copy-Item (Join-Path $here 'Check Installation.cmd') $Output
Copy-Item (Join-Path $here 'Check Test Songs Unchanged.cmd') $Output
Copy-Item (Join-Path $here 'README-FIRST.txt') $Output

$size = (Get-ChildItem $Output -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("Package ready: {0} ({1:N0} MB, {2} files)" -f $Output, $size,
    (Get-ChildItem $Output -Recurse -File).Count)
