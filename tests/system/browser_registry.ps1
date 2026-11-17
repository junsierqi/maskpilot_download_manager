param([ValidateSet('register','restore')][string]$Mode,[string]$ManifestPath,[string]$BackupPath)
$ErrorActionPreference='Stop'
$paths=@('Software\Chromium\NativeMessagingHosts\com.maskpilot.downloads','Software\Google\Chrome\NativeMessagingHosts\com.maskpilot.downloads','Software\Microsoft\Edge\NativeMessagingHosts\com.maskpilot.downloads')
if($Mode -eq 'register') {
  $records=@()
  foreach($path in $paths) {
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path)
    $record=@{path=$path;existed=($null -ne $key);hasDefault=$false;value=$null;kind='String'}
    if($key) {
      $record.hasDefault=$key.GetValueNames() -contains ''
      if($record.hasDefault){$record.value=$key.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames);$record.kind=$key.GetValueKind('').ToString()}
      $key.Dispose()
    }
    $records+=$record
  }
  $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $BackupPath -Encoding UTF8
  foreach($path in $paths){$key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($path);try{$key.SetValue('', $ManifestPath, [Microsoft.Win32.RegistryValueKind]::String)}finally{$key.Dispose()}}
  Write-Output 'Registered only temporary MaskPilot native-host entries.'
} else {
  foreach($record in (Get-Content -Raw -LiteralPath $BackupPath | ConvertFrom-Json)) {
    if($paths -notcontains $record.path){throw 'Unexpected registry backup path'}
    $current=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($record.path)
    try { if($current -and $current.GetValue('') -ne $ManifestPath){throw 'Registration changed concurrently; refusing to replace another registration.'} }
    finally {if($current){$current.Dispose()}}
    if($record.existed) {
      $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($record.path)
      try{if($record.hasDefault){$kind=[System.Enum]::Parse([Microsoft.Win32.RegistryValueKind],[string]$record.kind);$key.SetValue('', $record.value, $kind)}else{$key.DeleteValue('', $false)}}finally{$key.Dispose()}
    } else {
      [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($record.path,$false)
    }
  }
  Write-Output 'Restored prior native-host registration defaults.'
}
