$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $env:QT_ROOT_DIR) {
    $qmakeCommand = Get-Command qmake -ErrorAction SilentlyContinue
    if ($qmakeCommand) { $env:QT_ROOT_DIR = (& $qmakeCommand.Source -query QT_INSTALL_PREFIX).Trim() }
}
if (-not $env:QT_ROOT_DIR -or -not (Test-Path "$env:QT_ROOT_DIR/lib/cmake/Qt6")) {
    throw 'Set QT_ROOT_DIR to a Qt 6 MSVC x64 installation.'
}
$vswherePath = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $vswherePath)) { throw 'Visual Studio with Desktop development with C++ is required.' }
$vsInstall = (& $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $vsInstall) { throw 'MSVC x64 build tools were not found.' }
if (-not $env:VCPKG_ROOT) {
    $bundledVcpkg = Join-Path $vsInstall 'VC/vcpkg'
    if (Test-Path "$bundledVcpkg/vcpkg.exe") { $env:VCPKG_ROOT = $bundledVcpkg }
    else { throw 'Set VCPKG_ROOT to a bootstrapped vcpkg installation.' }
}
$compilerRoot = Get-ChildItem -LiteralPath (Join-Path $vsInstall 'VC/Tools/MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'bin/Hostx64/x64/cl.exe')) } |
    Sort-Object { [Version]$_.Name } -Descending | Select-Object -First 1
if (-not $compilerRoot) { throw 'An installed MSVC x64 compiler is required.' }
$compilerVersion = [Version]$compilerRoot.Name
$compilerSelection = '{0}.{1}' -f $compilerVersion.Major, $compilerVersion.Minor
$devCommand = 'call "' + (Join-Path $vsInstall 'Common7/Tools/VsDevCmd.bat') + '" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver=' + $compilerSelection + ' >nul && set'
$devEnvironment = & $env:ComSpec /s /c $devCommand
if ($LASTEXITCODE -ne 0) { throw 'Failed to initialize MSVC environment.' }
foreach ($entry in $devEnvironment) {
    if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$ninjaCandidate = Join-Path (Split-Path (Split-Path $env:QT_ROOT_DIR -Parent) -Parent) 'Tools/Ninja'
if (-not (Test-Path "$ninjaCandidate/ninja.exe")) {
    $ninjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($ninjaCommand) { $ninjaCandidate = Split-Path -Parent $ninjaCommand.Source }
    else { $ninjaCandidate = Join-Path $vsInstall 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja' }
}
if (-not (Test-Path "$ninjaCandidate/ninja.exe")) { throw 'Install Ninja and add ninja.exe to PATH.' }
$env:PATH = "$ninjaCandidate;$env:PATH"
$env:PATH = "$env:QT_ROOT_DIR/bin;$env:PATH"
Set-Location -LiteralPath $projectRoot
