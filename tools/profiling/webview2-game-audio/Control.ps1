param([Parameter(Mandatory)][string]$RunPath,
    [ValidateSet('Setup','Snapshot','Meter','Tone','Browser','Ready','Watch','SpawnTwo','Console','Capture')][string]$Action='Snapshot',
    [string]$Name='',[string]$Value='',[string]$Payload='{}',[double]$ToneHz=997,[switch]$Interactive)
$ErrorActionPreference='Stop'
$requestedInteractive=$Interactive.IsPresent
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $RunPath
$script:AllowInteractive=$requestedInteractive
if(-not $Name){$Name=$Action.ToLowerInvariant()+'-'+[DateTime]::UtcNow.ToString('HHmmssfff')}
if($Name -notmatch '^[a-z0-9-]+$'){throw 'Invalid evidence label'}
function PageValue($Page,[string[]]$Keys) {
    foreach($key in $Keys){
        if($Page -isnot [Collections.IDictionary] -or -not $Page.Contains($key)){return $null}
        $Page=$Page[$key]
    }
    return $Page
}
function Browser([string]$Command,[hashtable]$Data=@{}) {
    $id=[guid]::NewGuid().ToString('N')
    $null=Send-Probe event @{view='osfui.browser/tv';name='osfui-browser-test.command';value=@{id=$id;action=$Command;payload=$Data}}
    Wait-Observed "Browser $Command completed" {Read-WorldPage 'osfui.browser/tv'} {param($p)
        (PageValue $p @('result','id')) -eq $id -and (PageValue $p @('result','ok')) -eq $true
    } 20
}
function Tone([string]$Command) {
    $id=[guid]::NewGuid().ToString('N')
    $null=Send-Probe event @{view='osfui.browser/tv';name='osfui-browser-audio.command';value=@{id=$id;action=$Command}}
    Wait-Observed 'Quiet tone command completed' {Read-WorldPage 'osfui.browser/tv'} {param($p)
        (PageValue $p @('audioProbe','reply','id')) -eq $id -and (PageValue $p @('audioProbe','reply','ok')) -eq $true
    } 10
}
function Snapshot {
    $page=Read-WorldPage 'osfui.browser/tv'
    @{page=$page;audio=(PageValue $page @('browserState','audio'));game=(Get-GameState);atUtc=[DateTime]::UtcNow.ToString('o')}
}
function Spawn([bool]$Second) {
    $log=Join-Path $script:GameDocuments 'Logs/Script/Papyrus.0.log'
    $pattern=if($Second){'\[BrowserTVProbe\] second=.* id=(-?\d+)'}else{'\[BrowserTVProbe\] spawned=.* id=(-?\d+)'}
    $previous=@(Select-String -LiteralPath $log -Pattern $pattern -ErrorAction SilentlyContinue).Count
    $before=Get-GameState
    $command=if($Second){'cgf "OSFBrowserTVProbe.SpawnAt" 4.0 2.5 1.5'}else{'cgf "OSFBrowserTVProbe.Spawn"'}
    $null=Invoke-ConsoleCommand $command
    $reference=Wait-Observed 'A new real TV reference was spawned' {
        $matches=@(Select-String -LiteralPath $log -Pattern $pattern -ErrorAction SilentlyContinue)
        if($matches.Count -gt $previous){[int]$matches[-1].Matches[0].Groups[1].Value}
    } {param($id)$null -ne $id -and $id -ne 0} 15
    $filename=if($Second){'tv2.json'}else{'tv.json'}
    $hex=[BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$reference),0).ToString('X8')
    Write-JsonFile (Join-Path $script:Run.path $filename) @{reference=$reference;hex=$hex;player=$before.position}
}
$lock=[IO.File]::Open((Join-Path $context.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
try {
    switch($Action) {
        Setup {
            if((Get-GameState).cellFormId -ne 2876329){throw 'Unexpected fixture cell'}
            Spawn $false
            $null=Wait-Observed 'World browser enabled and ready' {Read-WorldPage 'osfui.browser/tv'} {param($p)(PageValue $p @('browserState','enabled')) -eq $true} 45
            $null=Browser home
            $null=Tone start
            $null=Wait-Observed 'Production chrome capture and native scene are live' {Read-WorldPage 'osfui.browser/tv'} {
                param($p)(PageValue $p @('browserState','audio','chrome','state')) -eq 'capturing' -and
                    (PageValue $p @('browserState','audio','chrome','spatial','fresh')) -eq $true
            } 20
        }
        SpawnTwo {Spawn $true; $null=Wait-Observed 'Two actual TV emitters' {Read-WorldPage 'osfui.browser/tv'} {param($p)(PageValue $p @('browserState','audio','chrome','spatial','emitterCount')) -eq 2} 15}
        Tone {$null=Tone $Value}
        Browser {$null=Browser $Value (ConvertFrom-Json -InputObject $Payload -AsHashtable)}
        Ready {
            $null=Wait-Observed 'The requested remote document is capturing with a fresh scene' {Read-WorldPage 'osfui.browser/tv'} {param($p)
                (PageValue $p @('browserState','url')) -ceq $Value -and
                (PageValue $p @('browserState','loading')) -eq $false -and
                (PageValue $p @('browserState','landing')) -eq $false -and
                (PageValue $p @('browserState','error')) -ceq '' -and
                (PageValue $p @('browserState','audio','state')) -ceq 'capturing' -and
                (PageValue $p @('browserState','audio','fresh')) -eq $true
            } 20
        }
        Watch {
            if($requestedInteractive){$null=Invoke-Input 'focus'}
            $tv=Read-JsonFile (Join-Path $script:Run.path 'tv.json')
            $null=Invoke-ConsoleCommand ('cgf "OSFBrowserTVProbe.Watch" '+$tv.reference)
            $null=Wait-Observed 'Actual TV Watch opens fullscreen' {Read-WorldPage 'osfui.browser/tv'} {param($p)
                (PageValue $p @('browserState','fullscreen')) -eq $true -and (PageValue $p @('interaction','active')) -eq $true
            } 15
        }
        Console {Write-JsonFile (Join-Path $script:Run.path ('receipt-'+$Name+'.json')) (Invoke-ConsoleCommand $Value); Start-Sleep -Milliseconds 700}
        Capture {Save-Capture $Name}
        Meter {
            $output=Join-Path $script:Run.path ('meter-'+$Name)
            if(Test-Path -LiteralPath (Join-Path $output 'result.json')){throw 'This meter phase already has a result; choose a new evidence label.'}
            Write-JsonFile (Join-Path $output 'before.json') (Snapshot)
            try { & (Join-Path $repo 'tools/profiling/audio-endpoint-meter/measure.ps1') -OutputDirectory $output -ToneHz $ToneHz }
            finally {
                try {Write-JsonFile (Join-Path $output 'after.json') (Snapshot)}
                catch {Write-JsonFile (Join-Path $output 'after-error.json') @{error=$_.Exception.Message;atUtc=[DateTime]::UtcNow.ToString('o')}}
            }
        }
    }
    if($Action -ne 'Meter') {
        $snapshot=Snapshot
        Write-JsonFile (Join-Path $script:Run.path ('snapshot-'+$Name+'.json')) $snapshot
        $snapshot | ConvertTo-Json -Depth 14
    }
} finally {$lock.Dispose()}
