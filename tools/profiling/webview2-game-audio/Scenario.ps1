param([Parameter(Mandatory)][string]$RunPath,[switch]$ResumeAfterWatch,[switch]$Interactive)
$ErrorActionPreference='Stop'
$requestedInteractive=$Interactive.IsPresent
$control=Join-Path $PSScriptRoot 'Control.ps1'
function Step([string]$Action,[string]$Name,[string]$Value='',[string]$Payload='{}') {
    Write-Host "$Action $Name"
    & $control -RunPath $RunPath -Action $Action -Name $Name -Value $Value -Payload $Payload -Interactive:($requestedInteractive -and $Action -eq 'Watch') | Out-Null
}
function Snap([string]$Name) {Get-Content -LiteralPath (Join-Path $RunPath ('snapshot-'+$Name+'.json')) -Raw|ConvertFrom-Json}
function Command([string]$Name,[string]$Value) {Step Console $Name $Value}
function Meter([string]$Name) {Step Meter $Name}
function MoveTV([string]$Hex,[double[]]$Point,[string]$Name) {
    # The fixture's native emitter is 0.143m above the placed reference origin.
    foreach($axis in 0..2) {
        $v=$Point[$axis]; if($axis -eq 2){$v-=0.143}
        Command ($Name+'-'+@('x','y','z')[$axis]) ($Hex+'.setpos '+@('x','y','z')[$axis]+' '+$v.ToString('R',[cultureinfo]::InvariantCulture))
    }
}
Step Snapshot preflight
$pre=Snap preflight
if($pre.audio.chrome.state -ne 'capturing'){throw 'Run Setup successfully before this scenario.'}
if(-not $ResumeAfterWatch) {
    Step Tone stop-background stop
    Meter quiet-background
    Step Tone start-reference start
    Command free-camera-on 'tfc'
    Meter one-tv
    Step Watch watch
    Meter fullscreen
    Step Browser close-watch close
    Meter after-close
}

if($ResumeAfterWatch){Command free-camera-on 'tfc'}
Step Snapshot free-camera-before
$fixed=Snap free-camera-before
$origin=$fixed.game.position
Command free-camera-move-player ('player.setpos x '+([double]$origin.x+10).ToString('R',[cultureinfo]::InvariantCulture))
Step Snapshot free-camera-player-moved
Command free-camera-restore-player ('player.setpos x '+([double]$origin.x).ToString('R',[cultureinfo]::InvariantCulture))

$gain=[double]$fixed.audio.chrome.spatial.gain
Meter fixed-master-full
Command half-master ('setini "fAudioVolumeMaster:Audio" '+($gain/2).ToString('R',[cultureinfo]::InvariantCulture))
Meter fixed-master-half
Command restore-master ('setini "fAudioVolumeMaster:Audio" '+$gain.ToString('R',[cultureinfo]::InvariantCulture))
Meter fixed-master-restored

$tv=Get-Content -LiteralPath (Join-Path $RunPath 'tv.json') -Raw|ConvertFrom-Json
$scene=$fixed.audio.chrome.spatial
$f=$scene.front; $u=$scene.up; $p=$scene.listener
$right=@(($f[1]*$u[2]-$f[2]*$u[1]),($f[2]*$u[0]-$f[0]*$u[2]),($f[0]*$u[1]-$f[1]*$u[0]))
$leftPoint=@(0..2|ForEach-Object {$p[$_]-3*$right[$_]})
$rightPoint=@(0..2|ForEach-Object {$p[$_]+3*$right[$_]})
MoveTV $tv.hex $leftPoint left-pose
Meter tv-left
MoveTV $tv.hex $rightPoint right-pose
Meter tv-right
MoveTV $tv.hex @(0..2|ForEach-Object {$p[$_]+15*$f[$_]}) far-pose
Meter distant
MoveTV $tv.hex @(0..2|ForEach-Object {$p[$_]+25*$f[$_]}) range-pose
Meter out-of-range
MoveTV $tv.hex $leftPoint near-pose
Step Browser mute mute '{"muted":true}'
Meter muted
Step Browser unmute mute '{"muted":false}'
Meter unmuted

Step SpawnTwo spawn-two
$tv2=Get-Content -LiteralPath (Join-Path $RunPath 'tv2.json') -Raw|ConvertFrom-Json
MoveTV $tv2.hex $rightPoint second-pose
Meter two-tvs
Command disable-first ($tv.hex+'.disable')
Meter remaining-tv
Command disable-second ($tv2.hex+'.disable')
Meter no-tvs
Command enable-first ($tv.hex+'.enable')
Command enable-second ($tv2.hex+'.enable')
Meter restored-tvs
Command free-camera-off 'tfc'
Step Snapshot tone-scenario-complete
