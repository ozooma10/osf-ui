param([Parameter(Mandatory)][string]$RunPath,
    [ValidatePattern('^[a-z0-9-]+$')][string]$Name='collector-crash',
    [ValidateRange(5,60)][int]$TimeoutSeconds=25)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $RunPath
$script:AllowInteractive=$false

function ValueAt($Object,[string[]]$Keys) {
    foreach($key in $Keys){
        if($Object -isnot [Collections.IDictionary] -or -not $Object.Contains($key)){return $null}
        $Object=$Object[$key]
    }
    return $Object
}
function SwitchValue([string]$CommandLine,[string]$Name) {
    $pattern='(?<!\S)--'+[regex]::Escape($Name)+'=(?:"([^"]*)"|([^\s"]+))(?=\s|$)'
    $found=[regex]::Matches($CommandLine,$pattern)
    if($found.Count -gt 1){throw "Duplicate process switch --$Name"}
    if($found.Count -eq 0){return $null}
    if($found[0].Groups[1].Success){return $found[0].Groups[1].Value}
    return $found[0].Groups[2].Value
}
function HasSwitch([string]$CommandLine,[string]$Name) {
    return [regex]::IsMatch($CommandLine,'(?<!\S)--'+[regex]::Escape($Name)+'(?=\s|$)')
}
function ProcessRecord($Process) {
    if(-not $Process -or -not $Process.CreationDate -or -not $Process.CommandLine -or -not $Process.ExecutablePath){
        throw 'Process identity metadata is incomplete.'
    }
    return @{pid=[uint32]$Process.ProcessId;parentPid=[uint32]$Process.ParentProcessId;name=$Process.Name;
        createdUtc=$Process.CreationDate.ToUniversalTime().ToString('o');
        creationFileTime=$Process.CreationDate.ToUniversalTime().ToFileTimeUtc().ToString();
        executable=$Process.ExecutablePath;commandLine=$Process.CommandLine}
}
function Assert-RecordSame($Before,$After) {
    foreach($field in @('pid','parentPid','name','creationFileTime','executable','commandLine')){
        if($Before[$field] -cne $After[$field]){throw "Owned process changed before/after collector termination ($field)."}
    }
}
function Assert-ProfileLease {
    $lease=Read-JsonFile (Join-Path $script:Run.path 'browser-profile-lease.json')
    if(-not $lease -or (ValueAt $lease @('restored'))){throw 'An active private audio profile lease is required.'}
    $root=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'OSFUI/browser/webview2'
    $profile=Assert-ChildPath $root $lease.profile
    if($profile -ine [IO.Path]::GetFullPath((Join-Path $root 'surface-osfui.browser!tv'))){throw 'Unexpected audio profile lease path.'}
    $marker=Read-JsonFile (Join-Path $profile '.osfui-audio-test.json')
    if(-not $marker -or $marker.session -cne $script:ProbeSession){throw 'Audio profile ownership marker mismatch.'}
    return @{profile=$profile;userData=[IO.Path]::GetFullPath((Join-Path $profile 'EBWebView'));session=$marker.session}
}
function Assert-FreshGame {
    $game=Get-OwnedGame
    if(-not $game){throw 'Owned game has exited.'}
    $identity=Get-ProcessIdentity $game
    $state=Get-GameState
    if(-not $state -or $state.snapshotStale -or -not $state.loaded -or $state.snapshotAgeMs -ge 2000 -or
        $state.pid -ne $identity.pid -or [string]$state.processStartFileTime -cne $identity.processStartFileTime -or
        $state.sessionId -cne $script:Session.sessionId){throw 'Fresh loaded state from the owned game is required.'}
    return $state
}
function OwnedProcesses($Lease,$Game) {
    # Read metadata broadly enough to discover fresh PIDs; retain only the exact leased profile.
    $profileProcesses=@(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object {
        (SwitchValue $_.CommandLine 'user-data-dir') -ieq $Lease.userData
    })
    $renderers=@($profileProcesses | Where-Object {
        (SwitchValue $_.CommandLine 'type') -ceq 'renderer' -and
        (SwitchValue $_.CommandLine 'renderer-sub-type') -ceq 'extension' -and
        (HasSwitch $_.CommandLine 'extension-process')
    })
    if($renderers.Count -ne 1){throw "Expected exactly one collector extension renderer in the owned profile; found $($renderers.Count)."}
    $renderer=$renderers[0]
    $browsers=@($profileProcesses | Where-Object {$_.ProcessId -eq $renderer.ParentProcessId})
    if($browsers.Count -ne 1){throw 'Collector parent is not the uniquely identified owned browser.'}
    $browser=$browsers[0]
    if((SwitchValue $browser.CommandLine 'type') -or
        (SwitchValue $browser.CommandLine 'webview-exe-name') -cne 'osfui_webview2_host.exe' -or
        (SwitchValue $browser.CommandLine 'allowlisted-extension-id') -cne 'nbligbkobhajkimckkfnadnghkimjcld' -or
        (SwitchValue $renderer.CommandLine 'allowlisted-extension-id') -cne 'nbligbkobhajkimckkfnadnghkimjcld'){
        throw 'Unexpected browser/collector process role or extension identity.'
    }
    $hostRow=Get-CimInstance Win32_Process -Filter ("ProcessId="+[uint32]$browser.ParentProcessId)
    $hostRecord=ProcessRecord $hostRow
    $expectedHost=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'OSFUI/bin/osfui_webview2_host.exe'
    $expectedLog=Join-Path $script:GameDocuments 'SFSE/Logs/surface-osfui.browser!tv.log'
    if($hostRecord.name -cne 'osfui_webview2_host.exe' -or $hostRecord.executable -ine $expectedHost -or
        (SwitchValue $hostRecord.commandLine 'game-pid') -cne [string]$Game.pid -or
        (SwitchValue $hostRecord.commandLine 'log') -ine $expectedLog -or
        (SwitchValue $hostRecord.commandLine 'pipe') -cnotmatch ('^osfui-browser-'+$Game.pid+'-[0-9a-f]{8}$')){
        throw 'Browser parent does not identify the owned game and reserved TV host.'
    }
    $browserRecord=ProcessRecord $browser
    $rendererRecord=ProcessRecord $renderer
    if($browserRecord.executable -ine $rendererRecord.executable -or
        [uint64]$hostRecord.creationFileTime -lt [uint64]$Game.processStartFileTime -or
        [uint64]$browserRecord.creationFileTime -lt [uint64]$hostRecord.creationFileTime -or
        [uint64]$rendererRecord.creationFileTime -lt [uint64]$browserRecord.creationFileTime){throw 'Process lineage creation times are invalid.'}
    $sources=@($profileProcesses | Where-Object {
        $_.ParentProcessId -eq $browser.ProcessId -and (SwitchValue $_.CommandLine 'type') -ceq 'renderer' -and
        -not (HasSwitch $_.CommandLine 'extension-process')
    } | ForEach-Object {ProcessRecord $_})
    if($sources.Count -lt 1){throw 'No ordinary source renderer was identified; collector-only targeting is unproven.'}
    return @{host=$hostRecord;browser=$browserRecord;collector=$rendererRecord;sources=$sources;log=$expectedLog}
}
function Snapshot {
    $game=Assert-FreshGame
    $page=Read-WorldPage 'osfui.browser/tv'
    $age=if($page){[Environment]::TickCount64-[long]$page.reportedTickMs}else{3000}
    if(-not $page -or $age -lt 0 -or $age -ge 3000){throw 'Fresh trusted chrome observation is unavailable.'}
    return @{game=$game;page=$page;audio=(ValueAt $page @('browserState','audio'));atUtc=[DateTime]::UtcNow.ToString('o')}
}
function ContentRecoveryExpectation($Before) {
    $url=ValueAt $Before @('page','browserState','url')
    $settled=(ValueAt $Before @('page','browserState','loading')) -eq $false
    $landing=ValueAt $Before @('page','browserState','landing')
    $state=ValueAt $Before @('audio','state')
    if((ValueAt $Before @('audio','error')) -cne '' -or
        (ValueAt $Before @('page','browserState','error')) -cne ''){throw 'Content route must be error-free before the crash test.'}
    if($state -ceq 'capturing' -and $url -match '^https?://' -and $settled -and $landing -eq $false){
        return @{required=$true;sourceUrl=$url;skipReason=''}
    }
    if($url -ceq 'about:blank' -and $settled -and $landing -eq $true -and
        $state -in @('capturing','silent','waiting','starting')){
        return @{required=$false;sourceUrl=$url;skipReason='Inactive about:blank source waits for a real URL after collector recreation.'}
    }
    throw 'Content must be a settled captured remote source or an error-free inactive about:blank source.'
}
function ContentRecovered($After,$Expectation) {
    if((ValueAt $After @('page','browserState','url')) -cne $Expectation.sourceUrl -or
        (ValueAt $After @('page','browserState','loading')) -ne $false -or
        (ValueAt $After @('page','browserState','error')) -cne '' -or
        (ValueAt $After @('audio','error')) -cne ''){return $false}
    if($Expectation.required){
        return (ValueAt $After @('page','browserState','landing')) -eq $false -and
            (ValueAt $After @('audio','state')) -ceq 'capturing' -and
            (ValueAt $After @('audio','spatial','fresh')) -eq $true
    }
    return $Expectation.sourceUrl -ceq 'about:blank' -and
        (ValueAt $After @('page','browserState','landing')) -eq $true -and
        (ValueAt $After @('audio','state')) -in @('waiting','starting')
}

$lock=[IO.File]::Open((Join-Path $context.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
$evidence=$null
$processes=$null
$report=@{result='not-started';terminated=$false;observations=@();session=$script:ProbeSession;logOffset=0}
try {
    $evidence=Assert-ChildPath $script:Run.path (Join-Path $script:Run.path $Name)
    if(Test-Path -LiteralPath $evidence){$evidence=$null;throw 'Collector-crash evidence directory already exists; use a new name.'}
    [IO.Directory]::CreateDirectory($evidence)|Out-Null
    $lease=Assert-ProfileLease
    $before=Snapshot
    if((ValueAt $before @('audio','chrome','state')) -cne 'capturing' -or
        (ValueAt $before @('page','audioProbe','state')) -cne 'running' -or
        (ValueAt $before @('page','audioProbe','toneHz')) -ne 997 -or
        -not (ValueAt $before @('page','documentId'))){throw 'Start the quiet chrome tone and confirm capture before this crash test.'}
    $report.contentRecovery=ContentRecoveryExpectation $before
    $processes=OwnedProcesses $lease $before.game
    Write-JsonFile (Join-Path $evidence 'before.json') $before
    Write-JsonFile (Join-Path $evidence 'processes-before.json') $processes
    $report.lease=$lease
    $report.logOffset=(Get-Item -LiteralPath $processes.log).Length

    # Open/pin the exact process object, then revalidate PID, creation time and full ancestry.
    # Stop-Process receives this one object, never a name, wildcard, pipeline or process tree.
    $target=[Diagnostics.Process]::GetProcessById([int]$processes.collector.pid)
    try {
        $null=$target.SafeHandle
        $targetStart=$target.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()
        $freshGame=Assert-FreshGame
        if($freshGame.pid -ne $before.game.pid -or $freshGame.processStartFileTime -cne $before.game.processStartFileTime){throw 'Game identity changed.'}
        $null=Assert-ProfileLease
        $checked=OwnedProcesses $lease $freshGame
        foreach($role in @('host','browser','collector')){Assert-RecordSame $processes[$role] $checked[$role]}
        if($target.HasExited -or $target.Id -ne $checked.collector.pid -or
            $target.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() -cne $targetStart -or
            [Math]::Abs([long]$targetStart-[long]$checked.collector.creationFileTime) -ge 10){throw 'Collector process creation identity changed.'}
        $report.targetStartFileTime=$targetStart
        $report.result='terminating-collector'
        $report.stopAtUtc=[DateTime]::UtcNow.ToString('o')
        $report.stopTickMs=[Environment]::TickCount64
        Write-JsonFile (Join-Path $evidence 'result.json') $report
        Stop-Process -InputObject $target -Force -ErrorAction Stop
        if(-not $target.WaitForExit(5000)){throw 'Exact collector renderer did not exit.'}
        $report.terminated=$true
    } finally {$target.Dispose()}

    $timer=[Diagnostics.Stopwatch]::StartNew()
    $recovered=$null
    do {
        $after=Snapshot
        $report.observations+=@{atUtc=$after.atUtc;reportedTickMs=$after.page.reportedTickMs;
            contentState=(ValueAt $after @('audio','state'));contentError=(ValueAt $after @('audio','error'));
            chromeState=(ValueAt $after @('audio','chrome','state'));chromeError=(ValueAt $after @('audio','chrome','error'));
            contentUrl=(ValueAt $after @('page','browserState','url'));
            contentLoading=(ValueAt $after @('page','browserState','loading'));
            contentLanding=(ValueAt $after @('page','browserState','landing'));
            documentId=$after.page.documentId;tone=$after.page.audioProbe}
        Write-JsonFile (Join-Path $evidence 'after-latest.json') $after
        $contentRecovered=ContentRecovered $after $report.contentRecovery
        if($contentRecovered -and [long]$after.page.reportedTickMs -gt $report.stopTickMs -and
            (ValueAt $after @('audio','chrome','state')) -ceq 'capturing' -and
            (ValueAt $after @('audio','chrome','spatial','fresh')) -eq $true){
            try {
                $current=OwnedProcesses $lease $after.game
                if($current.collector.creationFileTime -cne $processes.collector.creationFileTime){$recovered=$after;break}
            } catch { $report.lastDiscoveryError=$_.Exception.Message }
        }
        Start-Sleep -Milliseconds 100
    } while($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    if(-not $recovered){throw 'Collector capture did not recover within the observation deadline.'}
    Write-JsonFile (Join-Path $evidence 'after.json') $recovered
    Write-JsonFile (Join-Path $evidence 'processes-after.json') $current
    foreach($role in @('host','browser')){Assert-RecordSame $processes[$role] $current[$role]}
    foreach($source in $processes.sources){
        $same=@($current.sources | Where-Object {$_.pid -eq $source.pid})
        if($same.Count -ne 1){throw 'An ordinary source renderer changed during collector recovery.'}
        Assert-RecordSame $source $same[0]
    }
    if($recovered.page.documentId -cne $before.page.documentId -or
        $recovered.page.audioProbe.state -cne 'running' -or $recovered.page.audioProbe.toneHz -ne 997 -or
        $recovered.page.audioProbe.reply.id -cne $before.page.audioProbe.reply.id -or
        $recovered.page.audioProbe.time -lt $before.page.audioProbe.time){throw 'Source document or acknowledged running tone did not survive collector recovery.'}
    $report.result='recovered-source-preserved'
    $report.recoverySeconds=$timer.Elapsed.TotalSeconds
    $report.failureStateObserved=@($report.observations | Where-Object {$_.contentState -eq 'failed' -or $_.chromeState -eq 'failed'}).Count -gt 0
    $report.note='Observer reports every 500 ms; a brief failure may appear only in the retained native log. Measure endpoint output separately.'
} catch {
    $report.result=if($report.terminated){'recovery-check-failed'}else{'blocked-before-confirmed-termination'}
    $report.error=$_.Exception.Message
    throw
} finally {
    try {
        if($evidence -and (Test-Path -LiteralPath $evidence)){
            if($processes -and (Test-Path -LiteralPath $processes.log)){
                $stream=[IO.File]::Open($processes.log,'Open','Read','ReadWrite')
                try {
                    if($stream.Length -ge [long]$report.logOffset){$null=$stream.Seek([long]$report.logOffset,'Begin')}
                    $reader=[IO.StreamReader]::new($stream)
                    try {[IO.File]::WriteAllText((Join-Path $evidence 'native-log.txt'),$reader.ReadToEnd())}finally{$reader.Dispose()}
                } finally {$stream.Dispose()}
            }
            Write-JsonFile (Join-Path $evidence 'result.json') $report
        }
    } finally {$lock.Dispose()}
}
$report | ConvertTo-Json -Depth 12
