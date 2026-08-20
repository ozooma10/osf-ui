#requires -Version 7.2
<#
.SYNOPSIS
  Generate the complete controlled UIBench capture matrix.
#>
[CmdletBinding()]
param(
    [string[]] $Resolutions = @('2560x1440'),
    [ValidateSet('Fixed60', 'Fixed120', 'Uncapped')]
    [string[]] $FrameRateModes = @('Fixed60'),
    [ValidateSet('static', 'transforms', 'repaint', 'layout', 'text-scroll', 'canvas')]
    [string[]] $Scenarios = @('static', 'layout', 'canvas'),
    [ValidateRange(1, 20)]
    [int] $Repeats = 3,
    [ValidateSet('Off', 'On')]
    [string] $FrameGeneration = 'Off',
    [string] $RenderPreset = 'Ultra-DLSSQuality',
    [ValidateSet('FrameworkDefault', 'PixelMatched')]
    [string] $RasterizationPolicy = 'FrameworkDefault',
    [string] $OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $OutputPath) { $OutputPath = Join-Path $repo 'build\profiles\ui-bench\matrix.csv' }
New-Item -ItemType Directory -Force -Path (Split-Path $OutputPath -Parent) | Out-Null

$orders = @(
    @('Baseline', 'OSFUI', 'CarbonUI'),
    @('OSFUI', 'CarbonUI', 'Baseline'),
    @('CarbonUI', 'Baseline', 'OSFUI')
)
$rows = [Collections.Generic.List[object]]::new()
$id = 0
foreach ($resolution in $Resolutions) {
    if ($resolution -notmatch '^\d{3,5}x\d{3,5}$') { throw "Invalid resolution: $resolution" }
    foreach ($rate in $FrameRateModes) {
        foreach ($scenario in $Scenarios) {
            for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
                $order = $orders[($repeat - 1) % $orders.Count]
                foreach ($framework in $order) {
                    ++$id
                    $rows.Add([pscustomobject][ordered]@{
                        Id = $id
                        Status = 'Pending'
                        Framework = $framework
                        Scenario = $scenario
                        Repeat = $repeat
                        Resolution = $resolution
                        FrameRateMode = $rate
                        FrameGeneration = $FrameGeneration
                        RenderPreset = $RenderPreset
                        RasterizationPolicy = $RasterizationPolicy
                        CaptureCommand = "pwsh -NoProfile -File .\tools\profiling\Capture-UIBench.ps1 -Framework $framework -Scenario $scenario -Repeat $repeat -Resolution $resolution -FrameGeneration $FrameGeneration -FrameRateMode $rate -RenderPreset $RenderPreset -RasterizationPolicy $RasterizationPolicy -DurationSeconds 60 -WprProfile None"
                    })
                }
            }
        }
    }
}
$rows | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding utf8NoBOM
Write-Host "Matrix: $OutputPath" -ForegroundColor Green
Write-Host "$($rows.Count) captures ($($Resolutions.Count) resolutions x $($FrameRateModes.Count) rate modes x $($Scenarios.Count) workloads x $Repeats repeats x 3 framework states)."
