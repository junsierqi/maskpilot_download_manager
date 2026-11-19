param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/build.ps1" -Configuration $Configuration
if (-not $?) { throw 'Build failed.' }
$preset = 'windows-msvc-' + $Configuration.ToLowerInvariant()
& ctest --preset $preset --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
foreach ($suite in @('release_paths_test.py', 'release_manifest_test.py')) {
    & python "$PSScriptRoot/../tests/system/$suite"
    if ($LASTEXITCODE -ne 0) { throw "Release verification tests failed: $suite" }
}
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/../tests/system/package_update_test.ps1" -SourceRoot (Split-Path -Parent $PSScriptRoot)
if ($LASTEXITCODE -ne 0) { throw 'Package update and recovery tests failed.' }
if (Get-Command npm.cmd -ErrorAction SilentlyContinue) {
    & npm.cmd --prefix browser_extension ci
    if ($LASTEXITCODE -ne 0) { throw 'Extension dependency install failed.' }
    & npm.cmd --prefix browser_extension test
    if ($LASTEXITCODE -ne 0) { throw 'Extension tests failed.' }
} else { throw 'Node and npm are required for the complete verification suite.' }
