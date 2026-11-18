param(
    [Parameter(Mandatory=$true)][string]$InstallDirectory,
    [string]$DataDirectory = '',
    [switch]$Unregister
)
$ErrorActionPreference = 'Stop'
$installRoot = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $InstallDirectory).Path).TrimEnd('\')
$manifestPath = Join-Path $installRoot 'com.maskpilot.downloads.json'
$firefoxManifest = Join-Path $installRoot 'com.maskpilot.downloads.firefox.json'
$journalPath = Join-Path $installRoot 'browser-registration.json'
$geckoId = 'downloads@maskpilot.invalid'
$registrations = @(
    @{ key='Software\Google\Chrome\NativeMessagingHosts\com.maskpilot.downloads'; manifest=$manifestPath },
    @{ key='Software\Microsoft\Edge\NativeMessagingHosts\com.maskpilot.downloads'; manifest=$manifestPath },
    @{ key='Software\Mozilla\NativeMessagingHosts\com.maskpilot.downloads'; manifest=$firefoxManifest }
)
function Assert-NoReparse([string]$Path) {
    while ($Path) {
        if ((Test-Path -LiteralPath $Path) -and ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Browser registration paths must not contain links or junctions.' }
        $parent = Split-Path $Path -Parent
        if ($parent -eq $Path) { break }
        $Path = $parent
    }
}
foreach ($path in @($installRoot,$journalPath,$manifestPath,$firefoxManifest,(Join-Path $installRoot 'native-bridge.json'))) { Assert-NoReparse $path }
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
function Snapshot([string]$Key) {
    $handle = $registry.OpenSubKey($Key,$false)
    try {
        $record = @{ keyWasPresent=($null -ne $handle); hadDefault=$false; kind=''; value=$null }
        if ($null -ne $handle -and $handle.GetValueNames() -contains '') {
            $record.hadDefault = $true
            $kind = $handle.GetValueKind('').ToString()
            $value = $handle.GetValue('',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $record.kind = $kind
            switch ($kind) {
                { $_ -in @('String','ExpandString') } { $record.value = [string]$value }
                'DWord' { $record.value = ([int]$value).ToString([Globalization.CultureInfo]::InvariantCulture) }
                'QWord' { $record.value = ([long]$value).ToString([Globalization.CultureInfo]::InvariantCulture) }
                { $_ -in @('Binary','None') } { $record.value = [Convert]::ToBase64String([byte[]]$value) }
                'MultiString' { $record.value = @([string[]]$value) }
                default { throw 'Unsupported prior browser registry value kind.' }
            }
        }
        return $record
    } finally { if ($null -ne $handle) { $handle.Dispose() } }
}
function Is-Owned($State,[string]$Manifest) { return $State.hadDefault -and $State.kind -eq 'String' -and [string]::Equals([string]$State.value,$Manifest,[StringComparison]::OrdinalIgnoreCase) }
function Same-Default($First,$Second) {
    if ([bool]$First.hadDefault -ne [bool]$Second.hadDefault) { return $false }
    if (-not $First.hadDefault) { return $true }
    return $First.kind -eq $Second.kind -and (ConvertTo-Json -InputObject $First.value -Compress -Depth 5) -ceq (ConvertTo-Json -InputObject $Second.value -Compress -Depth 5)
}
function Prior-Value($Entry) {
    switch ($Entry.kind) {
        { $_ -in @('String','ExpandString') } { if ($Entry.value -isnot [string]) { throw 'Invalid browser registry journal value.' }; return [string]$Entry.value }
        'DWord' { return [int]::Parse([string]$Entry.value,[Globalization.CultureInfo]::InvariantCulture) }
        'QWord' { return [long]::Parse([string]$Entry.value,[Globalization.CultureInfo]::InvariantCulture) }
        { $_ -in @('Binary','None') } { return ,([Convert]::FromBase64String([string]$Entry.value)) }
        'MultiString' { foreach ($value in @($Entry.value)) { if ($value -isnot [string]) { throw 'Invalid browser registry journal array.' } }; return ,([string[]]@($Entry.value)) }
        default { throw 'Invalid browser registry journal kind.' }
    }
}
function Read-Journal {
    if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf)) { return $null }
    if ((Get-Item -LiteralPath $journalPath).Length -gt 2097152) { throw 'Browser registration journal exceeds its size limit.' }
    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
    if ($journal.product -ne 'MaskPilot' -or $journal.schema -ne 1 -or $journal.directory -cne $installRoot -or @($journal.entries).Count -ne 3) { throw 'Invalid browser registration journal.' }
    $seen = @{}
    foreach ($entry in @($journal.entries)) {
        $expected = @($registrations | Where-Object { $_.key -ceq $entry.key })
        if ($expected.Count -ne 1 -or $entry.manifest -cne $expected[0].manifest -or $seen.ContainsKey($entry.key) -or $entry.hadDefault -isnot [bool] -or $entry.keyWasPresent -isnot [bool]) { throw 'Invalid browser registration journal entry.' }
        $seen[$entry.key] = $true
        if ($entry.hadDefault) { $null = Prior-Value $entry }
    }
    return $journal
}
function Remove-EmptyKey([string]$Key) {
    $handle = $registry.OpenSubKey($Key,$false)
    $empty = $false
    try { if ($null -ne $handle) { $empty = $handle.ValueCount -eq 0 -and $handle.SubKeyCount -eq 0 } }
    finally { if ($null -ne $handle) { $handle.Dispose() } }
    if ($empty) { $registry.DeleteSubKey($Key,$false) }
}
try {
    $journal = Read-Journal
    if ($Unregister) {
        # Old installations have no journal. Remove only their still-owned default, never a whole populated key.
        $entries = if ($null -ne $journal) { @($journal.entries) } else { @($registrations | ForEach-Object { @{ key=$_.key; manifest=$_.manifest; keyWasPresent=$false; hadDefault=$false } }) }
        foreach ($entry in $entries) {
            $current = Snapshot $entry.key
            if (-not (Is-Owned $current $entry.manifest)) { continue }
            $handle = $registry.OpenSubKey($entry.key,$true)
            try {
                if ($entry.hadDefault) { $handle.SetValue('',(Prior-Value $entry),[Microsoft.Win32.RegistryValueKind]([Enum]::Parse([Microsoft.Win32.RegistryValueKind],$entry.kind))) }
                else { $handle.DeleteValue('',$false) }
            } finally { if ($null -ne $handle) { $handle.Dispose() } }
            if (-not $entry.keyWasPresent) { Remove-EmptyKey $entry.key }
        }
        if ($null -ne $journal) { Remove-Item -LiteralPath $journalPath -Force }
        Write-Output 'Restored this installation browser bridge registrations; external changes were preserved.'
        return
    }
    $hostPath = Join-Path $installRoot 'maskpilot_native_host.exe'
    $desktopPath = Join-Path $installRoot 'maskpilot.exe'
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf) -or -not (Test-Path -LiteralPath $desktopPath -PathType Leaf)) { throw 'Desktop and native host executables must be deployed together.' }
    if ($null -eq $journal) {
        $entries = @()
        foreach ($registration in $registrations) {
            $entry = Snapshot $registration.key
            # A pre-journal registration owned by this exact installation cannot be restored to a deleted manifest.
            if (Is-Owned $entry $registration.manifest) { $entry.hadDefault=$false; $entry.kind=''; $entry.value=$null }
            $entry.key=$registration.key; $entry.manifest=$registration.manifest
            $entries += $entry
        }
        $journal = @{ product='MaskPilot'; schema=1; directory=$installRoot; entries=$entries }
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($journal | ConvertTo-Json -Depth 8))
        if ($bytes.Length -gt 2097152) { throw 'Browser registration journal exceeds its size limit.' }
        $stream = [IO.FileStream]::new($journalPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    } else {
        foreach ($entry in @($journal.entries)) {
            $current = Snapshot $entry.key
            if (-not (Is-Owned $current $entry.manifest) -and -not (Same-Default $current $entry)) { throw 'Browser registration changed outside this installation. Unregister before retrying setup.' }
        }
    }
    $origin = 'chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
    $manifest = @{ name='com.maskpilot.downloads'; description='MaskPilot download bridge'; path=$hostPath; type='stdio'; allowed_origins=@($origin) }
    $firefox = @{ name='com.maskpilot.downloads'; description='MaskPilot download bridge'; path=$hostPath; type='stdio'; allowed_extensions=@($geckoId) }
    $bridge = @{ allowedOrigins=@($origin); allowedFirefoxExtensions=@($geckoId); desktopPath=$desktopPath }
    if ($DataDirectory) { $bridge.dataDirectory = [IO.Path]::GetFullPath($DataDirectory) }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 5),$utf8)
    [IO.File]::WriteAllText($firefoxManifest,($firefox | ConvertTo-Json -Depth 5),$utf8)
    [IO.File]::WriteAllText((Join-Path $installRoot 'native-bridge.json'),($bridge | ConvertTo-Json -Depth 5),$utf8)
    foreach ($registration in $registrations) {
        $handle = $registry.CreateSubKey($registration.key,$true)
        try { $handle.SetValue('',$registration.manifest,[Microsoft.Win32.RegistryValueKind]::String) } finally { $handle.Dispose() }
    }
    Write-Output 'Registered the current-user browser bridges. Load the matching Chromium or Firefox developer extension.'
} finally { $registry.Dispose() }
