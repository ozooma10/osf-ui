[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunPath,
    [Parameter(Mandatory)][ValidateSet('Setup','SpawnTwo','MultipleTVs','SaveLoad','HostRecovery','StalledRecovery','CellUnload','CellReload','Navigation','Video','Snapshot','Meter','Tone','Browser','Console','Capture')][string]$Case,
    [ValidatePattern('^[a-z0-9-]+$')][string]$Name='',
    [string]$Value='', [string]$Payload='{}',
    [string]$VideoUrl='https://media.w3.org/2010/05/sintel/trailer.webm',
    [string]$SlowUrl='https://httpbin.org/delay/10',
    [ValidateRange(1000,30000)][int]$DurationMs=3000
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $RunPath
$script:AllowInteractive=$false
if($context.backend -notin @('webview2','cef')){throw 'Unknown validation backend.'}
if(-not $Name){$Name=$Case.ToLowerInvariant()+'-'+[DateTime]::UtcNow.ToString('HHmmssfff')}
$casePath=Join-Path $script:Run.path ('case-'+$Name+'.json')
$record=@{case=$Case;name=$Name;backend=$context.backend;passed=$false;startedUtc=[DateTime]::UtcNow.ToString('o');observations=@()}

function Page { Read-WorldPage 'osfui.browser/tv' }
function At($Object,[string[]]$Keys) {
    foreach($key in $Keys){if($Object -isnot [Collections.IDictionary] -or -not $Object.Contains($key)){return $null};$Object=$Object[$key]}
    return $Object
}
function Snapshot {
    $page=Page
    @{page=$page;audio=(At $page @('browserState','audio'));game=(Get-GameState);atUtc=[DateTime]::UtcNow.ToString('o');tickMs=[Environment]::TickCount64}
}
function Keep([string]$Label,$Data) {
    $record.observations+=@{label=$Label;data=$Data}
    Write-JsonFile $casePath $record
}
function Check([bool]$Passed,[string]$Message,$Data=$null) {
    Assert-Test $Passed $Message $Data
    if(-not $Passed){throw $Message}
}
function Browser([string]$Action,[hashtable]$Data=@{}) {
    $id=[guid]::NewGuid().ToString('N')
    $null=Send-Probe event @{view='osfui.browser/tv';name='osfui-browser-test.command';value=@{id=$id;action=$Action;payload=$Data}}
    $result=Wait-Observed "Browser $Action acknowledged" {Page} {param($p)(At $p @('result','id')) -eq $id} 20
    Check ((At $result @('result','ok')) -eq $true) "Browser $Action succeeded" $result.result
    return $result
}
function Tone([string]$Action) {
    $id=[guid]::NewGuid().ToString('N')
    $null=Send-Probe event @{view='osfui.browser/tv';name='osfui-browser-audio.command';value=@{id=$id;action=$Action}}
    Wait-Observed "Fixture tone $Action acknowledged" {Page} {param($p)
        (At $p @('audioProbe','reply','id')) -eq $id -and (At $p @('audioProbe','reply','ok')) -eq $true
    } 15
}
function AudioReady($Page,[bool]$Content=$false) {
    $route=At $Page @('browserState','audio')
    if(-not $Content){$route=At $route @('chrome')}
    if((At $route @('error')) -cne '' -or (At $route @('spatial','fresh')) -ne $true){return $false}
    if($context.backend -eq 'cef'){return (At $route @('active')) -eq $true -and (At $route @('frames')) -gt 0}
    return (At $route @('state')) -eq 'capturing' -and (At $route @('fresh')) -eq $true
}
function WaitTone([int]$Count=2) {
    Wait-Observed "Live tone with $Count TV emitters" {Page} {param($p)
        (AudioReady $p) -and (At $p @('audioProbe','state')) -eq 'running' -and
        (At $p @('browserState','audio','chrome','spatial','emitterCount')) -eq $Count
    } 30
}
function WaitCount([int]$Count) {
    Wait-Observed "$Count actual TV emitters" {Page} {param($p)
        (At $p @('browserState','audio','chrome','spatial','emitterCount')) -eq $Count
    } 8
}
function Spawn([bool]$Second) {
    $file=if($Second){'tv2.json'}else{'tv.json'}
    if(Test-Path -LiteralPath (Join-Path $script:Run.path $file)){throw 'TV fixture already exists; do not blindly repeat a mutation.'}
    $log=Join-Path $script:GameDocuments 'Logs/Script/Papyrus.0.log'
    $pattern=if($Second){'\[BrowserTVProbe\] second=.* id=(-?\d+)'}else{'\[BrowserTVProbe\] spawned=.* id=(-?\d+)'}
    $prior=@(Select-String -LiteralPath $log -Pattern $pattern -ErrorAction SilentlyContinue).Count
    $command=if($Second){'cgf "OSFBrowserTVProbe.SpawnAt" 4.0 2.5 1.5'}else{'cgf "OSFBrowserTVProbe.Spawn"'}
    Keep ('spawn-receipt-'+$file) (Invoke-ConsoleCommand $command)
    $reference=Wait-Observed 'New TV created by real Papyrus placement' {
        $matches=@(Select-String -LiteralPath $log -Pattern $pattern -ErrorAction SilentlyContinue)
        if($matches.Count -gt $prior){
            $id=[int]$matches[-1].Matches[0].Groups[1].Value
            if($id -eq 0){throw 'Papyrus placement returned None; verify the private Browser TV ESM is enabled.'}
            $id
        }
    } {param($id)$null -ne $id -and $id -ne 0} 20
    $hex='0x'+[BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$reference),0).ToString('X8')
    $fixture=@{reference=$reference;hex=$hex;player=(Get-GameState).position}
    Write-JsonFile (Join-Path $script:Run.path $file) $fixture
    return $fixture
}
function Meter([string]$Label,[bool]$Silent=$false,[bool]$Content=$false) {
    $output=Join-Path $script:Run.path ('meter-'+$Name+'-'+$Label)
    if(Test-Path -LiteralPath (Join-Path $output 'result.json')){throw 'Meter evidence already exists.'}
    $before=Snapshot
    Write-JsonFile (Join-Path $output 'before.json') $before
    try { & (Join-Path $repo 'tools/profiling/audio-endpoint-meter/measure.ps1') -OutputDirectory $output -DurationMs $DurationMs | Out-Null }
    finally { $after=Snapshot; Write-JsonFile (Join-Path $output 'after.json') $after }
    $result=Read-JsonFile (Join-Path $output 'result.json');$endpoint=$result.endpoint
    Check ($result.outcome -eq 'measured' -and $result.exitCode -eq 0 -and $endpoint.ready -and $endpoint.error -eq $null -and
        $endpoint.discontinuities -eq 0 -and $endpoint.timestampErrors -eq 0 -and $endpoint.invalidSamples -eq 0) "$Label endpoint measurement is healthy" $endpoint
    if(-not $Silent){Check ($before.page.documentId -and $before.page.documentId -eq $after.page.documentId) "$Label preserves the source document" @($before.page.documentId,$after.page.documentId)}
    if(-not $Silent) {
        Check ((AudioReady $before.page $Content) -and (AudioReady $after.page $Content)) "$Label source routes are healthy around output measurement"
        Check ($endpoint.frames -ge $endpoint.format.sampleRate*($DurationMs/1000-0.2)) "$Label captured enough endpoint frames" $endpoint.frames
        if($Content){Check ($endpoint.rms -gt 1e-4) "$Label remote video is audible" $endpoint.rms}
        else {
            Check ($endpoint.toneRms -gt 1e-4) "$Label controlled tone is audible" $endpoint.toneRms
            Check ($after.page.audioProbe.time -gt $before.page.audioProbe.time) "$Label tone advances through measurement"
        }
    }
    if($context.backend -eq 'cef' -and -not $Silent) {
        $a=$before.audio;$b=$after.audio;if(-not $Content){$a=$a.chrome;$b=$b.chrome}
        Check ($b.frames -gt $a.frames -and $b.generation -eq $a.generation) "$Label PCM stream advances without restart" @($a,$b)
        Check ($b.droppedPackets -eq $a.droppedPackets) "$Label drops no PCM packets" @($a.droppedPackets,$b.droppedPackets)
    }
    Keep ('meter-'+$Label) @{path=$output;toneRms=$endpoint.toneRms;rms=$endpoint.rms;endpointId=$endpoint.endpointId;format=$endpoint.format}
    return $result
}
function HostIdentity {
    $game=Get-OwnedGame;$state=Get-GameState
    if(-not $game -or $state.snapshotStale -or -not $state.loaded){throw 'Fresh owned game required.'}
    $name=if($context.backend -eq 'cef'){'osfui_cef_host.exe'}else{'osfui_webview2_host.exe'}
    $pattern='--game-pid='+$script:Session.pid+'(?:\s|$)'
    $rows=@(Get-CimInstance Win32_Process -Filter "Name='$name'"|Where-Object {
        $_.CommandLine -match $pattern -and $_.CommandLine -like '*surface-osfui.browser!tv.log*' -and $_.CommandLine -notmatch '--type='
    })
    if($rows.Count -ne 1){throw "Expected exactly one owned $name TV helper."}
    $process=Get-Process -Id $rows[0].ProcessId -ErrorAction Stop
    $null=$process.SafeHandle
    $identity=Get-ProcessIdentity $process
    if($process.HasExited -or [uint64]$identity.processStartFileTime -lt [uint64]$script:Session.processStartFileTime){throw 'Invalid helper process lifetime.'}
    return @{process=$process;pid=$identity.pid;processStartFileTime=$identity.processStartFileTime;executable=$rows[0].ExecutablePath;commandLine=$rows[0].CommandLine}
}
function HostRecord($HostInfo) { @{pid=$HostInfo.pid;processStartFileTime=$HostInfo.processStartFileTime;executable=$HostInfo.executable;commandLine=$HostInfo.commandLine} }
function Initialize-HostSuspension {
    if('OSFUIValidation.HostSuspension' -as [type]){return}
    # A test-only fault. The explicit handle pins one already validated helper;
    # there is no process-name suspension and no child-process enumeration.
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace OSFUIValidation {
 public static class HostSuspension {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessTimes(IntPtr handle, out long created, out long exited, out long kernel, out long user);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr handle);
  [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr handle);
  public static IntPtr Suspend(int pid, long expectedCreated) {
   var handle=OpenProcess(0x1800, false, pid); // query limited information + suspend/resume
   if(handle==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
   try {
    long created,exited,kernel,user;
    if(!GetProcessTimes(handle,out created,out exited,out kernel,out user)) throw new Win32Exception(Marshal.GetLastWin32Error());
    if(created!=expectedCreated || exited!=0) throw new InvalidOperationException("Helper identity changed before suspension");
    int status=NtSuspendProcess(handle);
    if(status!=0) throw new InvalidOperationException("NtSuspendProcess failed: "+status.ToString("X8"));
    return handle;
   } catch { CloseHandle(handle); throw; }
  }
  public static int ResumeAndClose(IntPtr handle) {
   try { return NtResumeProcess(handle); } finally { CloseHandle(handle); }
  }
 }
}
'@
}

$lock=[IO.File]::Open((Join-Path $context.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
$mayWrite=$false
try {
    if(Test-Path -LiteralPath $casePath){throw 'Case evidence already exists; use a fresh name.'}
    $mayWrite=$true
    Keep before (Snapshot)
    switch($Case) {
        Setup {
            Check ((Get-GameState).cellFormId -eq 2876329) 'Expected pinned fixture cell'
            $null=Spawn $false
            $null=Wait-Observed 'Reserved browser enabled' {Page} {param($p)(At $p @('browserState','enabled')) -eq $true} 45
            $null=Browser home; $null=Tone start; $null=WaitTone 1
            $null=Meter one-tv
            Save-Capture ($Name+'-one-tv')
        }
        SpawnTwo {$null=Spawn $true; $null=WaitTone 2}
        MultipleTVs {
            if(-not (Test-Path -LiteralPath (Join-Path $script:Run.path 'tv2.json'))){$null=Spawn $true}
            $page=WaitTone 2
            Check ($page.browserState.screens -eq 2) 'Two bound TVs are reported independently of their shared feed' $page.browserState.screens
            $first=Read-JsonFile (Join-Path $script:Run.path 'tv.json');$second=Read-JsonFile (Join-Path $script:Run.path 'tv2.json')
            $reference=Meter two-tvs;$prior=HostIdentity
            try {
                Keep disable-first (Invoke-ConsoleCommand ($first.hex+'.disable'));$null=WaitTone 1
                $null=Meter remaining-tv
                Keep disable-second (Invoke-ConsoleCommand ($second.hex+'.disable'))
                $null=Wait-Observed 'All TV emitters detach or their prior host retires' {
                    @{page=(Page);hostExited=$prior.process.HasExited}
                } {param($s)$s.hostExited -or (At $s.page @('browserState','audio','chrome','spatial','emitterCount')) -eq 0} 20
                $silence=Meter no-tvs $true
                Check ($silence.endpoint.toneRms -lt $reference.endpoint.toneRms*.01) 'No TV suppresses the controlled tone' $silence.endpoint.toneRms
            } finally {
                Keep enable-first (Invoke-ConsoleCommand ($first.hex+'.enable'))
                Keep enable-second (Invoke-ConsoleCommand ($second.hex+'.enable'))
                $prior.process.Dispose()
            }
            $null=Wait-Observed 'Reserved browser page returns after reenable' {Page} {param($p)$p -and $p.browserState.enabled} 45
            $null=Tone start;$null=WaitTone 2;$null=Meter restored-tvs
            Save-Capture ($Name+'-two-tvs')
        }
        SaveLoad {
            $before=WaitTone 2;$oldIds=@($before.browserState.audio.chrome.spatial.emitters|ForEach-Object {$_.id})
            Check (@($oldIds|Select-Object -Unique).Count -eq 2) 'Two distinct current binding identities'
            $saveRoot=Assert-ChildPath (Join-Path $script:GameDocuments 'OSFTestingFixture') $script:SaveRoot
            $expected=[IO.Path]::GetFullPath((Join-Path (Join-Path $script:GameDocuments 'OSFTestingFixture') $script:Run.id))
            $marker=Read-JsonFile (Join-Path $saveRoot '.osf-test-harness.json')
            if($saveRoot -ne $expected -or $marker.harnessRoot -ne $script:HarnessRoot){throw 'Unowned checkpoint directory.'}
            $checkpoint='osfworld'+[guid]::NewGuid().ToString('N');$path=Join-Path $saveRoot ($checkpoint+'.sfs')
            Keep save-receipt (Invoke-ConsoleCommand ('save '+$checkpoint))
            $written=Wait-Observed 'Unique private checkpoint is complete and readable' {
                if(Test-Path -LiteralPath $path){try{$stream=[IO.File]::Open($path,'Open','Read','Read');try{if($stream.Length -gt 0){@{path=$path;length=$stream.Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}}}finally{$stream.Dispose()}}catch [IO.IOException]{}}
            } {param($p)$p -and $p.length -gt 0} 30
            Keep checkpoint $written
            Write-JsonFile (Join-Path $script:Run.path 'checkpoint.json') @{name=$checkpoint;path=$path;sha256=$written.sha256;cellFormId=(Get-GameState).cellFormId}
            Keep load-receipt (Invoke-ConsoleCommand ('load '+$checkpoint))
            $page=Wait-Observed 'Load creates a new trusted document' {Page} {param($p)$p -and $p.documentId -ne $before.documentId} 60
            $null=Tone start;$page=WaitTone 2
            $newIds=@($page.browserState.audio.chrome.spatial.emitters|ForEach-Object {$_.id})
            Check (@($newIds|Select-Object -Unique).Count -eq 2 -and @($newIds|Where-Object {$_ -in $oldIds}).Count -eq 0) 'Load renews both session binding identities' @($oldIds,$newIds)
            $state=Wait-Observed 'Load returns to fresh unpaused fixture gameplay' {Get-GameState} {param($s)-not $s.snapshotStale -and $s.loaded -and $s.cellFormId -eq 2876329 -and $s.pauseRequestCount -eq 0} 30
            Keep game-after $state;$null=Meter after-load;Save-Capture ($Name+'-loaded')
        }
        HostRecovery {
            $before=WaitTone 2;$old=HostIdentity
            try {
                Keep host-before (HostRecord $old)
                $check=HostIdentity
                try {
                    Check ($check.pid -eq $old.pid -and $check.processStartFileTime -ceq $old.processStartFileTime -and $check.commandLine -ceq $old.commandLine -and $check.executable -ceq $old.executable) 'Exact owned TV helper identity remains stable before termination'
                } finally {$check.process.Dispose()}
                $timer=[Diagnostics.Stopwatch]::StartNew()
                Keep termination @{atUtc=[DateTime]::UtcNow.ToString('o');tickMs=[Environment]::TickCount64;target=(HostRecord $old)}
                Stop-Process -InputObject $old.process -Force -ErrorAction Stop
                Check ($old.process.WaitForExit(5000)) 'Pinned whole TV helper exited after requested fault'
                $page=Wait-Observed 'Whole TV helper recovery creates a new trusted document' {Page} {param($p)$p -and $p.documentId -ne $before.documentId} 60
                Keep recovery-time @{documentMs=$timer.ElapsedMilliseconds;page=$page}
                $new=HostIdentity
                try {Check ($new.processStartFileTime -cne $old.processStartFileTime) 'Replacement host has a new process identity';Keep host-after (HostRecord $new)} finally {$new.process.Dispose()}
                $null=Tone start;$null=WaitTone 2;Keep audio-recovery-time @{elapsedMs=$timer.ElapsedMilliseconds}
                $null=Meter after-recovery;Save-Capture ($Name+'-recovered')
            } finally {$old.process.Dispose()}
        }
        StalledRecovery {
            # Production retains its retry budget until 60 seconds of healthy
            # operation. Separate fault trials so recovery backoff is comparable.
            $previous=@(Get-ChildItem -LiteralPath $script:Run.path -Filter 'case-*.json' | ForEach-Object {
                $priorCase=Read-JsonFile $_.FullName
                if($priorCase.passed -and $priorCase.case -in @('HostRecovery','StalledRecovery')){$priorCase}
            } | Sort-Object completedUtc -Descending | Select-Object -First 1)
            if($previous.Count){
                $healthyUntil=([DateTimeOffset]$previous[0].completedUtc).AddSeconds(65)
                while([DateTimeOffset]::UtcNow -lt $healthyUntil){Start-Sleep -Milliseconds 500}
                Keep healthy-cooldown @{previous=$previous[0].name;untilUtc=$healthyUntil.ToString('o')}
            }
            $before=WaitTone 2;$old=HostIdentity;$suspendHandle=[IntPtr]::Zero
            try {
                Initialize-HostSuspension
                Keep host-before (HostRecord $old)
                $verified=HostIdentity
                try {Check ($verified.pid -eq $old.pid -and $verified.processStartFileTime -ceq $old.processStartFileTime -and $verified.commandLine -ceq $old.commandLine) 'Pinned stalled helper identity is stable'} finally {$verified.process.Dispose()}
                $timer=[Diagnostics.Stopwatch]::StartNew()
                $suspendHandle=[OSFUIValidation.HostSuspension]::Suspend([int]$old.pid,[long]$old.processStartFileTime)
                Keep suspension @{atUtc=[DateTime]::UtcNow.ToString('o');tickMs=[Environment]::TickCount64;host=(HostRecord $old)}
                $samples=[Collections.Generic.List[object]]::new();$recovered=$null
                do {
                    $sampleAt=[Environment]::TickCount64
                    try {
                        $game=Get-GameState;$page=Page
                        $samples.Add(@{elapsedMs=$timer.ElapsedMilliseconds;requestedTickMs=$sampleAt;completedTickMs=[Environment]::TickCount64;
                            tickCount=$game.tickCount;snapshotAtUtcMs=$game.snapshotAtUtcMs;snapshotAgeMs=$game.snapshotAgeMs;snapshotStale=$game.snapshotStale;
                            loaded=$game.loaded;hostExited=$old.process.HasExited;documentId=$page.documentId})
                        if($page -and $page.documentId -ne $before.documentId -and $old.process.HasExited){$recovered=$page;break}
                    } catch {
                        $samples.Add(@{elapsedMs=$timer.ElapsedMilliseconds;requestedTickMs=$sampleAt;error=$_.Exception.Message})
                    }
                    Start-Sleep -Milliseconds 100
                } while($timer.Elapsed.TotalSeconds -lt 65)
                Keep native-progress $samples.ToArray()
                Keep recovery-time @{documentMs=$timer.ElapsedMilliseconds;page=$recovered}
                Check ($null -ne $recovered -and $old.process.HasExited) 'Heartbeat failure retires the stalled helper and creates a new document'
                $new=HostIdentity
                try {Check ($new.processStartFileTime -cne $old.processStartFileTime) 'Stalled helper replacement has a new process identity';Keep host-after (HostRecord $new)} finally {$new.process.Dispose()}
                $null=Tone start;$null=WaitTone 2;$null=Meter after-stalled-recovery
                $record.scope='Only the pinned TV helper was suspended. Native tickCount is sampled drain progress, not rendered frame count. The old helper must be terminated by normal production recovery before a replacement is accepted.'
            } finally {
                if($suspendHandle -ne [IntPtr]::Zero){
                    $resumeStatus=[OSFUIValidation.HostSuspension]::ResumeAndClose($suspendHandle)
                    Keep suspension-cleanup @{resumeStatus=$resumeStatus;oldProcessExited=$old.process.HasExited}
                }
                $old.process.Dispose()
            }
        }
        CellUnload {
            $null=WaitTone 2;$old=HostIdentity
            try {
                Keep host-before (HostRecord $old)
                Keep transfer (Invoke-ConsoleCommand 'coc SettleRedMile02')
                $state=Wait-Observed 'Native cell transfer completes away from TVs' {Get-GameState} {param($s)-not $s.snapshotStale -and $s.loaded -and $s.cellFormId -eq 1281796 -and $s.pauseRequestCount -eq 0} 45
                Keep cell-after $state
                Check ($old.process.WaitForExit(15000)) 'Prior TV host retires after cell detach'
            } finally {$old.process.Dispose()}
        }
        CellReload {
            $checkpoint=Read-JsonFile (Join-Path $script:Run.path 'checkpoint.json')
            if(-not $checkpoint -or (Get-FileHash -LiteralPath $checkpoint.path -Algorithm SHA256).Hash -ne $checkpoint.sha256){throw 'Verified private checkpoint is unavailable.'}
            Keep reload (Invoke-ConsoleCommand ('load '+$checkpoint.name))
            $null=Wait-Observed 'Saved fixture TV page returns' {Page} {param($p)$p -and $p.browserState.enabled} 60
            $null=Tone start;$null=WaitTone 2;$null=Meter after-return;Save-Capture ($Name+'-returned')
        }
        Navigation {
            $null=Browser navigate @{input=($SlowUrl+'?osfui='+[guid]::NewGuid().ToString('N'))}
            $loading=Wait-Observed 'Remote navigation is observed loading before Stop' {Page} {param($p)(At $p @('browserState','loading')) -eq $true} 12
            Keep loading $loading;$null=Browser stop
            $settled=Wait-Observed 'Stop settles browser loading' {Page} {param($p)(At $p @('browserState','loading')) -eq $false} 15
            Keep stopped $settled
            $null=Browser navigate @{input='https://osfui-deliberately-missing.invalid/'}
            $failed=Wait-Observed 'Failed navigation settles with explicit error' {Page} {param($p)(At $p @('browserState','loading')) -eq $false -and (At $p @('browserState','error'))} 30
            Keep navigation-error $failed
            $null=Browser home;$null=Tone start;$null=WaitTone 2;$null=Meter after-navigation
            $record.limitations=@('The missing-domain check is a precommit failure; deterministic postcommit transport failure still requires a controlled allowed public fixture.')
        }
        Video {
            $null=Tone stop;$null=Browser navigate @{input=$VideoUrl}
            $ready=Wait-Observed 'Requested remote video has completed loading and has a fresh active route' {Page} {param($p)
                (At $p @('browserState','url')) -ceq $VideoUrl -and (At $p @('browserState','loading')) -eq $false -and
                (At $p @('browserState','error')) -ceq '' -and (AudioReady $p $true)
            } 45
            Keep ready $ready;$null=Meter video $false $true
            $null=Browser mute @{muted=$true}
            $null=Wait-Observed 'Video logical mute is observed' {Page} {param($p)$p -and $p.browserState.muted} 10
            $silent=Meter video-muted $true $true
            $null=Browser mute @{muted=$false}
            $null=Wait-Observed 'Video logical unmute is observed' {Page} {param($p)$p -and -not $p.browserState.muted} 10
            $null=Meter video-resumed $false $true
            Save-Capture ($Name+'-video')
            $record.limitations=@('Broadband endpoint energy includes game/desktop audio; nonstationary soundtrack cannot establish exact gain ratios or gap-free playback.')
        }
        Snapshot {}
        Meter {$null=Meter sample ($Value -eq 'silent') ($Payload -eq 'content')}
        Tone {$null=Tone $Value}
        Browser {$null=Browser $Value (ConvertFrom-Json -InputObject $Payload -AsHashtable)}
        Console {Keep receipt (Invoke-ConsoleCommand $Value)}
        Capture {Save-Capture $Name}
    }
    Keep after (Snapshot)
    $record.passed=$true
} catch {$record.error=$_.Exception.Message;throw}
finally {
    try {if($mayWrite){$record.completedUtc=[DateTime]::UtcNow.ToString('o');Write-JsonFile $casePath $record}} finally {$lock.Dispose()}
}
$record|ConvertTo-Json -Depth 12
