param(
    [ValidateSet('Apply','Recover','Rollback','Finalize')][string]$Action = 'Apply',
    [string]$Package = '',
    [string]$PackageSha256 = '',
    [Parameter(Mandatory=$true)][string]$InstallDirectory
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/package-common.ps1"
$target = MP-GetTarget $InstallDirectory
if ($Action -eq 'Apply' -and (-not $Package -or -not $PackageSha256)) { throw 'Apply requires a ZIP and its independently trusted SHA256.' }
if ($Action -ne 'Apply' -and ($Package -or $PackageSha256)) { throw 'Recovery actions accept only the installation directory.' }
$lock = MP-EnterTargetLock $target
try {
    $null = MP-InvokeUpdate $Action $target $Package $PackageSha256
    Write-Output "MaskPilot payload $Action completed. Data was not restored or downgraded; no application was launched."
} finally { MP-ExitTargetLock $lock }
