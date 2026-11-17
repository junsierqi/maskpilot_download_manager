param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $projectRoot 'out/tools' }
$toolsRoot = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
$parserPath = Join-Path $toolsRoot 'yt-dlp.exe'
$parserHash = 'F5389D3698ECDE4EF06F4E6B1CE0294FE9AC1396228C8990B8D3416D6D177A7D'
if (Test-Path -LiteralPath $parserPath) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $parserPath).Hash -ne $parserHash) { throw 'Existing media parser has a different checksum. Choose an empty destination.' }
} else {
    $client = Join-Path $projectRoot 'out/build/windows-msvc-debug/maskpilot_cli.exe'
    if (-not (Test-Path -LiteralPath $client)) { throw 'Run scripts/build.ps1 before fetching media components.' }
    & $client 'https://github.com/yt-dlp/yt-dlp/releases/download/2026.10.26/yt-dlp.exe' $parserPath (Join-Path $toolsRoot 'transfer-state') 4 $parserHash
    if ($LASTEXITCODE -ne 0) { throw 'Media parser download did not complete. Its partial data is preserved.' }
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $parserPath).Hash -ne $parserHash) { throw 'Media parser checksum mismatch.' }
$processor = Get-Command ffmpeg -ErrorAction SilentlyContinue
Write-Output "Media parser ready: $parserPath"
if ($processor) { Write-Output "Media processor ready: $($processor.Source)" }
else { Write-Output 'Use Tools > Media support in the desktop application to download or locate the media processor.' }
