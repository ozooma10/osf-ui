param([Parameter(Mandatory)][string]$RunPath,[ValidateSet('passed','failed','blocked')][string]$Outcome='passed',[string]$Message='World screen checks complete',[switch]$RestoreDefault)
$ErrorActionPreference='Stop'
$c=Get-Content (Join-Path $RunPath 'context.json') -Raw|ConvertFrom-Json -AsHashtable
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repo 'extensions/world-screens/tests/game/Context.ps1') -RunPath $c.run.path
$script:ValidationBackend=$c.backend
. (Join-Path $PSScriptRoot 'Profile.ps1')
$lock=[IO.File]::Open((Join-Path $c.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
try {
    $game=Get-OwnedGame
    if(-not $game){throw 'Owned game already exited; use Restore.ps1 for browser profile cleanup.'}
    Write-JsonFile (Join-Path $script:Run.path 'final-native-state.json') @{game=(Get-GameState);page=(Read-WorldPage 'osfui.browser/tv')}
    $receipt=Invoke-ConsoleCommand 'qqq'
    Write-JsonFile (Join-Path $script:Run.path 'normal-exit.json') $receipt
    if(-not $game.WaitForExit(15000)){throw 'Game did not exit normally'}
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while((Get-Process osfui_webview2_host,osfui_cef_host -ErrorAction SilentlyContinue) -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 200}
    Exit-AudioProfile
    $failedCases=@(Get-ChildItem -LiteralPath $script:Run.path -Filter 'case-*.json' | ForEach-Object {
        $caseRecord=Read-JsonFile $_.FullName
        if(-not $caseRecord.passed){$caseRecord.name}
    })
    if($Outcome -eq 'passed' -and $failedCases.Count){
        $Outcome='failed'
        $Message+=' Retained failed case attempts: '+($failedCases -join ', ')+'. Assess accepted cases separately.'
    }
    Finish-RunArtifacts $Outcome $Message
    if($RestoreDefault){
        $moProcesses=@(Get-Process ModOrganizer -ErrorAction SilentlyContinue)
        foreach($mo in $moProcesses){if($mo.Path -ne (Join-Path $script:Config.MO2 'ModOrganizer.exe')){throw 'Unexpected MO2'}}
        foreach($mo in $moProcesses){
            $null=$mo.CloseMainWindow(); if(-not $mo.WaitForExit(10000)){throw 'MO2 did not close normally'}
        }
        Start-Process (Join-Path $script:Config.MO2 'ModOrganizer.exe') -ArgumentList '-p "Default"' -WindowStyle Hidden|Out-Null
    }
}finally{$lock.Dispose()}
