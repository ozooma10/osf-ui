#requires -Version 7.2
#requires -RunAsAdministrator
<#
.SYNOPSIS
  Capture one validated OSF UI, Carbon UI, or no-framework benchmark run.

.DESCRIPTION
  Adds comparison metadata to the existing profiler and verifies the expected
  framework DLLs in the running Starfield process before recording. Run each
  framework in a separate MO2 profile/launch; never load OSF UI and Carbon UI
  together for a comparison capture.

.EXAMPLE
  .\Capture-UIBench.ps1 -Framework OSFUI -Scenario loaded-hidden -Repeat 1 `
    -Resolution 2560x1440 -FrameGeneration Off -FrameRateMode Fixed60 `
    -RenderPreset Ultra-DLSSQuality
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('OSFUI', 'CarbonUI', 'Baseline')]
    [string] $Framework,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')]
    [string] $Scenario,

    [ValidateRange(1, 1000)]
    [int] $Repeat = 1,

    [Parameter(Mandatory)]
    [ValidatePattern('^\d{3,5}x\d{3,5}$')]
    [string] $Resolution,

    [Parameter(Mandatory)]
    [ValidateSet('Off', 'On')]
    [string] $FrameGeneration,

    [Parameter(Mandatory)]
    [ValidateSet('Fixed60', 'Fixed120', 'Uncapped')]
    [string] $FrameRateMode,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string] $RenderPreset,

    [ValidateSet('FrameworkDefault', 'PixelMatched')]
    [string] $RasterizationPolicy = 'FrameworkDefault',

    [ValidateRange(5, 86400)]
    [int] $DurationSeconds = 60,

    [ValidateRange(1, 30)]
    [int] $IntervalSeconds = 1,

    [ValidateRange(0, 30)]
    [int] $CountdownSeconds = 5,

    [ValidateSet('CpuGpu', 'General', 'None')]
    [string] $WprProfile = 'CpuGpu',

    [ValidateRange(0, [int]::MaxValue)]
    [int] $GamePid = 0,

    [string] $PresentMonPath,

    [string] $FixtureTelemetryPath,

    [string] $OutputRoot,

    [string] $Notes = '',

    [switch] $NoHardwareTelemetry,

    [switch] $OpenWpa
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-ToSafePathPart([string] $Value)
{
    $safe = ($Value -replace '[^A-Za-z0-9._-]', '-').Trim('-')
    if (-not $safe) { throw "Value cannot be converted to a safe path component: '$Value'" }
    return $safe
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$safeFramework = Convert-ToSafePathPart $Framework
$safeScenario = Convert-ToSafePathPart $Scenario
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $repo "build\profiles\ui-bench\runs\$safeFramework\$safeScenario"
}

$label = "$safeScenario-$safeFramework-r$Repeat"
$capture = Join-Path $PSScriptRoot 'Capture-OSFUI.ps1'
$captureParams = @{
    Label = $label
    Framework = $Framework
    Scenario = $Scenario
    Repeat = $Repeat
    Resolution = $Resolution
    FrameGeneration = $FrameGeneration
    FrameRateMode = $FrameRateMode
    RenderPreset = $RenderPreset
    RasterizationPolicy = $RasterizationPolicy
    DurationSeconds = $DurationSeconds
    IntervalSeconds = $IntervalSeconds
    CountdownSeconds = $CountdownSeconds
    WprProfile = $WprProfile
    GamePid = $GamePid
    OutputRoot = $OutputRoot
    Notes = $Notes
    ValidateFrameworkState = $true
}
if ($PresentMonPath) { $captureParams.PresentMonPath = $PresentMonPath }
if ($FixtureTelemetryPath) { $captureParams.FixtureTelemetryPath = $FixtureTelemetryPath }
$controlledScenarios = @('static', 'transforms', 'repaint', 'layout', 'text-scroll', 'canvas')
if ($Scenario -in $controlledScenarios) {
    $captureParams.RequireFixtureModule = $true
    if ($Framework -ne 'Baseline') {
        $captureParams.RequireFixture = $true
    }
}
if ($NoHardwareTelemetry) { $captureParams.NoHardwareTelemetry = $true }
if ($OpenWpa) { $captureParams.OpenWpa = $true }

& $capture @captureParams
