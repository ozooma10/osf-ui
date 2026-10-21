# Read-only observation of one owned game session. No focus, input or game commands.
# PresentMon is the only optional child process; it records the pinned game PID.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunPath,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Workload,
    [ValidateSet('baseline','current')][string]$Variant='current',
    [ValidateRange(1,100)][int]$Repeat=1,
    [ValidateRange(5,600)][int]$DurationSeconds=60,
    [ValidateRange(500,5000)][int]$IntervalMs=1000,
    [ValidateSet('steady','recovery')][string]$CaptureKind='steady',
    [string]$PresentMonPath='',
    [switch]$NoPresentMon,
    [string]$Notes=''
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$captureRunPath=[IO.Path]::GetFullPath($RunPath)
$context=Get-Content -LiteralPath (Join-Path $captureRunPath 'context.json') -Raw|ConvertFrom-Json -AsHashtable
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $captureRunPath
$session=Get-Content -LiteralPath (Join-Path $context.state 'session.json') -Raw|ConvertFrom-Json -AsHashtable
$owner=Get-Content -LiteralPath (Join-Path $context.state 'owner.json') -Raw|ConvertFrom-Json -AsHashtable
if([IO.Path]::GetFullPath($context.run.path) -ne $captureRunPath){throw 'Context belongs to another run.'}
if($owner.pid -ne $session.pid -or [string]$owner.processStartFileTime -ne [string]$session.processStartFileTime){throw 'Owner and native session identities differ.'}
$backend=[string]$context.backend
if($backend -notin @('webview2','cef')){throw 'Context must identify the backend.'}
if($context.ContainsKey('label') -and $context.label -ne $Variant){throw 'Variant differs from the launched context label.'}
$gameId=[int]$session.pid
$hostName=if($backend -eq 'cef'){'osfui_cef_host.exe'}else{'osfui_webview2_host.exe'}
$output=Join-Path $captureRunPath ("performance/$Variant-$Workload-r$Repeat")
if(Test-Path -LiteralPath $output){throw 'Performance evidence already exists; choose a fresh repeat number.'}

function Assert-OwnedGame {
    $games=@(Get-Process Starfield -ErrorAction SilentlyContinue)
    if($games.Count -ne 1 -or $games[0].Id -ne $gameId -or
        $games[0].StartTime.ToUniversalTime().ToFileTimeUtc().ToString() -ne [string]$session.processStartFileTime){
        throw 'Owned game exited, changed identity, or another game is running.'
    }
    return $games[0]
}
function Read-Page {
    $path=Join-Path $context.probe 'consumer.json'
    if(-not (Test-Path -LiteralPath $path)){return $null}
    $value=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json -AsHashtable
    if($value -and $value.ContainsKey('pages') -and $value.pages.ContainsKey('osfui.browser/tv')){return $value.pages['osfui.browser/tv']}
    return $null
}
function Hash-File([string]$Path) {
    @{path=$Path;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
}
function Inventory([bool]$AllowMissingHost=$false) {
    $null=Assert-OwnedGame
    $all=@(Get-CimInstance Win32_Process -Filter "Name='Starfield.exe' OR Name='osfui_webview2_host.exe' OR Name='msedgewebview2.exe' OR Name='osfui_cef_host.exe'")
    $roles=@{};$roles[$gameId]='Game'
    foreach($p in $all){
        if($p.Name -in @('osfui_webview2_host.exe','osfui_cef_host.exe') -and $p.CommandLine -notmatch '(?:^|\s)--type='){
            if($p.Name -ne $hostName -or $p.CommandLine -notmatch ("(?:^|\s)--game-pid=$gameId(?:\s|$)")){
                throw 'A browser helper outside the owned selected backend would contaminate this capture.'
            }
            $roles[[int]$p.ProcessId]='Host'
        }
    }
    if(-not $AllowMissingHost -and @($roles.Values|Where-Object {$_ -eq 'Host'}).Count -eq 0){throw 'No owned browser host is running.'}
    do {
        $added=$false
        foreach($p in $all){
            $id=[int]$p.ProcessId;$parent=[int]$p.ParentProcessId
            if(-not $roles.ContainsKey($id) -and $roles.ContainsKey($parent) -and $roles[$parent] -ne 'Game' -and
                $p.Name -in @('msedgewebview2.exe','osfui_cef_host.exe')){
                $roles[$id]='Browser';$added=$true
            }
        }
    }while($added)
    foreach($p in $all){if($roles.ContainsKey([int]$p.ProcessId)){@{process=$p;role=$roles[[int]$p.ProcessId]}}}
}

$game=Assert-OwnedGame
$initial=@(Inventory)
$hashes=[Collections.Generic.List[object]]::new()
foreach($name in @('OSFUI.dll','OSFUIWorldScreens.dll')){
    $loaded=@($game.Modules|Where-Object ModuleName -eq $name)
    if($loaded.Count -ne 1){throw "Expected one loaded $name."}
    $expected=Hash-File (Join-Path $context.mod "SFSE/Plugins/$name")
    $actual=Hash-File $loaded[0].FileName
    if($expected.sha256 -ne $actual.sha256){throw "Loaded $name differs from private payload."}
    $hashes.Add(@{role=$name;expected=$expected;actual=$actual})
}
$hostRelative=if($backend -eq 'cef'){'SFSE/Plugins/OSF/UI/bin/cef/osfui_cef_host.exe'}else{'SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe'}
$expectedHost=Hash-File (Join-Path $context.mod $hostRelative)
foreach($entry in $initial|Where-Object role -eq 'Host'){
    $actual=Hash-File $entry.process.ExecutablePath
    if($actual.sha256 -ne $expectedHost.sha256){throw 'Live helper executable differs from the private payload.'}
    $hashes.Add(@{role='Host';pid=$entry.process.ProcessId;expected=$expectedHost;actual=$actual})
    if($backend -eq 'cef'){
        $expected=Hash-File (Join-Path (Split-Path $expectedHost.path -Parent) 'osfui_cef_host.dll')
        $actual=Hash-File (Join-Path (Split-Path $entry.process.ExecutablePath -Parent) 'osfui_cef_host.dll')
        if($expected.sha256 -ne $actual.sha256){throw 'Live CEF implementation differs from private payload.'}
        $hashes.Add(@{role='CefHostDll';expected=$expected;actual=$actual})
    }
}
if(-not $PresentMonPath){$PresentMonPath=Join-Path $repo 'external/presentmon/PresentMon-2.4.1-x64.exe'}
if(-not $NoPresentMon -and -not (Test-Path -LiteralPath $PresentMonPath)){throw 'PresentMon unavailable; pass -NoPresentMon to explicitly record CPU-only evidence.'}
New-Item -ItemType Directory -Path $output -Force|Out-Null
$manifest=@{
    schemaVersion=1;status='prepared';runId=$context.run.id;runPath=$captureRunPath;backend=$backend;variant=$Variant;workload=$Workload;repeat=$Repeat
    durationSeconds=$DurationSeconds;intervalMs=$IntervalMs;captureKind=$CaptureKind;logicalProcessors=[Environment]::ProcessorCount;notes=$Notes
    session=@{pid=$gameId;processStartFileTime=[string]$session.processStartFileTime;sessionId=$session.sessionId}
    hashes=$hashes.ToArray();sampler=(Hash-File $PSCommandPath);startedAtUtc=$null;completedAtUtc=$null
    contextSha256=(Get-FileHash -LiteralPath (Join-Path $captureRunPath 'context.json') -Algorithm SHA256).Hash
    machine=@{cpu=@(Get-CimInstance Win32_Processor|Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
        gpu=@(Get-CimInstance Win32_VideoController|Select-Object Name,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution)}
    presentMon=if($NoPresentMon){$null}else{Hash-File $PresentMonPath};presentMonExitCode=$null
    contextSwitches='Not sampled: per-thread WMI/ETW adds material overhead; use a separate WPR attribution run.'
    scope='Process CPU/memory plus optional game presents. Game CPU includes the entire game. No input/focus commands, GPU utilization, or audio capture.'
}
$rows=[Collections.Generic.List[object]]::new();$observations=[Collections.Generic.List[object]]::new();$identities=@{};$prior=@{}
$timer=[Diagnostics.Stopwatch]::new();$present=$null;$failure=$null
$manifest|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8NoBOM
try {
    $page=Read-Page
    $page|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $output 'page-before.json') -Encoding utf8NoBOM
    if(-not $NoPresentMon){
        $arguments=@('--process_id',[string]$gameId,'--output_file',('"'+(Join-Path $output 'presentmon.csv')+'"'),
            '--qpc_time_ms','--v2_metrics','--timed',[string]$DurationSeconds,'--terminate_after_timed','--no_console_stats',
            '--session_name',('OSFUI-World-'+$gameId+'-'+[guid]::NewGuid().ToString('N')))
        $present=Start-Process -FilePath $PresentMonPath -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $output 'presentmon.stdout.log') -RedirectStandardError (Join-Path $output 'presentmon.stderr.log')
    }
    $manifest.status='recording';$manifest.startedAtUtc=[DateTime]::UtcNow.ToString('o');$timer.Start()
    $manifest|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8NoBOM
    do {
        $at=$timer.Elapsed.TotalSeconds;$inventory=@(Inventory ($CaptureKind -eq 'recovery'));$time=[DateTime]::UtcNow.ToString('o')
        foreach($entry in $inventory){
            $p=$entry.process;$created=$p.CreationDate.ToUniversalTime().ToString('o');$key="$($p.ProcessId)|$created"
            $cpu=([double]$p.KernelModeTime+[double]$p.UserModeTime)/1e7;$old=$prior[$key]
            $cpuPercent=if($old -and $at -gt $old.at){100*($cpu-$old.cpu)/($at-$old.at)}else{$null}
            $prior[$key]=@{at=$at;cpu=$cpu}
            $identities[$key]=@{pid=[int]$p.ProcessId;createdAtUtc=$created;parentPid=[int]$p.ParentProcessId;name=$p.Name;role=$entry.role;commandLine=$p.CommandLine}
            $rows.Add([pscustomobject]@{sample=$observations.Count;atSeconds=$at;atUtc=$time;role=$entry.role;pid=[int]$p.ProcessId;createdAtUtc=$created
                cpuSeconds=$cpu;cpuPercentOneCore=$cpuPercent;cpuPercentMachine=if($null -eq $cpuPercent){$null}else{$cpuPercent/[Environment]::ProcessorCount}
                workingSetBytes=[uint64]$p.WorkingSetSize;privateBytes=[uint64]$p.PrivatePageCount;threads=[int]$p.ThreadCount;handles=[int]$p.HandleCount})
        }
        $page=Read-Page
        $native=Get-GameState
        $observations.Add(@{atSeconds=$at;atUtc=$time;systemTickMs=[Environment]::TickCount64;processKeys=@($inventory|ForEach-Object {"$($_.process.ProcessId)|$($_.process.CreationDate.ToUniversalTime().ToString('o'))"});page=$page;native=$native})
        $delay=[math]::Min($IntervalMs-($timer.Elapsed.TotalSeconds-$at)*1000,($DurationSeconds-$timer.Elapsed.TotalSeconds)*1000)
        if($delay -gt 0){Start-Sleep -Milliseconds ([int]$delay)}
    }while($timer.Elapsed.TotalSeconds -lt $DurationSeconds)
    $null=Assert-OwnedGame
    $timer.Stop()
    if($present){
        if(-not $present.WaitForExit(15000)){throw 'Owned PresentMon did not finish its bounded capture.'}
        $present.Refresh();$manifest.presentMonExitCode=$present.ExitCode
        if($present.ExitCode -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $output 'presentmon.csv'))){throw 'PresentMon capture failed; retain this attempt and inspect its logs.'}
    }
    $manifest.status='complete'
}catch{$failure=$_;$manifest.status='failed';$manifest.error=$_.Exception.Message}
finally {
    $timer.Stop();$manifest.actualSeconds=$timer.Elapsed.TotalSeconds;$manifest.completedAtUtc=[DateTime]::UtcNow.ToString('o')
    $manifest.processes=@($identities.Values);$manifest.sampleCount=$observations.Count
    $manifest|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8NoBOM
    $rows|Export-Csv -LiteralPath (Join-Path $output 'process.csv') -NoTypeInformation
    ConvertTo-Json -InputObject $observations.ToArray() -Depth 20|Set-Content -LiteralPath (Join-Path $output 'observations.json') -Encoding utf8NoBOM
    if($present -and -not $present.HasExited){$present.Kill();$null=$present.WaitForExit(5000)}
}
if($failure){throw $failure}
Write-Output $output
