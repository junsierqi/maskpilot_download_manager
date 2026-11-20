param([Parameter(Mandatory=$true)][string]$PackageDirectory, [string]$PackageArchive)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$sourceZip = if ($PackageArchive) { (Resolve-Path -LiteralPath $PackageArchive).Path } else { "$package.zip" }
if (-not (Test-Path -LiteralPath $sourceZip -PathType Leaf)) { throw 'Supply the package ZIP for the offline update and rollback smoke test.' }
$python = (Get-Command python).Source
$projectRoot = Split-Path -Parent $PSScriptRoot
$originalLocation = (Get-Location).ProviderPath
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('maskpilot-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$originalPath = $env:PATH
$variables = @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QT_ROOT_DIR','QT_QPA_PLATFORM')
$saved = @{}
foreach ($name in $variables) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process'); [Environment]::SetEnvironmentVariable($name,$null,'Process') }
try {
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    foreach ($file in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Qt6Svg.dll','libcurl.dll','libz.dll','nghttp2.dll','sqlite3.dll','torrent-rasterbar.dll','libcrypto-3-x64.dll','libssl-3-x64.dll','libxml2.dll','libssh2.dll','plugins/platforms/qwindows.dll','plugins/platforms/qoffscreen.dll','media_guard/ffmpeg.exe','media_guard/ffprobe.exe','browser_extension/manifest.json','browser_extension_firefox/manifest.json','scripts/update.ps1','scripts/package-common.ps1','release.json')) { if (-not (Test-Path -LiteralPath "$package/$file")) { throw "Missing deployment file: $file" } }
    foreach ($file in @('licenses/libtorrent/copyright','licenses/openssl/copyright','licenses/boost-headers/copyright','licenses/libxml2/copyright','licenses/libssh2/copyright')) { if (-not (Test-Path -LiteralPath "$package/$file")) { throw "Missing dependency notice: $file" } }
    $release = Get-Content -LiteralPath "$package/release.json" -Raw | ConvertFrom-Json
    if ($release.schema -ne 1 -or $release.product -ne 'MaskPilot' -or $release.architecture -ne 'x64' -or $release.version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release metadata.' }
    foreach ($file in @('maskpilot.exe','maskpilot_cli.exe','maskpilot_native_host.exe','media_guard/ffmpeg.exe','media_guard/ffprobe.exe')) {
        $info = (Get-Item -LiteralPath "$package/$file").VersionInfo
        if ($info.ProductVersion -ne $release.version -or $info.FileVersion -ne ($release.version + '.0')) { throw "Stale executable metadata: $file" }
    }
    foreach ($folder in @('browser_extension','browser_extension_firefox')) {
        if ((Get-Content -LiteralPath "$package/$folder/manifest.json" -Raw | ConvertFrom-Json).version -ne $release.version) { throw 'Extension and release versions differ.' }
    }
    $process = Start-Process -FilePath "$package/maskpilot.exe" -ArgumentList @('--smoke-test','--data-dir',('"' + "$temporary/data" + '"')) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(20000)) { $process.Kill(); throw 'Packaged desktop did not exit.' }
    if ($process.ExitCode -ne 0) { throw "Packaged desktop failed: $($process.ExitCode)" }
    & "$package/maskpilot_cli.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Packaged verification client failed.' }
    & $python "$projectRoot/tests/integration/native_host_test.py" "$package/maskpilot_native_host.exe" "$package/maskpilot.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Packaged native bridge integration failed.' }
    $installed = Join-Path $temporary 'installed'
    & "$package/scripts/install.ps1" -InstallDirectory $installed -NoShortcuts -NoBrowserRegistration -NoUninstallRegistration
    New-Item -ItemType Directory -Path "$installed/downloads" | Out-Null
    $retained = Join-Path $installed 'downloads/keep.bin'
    [IO.File]::WriteAllText($retained,'smoke-owned downloaded bytes')
    & "$package/scripts/install.ps1" -InstallDirectory $installed -NoShortcuts -NoBrowserRegistration -NoUninstallRegistration
    & "$installed/scripts/update.ps1" -Action Finalize -InstallDirectory $installed
    $sourceHash = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash
    & "$installed/scripts/update.ps1" -Action Apply -Package $sourceZip -PackageSha256 $sourceHash -InstallDirectory $installed
    & "$installed/scripts/update.ps1" -Action Rollback -InstallDirectory $installed
    & "$installed/scripts/update.ps1" -Action Finalize -InstallDirectory $installed
    & "$installed/scripts/uninstall.ps1" -NoShortcuts
    if (-not (Test-Path -LiteralPath $retained) -or [IO.File]::ReadAllText($retained) -ne 'smoke-owned downloaded bytes' -or (Test-Path -LiteralPath "$installed/maskpilot.exe")) { throw 'Uninstall did not preserve an unknown download.' }
    Write-Output 'PASS packaged desktop, CLI, native handoff and ownership-preserving install/update/rollback with developer PATH removed.'
} finally {
    if (Test-Path -LiteralPath $originalLocation) { Set-Location -LiteralPath $originalLocation }
    $env:PATH = $originalPath
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
    $resolved = [IO.Path]::GetFullPath($temporary)
    if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf).StartsWith('maskpilot-smoke-')) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
