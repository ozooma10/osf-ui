[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('webview2','cef')][string]$Backend,
    [Parameter(Mandatory)][ValidateSet('baseline','current')][string]$Variant,
    [Parameter(Mandatory)][string]$PayloadData,
    [Parameter(Mandatory)][string]$Experiment,
    [ValidateRange(1,97)][int]$FirstRepeat=1,
    [switch]$ToneFirst,
    [switch]$ValidateControls
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$payload=[IO.Path]::GetFullPath($PayloadData)
# Start refuses an existing game and takes the shared runner lock. Never stop a
# foreign session or silently reuse a partially measured one.
& (Join-Path $PSScriptRoot 'Start.ps1') -Backend $Backend -CoreData $payload -ExtensionData $payload -Label $Variant
$context=Get-Content -LiteralPath (Join-Path $repo 'build/world-screen-validation/active.json') -Raw|ConvertFrom-Json
$run=$context.run.path
@{experiment=$Experiment;backend=$Backend;variant=$Variant;payload=$payload;
  firstRepeat=$FirstRepeat;toneFirst=$ToneFirst.IsPresent;warmupSeconds=120;
  sourceCommit=(& git -C $repo rev-parse HEAD);startedUtc=[DateTime]::UtcNow.ToString('o')
}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $run 'experiment.json') -Encoding utf8NoBOM
function Case([string]$Action,[string]$Name,[string]$Value='',[int]$Duration=3000){
    & (Join-Path $PSScriptRoot 'Cases.ps1') -RunPath $run -Case $Action -Name $Name -Value $Value -DurationMs $Duration |
        Out-File -LiteralPath (Join-Path $run ($Name+'-output.txt'))
}
try {
    Case Setup setup
    Case SpawnTwo spawntwo
    & (Join-Path $PSScriptRoot 'Focus.ps1') -RunPath $run -AllowForeground
    if(-not $ToneFirst){Case Tone stop-before-warmup stop}
    foreach($step in 1..4){Start-Sleep -Seconds 30;Write-Output "Warmup $($step*30)/120 seconds: $Experiment $Backend $Variant"}
    $workloads=if($ToneFirst){@('two-tv-tone','home-idle-tone-off')}else{@('home-idle-tone-off','two-tv-tone')}
    foreach($workload in $workloads){
        $tone=if($workload -eq 'two-tv-tone'){'start'}else{'stop'}
        Case Tone ('tone-for-'+$workload) $tone
        Start-Sleep -Seconds 10
        foreach($repeat in $FirstRepeat..($FirstRepeat+2)){
            & (Join-Path $PSScriptRoot 'Focus.ps1') -RunPath $run -AllowForeground
            & (Join-Path $PSScriptRoot 'Measure.ps1') -RunPath $run -Workload $workload -Variant $Variant -Repeat $repeat -DurationSeconds 30 -NoPresentMon -Notes $Experiment
        }
    }
    if($ValidateControls){
        Case Tone tone-for-checks start
        foreach($case in @('MultipleTVs','SaveLoad','Navigation','Video')){Case $case $case.ToLowerInvariant()}
        Case Browser home-after-checks home
        Case Tone tone-after-checks start
        Case Meter sustained-audio '' 30000
    }
    & (Join-Path $PSScriptRoot 'Finish.ps1') -RunPath $run -Outcome passed -RestoreDefault -Message "$Experiment $Backend $Variant captures complete."
} catch {
    $failure=$_
    # Preserve the failed capture and close only the session this script created.
    # If normal cleanup fails, leave its ownership records for explicit recovery.
    try {& (Join-Path $PSScriptRoot 'Finish.ps1') -RunPath $run -Outcome failed -RestoreDefault -Message $failure.Exception.Message}
    catch {Write-Warning "Normal cleanup failed; retain $run and inspect ownership before further work: $_"}
    throw $failure
}
Write-Output "Completed $run"
