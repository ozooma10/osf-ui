[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunPath,
    [Parameter(Mandatory)][ValidateSet('SaveLoad','CellUnload')][string]$Action)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $RunPath
$lock=[IO.File]::Open((Join-Path $context.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
$record=@{action=$Action;passed=$false;startedAtUtc=[DateTime]::UtcNow.ToString('o')}
$filename=if($Action -eq 'SaveLoad'){'save-load-audio-confirmed.json'}else{'cell-unload-audio.json'}
$reportPath=Join-Path $script:Run.path $filename
$mayWrite=$false
function Route($Page) {
    if(-not $Page -or -not $Page.ContainsKey('browserState')){return @{}}
    $browser=$Page['browserState']
    if(-not $browser -or -not $browser.ContainsKey('audio')){return @{}}
    $audio=$browser['audio']
    if(-not $audio -or -not $audio.ContainsKey('chrome')){return @{}}
    return $audio['chrome']
}
function Scene($Page) {
    $route=Route $Page
    if($route.ContainsKey('spatial')){return $route['spatial']}
    return @{}
}
try {
    if(Test-Path -LiteralPath $reportPath){throw 'Lifecycle evidence already exists; preserve the earlier attempt.'}
    $mayWrite=$true
    $game=Get-GameState
    if($game.snapshotStale -or -not $game.loaded -or $game.pauseRequestCount -ne 0){throw 'Lifecycle checks require fresh unpaused owned gameplay.'}
    $before=Read-WorldPage 'osfui.browser/tv'
    if(-not $before -or -not $before.ContainsKey('documentId')){throw 'No fresh trusted browser document.'}
    $record.before=$before
    $record.gameBefore=$game
    if($Action -eq 'SaveLoad') {
        $sceneBefore=Scene $before
        if(-not $sceneBefore.ContainsKey('emitterCount') -or $sceneBefore.emitterCount -ne 2 -or -not $sceneBefore.fresh){throw 'Save/load requires two fresh actual TV emitters.'}
        $oldIds=@($sceneBefore.emitters|ForEach-Object {[uint32]$_.id})
        if(@($oldIds|Select-Object -Unique).Count -ne 2){throw 'Expected two distinct binding identities.'}
        $saveRoot=Assert-ChildPath (Join-Path $script:GameDocuments 'OSFTestingFixture') $script:SaveRoot
        $expectedRoot=[IO.Path]::GetFullPath((Join-Path (Join-Path $script:GameDocuments 'OSFTestingFixture') $script:Run.id))
        $marker=Read-JsonFile (Join-Path $saveRoot '.osf-test-harness.json')
        if($saveRoot -ne $expectedRoot -or -not $marker -or -not $marker.ContainsKey('harnessRoot') -or $marker.harnessRoot -ne $script:HarnessRoot){throw 'Save directory is not the marked private directory for this run.'}
        $saveName='osfwv2audio'+[guid]::NewGuid().ToString('N')
        $savePath=Assert-ChildPath $saveRoot (Join-Path $saveRoot ($saveName+'.sfs'))
        if(Test-Path -LiteralPath $savePath){throw 'Unique checkpoint unexpectedly exists.'}
        $filesBefore=@(Get-ChildItem -LiteralPath $saveRoot -File|ForEach-Object {@{name=$_.Name;length=$_.Length;lastWriteTimeUtc=$_.LastWriteTimeUtc.ToString('o')}})
        $namesBefore=@($filesBefore|ForEach-Object {$_.name})
        $record.save=@{name=$saveName;path=$savePath;filesBefore=$filesBefore;requestedAtUtc=[DateTime]::UtcNow.ToString('o')}
        Write-JsonFile $reportPath $record
        $requestedAt=[DateTime]::UtcNow
        $saveReceipt=Invoke-ConsoleCommand ('save '+$saveName)
        $record.saveReceipt=$saveReceipt
        Write-JsonFile (Join-Path $script:Run.path 'receipt-audio-save.json') $saveReceipt
        $written=Wait-Observed 'Unique audio checkpoint is new, closed and readable' {
            $newFiles=@(Get-ChildItem -LiteralPath $saveRoot -Filter '*.sfs' -File|Where-Object {$_.Name -notin $namesBefore})
            $match=@($newFiles|Where-Object {$_.BaseName -ieq $saveName})
            if($match.Count -ne 1 -or $match[0].Length -le 0 -or $match[0].LastWriteTimeUtc -lt $requestedAt){return $null}
            $checked=Assert-ChildPath $saveRoot $match[0].FullName
            if($checked -ne $savePath){throw 'New checkpoint path differs from the requested private save.'}
            try {
                $stream=[IO.File]::Open($checked,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
                try {$length=$stream.Length} finally {$stream.Dispose()}
                return @{path=$checked;name=$match[0].Name;length=$length;lastWriteTimeUtc=$match[0].LastWriteTimeUtc.ToString('o');newFiles=@($newFiles|ForEach-Object {$_.Name})}
            }catch [IO.IOException]{return $null}
        } {param($file)$file -and $file.length -gt 0} 30
        $record.save.written=$written
        $record.save.sha256=(Get-FileHash -LiteralPath $savePath -Algorithm SHA256).Hash
        $null=Wait-Observed 'Private checkpoint returns to unpaused gameplay' {Get-GameState} {param($state)-not $state.snapshotStale -and $state.loaded -and $state.pauseRequestCount -eq 0} 15
        if((Get-FileHash -LiteralPath $savePath -Algorithm SHA256).Hash -ne $record.save.sha256){throw 'Checkpoint changed before load.'}
        Write-JsonFile $reportPath $record
        $loadReceipt=Invoke-ConsoleCommand ('load '+$saveName)
        $record.loadReceipt=$loadReceipt
        Write-JsonFile (Join-Path $script:Run.path 'receipt-audio-load.json') $loadReceipt
        $samples=[Collections.Generic.List[object]]::new()
        $toneDocument='';$toneId='';$toneRestarts=0;$complete=$false;$page=$null
        $timer=[Diagnostics.Stopwatch]::StartNew()
        do {
            $page=Read-WorldPage 'osfui.browser/tv'
            if($page -and $page.documentId -ne $before.documentId) {
                if($page.documentId -ne $toneDocument) {
                    if($toneRestarts -ge 2){throw 'Trusted document recreated repeatedly during save/load.'}
                    $toneDocument=$page.documentId;$toneId=[guid]::NewGuid().ToString('N');$toneRestarts++
                    $receipt=Send-Probe event @{view='osfui.browser/tv';name='osfui-browser-audio.command';value=@{id=$toneId;action='start'}}
                    Write-JsonFile (Join-Path $script:Run.path ('receipt-audio-load-tone-'+$toneRestarts+'.json')) $receipt
                }
                $route=Route $page;$scene=Scene $page
                $samples.Add(@{elapsedMs=$timer.ElapsedMilliseconds;documentId=$page.documentId;route=$route})
                $ids=if($scene.ContainsKey('emitters')){@($scene.emitters|ForEach-Object {[uint32]$_.id})}else{@()}
                $probe=if($page.ContainsKey('audioProbe')){$page['audioProbe']}else{$null}
                $toneReady=$probe -and $probe.state -eq 'running' -and $probe.reply -and $probe.reply.id -eq $toneId -and $probe.reply.ok
                if($route.ContainsKey('state') -and $route.state -eq 'capturing' -and $route.error -eq '' -and $route.fresh -and
                    $scene.emitterCount -eq 2 -and $scene.fresh -and $scene.enabled -and @($ids|Select-Object -Unique).Count -eq 2 -and
                    @($ids|Where-Object {$_ -in $oldIds}).Count -eq 0 -and $toneReady){$complete=$true;break}
            }
            Start-Sleep -Milliseconds 150
        }while($timer.Elapsed.TotalSeconds -lt 45)
        $record.after=$page;$record.samples=$samples.ToArray();$record.toneRestarts=$toneRestarts
        $record.elapsedMs=$timer.ElapsedMilliseconds
        $record.game=Get-GameState
        $record.passed=$complete -and -not $record.game.snapshotStale -and $record.game.loaded -and $record.game.pauseRequestCount -eq 0 -and $record.game.cellFormId -eq $game.cellFormId
        $record.scope='New private save verified on disk; new trusted document and two renewed bindings observed after exact-name load. Output audibility requires a separate endpoint measurement.'
        if(-not $record.passed){throw 'Save/load did not restore a fresh capturing route with two renewed bindings and acknowledged running tone.'}
    } else {
        $pattern='--game-pid='+$script:Session.pid+'(?:\s|$)'
        $hosts=@(Get-CimInstance Win32_Process -Filter "name='osfui_webview2_host.exe'"|Where-Object {$_.CommandLine -match $pattern -and $_.CommandLine -like '*surface-osfui.browser!tv.log*' -and $_.CommandLine -notmatch '--type='})
        if($hosts.Count -ne 1){throw 'Expected exactly one browser TV host for the owned game.'}
        $process=Get-Process -Id $hosts[0].ProcessId -ErrorAction Stop
        $identity=Get-ProcessIdentity $process
        if($process.HasExited){throw 'Selected browser host already exited.'}
        # Holding a process handle pins identity; a reused PID cannot satisfy this wait.
        $null=$process.Handle
        $record.host=@{pid=$identity.pid;processStartFileTime=$identity.processStartFileTime;commandLine=$hosts[0].CommandLine}
        $record.oldCellFormId=$game.cellFormId
        $timer=[Diagnostics.Stopwatch]::StartNew()
        Write-JsonFile $reportPath $record
        $receipt=Invoke-ConsoleCommand 'coc SettleRedMile02'
        $record.receipt=$receipt
        Write-JsonFile (Join-Path $script:Run.path 'receipt-audio-cell-unload.json') $receipt
        $state=Wait-Observed 'Player reaches Red Mile away from the TVs' {Get-GameState} {
            param($state)-not $state.snapshotStale -and $state.loaded -and $state.cellFormId -eq 1281796 -and $state.cellFormId -ne $game.cellFormId -and $state.pauseRequestCount -eq 0
        } 30
        $record.game=$state
        $record.cellChangedElapsedMs=$timer.ElapsedMilliseconds
        $exited=$process.WaitForExit(15000)
        $record.elapsedMs=$timer.ElapsedMilliseconds;$record.after=Read-WorldPage 'osfui.browser/tv'
        $record.passed=$exited
        $record.scope='The pinned prior WebView2 TV host exits within 15 seconds after confirmed cell transfer; no process is terminated by this check.'
        if(-not $exited){throw 'Pinned browser TV host did not retire within fifteen seconds of confirmed cell detachment.'}
    }
    $record|ConvertTo-Json -Depth 14
}catch {
    $record.passed=$false;$record.error=$_.Exception.Message
    throw
}finally {
    try {
        if($mayWrite){$record.completedAtUtc=[DateTime]::UtcNow.ToString('o');Write-JsonFile $reportPath $record}
    } finally {$lock.Dispose()}
}
