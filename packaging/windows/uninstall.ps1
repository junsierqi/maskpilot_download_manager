param([switch]$NoShortcuts)
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
function Assert-NoReparse([string]$Path) {
    while ($Path) {
        if ((Test-Path -LiteralPath $Path) -and ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing an uninstall path containing a junction or symbolic link.' }
        $parent = Split-Path $Path -Parent
        if ($parent -eq $Path) { break }
        $Path = $parent
    }
}
Assert-NoReparse $target
Assert-NoReparse (Join-Path $target 'maskpilot-install.json')
$markerPath = Join-Path $target 'maskpilot-install.json'
if (-not (Test-Path -LiteralPath $markerPath)) { throw 'Installation marker missing; refusing directory removal.' }
$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
if ($marker.product -ne 'MaskPilot' -or $marker.schema -ne 2 -or -not $marker.files -or $marker.directory -ne $target -or $target -eq [IO.Path]::GetPathRoot($target) -or $target -eq $env:USERPROFILE -or $target -eq $env:LOCALAPPDATA) { throw 'Invalid installation marker or target.' }
$inventory = @()
foreach ($record in $marker.files) {
    if (-not $record.path -or [IO.Path]::IsPathRooted($record.path) -or $record.path -match '(^|[\\/])\.\.([\\/]|$)|:' -or $record.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Unsafe installed inventory entry.' }
    foreach ($component in ($record.path -split '[\\/]')) {
        if (-not $component -or $component -match '[<>:"|?*\x00-\x1f]' -or $component.EndsWith('.') -or $component.EndsWith(' ') -or $component -match '^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(\.|$)') { throw 'Invalid Windows installed path component.' }
    }
    $file = [IO.Path]::GetFullPath((Join-Path $target $record.path))
    if (-not $file.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Installed path escaped the installation directory.' }
    Assert-NoReparse $file
    $inventory += @{ file=$file; sha256=$record.sha256 }
}
Get-Process maskpilot,maskpilot_native_host -ErrorAction SilentlyContinue | ForEach-Object { if ($_.Path -and $_.Path.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Close MaskPilot before uninstalling.' } }
# Restore exact-owned HKCU64 browser defaults before deleting their manifest/journal files.
# Any cleanup failure stops uninstall, leaving the recovery journal and installation intact.
Assert-NoReparse (Join-Path $target 'browser-registration.json')
& "$target/scripts/register-browser.ps1" -InstallDirectory $target -Unregister
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MaskPilot'
if ((Test-Path -LiteralPath $key) -and (Get-ItemProperty -LiteralPath $key).InstallLocation -eq $target) { Remove-Item -LiteralPath $key }
if (-not $NoShortcuts) {
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'MaskPilot'
    foreach ($name in @('MaskPilot.lnk','Uninstall MaskPilot.lnk')) {
        $file = Join-Path $menu $name
        if (Test-Path -LiteralPath $file) {
            $link = (New-Object -ComObject WScript.Shell).CreateShortcut($file)
            if ([IO.Path]::GetFullPath($link.TargetPath) -eq [IO.Path]::GetFullPath("$target/maskpilot.exe") -or $link.Arguments.Contains("$target/scripts/uninstall.ps1")) { Remove-Item -LiteralPath $file -Force }
        }
    }
    if ((Test-Path -LiteralPath $menu) -and @(Get-ChildItem -LiteralPath $menu -Force).Count -eq 0) { Remove-Item -LiteralPath $menu }
}
# Remove only inventoried files whose bytes still match this installation.
# Unknown downloads and user-modified files remain, even inside the application directory.
$current = (Get-Location).ProviderPath
if ($current -eq $target -or $current.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) { Set-Location -LiteralPath ([IO.Path]::GetTempPath()) }
$directories = @()
foreach ($record in $inventory) {
    if ((Test-Path -LiteralPath $record.file -PathType Leaf) -and (Get-FileHash -LiteralPath $record.file -Algorithm SHA256).Hash -ieq $record.sha256) { Remove-Item -LiteralPath $record.file -Force }
    $parent = Split-Path $record.file -Parent
    while ($parent.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) { $directories += $parent; $parent = Split-Path $parent -Parent }
}
Remove-Item -LiteralPath $markerPath -Force
foreach ($directory in $directories | Select-Object -Unique | Sort-Object { $_.Length } -Descending) {
    if ((Test-Path -LiteralPath $directory) -and @(Get-ChildItem -LiteralPath $directory -Force).Count -eq 0) { Remove-Item -LiteralPath $directory }
}
if (@(Get-ChildItem -LiteralPath $target -Force).Count -eq 0) { Remove-Item -LiteralPath $target }
Write-Output 'Uninstalled MaskPilot. Application data, task history and downloaded files were kept.'
