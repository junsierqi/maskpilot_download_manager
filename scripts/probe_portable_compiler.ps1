param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'out/qa/portable compiler probe' }
$probe = [IO.Path]::GetFullPath($OutputDirectory)
$owned = [IO.Path]::GetFullPath((Join-Path $repo 'out')) + [IO.Path]::DirectorySeparatorChar
if (-not $probe.StartsWith($owned, [StringComparison]::OrdinalIgnoreCase)) { throw 'Compiler probe output must be inside repository out/.' }
New-Item -ItemType Directory -Force -Path $probe | Out-Null
$policyModule = Join-Path $repo 'cmake/PortableFlags.cmake'
$policyLoader = Join-Path $probe 'load-policy.cmake'
# Transport the exact root through a file: Windows PowerShell's native argument
# quoting cannot represent a quoted path ending in a single backslash reliably.
[IO.File]::WriteAllText($policyLoader, @'
file(READ "${MASKPILOT_PROBE_ROOT_FILE}" MASKPILOT_SOURCE_ROOT)
include("${MASKPILOT_PROBE_POLICY_MODULE}")
'@)
$header = @'
#pragma once
#include <source_location>
inline const char* header_file() { return __FILE__; }
inline const char* header_location() { return std::source_location::current().file_name(); }
'@
$source = @'
#include "header with spaces.h"
#include <cstdio>
#ifdef MASKPILOT_PROBE_BOOST
#include <boost/system/error_code.hpp>
#endif
int main() {
  puts(__FILE__);
  puts(std::source_location::current().file_name());
  puts(header_file());
  puts(header_location());
#ifdef MASKPILOT_PROBE_BOOST
  boost::source_location where = BOOST_CURRENT_LOCATION;
  boost::system::error_code error(1, boost::system::generic_category(), &where);
  puts(error.what().c_str());
#endif
}
'@
[IO.File]::WriteAllText((Join-Path $probe 'header with spaces.h'), $header)
[IO.File]::WriteAllText((Join-Path $probe 'probe.cpp'), $source)
$boost = @('out/dependencies-portable/x64-windows-portable/include', 'out/dependencies/x64-windows/include') |
    ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path (Join-Path $_ 'boost/system/error_code.hpp') } | Select-Object -First 1
$boostFlags = if ($boost) { @("/I$boost", '/DMASKPILOT_PROBE_BOOST') } else { @() }
Push-Location -LiteralPath $probe
try {
    foreach ($case in @('override-native', 'override-forward', 'trailing-native', 'trailing-forward', 'default-native', 'default-forward')) {
        $mode, $style = $case.Split('-')
        $policy = Join-Path $probe ("flags-$case.json")
        $policyArguments = @("-DOUTPUT_FILE=$policy")
        $policyScript = $policyModule
        if ($mode -ne 'default') {
            $mappingRoot = if ($mode -eq 'trailing') {
                $probe + $(if ($style -eq 'native') { [IO.Path]::DirectorySeparatorChar } else { '/' })
            } else { $probe }
            $rootFile = Join-Path $probe ("root-$case.txt")
            [IO.File]::WriteAllText($rootFile, $mappingRoot)
            $policyArguments += @("-DMASKPILOT_PROBE_ROOT_FILE=$rootFile", "-DMASKPILOT_PROBE_POLICY_MODULE=$policyModule")
            $policyScript = $policyLoader
        }
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $policyOutput = & cmake @policyArguments -P $policyScript 2>&1
        $policyExit = $LASTEXITCODE
        $ErrorActionPreference = $oldPreference
        if ($policyExit -ne 0 -or ($policyOutput -join "`n") -match 'CMake (Warning|Error)') {
            throw "Cannot generate portable compiler policy: $($policyOutput -join [Environment]::NewLine)"
        }
        $flags = Get-Content -LiteralPath $policy -Raw | ConvertFrom-Json
        $relativeProbe = if ($mode -eq 'default') { $probe.Substring($repo.Length + 1).Replace('\', '/') + '/' } else { '' }
        $expectedSource = './' + $relativeProbe + 'probe.cpp'
        $expectedHeader = './' + $relativeProbe + 'header with spaces.h'
        $inputPath = Join-Path $probe 'probe.cpp'
        if ($style -eq 'forward') { $inputPath = $inputPath.Replace('\', '/') }
        $arguments = @('/nologo', '/std:c++20', '/EHsc', '/utf-8', '/Zi') + @($flags.compile) + $boostFlags + @($inputPath, '/Fo:probe.obj', '/Fd:compiler.pdb', '/Fe:probe.exe', '/link', '/DEBUG:FULL') + @($flags.link)
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $compilerOutput = & cl.exe @arguments 2>&1
        $compilerExit = $LASTEXITCODE
        $ErrorActionPreference = $oldPreference
        $compilerOutput | ForEach-Object { Write-Output $_ }
        if ($compilerExit -ne 0 -or ($compilerOutput -join "`n") -match 'D900[27]') { throw "Portable compiler policy failed for $case paths." }
        $actual = @(& ./probe.exe)
        $expectedCount = if ($boost) { 5 } else { 4 }
        if ($LASTEXITCODE -ne 0 -or $actual.Count -ne $expectedCount) { throw 'Compiler probe did not exercise all diagnostic paths.' }
        if (($actual -join "`n") -match '[A-Za-z]:[\\/]' -or ($actual -join "`n").Contains($probe)) { throw 'Compiler retained an absolute source path.' }
        $expectedNames = @($expectedSource, $expectedSource, $expectedHeader, $expectedHeader)
        for ($index = 0; $index -lt 4; $index++) {
            if ($actual[$index].Replace('\', '/') -ne $expectedNames[$index]) { throw "Portable $case diagnostics changed the relative source path." }
        }
        if ($boost -and -not $actual[4].Replace('\', '/').Contains($expectedSource)) { throw 'Boost diagnostics lost their useful relative filename.' }
        $actual | ForEach-Object { Write-Output $_ }
    }
} finally { Pop-Location }
Write-Output "PASS: MSVC default/override/trailing-root policies preserve source/header diagnostics for both path styles; Boost exercised: $([bool]$boost)."
