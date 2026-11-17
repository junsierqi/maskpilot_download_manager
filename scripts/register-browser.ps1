param(
    [Parameter(Mandatory=$true)][string]$InstallDirectory,
    [string]$DataDirectory = '',
    [switch]$Unregister
)
$ErrorActionPreference = 'Stop'
$installRoot = (Resolve-Path -LiteralPath $InstallDirectory).Path
$manifestPath = Join-Path $installRoot 'com.maskpilot.downloads.json'
$registryPaths = @('HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.maskpilot.downloads','HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.maskpilot.downloads')
if ($Unregister) {
    foreach ($registryPath in $registryPaths) {
        if ((Test-Path -LiteralPath $registryPath) -and (Get-Item -LiteralPath $registryPath).GetValue('') -eq $manifestPath) {
            Remove-Item -LiteralPath $registryPath
        }
    }
    Write-Output 'Removed this installation browser bridge registration.'
    return
}
$hostPath = Join-Path $installRoot 'maskpilot_native_host.exe'
$desktopPath = Join-Path $installRoot 'maskpilot.exe'
if (-not (Test-Path -LiteralPath $hostPath) -or -not (Test-Path -LiteralPath $desktopPath)) { throw 'Desktop and native host executables must be deployed together.' }
$origin = 'chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
$manifest = @{name='com.maskpilot.downloads';description='MaskPilot download bridge';path=$hostPath;type='stdio';allowed_origins=@($origin)}
$bridge = @{allowedOrigins=@($origin);desktopPath=$desktopPath}
if ($DataDirectory) { $bridge.dataDirectory = [IO.Path]::GetFullPath($DataDirectory) }
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 5),$utf8)
[IO.File]::WriteAllText((Join-Path $installRoot 'native-bridge.json'),($bridge | ConvertTo-Json -Depth 5),$utf8)
foreach ($registryPath in $registryPaths) { New-Item -Path $registryPath -Force | Out-Null; Set-Item -LiteralPath $registryPath -Value $manifestPath }
Write-Output 'Registered MaskPilot for the current user. Load the packaged browser_extension folder in browser developer mode.'
