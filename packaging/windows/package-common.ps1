# Shared, offline installation ownership and recovery. No networking or process termination.
$script:MPDynamic = @('native-bridge.json','com.maskpilot.downloads.json','com.maskpilot.downloads.firefox.json','browser-registration.json')
$script:MPRequired = @('maskpilot.exe','maskpilot_cli.exe','maskpilot_native_host.exe','media_guard/ffmpeg.exe','media_guard/ffprobe.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','Qt6Svg.dll','libcurl.dll','libz.dll','nghttp2.dll','sqlite3.dll','torrent-rasterbar.dll','libcrypto-3-x64.dll','libssl-3-x64.dll','libxml2.dll','qt.conf','plugins/platforms/qwindows.dll','plugins/platforms/qoffscreen.dll','scripts/install.ps1','scripts/uninstall.ps1','scripts/update.ps1','scripts/package-common.ps1','scripts/register-browser.ps1','release.json')

function MP-AssertNoReparse([string]$Path) {
    $walk = [IO.Path]::GetFullPath($Path)
    while ($walk) {
        try {
            # GetAttributes observes a dangling reparse entry; Exists follows it and can return false.
            $attributes=[IO.File]::GetAttributes($walk)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Junctions and symbolic links are not installation paths.' }
        } catch {
            $cause=$_.Exception
            while ($cause.InnerException) { $cause=$cause.InnerException }
            if ($cause -isnot [IO.FileNotFoundException] -and $cause -isnot [IO.DirectoryNotFoundException]) { throw }
        }
        $parent = [IO.Path]::GetDirectoryName($walk)
        if ($parent -eq $walk) { break }; $walk = $parent
    }
}
function MP-GetTarget([string]$Path) {
    if (-not $Path -or $Path -notmatch '^[a-zA-Z]:[\\/]' -or $Path.StartsWith('\\')) { throw 'Choose an absolute local application directory.' }
    $result = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($result)).DriveType -eq [IO.DriveType]::Network) { throw 'Mapped network drives are not local installation targets.' }
    foreach ($forbidden in @([IO.Path]::GetPathRoot($result).TrimEnd('\'),$env:USERPROFILE,$env:LOCALAPPDATA)) {
        if ($forbidden -and $result -ieq $forbidden.TrimEnd('\')) { throw 'Choose a dedicated application directory.' }
    }
    MP-AssertNoReparse $result
    return $result
}
function MP-Relative([string]$Path) {
    if (-not $Path -or $Path.Length -gt 240 -or [IO.Path]::IsPathRooted($Path) -or $Path -match '[:\x00-\x1f\x7f]') { throw 'Unsafe package path.' }
    $normalized = $Path.Replace('\','/')
    foreach ($part in $normalized.Split('/')) {
        if (-not $part -or $part -eq '.' -or $part -eq '..' -or $part -match '[<>"|?*]' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(\.|$)') { throw 'Unsafe Windows package component.' }
    }
    return $normalized
}
function MP-Join([string]$Root,[string]$Relative) {
    $result = [IO.Path]::GetFullPath((Join-Path $Root (MP-Relative $Relative)))
    if (-not $result.StartsWith($Root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Owned path escaped its directory.' }
    MP-AssertNoReparse $result
    $parent=[IO.Path]::GetDirectoryName($result)
    while ($parent.Length -gt $Root.Length) { if ([IO.File]::Exists($parent)) { throw 'A parent file conflicts with a package directory.' }; $parent=[IO.Path]::GetDirectoryName($parent) }
    return $result
}
function MP-Hash([string]$Path) { MP-AssertNoReparse $Path; return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function MP-BytesHash([byte[]]$Bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() } finally { $hash.Dispose() }
}
function MP-StateHash([string]$Path) {
    MP-AssertNoReparse $Path
    if ([IO.Directory]::Exists($Path)) { throw 'A directory conflicts with an owned file.' }
    if ([IO.File]::Exists($Path)) { return MP-Hash $Path }; return $null
}
function MP-EnterTargetLock([string]$Target) {
    $digest = MP-BytesHash ([Text.Encoding]::UTF8.GetBytes($Target.ToUpperInvariant()))
    $mutex = [Threading.Mutex]::new($false,('Local\MaskPilot.Install.'+$digest))
    try { try { $held = $mutex.WaitOne(5000) } catch [Threading.AbandonedMutexException] { $held = $true }
        if (-not $held) { throw 'Another installation operation is active.' }; return $mutex
    } catch { $mutex.Dispose(); throw }
}
function MP-ExitTargetLock($Mutex) { if ($Mutex) { $Mutex.ReleaseMutex(); $Mutex.Dispose() } }
function MP-AssertStopped([string]$Target) {
    foreach ($process in Get-Process) {
        try { $executable = $process.Path } catch { continue }
        if ($executable -and $executable.StartsWith($Target+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Close every application and helper running from the installation before continuing.' }
    }
}
function MP-WriteBytes([string]$Path,[byte[]]$Bytes,[switch]$Replace,$WriteFault=$null) {
    MP-AssertNoReparse $Path
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    $temporary = $Path+'.new'
    MP-AssertNoReparse $temporary
    $created=$false
    try {
        $stream = [IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $created=$true
        try {
            for ($offset=0; $offset -lt $Bytes.Length; $offset+=$count) {
                $count=[Math]::Min(65536,$Bytes.Length-$offset)
                $stream.Write($Bytes,$offset,$count)
                if ($WriteFault) { & $WriteFault 'MetadataChunk' ($offset+$count) }
            }
            $stream.Flush($true)
        } finally { $stream.Dispose() }
    } catch {
        # Only this invocation's successful exclusive creation authorizes partial-write cleanup.
        if ($created) { MP-AssertNoReparse $temporary; [IO.File]::Delete($temporary) }
        throw
    }
    if ([IO.File]::Exists($Path)) {
        if (-not $Replace) { throw 'An owned metadata file already exists.' }
        [IO.File]::Replace($temporary,$Path,[NullString]::Value)
    } else { [IO.File]::Move($temporary,$Path) }
}
function MP-JsonBytes($Value) { return ,([Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 12 -Compress))) }
function MP-ParseObject([string]$Text) {
    $textValue=$Text.TrimStart([char]0xfeff).TrimStart()
    if (-not $textValue.StartsWith('{')) { throw 'Metadata must be a JSON object.' }
    return ($textValue | ConvertFrom-Json)
}
function MP-ReadJson([string]$Path) {
    MP-AssertNoReparse $Path
    if ((Get-Item -LiteralPath $Path).Length -gt 16777216) { throw 'Owned metadata exceeds its bound.' }
    return MP-ParseObject ([IO.File]::ReadAllText($Path))
}
function MP-CheckRows($Rows,[switch]$Package) {
    if ($Rows -isnot [array] -or $Rows.Count -eq 0) { throw 'Inventory must be a nonempty array.' }
    $map = @{}
    foreach ($row in @($Rows)) {
        if ($row -is [array] -or $row.path -isnot [string] -or $row.sha256 -isnot [string]) { throw 'Inventory paths and hashes must be strings.' }
        if ($map.Count -ge 20000) { throw 'Too many inventory entries.' }
        $relative = MP-Relative ([string]$row.path)
        if ($row.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or $map.ContainsKey($relative)) { throw 'Invalid or duplicate inventory entry.' }
        if ($relative -ieq 'maskpilot-install.json' -or $relative -like '*.maskpilot-update/*') { throw 'Reserved installation path.' }
        if ($Package -and ($relative -ieq 'SHA256SUMS.txt' -or $script:MPDynamic -icontains $relative)) { throw 'Reserved package metadata path.' }
        $map[$relative] = @{path=$relative;sha256=([string]$row.sha256).ToLowerInvariant()}
    }
    foreach ($relative in @($map.Keys)) {
        $parent = $relative
        while ($parent.Contains('/')) { $parent=$parent.Substring(0,$parent.LastIndexOf('/')); if ($map.ContainsKey($parent)) { throw 'File/directory inventory collision.' } }
    }
    return $map
}
function MP-InventoryText([string]$Text) {
    if ($Text.Length -gt 8388608) { throw 'Inventory is too large.' }
    $Text=$Text.TrimStart([char]0xfeff)
    $rows = @()
    foreach ($line in ($Text -split '\r?\n')) {
        if (-not $line) { continue }
        if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw 'Invalid SHA256 inventory.' }
        $rows += @{path=$Matches[2];sha256=$Matches[1]}
    }
    return MP-CheckRows $rows -Package
}
function MP-ValidateRelease($Release) {
    if ($Release -is [array] -or ($Release.schema -isnot [int] -and $Release.schema -isnot [long]) -or $Release.schema -ne 1 -or $Release.product -isnot [string] -or $Release.product -cne 'MaskPilot' -or $Release.architecture -isnot [string] -or $Release.architecture -cne 'x64' -or $Release.version -isnot [string] -or $Release.version.Length -gt 32 -or $Release.version -notmatch '^\d+\.\d+\.\d+$') { throw 'Unsupported release metadata.' }
    $allowed=@('schema','product','version','architecture','sourceCommit','sourceDirty','qtVersion','compilerVersion','qualificationManifestSha256','qualifiedSourceCommit')
    foreach ($property in $Release.PSObject.Properties) {
        if ($allowed -cnotcontains $property.Name) { throw 'Unsupported public release metadata.' }
        switch ($property.Name) {
            { $_ -in @('sourceCommit','qualifiedSourceCommit') } { if ($property.Value -isnot [string] -or $property.Value -notmatch '^[a-fA-F0-9]{40}([a-fA-F0-9]{24})?$') { throw 'Invalid public commit identity.' } }
            'sourceDirty' { if ($property.Value -isnot [bool]) { throw 'Invalid public source state.' } }
            { $_ -in @('qtVersion','compilerVersion') } { if ($property.Value -isnot [string] -or $property.Value -notmatch '^\d+(\.\d+){1,3}$') { throw 'Invalid public tool version.' } }
            'qualificationManifestSha256' { if ($property.Value -isnot [string] -or $property.Value -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid qualification digest.' } }
        }
    }
}
function MP-ReadInstalled([string]$Target) {
    $markerPath = Join-Path $Target 'maskpilot-install.json'
    if (-not [IO.File]::Exists($markerPath)) {
        if ([IO.Directory]::Exists($Target) -and @(Get-ChildItem -LiteralPath $Target -Force).Count) { throw 'A nonempty target has no installation marker.' }
        return $null
    }
    $marker = MP-ReadJson $markerPath
    if (($marker.schema -isnot [int] -and $marker.schema -isnot [long]) -or $marker.product -isnot [string] -or $marker.product -cne 'MaskPilot' -or $marker.schema -ne 2 -or $marker.directory -isnot [string] -or $marker.directory -ine $Target) { throw 'Invalid installation ownership marker.' }
    if ($marker.PSObject.Properties['version'] -and ($marker.version -isnot [string] -or $marker.version -notmatch '^\d+\.\d+\.\d+$')) { throw 'Invalid installed version.' }
    $rows = MP-CheckRows $marker.files
    if (-not $rows.Count) { throw 'Empty installation inventory.' }
    return @{value=$marker;rows=$rows;bytes=[IO.File]::ReadAllBytes($markerPath)}
}
function MP-ReadSourceBytes($Source,[string]$Relative,[int64]$Limit=2147483648) {
    if ($Source.zip) { $input = $Source.entries[$Relative].Open() } else { $input = [IO.File]::OpenRead((MP-Join $Source.root $Relative)) }
    try {
        $memory = [IO.MemoryStream]::new()
        try {
            $buffer = New-Object byte[] 65536; [int64]$count=0
            while (($read=$input.Read($buffer,0,$buffer.Length)) -gt 0) { $count+=$read; if ($count -gt $Limit) { throw 'Package metadata exceeds its bound.' }; $memory.Write($buffer,0,$read) }
            return ,$memory.ToArray()
        } finally { $memory.Dispose() }
    } finally { $input.Dispose() }
}
function MP-ValidateSource($Source) {
    $inventoryBytes = MP-ReadSourceBytes $Source 'SHA256SUMS.txt' 8388608
    $Source.rows = MP-InventoryText ([Text.Encoding]::UTF8.GetString($inventoryBytes))
    foreach ($required in $script:MPRequired) { if (-not $Source.rows.ContainsKey($required)) { throw "Required package file is missing: $required" } }
    $Source.rows['SHA256SUMS.txt']=@{path='SHA256SUMS.txt';sha256=(MP-BytesHash $inventoryBytes)}
    if ($Source.entries.Count -ne $Source.rows.Count) { throw 'Package files do not match the complete inventory.' }
    foreach ($relative in $Source.entries.Keys) { if (-not $Source.rows.ContainsKey($relative)) { throw 'Uninventoried package file.' } }
    $releaseBytes=MP-ReadSourceBytes $Source 'release.json' 65536
    if ((MP-BytesHash $releaseBytes) -ne $Source.rows['release.json'].sha256) { throw 'Release metadata hash mismatch.' }
    $Source.release=MP-ParseObject ([Text.Encoding]::UTF8.GetString($releaseBytes))
    MP-ValidateRelease $Source.release
    # Reject corrupt input before establishing recovery state, not after partial staging.
    [int64]$expanded=0
    foreach ($row in $Source.rows.Values) {
        if ($Source.zip) { $input=$Source.entries[$row.path].Open() } else { $input=[IO.File]::OpenRead((MP-Join $Source.root $row.path)) }
        $sha=[Security.Cryptography.SHA256]::Create()
        try {
            $buffer=New-Object byte[] 65536; [int64]$length=0
            while (($read=$input.Read($buffer,0,$buffer.Length)) -gt 0) {
                $length+=$read; $expanded+=$read
                if ($length -gt 2147483648 -or $expanded -gt 8589934592) { throw 'Actual package expansion exceeds its bound.' }
                $null=$sha.TransformBlock($buffer,0,$read,$buffer,0)
            }
            $null=$sha.TransformFinalBlock((New-Object byte[] 0),0,0)
            $actual=([BitConverter]::ToString($sha.Hash)).Replace('-','').ToLowerInvariant()
            if ($actual -ne $row.sha256) { throw 'Package bytes do not match the inventory.' }
        } finally { $sha.Dispose(); $input.Dispose() }
    }
    return $Source
}
function MP-OpenDirectorySource([string]$Directory) {
    $root=[IO.Path]::GetFullPath($Directory).TrimEnd('\'); MP-AssertNoReparse $root
    $entries=@{}; [int64]$total=0
    $pending=[Collections.Generic.Stack[string]]::new(); $pending.Push($root)
    while ($pending.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            MP-AssertNoReparse $entry.FullName
            if ($entry.PSIsContainer) { $pending.Push($entry.FullName); continue }
            $relative=MP-Relative ($entry.FullName.Substring($root.Length+1))
            if ($entries.ContainsKey($relative) -or $entries.Count -ge 20000 -or $entry.Length -gt 2147483648) { throw 'Package file bounds exceeded.' }
            $total+=$entry.Length; if ($total -gt 8589934592) { throw 'Package expansion bound exceeded.' }; $entries[$relative]=$entry
        }
    }
    return MP-ValidateSource @{root=$root;zip=$null;handle=$null;entries=$entries;rows=$null;release=$null}
}
function MP-OpenZipSource([string]$Package,[string]$ExpectedHash) {
    if ($ExpectedHash -notmatch '^[a-fA-F0-9]{64}$') { throw 'Supply an independently trusted SHA256 digest.' }
    $path=[IO.Path]::GetFullPath($Package); MP-AssertNoReparse $path
    $handle=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $zip=$null
    try {
        if ($handle.Length -gt 2147483648) { throw 'Archive is too large.' }
        $sha=[Security.Cryptography.SHA256]::Create()
        try { $actual=([BitConverter]::ToString($sha.ComputeHash($handle))).Replace('-','') } finally { $sha.Dispose() }
        if ($actual -ine $ExpectedHash) { throw 'Archive SHA256 does not match the independently trusted digest.' }
        $handle.Position=0
        Add-Type -AssemblyName System.IO.Compression
        $zip=[IO.Compression.ZipArchive]::new($handle,[IO.Compression.ZipArchiveMode]::Read,$true)
        $entries=@{}; $all=@{}; [int64]$total=0; $prefix=$null
        if ($zip.Entries.Count -gt 20000) { throw 'Too many archive entries.' }
        # A release ZIP has exactly one outer package directory, as produced by package.ps1.
        foreach ($entry in $zip.Entries) {
            $name=$entry.FullName.Replace('\','/'); $directory=$name.EndsWith('/'); $name=$name.TrimEnd('/')
            $name=MP-Relative $name
            $bits=[int64]$entry.ExternalAttributes -band 4294967295L
            $unixType=($bits -shr 16) -band 61440
            if (($bits -band 1024) -or $unixType -notin @(0,16384,32768) -or ($unixType -eq 16384 -and -not $directory) -or ($unixType -eq 32768 -and $directory) -or (($bits -band 16) -and -not $directory)) { throw 'Archive links, special files and inconsistent entry types are forbidden.' }
            if ($all.ContainsKey($name)) { throw 'Duplicate archive path.' }; $all[$name]=$directory
            $separator=$name.IndexOf('/')
            if ($separator -lt 0) { if (-not $directory) { throw 'Archive requires one package directory.' }; $outer=$name } else { $outer=$name.Substring(0,$separator) }
            if ($null -eq $prefix) { $prefix=$outer } elseif ($prefix -ine $outer) { throw 'Archive has multiple roots.' }
            if ($directory) { continue }
            $relative=MP-Relative $name.Substring($separator+1)
            if ($entry.Length -gt 2147483648) { throw 'Archive member is too large.' }
            $total+=$entry.Length; if ($total -gt 8589934592) { throw 'Archive expansion bound exceeded.' }
            $entries[$relative]=$entry
        }
        foreach ($name in $all.Keys) { $parent=$name; while ($parent.Contains('/')) { $parent=$parent.Substring(0,$parent.LastIndexOf('/')); if ($all.ContainsKey($parent) -and -not $all[$parent]) { throw 'Archive file/directory collision.' } } }
        return MP-ValidateSource @{root=$null;zip=$zip;handle=$handle;entries=$entries;rows=$null;release=$null}
    } catch { if ($zip) { $zip.Dispose() }; $handle.Dispose(); throw }
}
function MP-CloseSource($Source) { if ($Source.zip) { $Source.zip.Dispose() }; if ($Source.handle) { $Source.handle.Dispose() } }
function MP-CopyVerified($Source,[string]$Relative,[string]$Destination,[string]$Hash,$WriteFault=$null) {
    MP-AssertNoReparse $Destination
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Destination)) | Out-Null
    if ($Source -is [string]) { $input=[IO.File]::OpenRead((MP-Join $Source $Relative)) } elseif ($Source.zip) { $input=$Source.entries[$Relative].Open() } else { $input=[IO.File]::OpenRead((MP-Join $Source.root $Relative)) }
    $created=$false
    try {
        try {
            $output=[IO.FileStream]::new($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            $created=$true
            try {
                $buffer=New-Object byte[] 65536; [int64]$size=0
                while (($read=$input.Read($buffer,0,$buffer.Length)) -gt 0) {
                    $size+=$read; if ($size -gt 2147483648) { throw 'File exceeds package bound.' }
                    $output.Write($buffer,0,$read)
                    if ($WriteFault) { & $WriteFault 'CopyChunk' $size }
                }
                $output.Flush($true)
            } finally { $output.Dispose() }
        } catch {
            if ($created) { MP-AssertNoReparse $Destination; [IO.File]::Delete($Destination) }
            throw
        }
    } finally { $input.Dispose() }
    if ((MP-Hash $Destination) -ne $Hash) { throw 'Staged file hash mismatch; recovery state was preserved.' }
}
function MP-TransactionPath([string]$Target) { return $Target+'.maskpilot-update' }
function MP-ValidateJournal($Journal,[string]$Target) {
    if ($Journal -is [array] -or ($Journal.schema -isnot [int] -and $Journal.schema -isnot [long]) -or $Journal.schema -ne 1 -or $Journal.product -isnot [string] -or $Journal.product -cne 'MaskPilot' -or $Journal.target -isnot [string] -or $Journal.target -ine $Target -or $Journal.nonce -isnot [string] -or $Journal.nonce -notmatch '^[a-f0-9]{32}$' -or $Journal.phase -isnot [string] -or $Journal.phase -notin @('Preparing','Applying','Committed','RollingBack','RolledBack','Finalizing')) { throw 'Invalid update ownership journal.' }
    if ($Journal.planHash -isnot [string] -or $Journal.planHash -notmatch '^[a-f0-9]{64}$' -or $Journal.planBase64 -isnot [string] -or $Journal.planBase64.Length -gt 22369624) { throw 'Invalid update plan envelope.' }
}
function MP-SaveJournal([string]$Root,$Journal) {
    $pending=Join-Path $Root 'journal.json.new'
    if ([IO.File]::Exists($pending)) {
        $next=MP-ReadJson $pending
        MP-ValidateJournal $next $Journal.target
        if ($next.nonce -cne $Journal.nonce -or $next.planHash -cne $Journal.planHash -or $next.planBase64 -cne $Journal.planBase64) { throw 'Unrecognized pending metadata was preserved.' }
        [IO.File]::Delete($pending)
    }
    MP-WriteBytes (Join-Path $Root 'journal.json') (MP-JsonBytes $Journal) -Replace
}
function MP-Fault($Callback,[string]$Phase,[int]$Index=-1) { if ($Callback) { & $Callback $Phase $Index } }
function MP-ReadTransaction([string]$Target) {
    $root=MP-TransactionPath $Target; MP-AssertNoReparse $root
    if (-not [IO.Directory]::Exists($root)) { return $null }
    if (@(Get-ChildItem -LiteralPath $root -Force).Count -eq 0) { [IO.Directory]::Delete($root); return $null }
    $journalPath=Join-Path $root 'journal.json'
    $promote=$false
    if (-not [IO.File]::Exists($journalPath) -and [IO.File]::Exists($journalPath+'.new')) { $journalPath+='.new'; $promote=$true }
    $journal=MP-ReadJson $journalPath
    MP-ValidateJournal $journal $Target
    $planBytes=[Convert]::FromBase64String($journal.planBase64)
    if ((MP-BytesHash $planBytes) -ne $journal.planHash) { throw 'Update plan hash mismatch.' }
    $plan=MP-ParseObject ([Text.Encoding]::UTF8.GetString($planBytes))
    if (($plan.schema -isnot [int] -and $plan.schema -isnot [long]) -or $plan.schema -ne 1 -or $plan.target -isnot [string] -or $plan.target -ine $Target -or $plan.nonce -isnot [string] -or $plan.nonce -cne $journal.nonce -or $plan.newVersion -isnot [string] -or $plan.newVersion -notmatch '^\d+\.\d+\.\d+$' -or $plan.rows -isnot [array] -or $plan.rows.Count -eq 0) { throw 'Update plan ownership mismatch.' }
    $seen=@{}
    foreach ($row in @($plan.rows)) {
        if ($row -is [array] -or $row.path -isnot [string] -or ($null -ne $row.oldHash -and $row.oldHash -isnot [string]) -or ($null -ne $row.newHash -and $row.newHash -isnot [string])) { throw 'Transaction paths and nullable hashes must be scalar strings.' }
        $relative=MP-Relative $row.path
        if ($seen.ContainsKey($relative) -or $seen.Count -ge 20001 -or ($null -eq $row.oldHash -and $null -eq $row.newHash) -or ($null -ne $row.oldHash -and $row.oldHash -notmatch '^[a-f0-9]{64}$') -or ($null -ne $row.newHash -and $row.newHash -notmatch '^[a-f0-9]{64}$') -or $script:MPDynamic -icontains $relative) { throw 'Invalid transaction row.' }
        $seen[$relative]=$true
    }
    if (-not $seen.ContainsKey('maskpilot-install.json') -or @($plan.rows)[-1].path -cne 'maskpilot-install.json') { throw 'The marker must be the last transaction row.' }
    foreach ($relative in $seen.Keys) { $parent=$relative; while ($parent.Contains('/')) { $parent=$parent.Substring(0,$parent.LastIndexOf('/')); if ($seen.ContainsKey($parent)) { throw 'Transaction path collision.' } } }
    if ($promote) { [IO.File]::Move($journalPath,(Join-Path $root 'journal.json')) }
    return @{root=$root;journal=$journal;plan=$plan}
}
function MP-AssertTargetStates($Transaction,[switch]$NewOnly) {
    foreach ($row in $Transaction.plan.rows) {
        $actual=MP-StateHash (MP-Join $Transaction.plan.target $row.path)
        if ($NewOnly) { if ($actual -ne $row.newHash) { throw 'Installed payload changed; transaction retained.' } }
        elseif ($actual -ne $row.oldHash -and $actual -ne $row.newHash) { throw 'An external file change conflicts with recovery; nothing was removed.' }
    }
}
function MP-ArtifactMap($Transaction) {
    $map=@{}
    foreach ($row in $Transaction.plan.rows) {
        if ($null -ne $row.newHash) { $map['stage/'+$row.path]=$row.newHash; $map['swap/'+$row.path]=$row.newHash }
        if ($null -ne $row.oldHash) { $map['backup/'+$row.path]=$row.oldHash; $map['restore/'+$row.path]=$row.oldHash }
    }
    $map['journal.json']=MP-Hash (Join-Path $Transaction.root 'journal.json')
    $nextPath=Join-Path $Transaction.root 'journal.json.new'
    if ([IO.File]::Exists($nextPath)) {
        $next=MP-ReadJson $nextPath
        MP-ValidateJournal $next $Transaction.plan.target
        if ($next.nonce -cne $Transaction.journal.nonce -or $next.planBase64 -cne $Transaction.journal.planBase64 -or $next.planHash -cne $Transaction.journal.planHash) { throw 'Unrecognized pending metadata was preserved.' }
        $map['journal.json.new']=MP-Hash $nextPath
    }
    return $map
}
function MP-AssertArtifacts($Transaction,[switch]$RequireBackups) {
    $map=MP-ArtifactMap $Transaction
    $pending=[Collections.Generic.Stack[string]]::new(); $pending.Push($Transaction.root)
    while ($pending.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            MP-AssertNoReparse $entry.FullName
            $relative=$entry.FullName.Substring($Transaction.root.Length+1).Replace('\','/')
            if ($entry.PSIsContainer) {
                if (-not @($map.Keys | Where-Object { $_.StartsWith($relative+'/',[StringComparison]::OrdinalIgnoreCase) }).Count) { throw 'An unknown transaction directory was preserved.' }
                $pending.Push($entry.FullName)
            } elseif (-not $map.ContainsKey($relative) -or (MP-Hash $entry.FullName) -ne $map[$relative]) { throw 'A modified or unknown transaction file was preserved.' }
        }
    }
    if ($RequireBackups) { foreach ($row in $Transaction.plan.rows) { if ($null -ne $row.oldHash -and (MP-StateHash (MP-Join (Join-Path $Transaction.root 'backup') $row.path)) -ne $row.oldHash) { throw 'A required rollback backup is unavailable.' } } }
    return $map
}
function MP-ClearTransaction($Transaction) {
    $map=MP-AssertArtifacts $Transaction
    # Persist cleanup-only intent before backup deletion; interrupted cleanup cannot roll back.
    $nextPath=Join-Path $Transaction.root 'journal.json.new'
    if ([IO.File]::Exists($nextPath)) { [IO.File]::Delete($nextPath) }
    $Transaction.journal.phase='Finalizing'; MP-SaveJournal $Transaction.root $Transaction.journal
    $map['journal.json']=MP-Hash (Join-Path $Transaction.root 'journal.json')
    # Remove the sole ownership envelope last; an empty leftover directory is harmless.
    foreach ($relative in @($map.Keys | Where-Object { $_ -ne 'journal.json' -and $_ -ne 'journal.json.new' })) {
        $path=MP-Join $Transaction.root $relative
        if ([IO.File]::Exists($path)) { if ((MP-Hash $path) -ne $map[$relative]) { throw 'Transaction changed during cleanup.' }; [IO.File]::Delete($path) }
    }
    $directories=@(); $pending=[Collections.Generic.Stack[string]]::new(); $pending.Push($Transaction.root)
    while ($pending.Count) { foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Directory -Force) { MP-AssertNoReparse $entry.FullName; $directories+=$entry; $pending.Push($entry.FullName) } }
    $directories=@($directories | Sort-Object { $_.FullName.Length } -Descending)
    foreach ($directory in $directories) { MP-AssertNoReparse $directory.FullName; if (@(Get-ChildItem -LiteralPath $directory.FullName -Force).Count) { throw 'Nonempty transaction directory was preserved.' }; [IO.Directory]::Delete($directory.FullName) }
    [IO.File]::Delete((Join-Path $Transaction.root 'journal.json'))
    [IO.Directory]::Delete($Transaction.root)
}
function MP-StartInstall($Source,[string]$Target,$FaultCallback=$null) {
    if (MP-ReadTransaction $Target) { throw 'Finalize or recover the previous update before another installation.' }
    if ($Source.root -and ($Target -ieq $Source.root -or $Target.StartsWith($Source.root+'\',[StringComparison]::OrdinalIgnoreCase) -or $Source.root.StartsWith($Target+'\',[StringComparison]::OrdinalIgnoreCase))) { throw 'Package source and installation must be separate.' }
    MP-AssertStopped $Target
    $old=MP-ReadInstalled $Target; $rows=@(); $newRows=@($Source.rows.Values | Sort-Object path)
    $oldRows=@{}; if ($old) { $oldRows=$old.rows }
    [int64]$backupBytes=0
    $all=@{}; foreach ($key in $oldRows.Keys) { $all[$key]=$true }; foreach ($key in $Source.rows.Keys) { $all[$key]=$true }
    foreach ($relative in @($all.Keys | Sort-Object)) {
        if ($script:MPDynamic -icontains $relative) { if ($oldRows.ContainsKey($relative)) { $newRows+=$oldRows[$relative] }; continue }
        $oldHash=$null; $newHash=$null
        if ($oldRows.ContainsKey($relative)) { $oldHash=$oldRows[$relative].sha256 }
        if ($Source.rows.ContainsKey($relative)) { $newHash=$Source.rows[$relative].sha256 }
        if ($null -ne $oldHash) {
            $oldPath=MP-Join $Target $relative
            if ([IO.File]::Exists($oldPath)) {
                $length=([IO.FileInfo]::new($oldPath)).Length; $backupBytes+=$length
                if ($length -gt 2147483648 -or $backupBytes -gt 8589934592) { throw 'Existing payload exceeds backup bounds.' }
            }
        }
        if ((MP-StateHash (MP-Join $Target $relative)) -ne $oldHash) { throw 'An unknown, missing or modified installed file conflicts with the package.' }
        $rows+=@{path=$relative;oldHash=$oldHash;newHash=$newHash}
    }
    $marker=@{product='MaskPilot';schema=2;version=$Source.release.version;installedAtUtc=[DateTime]::UtcNow.ToString('o');directory=$Target;files=$newRows}
    $newMarker=MP-JsonBytes $marker; $oldMarkerHash=$null
    if ($old) { $oldMarkerHash=MP-BytesHash $old.bytes }
    $rows+=@{path='maskpilot-install.json';oldHash=$oldMarkerHash;newHash=(MP-BytesHash $newMarker)}
    $root=MP-TransactionPath $Target; [IO.Directory]::CreateDirectory($root) | Out-Null
    $nonce=[Guid]::NewGuid().ToString('N')
    $plan=@{schema=1;target=$Target;nonce=$nonce;newVersion=$Source.release.version;rows=$rows}
    $planBytes=MP-JsonBytes $plan
    $journal=@{schema=1;product='MaskPilot';target=$Target;nonce=$nonce;phase='Preparing';planHash=(MP-BytesHash $planBytes);planBase64=[Convert]::ToBase64String($planBytes)}
    MP-SaveJournal $root $journal
    MP-Fault $FaultCallback 'Preparing'
    foreach ($row in $rows) {
        if ($null -ne $row.newHash) {
            $stage=MP-Join (Join-Path $root 'stage') $row.path
            if ($row.path -eq 'maskpilot-install.json') { MP-WriteBytes $stage $newMarker } else { MP-CopyVerified $Source $row.path $stage $row.newHash $FaultCallback }
            MP-CopyVerified (Join-Path $root 'stage') $row.path (MP-Join (Join-Path $root 'swap') $row.path) $row.newHash $FaultCallback
        }
        if ($null -ne $row.oldHash) { MP-CopyVerified $Target $row.path (MP-Join (Join-Path $root 'backup') $row.path) $row.oldHash $FaultCallback }
    }
    $transaction=MP-ReadTransaction $Target
    $null=MP-AssertArtifacts $transaction -RequireBackups
    MP-AssertTargetStates $transaction
    MP-AssertStopped $Target
    $journal.phase='Applying'; MP-SaveJournal $root $journal; MP-Fault $FaultCallback 'Applying'
    [IO.Directory]::CreateDirectory($Target) | Out-Null
    $index=0
    foreach ($row in $rows) {
        $destination=MP-Join $Target $row.path
        if ((MP-StateHash $destination) -ne $row.oldHash) { throw 'The installation changed during update; recovery state retained.' }
        if ($null -eq $row.newHash) { [IO.File]::Delete($destination) }
        else {
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            $swap=MP-Join (Join-Path $root 'swap') $row.path
            if ($null -ne $row.oldHash) { [IO.File]::Replace($swap,$destination,[NullString]::Value) } else { [IO.File]::Move($swap,$destination) }
        }
        MP-Fault $FaultCallback 'Applied' $index; $index++
    }
    MP-AssertTargetStates $transaction -NewOnly
    $journal.phase='Committed'; MP-SaveJournal $root $journal; MP-Fault $FaultCallback 'Committed'
    if (-not $old) { MP-ClearTransaction (MP-ReadTransaction $Target) }
    return @{fresh=(-not $old);version=$Source.release.version}
}
function MP-Rollback($Transaction,$FaultCallback=$null) {
    MP-AssertStopped $Transaction.plan.target
    MP-AssertTargetStates $Transaction
    $null=MP-AssertArtifacts $Transaction -RequireBackups
    # Prepare all replacement files before changing any live byte; interrupted restores can be reused.
    foreach ($row in $Transaction.plan.rows) {
        if ($null -ne $row.oldHash) {
            $restore=MP-Join (Join-Path $Transaction.root 'restore') $row.path
            if (-not [IO.File]::Exists($restore)) { MP-CopyVerified (Join-Path $Transaction.root 'backup') $row.path $restore $row.oldHash }
        }
    }
    $Transaction.journal.phase='RollingBack'; MP-SaveJournal $Transaction.root $Transaction.journal
    $index=0
    foreach ($row in $Transaction.plan.rows) {
        $destination=MP-Join $Transaction.plan.target $row.path; $actual=MP-StateHash $destination
        if ($actual -ne $row.oldHash) {
            if ($actual -ne $row.newHash) { throw 'External change interrupted rollback.' }
            if ($null -eq $row.oldHash) { [IO.File]::Delete($destination) }
            else {
                $restore=MP-Join (Join-Path $Transaction.root 'restore') $row.path
                if ($null -eq $actual) { [IO.File]::Move($restore,$destination) } else { [IO.File]::Replace($restore,$destination,[NullString]::Value) }
            }
        }
        MP-Fault $FaultCallback 'Restored' $index; $index++
    }
    $Transaction.journal.phase='RolledBack'; MP-SaveJournal $Transaction.root $Transaction.journal
    MP-ClearTransaction (MP-ReadTransaction $Transaction.plan.target)
}
function MP-CompleteAction([string]$Target,[string]$Action,$FaultCallback=$null) {
    $transaction=MP-ReadTransaction $Target
    if (-not $transaction) { if ($Action -eq 'Finalize') { return }; throw 'No update transaction exists.' }
    MP-AssertStopped $Target
    if ($transaction.journal.phase -eq 'Finalizing') { MP-ClearTransaction $transaction; return }
    if ($Action -eq 'Finalize') {
        if ($transaction.journal.phase -ne 'Committed') { throw 'Incomplete updates must be recovered, not finalized.' }
        MP-AssertTargetStates $transaction -NewOnly; MP-ClearTransaction $transaction
    } elseif ($Action -eq 'Recover' -and $transaction.journal.phase -eq 'Preparing') {
        foreach ($row in $transaction.plan.rows) { if ((MP-StateHash (MP-Join $Target $row.path)) -ne $row.oldHash) { throw 'Preparing target changed; transaction retained.' } }
        MP-ClearTransaction $transaction
    } elseif ($Action -eq 'Recover' -and $transaction.journal.phase -eq 'Committed') { throw 'The update completed. Choose Rollback or Finalize explicitly.' }
    else { MP-Rollback $transaction $FaultCallback }
}
function MP-InstallDirectoryPackage([string]$Source,[string]$Target,$FaultCallback=$null) {
    $package=MP-OpenDirectorySource $Source
    return MP-StartInstall $package $Target $FaultCallback
}
function MP-InvokeUpdate([string]$Action,[string]$Target,[string]$Package='', [string]$PackageSha256='', $FaultCallback=$null) {
    if ($Action -eq 'Apply') {
        $archivePath=[IO.Path]::GetFullPath($Package)
        if ($archivePath.StartsWith($Target+'\',[StringComparison]::OrdinalIgnoreCase) -or $archivePath.StartsWith((MP-TransactionPath $Target)+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Keep the supplied archive outside the installation and transaction directories.' }
        $source=MP-OpenZipSource $Package $PackageSha256
        try { return MP-StartInstall $source $Target $FaultCallback } finally { MP-CloseSource $source }
    } else { MP-CompleteAction $Target $Action $FaultCallback }
}
