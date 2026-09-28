# Records, or checks, every file and folder in a music folder: name, size
# and SHA-256. Used to prove Frankie's Karaoke Studio wrote nothing there.
#   test-songs-manifest.ps1 -Folder <music folder> -Write <manifest>
#   test-songs-manifest.ps1 -Folder <music folder> -Verify <manifest>
param(
    [Parameter(Mandatory)] [string] $Folder,
    [string] $Write,
    [string] $Verify
)
$ErrorActionPreference = 'Stop'

function Get-Listing([string] $root) {
    $root = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\')
    Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($root.Length + 1)
        if ($_.PSIsContainer) {
            "folder`t$relative"
        } else {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            "file`t$relative`t$($_.Length)`t$hash"
        }
    }
}

$listing = @(Get-Listing $Folder)
if ($Write) {
    $listing | Set-Content -LiteralPath $Write -Encoding utf8
    Write-Host "Recorded $($listing.Count) entries of $Folder"
    exit 0
}
if (-not $Verify) { throw 'Give -Write or -Verify' }
$expected = @(Get-Content -LiteralPath $Verify -Encoding utf8)
$differences = Compare-Object -CaseSensitive -ReferenceObject $expected -DifferenceObject $listing
if (-not $differences) {
    Write-Host ''
    Write-Host "UNCHANGED: all $($expected.Count) files and folders in"
    Write-Host "  $Folder"
    Write-Host 'are exactly as they were. Nothing was written there.'
    exit 0
}
Write-Host ''
Write-Host 'CHANGED: the folder is not as it was. Please report this:'
foreach ($difference in $differences) {
    $what = if ($difference.SideIndicator -eq '=>') { 'NEW or CHANGED' } else { 'MISSING or CHANGED' }
    Write-Host "  $what  $($difference.InputObject)"
}
exit 1
