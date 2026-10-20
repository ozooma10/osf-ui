# Dot-source to operate only the owned, disposable addon session through the existing harness.
param([Parameter(Mandatory)][string]$RunPath)
$context=Get-Content (Join-Path $RunPath 'context.json') -Raw|ConvertFrom-Json -AsHashtable
$Interactive=$context.ContainsKey('interactive') -and [bool]$context.interactive
$Scenario='WorldScreenAddons'
. (Join-Path $context.harness 'scripts/Common.ps1')
. (Join-Path $context.harness 'scripts/Environment.ps1')
. (Join-Path $context.harness 'scripts/Reports.ps1')
$script:Run=$context.run; $script:StateRoot=$context.state; $script:ProfileRoot=$context.profile
$script:ProbeDir=$context.probe; $script:ProbeSession=$context.session
$script:Config.TestProfile=$context.testProfile; $script:HarnessModName=$context.testProfile; $script:AutoLoadModName=$context.testProfile
$script:SaveRoot=$context.save; $script:FixtureRoot=$context.fixture
$script:Session=Read-JsonFile (Join-Path $script:StateRoot 'session.json')
$null=Get-OwnedGame
function Read-Consumer { Read-JsonFile (Join-Path $script:ProbeDir 'consumer.json') }
function Read-WorldPage([string]$View='starcade.arcade/screen-play') {
    $snapshot=Read-Consumer
    if($snapshot -and $snapshot.ContainsKey('pages') -and $snapshot.pages.ContainsKey($View)){
        $page=$snapshot.pages[$View]
        if([Environment]::TickCount64-$page.reportedTickMs -lt 3000){return $page}
    }
}
function Send-Probe([string]$Action,[hashtable]$Values=@{}) {
    $id=[guid]::NewGuid().ToString('N')
    $command=@{id=$id;session=$script:ProbeSession;action=$Action;deadline=([Environment]::TickCount64+10000)}
    foreach($key in $Values.Keys){$command[$key]=$Values[$key]}
    Write-JsonFile (Join-Path $script:ProbeDir 'command.json') $command
    Wait-Observed "Public API $Action returned" {Read-JsonFile (Join-Path $script:ProbeDir 'reply.json')} {param($r)$r -and $r.id -eq $id -and $r.session -eq $script:ProbeSession} 10
}
