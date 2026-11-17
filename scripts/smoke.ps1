param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
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
    foreach ($file in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Qt6Svg.dll','libcurl.dll','libz.dll','nghttp2.dll','sqlite3.dll','plugins/platforms/qwindows.dll','plugins/platforms/qoffscreen.dll')) { if (-not (Test-Path -LiteralPath "$package/$file")) { throw "Missing deployment file: $file" } }
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
    & "$installed/scripts/uninstall.ps1" -NoShortcuts
    if (-not (Test-Path -LiteralPath $retained) -or [IO.File]::ReadAllText($retained) -ne 'smoke-owned downloaded bytes' -or (Test-Path -LiteralPath "$installed/maskpilot.exe")) { throw 'Uninstall did not preserve an unknown download.' }
    Write-Output 'PASS packaged desktop, CLI and native handoff with developer PATH removed.'
} finally {
    if (Test-Path -LiteralPath $originalLocation) { Set-Location -LiteralPath $originalLocation }
    $env:PATH = $originalPath
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
    $resolved = [IO.Path]::GetFullPath($temporary)
    if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf).StartsWith('maskpilot-smoke-')) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
