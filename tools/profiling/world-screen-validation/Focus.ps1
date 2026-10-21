# Opt-in foreground control only; never sends keyboard or mouse input.
param([Parameter(Mandatory)][string]$RunPath,[switch]$AllowForeground)
$ErrorActionPreference='Stop'
if(-not $AllowForeground){throw 'Focusing the test game requires explicit AllowForeground authorization.'}
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $RunPath
$lock=[IO.File]::Open((Join-Path $context.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
try {
    $script:AllowInteractive=$true
    $null=Get-GameState
    $null=Invoke-Input 'focus'
    $state=Wait-Observed 'Owned test game has foreground' {Get-GameState} {
        param($s) $s.foreground -and -not $s.snapshotStale -and $s.loaded
    } 10
    Write-JsonFile (Join-Path $script:Run.path 'foreground-authorization.json') @{
        scope='User-authorized focus only. No keyboard or mouse input.';atUtc=[DateTime]::UtcNow.ToString('o');state=$state
    }
}finally{$lock.Dispose()}
