param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/build.ps1" -Configuration $Configuration
if (-not $?) { throw 'Build failed.' }
$preset = 'windows-msvc-' + $Configuration.ToLowerInvariant()
& ctest --preset $preset --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
if (Get-Command npm.cmd -ErrorAction SilentlyContinue) {
    & npm.cmd --prefix browser_extension ci
    if ($LASTEXITCODE -ne 0) { throw 'Extension dependency install failed.' }
    & npm.cmd --prefix browser_extension test
    if ($LASTEXITCODE -ne 0) { throw 'Extension tests failed.' }
} else { throw 'Node and npm are required for the complete verification suite.' }
