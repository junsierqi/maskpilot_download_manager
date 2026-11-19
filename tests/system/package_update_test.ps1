param([string]$SourceRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference = 'Stop'
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
. "$SourceRoot/packaging/windows/package-common.ps1"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$base = Join-Path $SourceRoot 'out/TEMP'
$fixture = Join-Path $base ('package-update-'+[Guid]::NewGuid().ToString('N'))
$target = Join-Path $fixture 'installed'
$children = @()
function Assert($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Operation,[string]$Message) {
    $rejected=$false
    try { & $Operation | Out-Null } catch { $rejected=$true }
    Assert $rejected $Message
}
function Write-File([string]$Root,[string]$Relative,[string]$Content) {
    $path=MP-Join $Root $Relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path,$Content,[Text.UTF8Encoding]::new($false))
}
function Refresh-Inventory([string]$Root) {
    $lines=@(Get-ChildItem -LiteralPath $Root -File -Recurse | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object FullName | ForEach-Object { (MP-Hash $_.FullName)+'  '+$_.FullName.Substring($Root.Length+1).Replace('\','/') })
    [IO.File]::WriteAllLines((Join-Path $Root 'SHA256SUMS.txt'),[string[]]$lines,[Text.UTF8Encoding]::new($false))
}
function New-Package([string]$Name,[string]$Version,[string]$Extra) {
    $root=Join-Path $fixture $Name; [IO.Directory]::CreateDirectory($root) | Out-Null
    foreach ($relative in $script:MPRequired) { Write-File $root $relative "synthetic filesystem fixture $Version $relative" }
    foreach ($name in @('install.ps1','uninstall.ps1','update.ps1','package-common.ps1')) { [IO.File]::Copy((Join-Path $SourceRoot ('packaging/windows/'+$name)),(MP-Join $root ('scripts/'+$name)),$true) }
    Write-File $root 'release.json' (@{schema=1;product='MaskPilot';version=$Version;architecture='x64'} | ConvertTo-Json -Compress)
    Write-File $root $Extra "owned extra $Version"
    Refresh-Inventory $root
    return $root
}
function Zip-Package([string]$Root,[string]$Name) {
    $zip=Join-Path $fixture $Name
    [IO.Compression.ZipFile]::CreateFromDirectory($Root,$zip,[IO.Compression.CompressionLevel]::Optimal,$true)
    return $zip
}
function Action([string]$Name,[string]$Zip='',[scriptblock]$Fault=$null) {
    $lock=MP-EnterTargetLock $target
    try {
        $hash=''; if ($Zip) { $hash=MP-Hash $Zip }
        $null=MP-InvokeUpdate $Name $target $Zip $hash $Fault
    } finally { MP-ExitTargetLock $lock }
}
try {
    MP-AssertNoReparse $base
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    $old=New-Package 'old' '0.2.0' 'obsolete.txt'
    $new=New-Package 'new' '0.3.0' 'added.txt'
    Write-File $new '000-large.bin' ('L'*200000)
    Refresh-Inventory $new
    $zip=Zip-Package $new 'new.zip'
    $null=MP-InstallDirectoryPackage $old $target
    Assert (-not (MP-ReadTransaction $target)) 'Fresh installation must auto-finalize.'
    foreach ($field in @('schema','product','version','architecture','sourceDirty','sourceCommit','qtVersion','compilerVersion','qualificationManifestSha256','qualifiedSourceCommit')) {
        $release=MP-ParseObject '{"schema":1,"product":"MaskPilot","version":"0.3.0","architecture":"x64"}'
        $value='0.3.0'
        if ($field -eq 'schema') { $value=1 } elseif ($field -eq 'sourceDirty') { $value=$true } elseif ($field -eq 'product') { $value='MaskPilot' } elseif ($field -eq 'architecture') { $value='x64' } elseif ($field -in @('sourceCommit','qualifiedSourceCommit')) { $value='a'*40 } elseif ($field -eq 'qualificationManifestSha256') { $value='a'*64 }
        $release | Add-Member -MemberType NoteProperty -Name $field -Value @($value) -Force
        Reject { MP-ValidateRelease $release } 'Singleton array release metadata was accepted.'
    }
    Reject { MP-CheckRows @([pscustomobject]@{path=@('payload.bin');sha256=('a'*64)}) } 'Array-valued inventory path was accepted.'
    Reject { MP-CheckRows @([pscustomobject]@{path='payload.bin';sha256=@('a'*64)}) } 'Array-valued inventory hash was accepted.'
    Reject { MP-CheckRows ([pscustomobject]@{path='payload.bin';sha256=('a'*64)}) } 'Scalar inventory container was accepted.'
    Reject { MP-ParseObject '[{"schema":1}]' } 'An array root was unwrapped into a metadata object.'
    $markerPath=Join-Path $target 'maskpilot-install.json'; $markerBytes=[IO.File]::ReadAllBytes($markerPath)
    try {
        foreach ($field in @('product','directory','schema','files')) {
            $candidate=MP-ParseObject ([Text.Encoding]::UTF8.GetString($markerBytes))
            if ($field -eq 'files') { $candidate.files=$candidate.files[0] } else { $candidate.$field=@($candidate.$field) }
            [IO.File]::WriteAllBytes($markerPath,(MP-JsonBytes $candidate))
            Reject { MP-ReadInstalled $target } 'Malformed installed marker scalar/array type was accepted.'
        }
        $legacy=MP-ParseObject ([Text.Encoding]::UTF8.GetString($markerBytes)); $legacy.PSObject.Properties.Remove('version')
        [IO.File]::WriteAllBytes($markerPath,(MP-JsonBytes $legacy))
        $null=MP-ReadInstalled $target
    } finally { [IO.File]::WriteAllBytes($markerPath,$markerBytes) }
    Write-Output 'PASS strict release/inventory/marker scalar types with legacy version absence'
    Write-File $target 'downloads/keep.bin' 'unknown downloaded content'
    Write-File $target 'native-bridge.json' 'old dynamic configuration'
    $marker=MP-ReadInstalled $target
    $dynamicHash=MP-Hash (Join-Path $target 'native-bridge.json')
    $marker.value.files+=@{path='native-bridge.json';sha256=$dynamicHash}
    MP-WriteBytes (Join-Path $target 'maskpilot-install.json') (MP-JsonBytes $marker.value) -Replace
    Write-File $target 'native-bridge.json' 'user-edited dynamic configuration'
    $oldApp=MP-Hash (Join-Path $target 'maskpilot.exe')
    Action 'Apply' $zip
    Assert ((MP-Hash (Join-Path $target 'maskpilot.exe')) -eq (MP-Hash (Join-Path $new 'maskpilot.exe'))) 'New payload was not installed.'
    Assert (-not [IO.File]::Exists((Join-Path $target 'obsolete.txt'))) 'Obsolete owned file remains.'
    Assert ([IO.File]::ReadAllText((Join-Path $target 'native-bridge.json')) -eq 'user-edited dynamic configuration') 'Dynamic configuration changed.'
    Assert ((MP-ReadInstalled $target).rows['native-bridge.json'].sha256 -eq $dynamicHash) 'A user edit became newly owned.'
    Reject { Action 'Apply' $zip } 'An existing rollback receipt did not block another upgrade.'
    Action 'Rollback'; Action 'Finalize'
    Assert ((MP-Hash (Join-Path $target 'maskpilot.exe')) -eq $oldApp) 'Rollback did not restore the original app bytes.'
    Assert ([IO.File]::Exists((Join-Path $target 'obsolete.txt')) -and -not [IO.File]::Exists((Join-Path $target 'added.txt'))) 'Rollback inventory differs.'
    Assert ([IO.File]::ReadAllText((Join-Path $target 'downloads/keep.bin')) -eq 'unknown downloaded content') 'Unknown download changed.'
    Write-Output 'PASS upgrade, obsolete cleanup, receipt, rollback and dynamic/download preservation'

    foreach ($phase in @('Preparing','Applying','Applied','Committed')) {
        $selected=$phase
        Reject { Action 'Apply' $zip { param($at,$index) if ($at -eq $selected -and ($at -ne 'Applied' -or $index -eq 1)) { throw 'Owned deterministic fault.' } } } 'Phase fault was not exercised.'
        $transaction=MP-ReadTransaction $target
        Assert ($null -ne $transaction) 'Fault lost recovery ownership.'
        if ($transaction.journal.phase -eq 'Committed') { Action 'Rollback' } else { Action 'Recover' }
        Assert ((MP-Hash (Join-Path $target 'maskpilot.exe')) -eq $oldApp) 'Recovery did not restore old bytes.'
    }
    Write-Output 'PASS durable Preparing/Applying/per-file/Committed fault recovery'

    Reject { Action 'Apply' $zip { param($at,$index) if ($at -eq 'Preparing') { throw 'Owned typed-journal fixture.' } } } 'Typed journal fixture did not stop.'
    $typed=MP-ReadTransaction $target; $journalPath=Join-Path $typed.root 'journal.json'; $journalBytes=[IO.File]::ReadAllBytes($journalPath)
    try {
        foreach ($field in @('schema','product','target','nonce','phase','planHash','planBase64')) {
            $candidate=MP-ParseObject ([Text.Encoding]::UTF8.GetString($journalBytes)); $candidate.$field=@($candidate.$field)
            [IO.File]::WriteAllBytes($journalPath,(MP-JsonBytes $candidate))
            Reject { MP-ReadTransaction $target } 'Array-valued journal identity was accepted.'
        }
        foreach ($field in @('schema','target','nonce','newVersion','rows','path','oldHash','newHash')) {
            $candidate=MP-ParseObject ([Text.Encoding]::UTF8.GetString($journalBytes))
            $plan=MP-ParseObject ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($candidate.planBase64)))
            if ($field -eq 'rows') { $plan.rows=$plan.rows[0] }
            elseif ($field -in @('path','oldHash','newHash')) {
                $value=$plan.rows[0].$field; if ($null -eq $value) { $value='a'*64 }; $plan.rows[0].$field=@($value)
            } else { $plan.$field=@($plan.$field) }
            $planBytes=MP-JsonBytes $plan; $candidate.planBase64=[Convert]::ToBase64String($planBytes); $candidate.planHash=MP-BytesHash $planBytes
            [IO.File]::WriteAllBytes($journalPath,(MP-JsonBytes $candidate))
            Reject { MP-ReadTransaction $target } 'Malformed plan scalar/array type passed its valid outer checksum.'
        }
    } finally { [IO.File]::WriteAllBytes($journalPath,$journalBytes) }
    Action 'Recover'
    Write-Output 'PASS typed journal and hash-valid plan scalar/nullable-hash validation'

    Reject { Action 'Apply' $zip { param($at,$size) if ($at -eq 'CopyChunk' -and $size -eq 65536) { throw 'Owned mid-stream write failure.' } } } 'Mid-stream write fault was not exercised.'
    $preparing=MP-ReadTransaction $target
    Assert ($preparing.journal.phase -eq 'Preparing') 'A streaming fault reached live payload mutation.'
    Assert (-not [IO.File]::Exists((Join-Path $preparing.root 'stage/000-large.bin'))) 'Caught exclusive-copy failure left a partial artifact.'
    Assert ((MP-Hash (Join-Path $target 'maskpilot.exe')) -eq $oldApp) 'Caught streaming fault changed live payload.'
    Action 'Recover'
    $metadata=Join-Path $fixture 'metadata.json'
    [IO.File]::WriteAllText($metadata,'prior metadata')
    Reject { MP-WriteBytes $metadata ([Text.Encoding]::UTF8.GetBytes(('M'*200000))) -Replace -WriteFault { throw 'Owned metadata mid-write fault.' } } 'Metadata write fault was not exercised.'
    Assert ([IO.File]::ReadAllText($metadata) -eq 'prior metadata' -and -not [IO.File]::Exists($metadata+'.new')) 'Caught metadata write failure changed prior data or left a partial temporary.'
    [IO.File]::WriteAllText($metadata+'.new','foreign pending metadata')
    Reject { MP-WriteBytes $metadata ([Text.Encoding]::UTF8.GetBytes('replacement')) -Replace } 'Preexisting metadata temporary was overwritten.'
    Assert ([IO.File]::ReadAllText($metadata+'.new') -eq 'foreign pending metadata') 'Preexisting metadata temporary was deleted.'
    [IO.File]::Delete($metadata+'.new')
    $foreignCopy=Join-Path $fixture 'foreign-copy.bin'; [IO.File]::WriteAllText($foreignCopy,'foreign copy')
    Reject { MP-CopyVerified $new '000-large.bin' $foreignCopy (MP-Hash (Join-Path $new '000-large.bin')) } 'Preexisting copy destination was overwritten.'
    Assert ([IO.File]::ReadAllText($foreignCopy) -eq 'foreign copy') 'Preexisting copy destination was deleted.'
    Write-Output 'PASS mid-stream exclusive-write cleanup and preexisting-temporary preservation'

    Reject { Action 'Apply' $zip { param($at,$index) if ($at -eq 'Preparing') { throw 'Owned initial metadata boundary.' } } } 'Initial journal fixture did not stop.'
    $preparing=MP-ReadTransaction $target
    [IO.File]::WriteAllText((Join-Path $preparing.root 'journal.json.new'),'{')
    Reject { Action 'Recover' } 'Unverifiable torn metadata was guessed safe to remove.'
    Assert ((MP-Hash (Join-Path $target 'maskpilot.exe')) -eq $oldApp) 'Torn metadata changed old payload.'
    Assert ([IO.File]::ReadAllText((Join-Path $preparing.root 'journal.json.new')) -eq '{') 'Torn metadata was silently deleted.'
    # Explicit fixture-owned manual repair demonstrates the honest fail-closed boundary.
    [IO.File]::Delete((Join-Path $preparing.root 'journal.json.new')); Action 'Recover'
    Write-Output 'PASS torn metadata is preserved for explicit manual repair, with old payload unchanged'

    Reject { Action 'Apply' $zip { param($at,$index) if ($at -eq 'Applying') { throw 'Owned pending transaction.' } } } 'Pending-state fixture did not stop.'
    # The real uninstaller must stop BEFORE its browser/shell branch for pending payloads.
    $previousPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $output=& "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File (Join-Path $target 'scripts/uninstall.ps1') -NoShortcuts 2>&1
        $uninstallCode=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previousPreference }
    Assert ($uninstallCode -ne 0) 'Uninstall accepted an incomplete transaction.'
    Assert ([IO.File]::Exists((Join-Path $target 'maskpilot.exe')) -and (MP-ReadTransaction $target)) 'Rejected uninstall damaged installation ownership.'
    Action 'Recover'
    Write-Output 'PASS actual uninstaller refuses an incomplete transaction before shell changes'

    # Real locked destination failure leaves a complete recoverable journal, not partial new bytes.
    $lockedPath=Join-Path $target 'maskpilot.exe'
    $fileLock=[IO.FileStream]::new($lockedPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { Reject { Action 'Apply' $zip } 'Replacement of a locked owned file unexpectedly succeeded.' } finally { $fileLock.Dispose() }
    Action 'Recover'
    Assert ((MP-Hash $lockedPath) -eq $oldApp) 'Locked-file failure recovery failed.'
    Write-Output 'PASS real locked-file replacement failure'

    Write-File $target 'added.txt' 'foreign collision'
    Reject { Action 'Apply' $zip } 'Unknown colliding file was overwritten.'
    Assert ([IO.File]::ReadAllText((Join-Path $target 'added.txt')) -eq 'foreign collision') 'Collision bytes changed.'
    [IO.File]::Delete((Join-Path $target 'added.txt'))
    Write-File $target 'maskpilot.exe' 'user modification'
    Reject { Action 'Apply' $zip } 'Modified owned file was overwritten.'
    [IO.File]::Copy((Join-Path $old 'maskpilot.exe'),$lockedPath,$true)
    Reject { $null=MP-InvokeUpdate 'Apply' $target $zip ('0'*64) } 'Incorrect outer digest was accepted.'
    Assert (-not (MP-ReadTransaction $target)) 'Preflight failure created a live transaction.'
    Write-Output 'PASS unknown/modified collision and independent archive digest checks'

    Action 'Apply' $zip
    $txn=MP-ReadTransaction $target
    Write-File $txn.root 'unknown.bin' 'external transaction artifact'
    Reject { Action 'Finalize' } 'Unknown transaction artifact was removed.'
    Assert ([IO.File]::Exists((Join-Path $txn.root 'unknown.bin'))) 'Unknown transaction artifact was lost.'
    [IO.File]::Delete((Join-Path $txn.root 'unknown.bin'))
    Write-File $target 'maskpilot.exe' 'external change after upgrade'
    Reject { Action 'Rollback' } 'Rollback overwrote an external edit.'
    Assert ([IO.File]::ReadAllText($lockedPath) -eq 'external change after upgrade') 'External edit was overwritten.'
    [IO.File]::Copy((Join-Path $new 'maskpilot.exe'),$lockedPath,$true)
    Action 'Rollback'
    Action 'Apply' $zip; Action 'Finalize'
    Assert (-not [IO.Directory]::Exists((MP-TransactionPath $target))) 'Finalize left an owned receipt.'
    Write-Output 'PASS conservative foreign-edit rollback/finalize preflight'

    $bad=New-Package 'bad' '0.4.0' 'extra.txt'
    [IO.File]::AppendAllText((Join-Path $bad 'SHA256SUMS.txt'),"`n"+('a'*64)+'  ../outside.txt')
    Reject { $null=MP-OpenDirectorySource $bad } 'Traversal inventory was accepted.'
    Refresh-Inventory $bad
    [IO.File]::AppendAllText((Join-Path $bad 'SHA256SUMS.txt'),"`n"+('a'*64)+'  MASKPILOT.EXE')
    Reject { $null=MP-OpenDirectorySource $bad } 'Case-fold inventory collision was accepted.'
    $evil=Join-Path $fixture 'evil.zip'
    $archive=[IO.Compression.ZipFile]::Open($evil,[IO.Compression.ZipArchiveMode]::Create)
    try { $entry=$archive.CreateEntry('pkg/../escaped.txt'); $writer=[IO.StreamWriter]::new($entry.Open()); try { $writer.Write('malicious path') } finally { $writer.Dispose() } } finally { $archive.Dispose() }
    Reject { $source=MP-OpenZipSource $evil (MP-Hash $evil); MP-CloseSource $source } 'Traversal ZIP was accepted.'
    Assert (-not [IO.File]::Exists((Join-Path $fixture 'escaped.txt'))) 'ZIP escaped its stage.'
    Write-Output 'PASS strict inventory/ZIP path and case-fold validation'

    $junctionTarget=Join-Path $fixture 'junction-target'
    $junction=Join-Path $fixture 'dangling-junction'
    [IO.Directory]::CreateDirectory($junctionTarget) | Out-Null
    New-Item -ItemType Junction -Path $junction -Target $junctionTarget | Out-Null
    try {
        [IO.Directory]::Delete($junctionTarget)
        Reject { MP-AssertNoReparse $junction } 'A dangling junction was accepted.'
        Reject { MP-AssertNoReparse (Join-Path $junction 'absent-child') } 'A dangling junction ancestor was accepted.'
    } finally { [IO.Directory]::Delete($junction) }
    Write-Output 'PASS actual dangling-junction and ancestor rejection'

    # A copied Windows ping binary is a real owned executable, unlike the package placeholders.
    $helper=Join-Path $target 'owned-liveness.exe'
    [IO.File]::Copy((Join-Path $env:SystemRoot 'System32/ping.exe'),$helper)
    $process=Start-Process -FilePath $helper -ArgumentList @('-t','127.0.0.1') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $fixture 'ping.out') -RedirectStandardError (Join-Path $fixture 'ping.err')
    $children+=$process
    try { Start-Sleep -Milliseconds 150; Assert (-not $process.HasExited) 'Owned liveness fixture did not stay running.'; Reject { MP-AssertStopped $target } 'An installed helper process was not detected.' }
    finally { if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }; $process.Dispose(); $children=@() }
    [IO.File]::Delete($helper)
    Write-Output 'PASS actual installed executable-path liveness guard'

    # Terminate only this fixture-owned child after a durable first file mutation.
    $worker=Join-Path $fixture 'interrupt.ps1'; $ready=Join-Path $fixture 'interrupt.ready'
    $workerText=@'
param($Common,$Target,$Zip,$Digest,$Ready,$StopAt='Applied')
$ErrorActionPreference='Stop'
. $Common
$mutex=MP-EnterTargetLock $Target
try { $null=MP-InvokeUpdate 'Apply' $Target $Zip $Digest { param($phase,$index) if (($StopAt -eq 'Applied' -and $phase -eq 'Applied' -and $index -eq 0) -or ($StopAt -eq 'CopyChunk' -and $phase -eq 'CopyChunk' -and $index -eq 65536)) { [IO.File]::WriteAllText($Ready,'owned ready'); Start-Sleep -Seconds 60 } } }
finally { MP-ExitTargetLock $mutex }
'@
    [IO.File]::WriteAllText($worker,$workerText,[Text.UTF8Encoding]::new($false))
    # A same-version reinstall still changes its marker and has a prior payload to recover.
    $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$worker+'"'),('"'+(Join-Path $SourceRoot 'packaging/windows/package-common.ps1')+'"'),('"'+$target+'"'),('"'+$zip+'"'),(MP-Hash $zip),('"'+$ready+'"'))
    $process=Start-Process -FilePath "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $fixture 'child.out') -RedirectStandardError (Join-Path $fixture 'child.err')
    $children+=$process
    try {
        $deadline=[DateTime]::UtcNow.AddSeconds(20)
        while (-not [IO.File]::Exists($ready) -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
        Assert ([IO.File]::Exists($ready)) 'Owned interruption child did not reach durable mutation.'
        $process.Kill(); $process.WaitForExit(5000) | Out-Null
    } finally { if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }; $process.Dispose(); $children=@() }
    Action 'Recover'
    Assert (-not (MP-ReadTransaction $target)) 'Interrupted child recovery left a receipt.'
    Assert ([IO.File]::ReadAllText((Join-Path $target 'downloads/keep.bin')) -eq 'unknown downloaded content') 'Interrupted recovery changed user bytes.'
    Write-Output 'PASS real child-termination recovery and preserved user data'

    [IO.File]::Delete($ready)
    $beforePreparingKill=MP-Hash (Join-Path $target 'maskpilot.exe')
    $process=Start-Process -FilePath "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -ArgumentList ($arguments+@('CopyChunk')) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $fixture 'partial-child.out') -RedirectStandardError (Join-Path $fixture 'partial-child.err')
    $children+=$process
    try {
        $deadline=[DateTime]::UtcNow.AddSeconds(20)
        while (-not [IO.File]::Exists($ready) -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
        Assert ([IO.File]::Exists($ready)) 'Preparing child did not reach an actual partial stream.'
        $process.Kill(); $process.WaitForExit(5000) | Out-Null
    } finally { if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }; $process.Dispose(); $children=@() }
    $preparing=MP-ReadTransaction $target
    $partial=Join-Path $preparing.root 'stage/000-large.bin'
    Assert ($preparing.journal.phase -eq 'Preparing' -and (Get-Item -LiteralPath $partial).Length -eq 65536) 'Interrupted staging was not a genuine partial write.'
    Reject { Action 'Recover' } 'Killed unverified partial staging was silently deleted.'
    Assert ([IO.File]::Exists($partial) -and (MP-Hash (Join-Path $target 'maskpilot.exe')) -eq $beforePreparingKill) 'Preparing interruption changed old payload or erased evidence.'
    [IO.File]::Delete($partial); Action 'Recover'
    Write-Output 'PASS real process-kill during staging fails closed; explicit fixture-owned manual repair preserves payload'
    Write-Output 'PASS offline updater synthetic filesystem qualification (no native application compatibility claim)'
} finally {
    foreach ($child in $children) { try { if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit(5000) | Out-Null } } finally { $child.Dispose() } }
    $resolved=[IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith([IO.Path]::GetFullPath($base)+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe owned fixture cleanup path.' }
    MP-AssertNoReparse $resolved
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
