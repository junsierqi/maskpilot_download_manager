param([string]$Configuration = 'Release', [switch]$SkipBuild, [string]$BuildDirectory,
      [string]$OutputDirectory, [string]$QualificationManifest, [string]$QualificationRuntimeDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/bootstrap.ps1"
. "$projectRoot/packaging/windows/package-common.ps1"
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
if ([IO.Path]::GetFullPath((Read-BuildCacheValue 'CMAKE_HOME_DIRECTORY')).TrimEnd('\','/') -ine $projectRoot.TrimEnd('\','/')) {
    throw 'The build does not belong to this source checkout.'
}
if ((Read-BuildCacheValue 'VCPKG_TARGET_TRIPLET') -ne 'x64-windows-portable') { throw 'Packaging requires the portable Release dependency triplet.' }
$dependencyRoot = Join-Path (Read-BuildCacheValue 'VCPKG_INSTALLED_DIR') (Read-BuildCacheValue 'VCPKG_TARGET_TRIPLET')
MP-AssertNoReparse $dependencyRoot
if (-not (Test-Path -LiteralPath "$dependencyRoot/bin")) { throw 'Configured portable dependency runtime is missing.' }
if ([IO.Path]::GetFullPath((Read-BuildCacheValue 'Qt6_DIR')) -ine [IO.Path]::GetFullPath("$env:QT_ROOT_DIR/lib/cmake/Qt6") -or
    [IO.Path]::GetFullPath((Read-BuildCacheValue 'CMAKE_CXX_COMPILER')) -ine [IO.Path]::GetFullPath("$env:VCToolsInstallDir/bin/Hostx64/x64/cl.exe")) {
    throw 'The build uses a different Qt installation or MSVC toolset. Reconfigure with cmake --fresh and rebuild.'
}
$version = ([regex]::Match((Get-Content "$projectRoot/CMakeLists.txt" -Raw), 'project\(MaskPilot VERSION ([0-9.]+)')).Groups[1].Value
if (-not $version) { throw 'Project version was not found.' }
if ((Read-BuildCacheValue 'CMAKE_PROJECT_VERSION') -ne $version) { throw 'Build version does not match the current project. Reconfigure and rebuild before packaging.' }
$clientInfo = & "$buildRoot/maskpilot_cli.exe"
if ($LASTEXITCODE -ne 0 -or $clientInfo[0] -ne "MaskPilot $version verification client") { throw 'The built verification client is stale or cannot load its runtime.' }
$qualification = $null
if ([bool]$QualificationManifest -xor [bool]$QualificationRuntimeDirectory) { throw 'Supply both qualification manifest and runtime directory.' }
if ($QualificationManifest) {
    & python "$PSScriptRoot/release_manifest.py" verify --root $projectRoot --build-directory $QualificationRuntimeDirectory --manifest $QualificationManifest
    if ($LASTEXITCODE -ne 0) { throw 'Frozen runtime qualification verification failed.' }
    $qualification = Get-Content -LiteralPath $QualificationManifest -Raw | ConvertFrom-Json
}
$packageRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'out/package'))
$packageBase = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\') } else { $packageRoot }
if ($packageBase -ine $packageRoot -and -not $packageBase.StartsWith($packageRoot + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Package output must stay under repository out/package.' }
MP-AssertNoReparse $packageBase
$stage = [IO.Path]::GetFullPath((Join-Path $packageBase "MaskPilot-$version-windows-x64"))
if (-not $stage.StartsWith($packageBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staging path.' }
$zip = "$stage.zip"
foreach ($path in @($stage, $zip, "$zip.sha256")) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite an existing package artifact: $path. Choose a new -OutputDirectory." }
}
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($exe in @('maskpilot.exe','maskpilot_cli.exe','maskpilot_native_host.exe')) { Copy-Item -LiteralPath "$buildRoot/$exe" -Destination $stage }
New-Item -ItemType Directory -Path "$stage/media_guard" -Force | Out-Null
foreach ($exe in @('ffmpeg.exe','ffprobe.exe')) { Copy-Item -LiteralPath "$buildRoot/media_guard/$exe" -Destination "$stage/media_guard" }
foreach ($dll in @('libcurl.dll','libz.dll','nghttp2.dll','sqlite3.dll','torrent-rasterbar.dll','libcrypto-3-x64.dll','libssl-3-x64.dll','libxml2.dll')) { Copy-Item -LiteralPath "$dependencyRoot/bin/$dll" -Destination $stage }
# Keep the deployed modules identical to the independently qualified runtime.
# Windows supplies its own system dependencies, including ICU.
foreach ($dll in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Qt6Svg.dll')) {
    Copy-Item -LiteralPath "$env:QT_ROOT_DIR/bin/$dll" -Destination $stage
}
$runtimeRoot = Get-ChildItem -LiteralPath (Join-Path $env:VSINSTALLDIR 'VC/Redist/MSVC') -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [Version]$_.Name } -Descending | Select-Object -First 1
if (-not $runtimeRoot) { throw 'MSVC redistributable runtime directory missing.' }
Copy-Item -Path (Join-Path $runtimeRoot.FullName 'x64/Microsoft.VC143.CRT/*.dll') -Destination $stage
# Deploy only the desktop plugins used by this application, without unrelated Qt modules.
foreach ($pair in @(@('platforms','qwindows.dll'),@('platforms','qoffscreen.dll'),@('styles','qmodernwindowsstyle.dll'),@('iconengines','qsvgicon.dll'),@('imageformats','qsvg.dll'),@('imageformats','qjpeg.dll'),@('imageformats','qgif.dll'),@('tls','qschannelbackend.dll'))) {
    $folder = Join-Path $stage ('plugins/' + $pair[0]); New-Item -ItemType Directory -Path $folder -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $env:QT_ROOT_DIR ('plugins/' + $pair[0] + '/' + $pair[1])) -Destination $folder
}
New-Item -ItemType Directory -Path "$stage/translations" -Force | Out-Null
Copy-Item -LiteralPath "$env:QT_ROOT_DIR/translations/qtbase_zh_CN.qm" -Destination "$stage/translations"
@('[Paths]','Prefix=.','Plugins=plugins','Translations=translations') | Set-Content -LiteralPath "$stage/qt.conf" -Encoding ASCII
if ($qualification) {
    $runtimeFiles = @(Get-ChildItem -LiteralPath $stage -File | Where-Object { $_.Extension -in @('.exe','.dll') -or $_.Name -eq 'qt.conf' })
    $runtimeFiles += @(Get-ChildItem -LiteralPath "$stage/media_guard" -File | Where-Object { $_.Extension -in @('.exe','.dll') })
    foreach ($directory in @('plugins','translations','platforms','styles','imageformats','iconengines','networkinformation','tls','generic')) {
        if (Test-Path -LiteralPath "$stage/$directory") { $runtimeFiles += @(Get-ChildItem -LiteralPath "$stage/$directory" -File -Recurse) }
    }
    $actualNames = @($runtimeFiles | ForEach-Object { $_.FullName.Substring($stage.Length + 1).Replace('\','/') } | Sort-Object)
    $expectedNames = @($qualification.runtime.PSObject.Properties.Name | Sort-Object)
    if (@(Compare-Object $actualNames $expectedNames).Count) { throw 'Packaged runtime file set differs from the frozen qualification.' }
    foreach ($entry in $qualification.runtime.PSObject.Properties) {
        $file = Join-Path $stage $entry.Name
        if ((Get-Item -LiteralPath $file).Length -ne $entry.Value.size -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $entry.Value.sha256) { throw "Packaged runtime differs from qualification: $($entry.Name)" }
    }
    Write-Output "PASS packaged runtime matches all $($expectedNames.Count) frozen qualification files."
}
& npm.cmd --prefix "$projectRoot/browser_extension" run build
if ($LASTEXITCODE -ne 0) { throw 'Browser extension build failed.' }
Copy-Item -LiteralPath "$projectRoot/browser_extension/dist" -Destination "$stage/browser_extension" -Recurse
Copy-Item -LiteralPath "$projectRoot/browser_extension/dist-firefox" -Destination "$stage/browser_extension_firefox" -Recurse
foreach ($dir in @('scripts','licenses','dependency-sources')) { New-Item -ItemType Directory -Path "$stage/$dir" -Force | Out-Null }
Copy-Item -LiteralPath "$projectRoot/scripts/register-browser.ps1" -Destination "$stage/scripts"
Copy-Item -LiteralPath "$projectRoot/packaging/windows/install.ps1","$projectRoot/packaging/windows/uninstall.ps1","$projectRoot/packaging/windows/update.ps1","$projectRoot/packaging/windows/package-common.ps1" -Destination "$stage/scripts"
Copy-Item -LiteralPath "$projectRoot/packaging/windows/Install.cmd","$projectRoot/packaging/windows/Uninstall.cmd","$projectRoot/README.md","$projectRoot/LICENSE" -Destination $stage
Copy-Item -LiteralPath "$projectRoot/third_party/NOTICES.md" -Destination "$stage/licenses/NOTICES.md"
Copy-Item -LiteralPath "$projectRoot/docs" -Destination "$stage/docs" -Recurse
New-Item -ItemType Directory -Path "$stage/third_party" -Force | Out-Null
Copy-Item -LiteralPath "$projectRoot/third_party/NOTICES.md","$projectRoot/third_party/sources.json" -Destination "$stage/third_party"
Copy-Item -LiteralPath "$projectRoot/third_party/sources.json" -Destination "$stage/dependency-sources/sources.json"
Copy-Item -LiteralPath "$projectRoot/cmake/ports" -Destination "$stage/dependency-sources/portable-ports" -Recurse
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
foreach ($dependency in @('curl','nghttp2','zlib','sqlite3','libtorrent','openssl','libxml2')) {
    New-Item -ItemType Directory -Path "$stage/licenses/$dependency" -Force | Out-Null
    Copy-Item -LiteralPath "$dependencyRoot/share/$dependency/copyright" -Destination "$stage/licenses/$dependency"
}
# Versioned Boost CMake configuration folders are not installed library ports.
foreach ($dependency in @(Get-ChildItem -LiteralPath "$dependencyRoot/share" -Directory -Filter 'boost-*' | Where-Object { $_.Name -notin @('boost-uninstall','boost-cmake') -and $_.Name -notmatch '^boost-\d+(\.\d+)+$' })) {
    $copyright = Join-Path $dependency.FullName 'copyright'
    if (-not (Test-Path -LiteralPath $copyright)) { throw "Missing Boost dependency notice: $($dependency.Name)" }
    $noticeDirectory = Join-Path $stage ('licenses/' + $dependency.Name)
    New-Item -ItemType Directory -Path $noticeDirectory -Force | Out-Null
    Copy-Item -LiteralPath $copyright -Destination $noticeDirectory
}
$licensePath = Join-Path $env:VSINSTALLDIR 'Licenses/1033/Redist.txt'
Copy-Item -LiteralPath $licensePath -Destination "$stage/licenses/Microsoft-runtime-redistribution.txt"
$sourceCommit = (& git -C $projectRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceCommit -notmatch '^[a-f0-9]{40,64}$') { throw 'Cannot identify package source commit.' }
$sourceStatus = @(& git -C $projectRoot status --porcelain=v1)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect package source status.' }
$release = [ordered]@{ schema=1; product='MaskPilot'; version=$version; architecture='x64'; sourceCommit=$sourceCommit; sourceDirty=($sourceStatus.Count -ne 0); qtVersion=$qtVersion; compilerVersion=([IO.DirectoryInfo]$env:VCToolsInstallDir).Name }
if ($qualification) {
    $release['qualificationManifestSha256'] = (Get-FileHash -LiteralPath $QualificationManifest -Algorithm SHA256).Hash.ToLowerInvariant()
    $release['qualifiedSourceCommit'] = $qualification.sourceCommit
}
[IO.File]::WriteAllText("$stage/release.json",($release | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
$hashes = Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($stage.Length + 1).Replace('\','/')
}
[IO.File]::WriteAllText("$stage/SHA256SUMS.txt",(($hashes -join "`n") + "`n"),[Text.UTF8Encoding]::new($false))
$crtRoot = Join-Path $runtimeRoot.FullName 'x64/Microsoft.VC143.CRT'
& python "$PSScriptRoot/audit_release_paths.py" --root $projectRoot --directory $stage --qt-root $env:QT_ROOT_DIR --crt-root $crtRoot
if ($LASTEXITCODE -ne 0) { throw 'Package stage path/provenance audit failed.' }
$zipPending = Join-Path $packageBase ('.MaskPilot-' + $version + '-' + [Guid]::NewGuid().ToString('N') + '.zip')
Compress-Archive -LiteralPath $stage -DestinationPath $zipPending -CompressionLevel Optimal
[IO.File]::Move($zipPending,$zip)
& python "$PSScriptRoot/audit_release_paths.py" --root $projectRoot --archive $zip --qt-root $env:QT_ROOT_DIR --crt-root $crtRoot
if ($LASTEXITCODE -ne 0) { throw 'Package archive path/provenance audit failed.' }
$digest = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$digestBytes = [Text.Encoding]::ASCII.GetBytes($digest + '  ' + [IO.Path]::GetFileName($zip) + "`n")
$digestStream = [IO.File]::Open("$zip.sha256",[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try { $digestStream.Write($digestBytes,0,$digestBytes.Length); $digestStream.Flush($true) } finally { $digestStream.Dispose() }
Write-Output "Packaged $zip"
Write-Output "SHA256 $digest"
