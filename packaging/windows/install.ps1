param([string]$InstallDirectory = '', [switch]$NoShortcuts, [switch]$NoBrowserRegistration, [switch]$NoUninstallRegistration)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
if (-not $InstallDirectory) { $InstallDirectory = Join-Path $env:LOCALAPPDATA 'Programs/MaskPilot' }
$target = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
function Assert-NoReparse([string]$Path) {
    while ($Path) {
        if ((Test-Path -LiteralPath $Path) -and ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Installation paths must not contain junctions or symbolic links.' }
        $parent = Split-Path $Path -Parent
        if ($parent -eq $Path) { break }
        $Path = $parent
    }
}
Assert-NoReparse $target
Assert-NoReparse (Join-Path $target 'maskpilot-install.json')
if ($target -eq [IO.Path]::GetPathRoot($target) -or $target -eq $env:USERPROFILE -or $target -eq $env:LOCALAPPDATA) { throw 'Choose a dedicated application directory.' }
if ($target.StartsWith($source + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Install outside the package source directory.' }
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MaskPilot'
if (-not $NoUninstallRegistration -and (Test-Path -LiteralPath $key) -and (Get-ItemProperty -LiteralPath $key).InstallLocation -ne $target) { throw 'A different MaskPilot installation owns the uninstall registration.' }
$ownedFiles = @()
foreach ($line in Get-Content -LiteralPath "$source/SHA256SUMS.txt") {
    if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw 'Invalid package inventory.' }
    $hash = $matches[1]; $relative = $matches[2]
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)|:' -or $relative -eq 'maskpilot-install.json') { throw 'Unsafe package path.' }
    foreach ($component in ($relative -split '[\\/]')) {
        if (-not $component -or $component -match '[<>:"|?*\x00-\x1f]' -or $component.EndsWith('.') -or $component.EndsWith(' ') -or $component -match '^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(\.|$)') { throw 'Invalid Windows package path component.' }
    }
    $file = [IO.Path]::GetFullPath((Join-Path $source $relative))
    if (-not $file.StartsWith($source + '\',[StringComparison]::OrdinalIgnoreCase) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $hash) { throw "Package integrity check failed: $relative" }
    $ownedFiles += @{ path=$relative; sha256=$hash }
    Assert-NoReparse ([IO.Path]::GetFullPath((Join-Path $target $relative)))
}
$ownedFiles += @{ path='SHA256SUMS.txt'; sha256=(Get-FileHash -LiteralPath "$source/SHA256SUMS.txt" -Algorithm SHA256).Hash }
if ($target -ne $source) {
    if ((Test-Path -LiteralPath $target) -and @(Get-ChildItem -LiteralPath $target -Force).Count -gt 0 -and -not (Test-Path -LiteralPath "$target/maskpilot-install.json")) { throw 'The target is not an existing MaskPilot installation or an empty directory.' }
    if (Test-Path -LiteralPath "$target/maskpilot-install.json") {
        $existing = Get-Content -LiteralPath "$target/maskpilot-install.json" -Raw | ConvertFrom-Json
        if ($existing.product -ne 'MaskPilot' -or $existing.schema -ne 2 -or $existing.directory -ne $target) { throw 'Invalid existing installation marker.' }
    }
    Get-Process maskpilot,maskpilot_native_host -ErrorAction SilentlyContinue | ForEach-Object { if ($_.Path -and $_.Path.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Close the installed MaskPilot before upgrading.' } }
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($record in $ownedFiles) {
        $destination = Join-Path $target $record.path
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $record.path) -Destination $destination -Force
    }
}
$marker = @{ product='MaskPilot'; schema=2; installedAtUtc=[DateTime]::UtcNow.ToString('o'); directory=$target; files=$ownedFiles }
$marker | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$target/maskpilot-install.json" -Encoding UTF8
if (-not $NoBrowserRegistration) { & "$target/scripts/register-browser.ps1" -InstallDirectory $target }
foreach ($name in @('native-bridge.json','com.maskpilot.downloads.json')) {
    if (-not $NoBrowserRegistration -and (Test-Path -LiteralPath "$target/$name")) { $ownedFiles += @{ path=$name; sha256=(Get-FileHash -LiteralPath "$target/$name" -Algorithm SHA256).Hash } }
}
$marker.files = $ownedFiles
$marker | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$target/maskpilot-install.json" -Encoding UTF8
if (-not $NoShortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'MaskPilot'
    New-Item -ItemType Directory -Path $menu -Force | Out-Null
    foreach ($entry in @(@('MaskPilot','maskpilot.exe',''),@('Uninstall MaskPilot',"$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe",'-NoProfile -ExecutionPolicy Bypass -File "' + "$target/scripts/uninstall.ps1" + '"'))) {
        $shortcut = $shell.CreateShortcut((Join-Path $menu ($entry[0] + '.lnk')))
        $shortcut.TargetPath = if ([IO.Path]::IsPathRooted($entry[1])) { $entry[1] } else { Join-Path $target $entry[1] }
        $shortcut.Arguments = $entry[2]; $shortcut.WorkingDirectory = $target; $shortcut.Save()
    }
}
if (-not $NoUninstallRegistration) {
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name DisplayName -Value 'MaskPilot' -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name InstallLocation -Value $target -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name DisplayIcon -Value "$target/maskpilot.exe" -PropertyType String -Force | Out-Null
    $uninstallCommand = '"' + "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" + '" -NoProfile -ExecutionPolicy Bypass -File "' + "$target/scripts/uninstall.ps1" + '"'
    New-ItemProperty -LiteralPath $key -Name UninstallString -Value $uninstallCommand -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
}
Write-Output "Installed MaskPilot in $target. Your task database and downloaded files are preserved."
