param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/bootstrap.ps1"
$preset = 'windows-msvc-' + $Configuration.ToLowerInvariant()
if ($Configuration -eq 'Release') {
    & "$PSScriptRoot/probe_portable_compiler.ps1"
    if (-not $?) { throw 'Portable compiler capability check failed.' }
}
& cmake --preset $preset
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build --preset $preset --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
