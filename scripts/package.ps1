param([string]$Configuration = 'Release', [switch]$SkipBuild, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/bootstrap.ps1"
if ($Configuration -ne 'Release') { throw 'Distributable packages require Release binaries.' }
if ($BuildDirectory -and -not $SkipBuild) { throw 'Use -SkipBuild when packaging an explicit existing build directory.' }
if (-not $SkipBuild) { & "$PSScriptRoot/build.ps1" -Configuration Release }
$buildRoot = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $projectRoot 'out/build/windows-msvc-release' }
$cache = Get-Content -LiteralPath "$buildRoot/CMakeCache.txt" -Raw
function Read-BuildCacheValue([string]$Name) {
    $match = [regex]::Match($cache, '(?m)^' + [regex]::Escape($Name) + ':[^=]+=([^\r\n]*)')
    if (-not $match.Success) { throw "Missing build configuration: $Name" }
    return $match.Groups[1].Value
}
if ((Read-BuildCacheValue 'CMAKE_BUILD_TYPE') -ne 'Release' -or (Read-BuildCacheValue 'MASKPILOT_ENABLE_ASAN') -ne 'OFF') {
    throw 'Rebuild a non-instrumented Release before packaging.'
}
if ([IO.Path]::GetFullPath((Read-BuildCacheValue 'Qt6_DIR')) -ine [IO.Path]::GetFullPath("$env:QT_ROOT_DIR/lib/cmake/Qt6") -or
    [IO.Path]::GetFullPath((Read-BuildCacheValue 'CMAKE_CXX_COMPILER')) -ine [IO.Path]::GetFullPath("$env:VCToolsInstallDir/bin/Hostx64/x64/cl.exe")) {
    throw 'The build uses a different Qt installation or MSVC toolset. Reconfigure with cmake --fresh and rebuild.'
}
$version = ([regex]::Match((Get-Content "$projectRoot/CMakeLists.txt" -Raw), 'project\(MaskPilot VERSION ([0-9.]+)')).Groups[1].Value
if (-not $version) { throw 'Project version was not found.' }
if ((Read-BuildCacheValue 'CMAKE_PROJECT_VERSION') -ne $version) { throw 'Build version does not match the current project. Reconfigure and rebuild before packaging.' }
$clientInfo = & "$buildRoot/maskpilot_cli.exe"
if ($LASTEXITCODE -ne 0 -or $clientInfo[0] -ne "MaskPilot $version verification client") { throw 'The built verification client is stale or cannot load its runtime.' }
$packageBase = [IO.Path]::GetFullPath((Join-Path $projectRoot 'out/package'))
$stage = [IO.Path]::GetFullPath((Join-Path $packageBase "MaskPilot-$version-windows-x64"))
if (-not $stage.StartsWith($packageBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staging path.' }
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($exe in @('maskpilot.exe','maskpilot_cli.exe','maskpilot_native_host.exe')) { Copy-Item -LiteralPath "$buildRoot/$exe" -Destination $stage }
foreach ($dll in @('libcurl.dll','libz.dll','nghttp2.dll','sqlite3.dll','torrent-rasterbar.dll','libcrypto-3-x64.dll','libssl-3-x64.dll')) { Copy-Item -LiteralPath "$projectRoot/out/dependencies/x64-windows/bin/$dll" -Destination $stage }
& "$env:QT_ROOT_DIR/bin/windeployqt.exe" --release --compiler-runtime --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler --no-ffmpeg --no-plugins --no-translations --dir $stage "$stage/maskpilot.exe"
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
# Windows supplies ICU. Do not redistribute a system DLL selected from the build machine.
if (Test-Path -LiteralPath "$stage/icuuc.dll") { Remove-Item -LiteralPath "$stage/icuuc.dll" -Force }
$runtimeRoot = Get-ChildItem -LiteralPath (Join-Path $env:VSINSTALLDIR 'VC/Redist/MSVC') -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [Version]$_.Name } -Descending | Select-Object -First 1
if (-not $runtimeRoot) { throw 'MSVC redistributable runtime directory missing.' }
Copy-Item -Path (Join-Path $runtimeRoot.FullName 'x64/Microsoft.VC143.CRT/*.dll') -Destination $stage
# Deploy only the desktop plugins used by this application, without unrelated Qt modules.
foreach ($pair in @(@('platforms','qwindows.dll'),@('platforms','qoffscreen.dll'),@('styles','qmodernwindowsstyle.dll'),@('iconengines','qsvgicon.dll'),@('imageformats','qsvg.dll'),@('imageformats','qjpeg.dll'),@('imageformats','qgif.dll'),@('tls','qschannelbackend.dll'))) {
    $folder = Join-Path $stage ('plugins/' + $pair[0]); New-Item -ItemType Directory -Path $folder -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $env:QT_ROOT_DIR ('plugins/' + $pair[0] + '/' + $pair[1])) -Destination $folder
}
Copy-Item -LiteralPath "$env:QT_ROOT_DIR/bin/Qt6Svg.dll" -Destination $stage
New-Item -ItemType Directory -Path "$stage/translations" -Force | Out-Null
Copy-Item -LiteralPath "$env:QT_ROOT_DIR/translations/qtbase_zh_CN.qm" -Destination "$stage/translations"
@('[Paths]','Prefix=.','Plugins=plugins','Translations=translations') | Set-Content -LiteralPath "$stage/qt.conf" -Encoding ASCII
& npm.cmd --prefix "$projectRoot/browser_extension" run build
if ($LASTEXITCODE -ne 0) { throw 'Browser extension build failed.' }
Copy-Item -LiteralPath "$projectRoot/browser_extension/dist" -Destination "$stage/browser_extension" -Recurse
foreach ($dir in @('scripts','licenses','dependency-sources')) { New-Item -ItemType Directory -Path "$stage/$dir" -Force | Out-Null }
Copy-Item -LiteralPath "$projectRoot/scripts/register-browser.ps1" -Destination "$stage/scripts"
Copy-Item -LiteralPath "$projectRoot/packaging/windows/install.ps1","$projectRoot/packaging/windows/uninstall.ps1" -Destination "$stage/scripts"
Copy-Item -LiteralPath "$projectRoot/packaging/windows/Install.cmd","$projectRoot/packaging/windows/Uninstall.cmd","$projectRoot/README.md","$projectRoot/LICENSE" -Destination $stage
Copy-Item -LiteralPath "$projectRoot/third_party/NOTICES.md" -Destination "$stage/licenses/NOTICES.md"
Copy-Item -LiteralPath "$projectRoot/docs" -Destination "$stage/docs" -Recurse
New-Item -ItemType Directory -Path "$stage/third_party" -Force | Out-Null
Copy-Item -LiteralPath "$projectRoot/third_party/NOTICES.md","$projectRoot/third_party/sources.json" -Destination "$stage/third_party"
Copy-Item -LiteralPath "$projectRoot/third_party/sources.json" -Destination "$stage/dependency-sources/sources.json"
$catalog = Get-Content "$projectRoot/third_party/sources.json" -Raw | ConvertFrom-Json
$qtVersion = (& "$env:QT_ROOT_DIR/bin/qmake.exe" -query QT_VERSION).Trim()
if ($qtVersion -ne $catalog.qtVersion) { throw 'Update the pinned source catalog before packaging a different Qt version.' }
foreach ($archive in $catalog.archives) {
    $source = "$projectRoot/out/dependency-sources/$($archive.file)"
    if (-not (Test-Path -LiteralPath $source) -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $archive.sha256) { throw "Run scripts/dependency-sources.ps1 first: $($archive.file)" }
    Copy-Item -LiteralPath $source -Destination "$stage/dependency-sources"
}
$qtLicenses = Join-Path (Split-Path (Split-Path $env:QT_ROOT_DIR -Parent) -Parent) 'Licenses'
if (Test-Path -LiteralPath $qtLicenses) {
    Copy-Item -LiteralPath $qtLicenses -Destination "$stage/licenses/Qt" -Recurse
} else {
    # Standalone SDK archives omit the installer-wide license directory.
    # Extract license texts from the already verified, matching module sources.
    foreach ($archive in $catalog.archives) {
        $module = $archive.file.Split('-')[0]
        $licenseModule = "$stage/licenses/Qt/$module"
        New-Item -ItemType Directory -Path $licenseModule -Force | Out-Null
        $archiveRoot = $archive.file -replace '\.tar\.xz$', ''
        & tar -xJf "$projectRoot/out/dependency-sources/$($archive.file)" -C $licenseModule --strip-components 1 "$archiveRoot/LICENSES"
        if ($LASTEXITCODE -ne 0 -or -not (Get-ChildItem -LiteralPath "$licenseModule/LICENSES" -File -ErrorAction SilentlyContinue)) {
            throw "Matching Qt license texts could not be extracted: $module"
        }
    }
}
foreach ($module in @('qtbase','qtsvg','qttranslations')) {
    Copy-Item -LiteralPath "$env:QT_ROOT_DIR/sbom/$module-$qtVersion.spdx.json" -Destination "$stage/licenses/Qt"
}
foreach ($dependency in @('curl','nghttp2','zlib','sqlite3','libtorrent','openssl')) {
    New-Item -ItemType Directory -Path "$stage/licenses/$dependency" -Force | Out-Null
    Copy-Item -LiteralPath "$projectRoot/out/dependencies/x64-windows/share/$dependency/copyright" -Destination "$stage/licenses/$dependency"
}
# Versioned Boost CMake configuration folders are not installed library ports.
foreach ($dependency in @(Get-ChildItem -LiteralPath "$projectRoot/out/dependencies/x64-windows/share" -Directory -Filter 'boost-*' | Where-Object { $_.Name -notin @('boost-uninstall','boost-cmake') -and $_.Name -notmatch '^boost-\d+(\.\d+)+$' })) {
    $copyright = Join-Path $dependency.FullName 'copyright'
    if (-not (Test-Path -LiteralPath $copyright)) { throw "Missing Boost dependency notice: $($dependency.Name)" }
    $noticeDirectory = Join-Path $stage ('licenses/' + $dependency.Name)
    New-Item -ItemType Directory -Path $noticeDirectory -Force | Out-Null
    Copy-Item -LiteralPath $copyright -Destination $noticeDirectory
}
$licensePath = Join-Path $env:VSINSTALLDIR 'Licenses/1033/Redist.txt'
Copy-Item -LiteralPath $licensePath -Destination "$stage/licenses/Microsoft-runtime-redistribution.txt"
$hashes = Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($stage.Length + 1).Replace('\','/')
}
$hashes | Set-Content -LiteralPath "$stage/SHA256SUMS.txt" -Encoding ASCII
$zip = "$stage.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
Write-Output "Packaged $zip"
