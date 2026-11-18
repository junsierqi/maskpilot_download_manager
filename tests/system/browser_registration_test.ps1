param([string]$SourceRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference = 'Stop'
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$register = Join-Path $SourceRoot 'scripts/register-browser.ps1'
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $SourceRoot 'out/TEMP'))
$fixture = [IO.Path]::GetFullPath((Join-Path $fixtureRoot ('browser-registration-' + [Guid]::NewGuid().ToString('N'))))
if (-not $fixture.StartsWith($fixtureRoot + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe registration fixture directory.' }
$tag = 'MaskPilotFixture' + [Guid]::NewGuid().ToString('N')
$keys = @('Software\Google\Chrome\NativeMessagingHosts\com.maskpilot.downloads','Software\Microsoft\Edge\NativeMessagingHosts\com.maskpilot.downloads','Software\Mozilla\NativeMessagingHosts\com.maskpilot.downloads')
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$mutex = [Threading.Mutex]::new($false,'Local\MaskPilot-BrowserRegistration-Test')
$held = $false
$originals = @()
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Capture([string]$Path) {
    $key = $registry.OpenSubKey($Path,$false)
    try {
        $state = @{ present=($null -ne $key); hadDefault=$false; kind=$null; value=$null }
        if ($null -ne $key -and $key.GetValueNames() -contains '') { $state.hadDefault=$true; $state.kind=$key.GetValueKind(''); $state.value=$key.GetValue('',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        return $state
    } finally { if ($null -ne $key) { $key.Dispose() } }
}
function Write-Default([string]$Path,$Value,[Microsoft.Win32.RegistryValueKind]$Kind) {
    $key = $registry.CreateSubKey($Path,$true)
    try { $key.SetValue('',$Value,$Kind) } finally { $key.Dispose() }
}
function Clear-Default([string]$Path) {
    $key = $registry.OpenSubKey($Path,$true)
    if ($null -ne $key) { try { $key.DeleteValue('',$false) } finally { $key.Dispose() } }
}
function Remove-Empty([string]$Path) {
    $key=$registry.OpenSubKey($Path,$false);$empty=$false
    try { if ($null -ne $key) { $empty=$key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0 } } finally { if ($null -ne $key) { $key.Dispose() } }
    if ($empty) { $registry.DeleteSubKey($Path,$false) }
}
try {
    try { $held=$mutex.WaitOne(10000) } catch [Threading.AbandonedMutexException] { $held=$true }
    Assert $held 'Another owned registration fixture is active.'
    foreach ($path in $keys) { $originals += Capture $path }
    New-Item -ItemType Directory -Path $fixture -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $fixture 'maskpilot_native_host.exe'),'owned non-executable fixture')
    [IO.File]::WriteAllText((Join-Path $fixture 'maskpilot.exe'),'owned non-executable fixture')
    foreach ($path in $keys) {
        $key=$registry.CreateSubKey($path,$true)
        try { $key.SetValue($tag,'unrelated value',[Microsoft.Win32.RegistryValueKind]::String) } finally { $key.Dispose() }
        $child=$registry.CreateSubKey(($path+'\'+$tag),$true);try { $child.SetValue('fixture','preserve') } finally { $child.Dispose() }
    }
    Write-Default $keys[0] 'C:\prior-browser-fixture\chrome.json' ([Microsoft.Win32.RegistryValueKind]::String)
    Write-Default $keys[1] '%LOCALAPPDATA%\prior-browser-fixture\edge.json' ([Microsoft.Win32.RegistryValueKind]::ExpandString)
    Write-Default $keys[2] 37 ([Microsoft.Win32.RegistryValueKind]::DWord)
    $sentinels=@();foreach($path in $keys){$sentinels+=Capture $path}
    & $register -InstallDirectory $fixture -DataDirectory (Join-Path $fixture 'data')
    $journal=Join-Path $fixture 'browser-registration.json'
    Assert (Test-Path -LiteralPath $journal -PathType Leaf) 'No durable registration journal was created.'
    $first=[IO.File]::ReadAllBytes($journal)
    $firefox=Get-Content -LiteralPath (Join-Path $fixture 'com.maskpilot.downloads.firefox.json') -Raw | ConvertFrom-Json
    Assert ($firefox.name -eq 'com.maskpilot.downloads' -and @($firefox.allowed_extensions).Count -eq 1 -and $firefox.allowed_extensions[0] -eq 'downloads@maskpilot.invalid') 'Wrong Firefox manifest identity.'
    $bridge=Get-Content -LiteralPath (Join-Path $fixture 'native-bridge.json') -Raw | ConvertFrom-Json
    Assert ($bridge.allowedFirefoxExtensions[0] -eq 'downloads@maskpilot.invalid') 'Missing native host Gecko allowlist.'
    & $register -InstallDirectory $fixture -DataDirectory (Join-Path $fixture 'data')
    Assert ([Convert]::ToBase64String($first) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($journal))) 'Re-registration replaced the original prior-default journal.'
    & $register -InstallDirectory $fixture -Unregister
    for($i=0;$i -lt $keys.Count;++$i){$now=Capture $keys[$i];Assert ($now.hadDefault -and $now.kind -eq $sentinels[$i].kind -and $now.value -ceq $sentinels[$i].value) 'Prior default kind/value was not restored.'}
    Assert (-not (Test-Path -LiteralPath $journal)) 'Clean unregister left its journal.'
    Write-Output 'PASS durable prior kind/value restoration and idempotent registration'
    foreach($path in $keys){Clear-Default $path}
    & $register -InstallDirectory $fixture
    Write-Default $keys[2] 'C:\external-browser-fixture\firefox.json' ([Microsoft.Win32.RegistryValueKind]::String)
    & $register -InstallDirectory $fixture -Unregister
    Assert (-not (Capture $keys[0]).hadDefault -and -not (Capture $keys[1]).hadDefault) 'Previously absent defaults were not removed.'
    Assert ((Capture $keys[2]).value -eq 'C:\external-browser-fixture\firefox.json') 'External default was overwritten during cleanup.'
    foreach($path in $keys){$key=$registry.OpenSubKey($path,$false);try{Assert ($key.GetValue($tag) -eq 'unrelated value') 'Unrelated value removed.';Assert ($key.GetSubKeyNames() -contains $tag) 'Unrelated subkey removed.'}finally{$key.Dispose()}}
    Write-Output 'PASS absent-default restoration and external/unrelated content preservation'
    & $register -InstallDirectory $fixture
    $before=[IO.File]::ReadAllBytes($journal)
    Write-Default $keys[1] 'C:\external-browser-fixture\edge.json' ([Microsoft.Win32.RegistryValueKind]::String)
    $rejected=$false;try{& $register -InstallDirectory $fixture}catch{$rejected=$true}
    Assert $rejected 'Registration silently replaced an external edit after journaling.'
    Assert ([Convert]::ToBase64String($before) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($journal))) 'Conflict destroyed the recovery journal.'
    & $register -InstallDirectory $fixture -Unregister
    Assert ((Capture $keys[1]).value -eq 'C:\external-browser-fixture\edge.json') 'Conflict cleanup replaced external default.'
    Write-Output 'PASS conflicting retry fails closed while preserving recovery'
    # Journal publication must succeed before any own registry write.
    New-Item -ItemType Directory -Path $journal | Out-Null
    $beforeDefaults=@();foreach($path in $keys){$beforeDefaults+=Capture $path}
    $rejected=$false;try{& $register -InstallDirectory $fixture}catch{$rejected=$true}
    Assert $rejected 'A non-writable journal did not stop registration.'
    for($i=0;$i -lt $keys.Count;++$i){$now=Capture $keys[$i];Assert ($now.kind -eq $beforeDefaults[$i].kind -and $now.value -ceq $beforeDefaults[$i].value -and $now.hadDefault -eq $beforeDefaults[$i].hadDefault) 'Registry changed before journal publication.'}
    Remove-Item -LiteralPath $journal
    Write-Output 'PASS failed journal publication leaves registry untouched'
} finally {
    try {
        if($originals.Count -eq $keys.Count){
            for($i=0;$i -lt $keys.Count;++$i){
                $path=$keys[$i];$original=$originals[$i]
                if($original.hadDefault){Write-Default $path $original.value $original.kind}else{Clear-Default $path}
                $key=$registry.OpenSubKey($path,$true);if($null -ne $key){try{$key.DeleteValue($tag,$false)}finally{$key.Dispose()}}
                $child=$path+'\'+$tag;$handle=$registry.OpenSubKey($child,$true);if($null -ne $handle){try{$handle.DeleteValue('fixture',$false)}finally{$handle.Dispose()};Remove-Empty $child}
                if(-not $original.present){Remove-Empty $path}
            }
        }
    } finally {
        $registry.Dispose()
        if($held){$mutex.ReleaseMutex()};$mutex.Dispose()
        # Only the independently generated, checked workspace is eligible for recursive deletion.
        if($fixture.StartsWith($fixtureRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $fixture)){Remove-Item -LiteralPath $fixture -Recurse -Force}
    }
}
