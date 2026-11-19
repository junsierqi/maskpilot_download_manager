param([string]$InstallDirectory = '', [switch]$NoShortcuts, [switch]$NoBrowserRegistration, [switch]$NoUninstallRegistration)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/package-common.ps1"
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
if (-not $InstallDirectory) { $InstallDirectory = Join-Path $env:LOCALAPPDATA 'Programs/MaskPilot' }
$target = MP-GetTarget $InstallDirectory
$lock = MP-EnterTargetLock $target
try {
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\MaskPilot'
if (-not $NoUninstallRegistration -and (Test-Path -LiteralPath $key) -and (Get-ItemProperty -LiteralPath $key).InstallLocation -ne $target) { throw 'A different MaskPilot installation owns the uninstall registration.' }
$result = MP-InstallDirectoryPackage $source $target
# Same-target upgrades preserve browser paths, settings, prior defaults and ownership hashes.
if ($result.fresh -and -not $NoBrowserRegistration) {
    & "$target/scripts/register-browser.ps1" -InstallDirectory $target
    $installed = MP-ReadInstalled $target
    foreach ($name in $script:MPDynamic) {
        $path = MP-Join $target $name
        if ([IO.File]::Exists($path)) { $installed.value.files += @{path=$name;sha256=(MP-Hash $path)} }
    }
    MP-WriteBytes (Join-Path $target 'maskpilot-install.json') (MP-JsonBytes $installed.value) -Replace
}
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
    New-ItemProperty -LiteralPath $key -Name DisplayVersion -Value $result.version -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name InstallLocation -Value $target -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name DisplayIcon -Value "$target/maskpilot.exe" -PropertyType String -Force | Out-Null
    $uninstallCommand = '"' + "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" + '" -NoProfile -ExecutionPolicy Bypass -File "' + "$target/scripts/uninstall.ps1" + '"'
    New-ItemProperty -LiteralPath $key -Name UninstallString -Value $uninstallCommand -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -LiteralPath $key -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
}
Write-Output "Installed MaskPilot in $target. Your task database and downloaded files are preserved."

} finally { MP-ExitTargetLock $lock }
