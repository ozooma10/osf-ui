param(
    [Parameter(Mandatory)][string]$RunPath,
    [string]$Message='Game exited normally; restored the browser profile after failed audio acceptance.',
    [switch]$RestoreDefault
)
$ErrorActionPreference='Stop'
$c=Get-Content -LiteralPath (Join-Path $RunPath 'context.json') -Raw | ConvertFrom-Json -AsHashtable
$Interactive=$false
$Scenario='WebView2Audio'
# Context.ps1 requires a live owned game. Recovery deliberately loads only the
# shared definitions and saved context, because normal game exit already finished.
. (Join-Path $c.harness 'scripts/Common.ps1')
. (Join-Path $c.harness 'scripts/Environment.ps1')
. (Join-Path $c.harness 'scripts/Reports.ps1')
. (Join-Path $PSScriptRoot 'Profile.ps1')
$script:Run=$c.run
$script:StateRoot=$c.state
$script:ProfileRoot=$c.profile
$script:ProbeDir=$c.probe
$script:ProbeSession=$c.session
$script:Config.TestProfile=$c.testProfile
$script:HarnessModName=$c.testProfile
$script:AutoLoadModName=$c.testProfile
$script:SaveRoot=$c.save
$script:FixtureRoot=$c.fixture
$lock=[IO.File]::Open((Join-Path $c.harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
try {
    Exit-AudioProfile
    Finish-RunArtifacts failed $Message
    if($RestoreDefault){
        $processes=@(Get-Process ModOrganizer -ErrorAction SilentlyContinue)
        $expected=Join-Path $script:Config.MO2 'ModOrganizer.exe'
        foreach($mo in $processes){if($mo.Path -ne $expected){throw 'Unexpected MO2 process; leave all MO2 instances untouched.'}}
        foreach($mo in $processes){
            $null=$mo.CloseMainWindow()
            if(-not $mo.WaitForExit(10000)){throw 'MO2 did not close normally.'}
        }
        Start-Process -FilePath $expected -ArgumentList '-p "Default"' -WindowStyle Hidden | Out-Null
    }
}finally{$lock.Dispose()}
