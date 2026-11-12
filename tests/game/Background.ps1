# Scoped background acceptance through the existing consumer and owned harness.
# The shared runner lock is held by Test-Backend.ps1 throughout these functions.
$script:BackgroundStorageLease=$null
function Get-BackgroundBrowserProcesses {
    @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -in @('Starfield.exe','osfui_cef_host.exe','osfui_webview2_host.exe','osfui_browser_broker.exe') -or
        ($_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -match '(?i)[/\\]OSFUI[/\\]')
    })
}
function Move-StorageChildren([string]$Source,[string]$Destination) {
    # Keep the known-folder root stable. Fail on collisions instead of allowing
    # Move-Item to silently nest an original backup inside a recreated directory.
    [IO.Directory]::CreateDirectory($Destination)|Out-Null
    foreach($item in @(Get-ChildItem -LiteralPath $Source -Force)){
        $target=Assert-ChildPath $Destination (Join-Path $Destination $item.Name)
        if($item.PSIsContainer){[IO.Directory]::Move($item.FullName,$target)}
        else{[IO.File]::Move($item.FullName,$target)}
    }
}
function Resolve-PhysicalBrowserStorage([string]$Root) {
    if(-not ('OSFUIBackgroundPath' -as [type])){
        Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class OSFUIBackgroundPath {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint size, uint flags);
}
'@
    }
    [IO.Directory]::CreateDirectory($Root)|Out-Null
    $handle=[OSFUIBackgroundPath]::CreateFileW($Root,0,7,[IntPtr]::Zero,3,0x02000000,[IntPtr]::Zero)
    try {
        if($handle.IsInvalid){throw 'Cannot open browser storage directory.'}
        $buffer=[Text.StringBuilder]::new(32768)
        $length=[OSFUIBackgroundPath]::GetFinalPathNameByHandleW($handle,$buffer,$buffer.Capacity,0)
        if(-not $length -or $length -ge $buffer.Capacity){throw 'Cannot verify physical browser storage location.'}
        $physical=$buffer.ToString()
        if($physical.StartsWith('\\?\UNC\')){$physical='\\'+$physical.Substring(8)}
        elseif($physical.StartsWith('\\?\')){$physical=$physical.Substring(4)}
        return $physical
    } finally {$handle.Dispose()}
}
function Enter-PrivateBackgroundStorage {
    if(@(Get-BackgroundBrowserProcesses).Count){throw '[blocked] Browser storage isolation requires stopped game/helpers/browser children.'}
    $parent=[Environment]::GetFolderPath('LocalApplicationData')
    $logicalRoot=Assert-ChildPath $parent (Join-Path $parent 'OSFUI')
    $existed=Test-Path -LiteralPath $logicalRoot
    $root=Assert-ChildPath $parent (Resolve-PhysicalBrowserStorage $logicalRoot)
    $parent=Split-Path $root -Parent
    $backup=Assert-ChildPath $parent (Join-Path $parent ('OSFUI.saved-'+$script:ProbeSession))
    if(Test-Path -LiteralPath $backup){throw 'Browser backup already exists.'}
    $script:BackgroundStorageLease=@{root=$root;logicalRoot=$logicalRoot;backup=$backup;existed=$existed;before=(Get-StorageHashes $root);moved=$false;created=$false}
    Write-JsonFile (Join-Path $script:Run.path 'background-storage-lease.json') $script:BackgroundStorageLease
    [IO.Directory]::CreateDirectory($root)|Out-Null
    $script:BackgroundStorageLease.moved=$true
    Write-JsonFile (Join-Path $script:Run.path 'background-storage-lease.json') $script:BackgroundStorageLease
    Move-StorageChildren $root $backup
    Write-JsonFile (Join-Path $root '.osfui-background-test.json') @{session=$script:ProbeSession}
    $script:BackgroundStorageLease.created=$true
    Write-JsonFile (Join-Path $script:Run.path 'background-storage-lease.json') $script:BackgroundStorageLease
}
function Exit-PrivateBackgroundStorage {
    if(-not $script:BackgroundStorageLease){return}
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    while(@(Get-BackgroundBrowserProcesses).Count -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 200}
    if(@(Get-BackgroundBrowserProcesses).Count){throw 'Processes remain; preserve browser storage and its backup for owned cleanup.'}
    $lease=$script:BackgroundStorageLease
    $parent=[Environment]::GetFolderPath('LocalApplicationData')
    $root=Assert-ChildPath $parent $lease.root
    $backup=Assert-ChildPath $parent $lease.backup
    if($lease.created){
        $marker=Read-JsonFile (Join-Path $root '.osfui-background-test.json')
        if(-not $marker -or $marker.session -ne $script:ProbeSession){throw 'Browser storage marker mismatch.'}
        $destination=Assert-ChildPath $script:Run.path (Join-Path $script:Run.path 'browser-storage')
        if(Test-Path -LiteralPath $destination){throw 'Browser evidence already exists.'}
        Move-StorageChildren $root $destination
    }
    if($lease.moved){Move-StorageChildren $backup $root;[IO.Directory]::Delete($backup)}
    Assert-Test ((Get-StorageHashes $root) -ceq $lease.before) 'Original browser profiles, caches and helper copies restored byte-for-byte'
    if(-not $lease.existed){[IO.Directory]::Delete($root)}
    $script:BackgroundStorageLease=$null
}
function Stop-BackgroundBrokers {
    if(-not $script:Session){return}
    $expected=Join-Path $mod 'SFSE/Plugins/OSF/UI/bin/broker/osfui_browser_broker.exe'
    foreach($broker in @(Get-CimInstance Win32_Process -Filter "Name='osfui_browser_broker.exe'")){
        if($broker.ParentProcessId -ne $script:Session.pid -or $broker.ExecutablePath -ne $expected -or
           $broker.CommandLine -notmatch ('--game-pid='+$script:Session.pid+'(?:\s|$)')){continue}
        $process=Get-Process -Id $broker.ProcessId -ErrorAction SilentlyContinue
        if(-not $process){continue}
        $null=$process.Handle
        if($process.StartTime.ToUniversalTime().ToFileTimeUtc() -lt [long]$script:Session.processStartFileTime){throw 'Broker predates the owned game.'}
        Add-Evidence 'cleanup' 'Pinned broker from owned game' (Get-ProcessIdentity $process)
        if(-not $process.WaitForExit(12000)){$process.Kill();if(-not $process.WaitForExit(5000)){throw 'Owned broker did not exit.'}}
    }
}
function Invoke-BackgroundBrowserChecks {
    $view='osfui-backend-test/hud'
    Open-Panel hud
    $hosts=@(Get-TestHosts)
    Assert-Test ($hosts.Count -eq 1) 'Exactly one owned production browser helper'
    $expected=if($Backend -eq 'cef'){'SFSE/Plugins/OSF/UI/bin/cef/osfui_cef_host.exe'}else{'SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe'}
    Assert-Test ((Get-FileHash -LiteralPath $hosts[0].Path).Hash -eq (Get-FileHash -LiteralPath (Join-Path $data $expected)).Hash) 'Running helper matches the tested production package'
    Capture-Panel 'background-hud'
    Send-Probe state @{value=@{revision=7}}
    Send-Probe event @{view=$view;name='probe';value=@{number=42}}
    $null=Wait-Observed 'HUD receives retained state and event' {Read-Page hud} {param($p)$p -and $p.state -and $p.state.revision -eq 7 -and @($p.events|Where-Object number -eq 42).Count -eq 1} 15
    $old=(Read-Page hud).document
    Send-Probe event @{view=$view;name='reload';value=@{}}
    $null=Wait-Observed 'Reload creates a fresh HUD with retained state' {Read-Page hud} {param($p)$p -and $p.document -ne $old -and $p.complete -and $p.state -and $p.state.revision -eq 7} 30
    foreach($cycle in 1..8){Send-Probe menu @{view=$view;open=$false};Open-Panel hud}
    $null=Invoke-ConsoleCommand 'cgf "OSFUIBackendPapyrus.Register"'
    Send-Probe event @{view=$view;name='papyrus';value=@{number=37}}
    $null=Wait-Observed 'Actual Papyrus request/reply reaches HUD' {Read-Page hud} {param($p)$p -and $p.ContainsKey('papyrus') -and $p.papyrus -eq 42} 15
    Capture-Panel 'background-reload'
    Send-Probe menu @{view=$view;open=$false}
    # Observe a changed position after load; a returned console handler alone is insufficient.
    Test-OpenPanelSave
    Test-OpenPanelSave -Scripted
    $before=Get-GameState
    $null=Invoke-ConsoleCommand 'save OSFUIBackground'
    $null=Wait-Observed 'Private checkpoint written' {
        $file=Get-ChildItem -LiteralPath $script:SaveRoot -Filter 'OSFUIBackground.sfs'|Select-Object -First 1
        if($file){@{path=$file.FullName;length=$file.Length}}
    } {param($f)$f -and $f.length -gt 0} 30
    $changed=$before.position.x+1.0
    $null=Invoke-ConsoleCommand ('player.setpos x '+$changed.ToString('F4',[Globalization.CultureInfo]::InvariantCulture))
    $null=Wait-Observed 'Private player mutation completed' {Get-GameState} {param($s)-not $s.snapshotStale -and [Math]::Abs($s.position.x-$changed) -lt 0.01} 15
    $null=Invoke-ConsoleCommand 'load OSFUIBackground'
    $null=Wait-Observed 'Load restores checkpoint position' {Get-GameState} {param($s)$s.loaded -and -not $s.snapshotStale -and $s.menus.Count -eq 0 -and [Math]::Abs($s.position.x-$before.position.x) -lt 0.01} 60
    Start-Sleep -Seconds 5
    Open-Panel hud
    Capture-Panel 'background-after-load'
    $old=(Read-Page hud).document
    $hosts=@(Get-TestHosts)
    Assert-Test ($hosts.Count -eq 1) 'Exactly one owned host before fault injection'
    $hostProcess=$hosts[0];$null=$hostProcess.Handle
    Add-Evidence 'fault' 'Terminate pinned owned browser helper' (Get-ProcessIdentity $hostProcess)
    $hostProcess.Kill()
    $null=Wait-Observed 'Recovery creates a fresh ready HUD' {Read-Page hud} {param($p)$p -and $p.document -ne $old -and $p.complete} 60
    Capture-Panel 'background-after-recovery'
    $timer=[Diagnostics.Stopwatch]::StartNew();$frame=(Read-Page hud).frame
    while($timer.Elapsed.TotalSeconds -lt $SoakSeconds){
        Start-Sleep -Seconds 5
        $page=Read-Page hud
        Assert-Test ($page -and $page.errors.Count -eq 0 -and $page.frame -gt $frame) 'HUD animation and bridge remain live'
        $frame=$page.frame
    }
    Send-Probe menu @{view=$view;open=$false}
    $null=Save-Capture 'background-final-gameplay'
}
