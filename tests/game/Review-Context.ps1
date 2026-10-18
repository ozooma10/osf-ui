# Dot-source only while Test-Backend -RealMods is waiting for explicit review.
param([Parameter(Mandatory)][string]$RunPath,[switch]$Interactive)
if(-not $Interactive){throw 'Review input requires -Interactive.'}
$ErrorActionPreference='Stop'
$reviewRepo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$reviewRoot=[IO.Path]::GetFullPath((Join-Path $reviewRepo 'build\backend-validation\game'))+'\'
$RunPath=[IO.Path]::GetFullPath($RunPath)
if(-not $RunPath.StartsWith($reviewRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Review must target a private packaged game run.'}
$review=Get-Content (Join-Path $RunPath 'review-ready.json') -Raw | ConvertFrom-Json -AsHashtable
. (Join-Path $review.harnessRoot 'scripts\Common.ps1')
$script:Run=@{path=$RunPath}
$script:StateRoot=Join-Path $RunPath 'state'
$script:ProbeDir=Join-Path $RunPath 'probe'
$script:ProbeSession=$review.session
$script:Session=Read-JsonFile (Join-Path $script:StateRoot 'session.json')
$null=Get-OwnedGame
if(Test-Path (Join-Path $RunPath 'review-complete.json')){throw 'This review has already completed.'}
function Read-Consumer {
    $value=Read-JsonFile (Join-Path $script:ProbeDir 'consumer.json')
    if($value -and $value.session -eq $script:ProbeSession -and [Environment]::TickCount64-$value.tickMs -lt 3000){return $value}
}
function Send-Probe([string]$Action,[hashtable]$Values=@{}) {
    $id=[guid]::NewGuid().ToString('N')
    $command=@{id=$id;session=$script:ProbeSession;action=$Action;deadline=([Environment]::TickCount64+10000)}
    foreach($key in $Values.Keys){$command[$key]=$Values[$key]}
    Write-JsonFile (Join-Path $script:ProbeDir 'command.json') $command
    $reply=Wait-Observed "Public API $Action returned" {Read-JsonFile (Join-Path $script:ProbeDir 'reply.json')} {param($r)$r -and $r.id -eq $id -and $r.session -eq $script:ProbeSession} 10
    Assert-Test $reply.accepted "Public API accepted $Action" $command
}
function Click-Review([double]$X,[double]$Y) {
    $window=Invoke-Input status
    $null=Invoke-Input click @('--x',[string][int]($X*$window.clientWidth),'--y',[string][int]($Y*$window.clientHeight))
}
