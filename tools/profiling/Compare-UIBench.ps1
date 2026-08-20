#requires -Version 7.2
<#
.SYNOPSIS
  Compare repeated OSF UI, Carbon UI, and baseline benchmark captures.

.DESCRIPTION
  Finds completed UI-bench summaries, keeps the newest capture for a duplicate
  framework/scenario/repeat identity, computes the median of each metric across
  repeats, and writes Markdown plus machine-readable JSON. Captures are only
  compared when scenario, resolution, frame-rate mode, frame-generation state,
  and render preset match exactly.
#>
[CmdletBinding()]
param(
    [string] $InputRoot,

    [string] $OutputPath,

    [string] $Scenario,

    [string] $Resolution,

    [ValidateSet('Off', 'On')]
    [string] $FrameGeneration,

    [ValidateSet('Fixed60', 'Fixed120', 'Uncapped')]
    [string] $FrameRateMode,

    [string] $RenderPreset,

    [ValidateSet('FrameworkDefault', 'PixelMatched')]
    [string] $RasterizationPolicy,

    [ValidateRange(1, 1000)]
    [int] $MinimumRepeats = 3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PathValue($Object, [string[]] $Path)
{
    $current = $Object
    foreach ($segment in $Path) {
        if ($null -eq $current) { return $null }
        $property = $current.PSObject.Properties[$segment]
        if ($null -eq $property) { return $null }
        $current = $property.Value
    }
    return $current
}

function Convert-ToNumber($Value)
{
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    $parsed = 0.0
    if ([double]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Get-Median($Values)
{
    $numbers = [double[]]@($Values | ForEach-Object { Convert-ToNumber $_ } |
        Where-Object { $null -ne $_ -and [double]::IsFinite($_) } | Sort-Object)
    if ($numbers.Count -eq 0) { return $null }
    $middle = [math]::Floor($numbers.Count / 2)
    if (($numbers.Count % 2) -eq 1) { return [double]$numbers[$middle] }
    return ([double]$numbers[$middle - 1] + [double]$numbers[$middle]) / 2.0
}

function Get-Mad($Values)
{
    $median = Get-Median $Values
    if ($null -eq $median) { return $null }
    return Get-Median @($Values | ForEach-Object {
        $number = Convert-ToNumber $_
        if ($null -ne $number) { [math]::Abs($number - $median) }
    })
}

function Format-Number($Value, [int] $Decimals = 2)
{
    if ($null -eq $Value) { return 'n/a' }
    return ([double]$Value).ToString("N$Decimals", [Globalization.CultureInfo]::InvariantCulture)
}

function Format-Delta($Value, [int] $Decimals = 2)
{
    if ($null -eq $Value) { return 'n/a' }
    return ([double]$Value).ToString("+0.$('0' * $Decimals);-0.$('0' * $Decimals);0.$('0' * $Decimals)", [Globalization.CultureInfo]::InvariantCulture)
}

function Get-Difference($Left, $Right)
{
    if ($null -eq $Left -or $null -eq $Right) { return $null }
    return [double]$Left - [double]$Right
}

function Get-Quotient($Numerator, $Denominator)
{
    if ($null -eq $Numerator -or $null -eq $Denominator -or [double]$Denominator -eq 0) { return $null }
    return [double]$Numerator / [double]$Denominator
}

function Convert-BytesToMiB($Value)
{
    $number = Convert-ToNumber $Value
    if ($null -eq $number) { return $null }
    return $number / 1MB
}

function Get-FrameStats($Summary)
{
    $metrics = Get-PathValue $Summary @('presentMon', 'application', 'metricsMilliseconds')
    if ($null -eq $metrics) {
        $metrics = Get-PathValue $Summary @('presentMon', 'metricsMilliseconds')
    }
    if ($null -eq $metrics) { return $null }
    foreach ($name in @('FrameTime', 'MsBetweenPresents')) {
        $property = $metrics.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Get-MetricMedians($Runs)
{
    return [ordered]@{
        systemCpuMeanPercent = Get-Median @($Runs | ForEach-Object SystemCpuMeanPercent)
        trackedCpuMeanPercent = Get-Median @($Runs | ForEach-Object TrackedCpuMeanPercent)
        trackedWorkingSetMaxMiB = Get-Median @($Runs | ForEach-Object TrackedWorkingSetMaxMiB)
        trackedPrivateMaxMiB = Get-Median @($Runs | ForEach-Object TrackedPrivateMaxMiB)
        trackedDedicatedVramMaxMiB = Get-Median @($Runs | ForEach-Object TrackedDedicatedVramMaxMiB)
        adapterGpuMeanPercent = Get-Median @($Runs | ForEach-Object AdapterGpuMeanPercent)
        adapterPowerMeanWatts = Get-Median @($Runs | ForEach-Object AdapterPowerMeanWatts)
        adapterVramMaxMiB = Get-Median @($Runs | ForEach-Object AdapterVramMaxMiB)
        averageFps = Get-Median @($Runs | ForEach-Object AverageFps)
        displayedFps = Get-Median @($Runs | ForEach-Object DisplayedFps)
        onePercentLowFps = Get-Median @($Runs | ForEach-Object OnePercentLowFps)
        generatedFrameRows = Get-Median @($Runs | ForEach-Object GeneratedFrameRows)
        frameP95Milliseconds = Get-Median @($Runs | ForEach-Object FrameP95Milliseconds)
        frameP99Milliseconds = Get-Median @($Runs | ForEach-Object FrameP99Milliseconds)
        gpuTimeP95Milliseconds = Get-Median @($Runs | ForEach-Object GpuTimeP95Milliseconds)
        trackedCpuCoreMsPerSecond = Get-Median @($Runs | ForEach-Object TrackedCpuCoreMsPerSecond)
        trackedCpuCoreMsPerFrame = Get-Median @($Runs | ForEach-Object TrackedCpuCoreMsPerFrame)
        trackedCpuCoreMsPerFramePerMegapixel = Get-Median @($Runs | ForEach-Object TrackedCpuCoreMsPerFramePerMegapixel)
        fixtureRafFps = Get-Median @($Runs | ForEach-Object FixtureRafFps)
    }
}

function Get-PairedDeltaSummary($FrameworkRuns, $BaselineRuns, [double] $Megapixels)
{
    $pairs = [Collections.Generic.List[object]]::new()
    foreach ($frameworkRun in @($FrameworkRuns)) {
        $baseline = @($BaselineRuns | Where-Object Repeat -eq $frameworkRun.Repeat |
            Select-Object -First 1)
        if (-not $baseline.Count) { continue }
        $baseline = $baseline[0]
        $coreMsPerSecond = Get-Difference $frameworkRun.TrackedCpuCoreMsPerSecond $baseline.TrackedCpuCoreMsPerSecond
        $coreMsPerFrame = Get-Quotient $coreMsPerSecond $frameworkRun.AverageFps
        $coreMsPerUiUpdate = Get-Quotient $coreMsPerSecond $frameworkRun.FixtureRafFps
        $pairs.Add([pscustomobject][ordered]@{
            repeat = $frameworkRun.Repeat
            systemCpuMeanPercent = Get-Difference $frameworkRun.SystemCpuMeanPercent $baseline.SystemCpuMeanPercent
            trackedCpuMeanPercent = Get-Difference $frameworkRun.TrackedCpuMeanPercent $baseline.TrackedCpuMeanPercent
            trackedPrivateMaxMiB = Get-Difference $frameworkRun.TrackedPrivateMaxMiB $baseline.TrackedPrivateMaxMiB
            trackedDedicatedVramMaxMiB = Get-Difference $frameworkRun.TrackedDedicatedVramMaxMiB $baseline.TrackedDedicatedVramMaxMiB
            adapterGpuMeanPercent = Get-Difference $frameworkRun.AdapterGpuMeanPercent $baseline.AdapterGpuMeanPercent
            adapterPowerMeanWatts = Get-Difference $frameworkRun.AdapterPowerMeanWatts $baseline.AdapterPowerMeanWatts
            adapterVramMaxMiB = Get-Difference $frameworkRun.AdapterVramMaxMiB $baseline.AdapterVramMaxMiB
            frameP95Milliseconds = Get-Difference $frameworkRun.FrameP95Milliseconds $baseline.FrameP95Milliseconds
            frameP99Milliseconds = Get-Difference $frameworkRun.FrameP99Milliseconds $baseline.FrameP99Milliseconds
            trackedCpuCoreMsPerSecond = $coreMsPerSecond
            trackedCpuCoreMsPerApplicationFrame = $coreMsPerFrame
            trackedCpuCoreMsPerFramePerMegapixel = Get-Quotient $coreMsPerFrame $Megapixels
            trackedCpuCoreMsPerUiUpdate = $coreMsPerUiUpdate
            trackedCpuCoreMsPerUiUpdatePerMegapixel = Get-Quotient $coreMsPerUiUpdate $Megapixels
        })
    }
    if (-not $pairs.Count) { return $null }

    $result = [ordered]@{
        pairedRunCount = $pairs.Count
        repeats = @($pairs.repeat)
    }
    foreach ($metric in @(
        'systemCpuMeanPercent', 'trackedCpuMeanPercent', 'trackedPrivateMaxMiB',
        'trackedDedicatedVramMaxMiB', 'adapterGpuMeanPercent', 'adapterPowerMeanWatts',
        'adapterVramMaxMiB', 'frameP95Milliseconds', 'frameP99Milliseconds',
        'trackedCpuCoreMsPerSecond', 'trackedCpuCoreMsPerApplicationFrame',
        'trackedCpuCoreMsPerFramePerMegapixel', 'trackedCpuCoreMsPerUiUpdate',
        'trackedCpuCoreMsPerUiUpdatePerMegapixel')) {
        $values = @($pairs | ForEach-Object { $_.$metric })
        $result[$metric] = Get-Median $values
        $result["${metric}Mad"] = Get-Mad $values
    }
    return $result
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $InputRoot) { $InputRoot = Join-Path $repo 'build\profiles\ui-bench' }
if (-not (Test-Path -LiteralPath $InputRoot -PathType Container)) {
    throw "UI benchmark input root not found: $InputRoot"
}
$InputRoot = (Resolve-Path -LiteralPath $InputRoot).Path
if (-not $OutputPath) { $OutputPath = Join-Path $InputRoot 'comparison.md' }
$outputDirectory = Split-Path $OutputPath -Parent
if (-not $outputDirectory) { $outputDirectory = (Get-Location).Path }
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$jsonPath = [IO.Path]::ChangeExtension($OutputPath, '.json')

$warnings = [Collections.Generic.List[string]]::new()
$candidates = [Collections.Generic.List[object]]::new()
$summaryFiles = @(Get-ChildItem -LiteralPath $InputRoot -Filter 'summary.json' -File -Recurse)
foreach ($file in $summaryFiles) {
    $summary = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    $manifest = $summary.manifest
    $benchmark = Get-PathValue $manifest @('benchmark')
    if ($null -eq $benchmark) { continue }
    if ([string]$manifest.status -ne 'complete') {
        $warnings.Add("Ignored incomplete capture: $($file.DirectoryName)")
        continue
    }

    $framework = [string](Get-PathValue $manifest @('framework'))
    $runScenario = [string](Get-PathValue $benchmark @('scenario'))
    $runResolution = [string](Get-PathValue $benchmark @('resolution'))
    $runFrameGeneration = [string](Get-PathValue $benchmark @('frameGeneration'))
    $runFrameRateMode = [string](Get-PathValue $benchmark @('frameRateMode'))
    if (-not $runFrameRateMode) { $runFrameRateMode = 'Unknown' }
    $runRenderPreset = [string](Get-PathValue $benchmark @('renderPreset'))
    $runRasterizationPolicy = [string](Get-PathValue $benchmark @('rasterizationPolicy'))
    if (-not $runRasterizationPolicy) { $runRasterizationPolicy = 'FrameworkDefault' }
    $repeat = [int](Get-PathValue $benchmark @('repeat'))
    if ($Scenario -and $runScenario -ne $Scenario) { continue }
    if ($Resolution -and $runResolution -ne $Resolution) { continue }
    if ($FrameGeneration -and $runFrameGeneration -ne $FrameGeneration) { continue }
    if ($FrameRateMode -and $runFrameRateMode -ne $FrameRateMode) { continue }
    if ($RenderPreset -and $runRenderPreset -ne $RenderPreset) { continue }
    if ($RasterizationPolicy -and $runRasterizationPolicy -ne $RasterizationPolicy) { continue }
    $frameRateValid = Get-PathValue $summary @('frameRateValidation', 'valid')
    if ($null -ne $frameRateValid -and -not [bool]$frameRateValid) {
        $warnings.Add("Ignored fixed-rate capture outside tolerance: $($file.DirectoryName)")
        continue
    }

    $hardware = @(Get-PathValue $summary @('hardware'))
    $adapter = @($hardware | Where-Object { $null -ne (Get-PathValue $_ @('gpuUtilizationPercent', 'mean')) } |
        Sort-Object { [double](Get-PathValue $_ @('gpuUtilizationPercent', 'mean')) } -Descending |
        Select-Object -First 1)
    $adapter = if ($adapter.Count) { $adapter[0] } else { $null }
    $frameStats = Get-FrameStats $summary
    $gameVersion = [string](Get-PathValue $manifest @('gameExecutable', 'fileVersion'))
    $machineName = [string](Get-PathValue $manifest @('machine', 'computerName'))
    $osVersion = [string](Get-PathValue $manifest @('machine', 'osVersion'))
    $displayAdapters = @((Get-PathValue $manifest @('machine', 'displayAdapters')) | ForEach-Object {
        "$(Get-PathValue $_ @('name'))@$(Get-PathValue $_ @('driverVersion'))"
    }) -join ','
    $presentMonHash = [string](Get-PathValue $manifest @('presentMonIdentity', 'sha256'))
    $displayMode = Get-PathValue $benchmark @('observedDisplayMode')
    $displayModeSignature = if ($displayMode) {
        "$(Get-PathValue $displayMode @('deviceName'))@$(Get-PathValue $displayMode @('width'))x$(Get-PathValue $displayMode @('height'))@$(Get-PathValue $displayMode @('refreshHz'))Hz"
    } else { 'unrecorded' }
    $environmentSignature = "$machineName|$osVersion|$gameVersion|$displayAdapters|Display=$displayModeSignature|PresentMon=$presentMonHash"
    $frameworkModuleSignature = @((Get-PathValue $manifest @('loadedUiModules')) | ForEach-Object {
        "$(Get-PathValue $_ @('name'))@$(Get-PathValue $_ @('fileVersion'))@$(Get-PathValue $_ @('productVersion'))@$(Get-PathValue $_ @('moduleMemorySize'))@$(Get-PathValue $_ @('sha256'))"
    } | Sort-Object) -join ','
    $conditionKey = "$runScenario|$runResolution|$runFrameGeneration|$runFrameRateMode|$runRenderPreset|$runRasterizationPolicy"
    $identityKey = "$conditionKey|$framework|$repeat"
    $applicationFps = Get-PathValue $summary @('presentMon', 'application', 'averageFps')
    if ($null -eq $applicationFps) { $applicationFps = Get-PathValue $summary @('presentMon', 'averageFps') }
    $applicationLowFps = Get-PathValue $summary @('presentMon', 'application', 'onePercentLowFps')
    if ($null -eq $applicationLowFps) { $applicationLowFps = Get-PathValue $summary @('presentMon', 'onePercentLowFps') }
    $gpuTimeP95 = Get-PathValue $summary @('presentMon', 'application', 'metricsMilliseconds', 'GPUTime', 'p95')
    if ($null -eq $gpuTimeP95) { $gpuTimeP95 = Get-PathValue $summary @('presentMon', 'metricsMilliseconds', 'GPUTime', 'p95') }

    $candidates.Add([pscustomobject]@{
        IdentityKey = $identityKey
        ConditionKey = $conditionKey
        Framework = $framework
        Scenario = $runScenario
        Repeat = $repeat
        Resolution = $runResolution
        FrameGeneration = $runFrameGeneration
        FrameRateMode = $runFrameRateMode
        RenderPreset = $runRenderPreset
        RasterizationPolicy = $runRasterizationPolicy
        FixtureHash = [string](Get-PathValue $summary @('fixture', 'fixtureHash'))
        FixtureDevicePixelRatio = Get-PathValue $summary @('fixture', 'viewport', 'devicePixelRatio')
        FixtureRasterWidth = Get-PathValue $summary @('fixture', 'effectiveRaster', 'width')
        FixtureRasterHeight = Get-PathValue $summary @('fixture', 'effectiveRaster', 'height')
        FixtureRasterSignature = if ($null -ne (Get-PathValue $summary @('fixture', 'effectiveRaster', 'width'))) {
            "$(Get-PathValue $summary @('fixture', 'effectiveRaster', 'width'))x$(Get-PathValue $summary @('fixture', 'effectiveRaster', 'height'))"
        } else { $null }
        EnvironmentSignature = $environmentSignature
        FrameworkModuleSignature = $frameworkModuleSignature
        CompletedUtc = [datetime](Get-PathValue $manifest @('completedUtc'))
        CaptureDirectory = [string](Get-PathValue $summary @('captureDirectory'))
        SystemCpuMeanPercent = Get-PathValue $summary @('system', 'cpuPercent', 'mean')
        TrackedCpuMeanPercent = Get-PathValue $summary @('trackedProcesses', 'cpuPercentMachine', 'mean')
        TrackedWorkingSetMaxMiB = Convert-BytesToMiB (Get-PathValue $summary @('trackedProcesses', 'workingSetBytes', 'max'))
        TrackedPrivateMaxMiB = Convert-BytesToMiB (Get-PathValue $summary @('trackedProcesses', 'privateBytes', 'max'))
        TrackedDedicatedVramMaxMiB = Convert-BytesToMiB (Get-PathValue $summary @('trackedProcesses', 'dedicatedGpuBytes', 'max'))
        AdapterGpuMeanPercent = Get-PathValue $adapter @('gpuUtilizationPercent', 'mean')
        AdapterPowerMeanWatts = Get-PathValue $adapter @('powerWatts', 'mean')
        AdapterVramMaxMiB = Get-PathValue $adapter @('vramUsedMiB', 'max')
        AverageFps = $applicationFps
        DisplayedFps = Get-PathValue $summary @('presentMon', 'displayedAverageFps')
        OnePercentLowFps = $applicationLowFps
        GeneratedFrameRows = Get-PathValue $summary @('presentMon', 'generated', 'rows')
        FrameP95Milliseconds = Get-PathValue $frameStats @('p95')
        FrameP99Milliseconds = Get-PathValue $frameStats @('p99')
        GpuTimeP95Milliseconds = $gpuTimeP95
        TrackedCpuCoreMsPerSecond = Get-PathValue $summary @('derived', 'trackedCpuCoreMillisecondsPerSecond')
        TrackedCpuCoreMsPerFrame = Get-PathValue $summary @('derived', 'trackedCpuCoreMillisecondsPerApplicationFrame')
        TrackedCpuCoreMsPerFramePerMegapixel = Get-PathValue $summary @('derived', 'trackedCpuCoreMillisecondsPerFramePerMegapixel')
        FixtureRafFps = Get-PathValue $summary @('fixture', 'rafFps', 'mean')
    })
}

if ($candidates.Count -eq 0) {
    throw "No completed UI benchmark summaries matched under $InputRoot"
}

$runs = [Collections.Generic.List[object]]::new()
foreach ($group in ($candidates | Group-Object IdentityKey)) {
    $ordered = @($group.Group | Sort-Object CompletedUtc)
    if ($ordered.Count -gt 1) {
        $warnings.Add("Duplicate repeat '$($group.Name)': kept newest of $($ordered.Count) captures.")
    }
    $runs.Add($ordered[-1])
}

$conditions = [Collections.Generic.List[object]]::new()
foreach ($conditionGroup in ($runs | Group-Object ConditionKey | Sort-Object Name)) {
    $conditionRuns = @($conditionGroup.Group)
    $environments = @($conditionRuns.EnvironmentSignature | Sort-Object -Unique)
    if ($environments.Count -ne 1) {
        throw "Condition '$($conditionGroup.Name)' mixes machines, game versions, or display drivers. Split the input set before comparing."
    }
    $fixtureHashes = @($conditionRuns | Where-Object Framework -ne 'Baseline' |
        ForEach-Object FixtureHash | Where-Object { $_ } | Sort-Object -Unique)
    if ($fixtureHashes.Count -gt 1) {
        throw "Condition '$($conditionGroup.Name)' mixes different fixture documents across OSF UI and Carbon UI."
    }
    if ($conditionRuns[0].Scenario -in @('static', 'transforms', 'repaint', 'layout', 'text-scroll', 'canvas') -and
        $fixtureHashes.Count -eq 0) {
        throw "Condition '$($conditionGroup.Name)' has no fixture SHA-256; identical-document provenance is required for controlled workloads."
    }
    $missingFixtureRuns = @($conditionRuns | Where-Object {
        $_.Framework -ne 'Baseline' -and -not $_.FixtureHash
    })
    if ($conditionRuns[0].Scenario -in @('static', 'transforms', 'repaint', 'layout', 'text-scroll', 'canvas') -and
        $missingFixtureRuns.Count) {
        throw "Condition '$($conditionGroup.Name)' has $($missingFixtureRuns.Count) framework capture(s) without fixture provenance."
    }

    $frameworks = [ordered]@{}
    foreach ($frameworkGroup in ($conditionRuns | Group-Object Framework | Sort-Object Name)) {
        $frameworkRuns = @($frameworkGroup.Group | Sort-Object Repeat)
        $moduleSignatures = @($frameworkRuns.FrameworkModuleSignature | Sort-Object -Unique)
        if ($moduleSignatures.Count -ne 1) {
            throw "Condition '$($conditionGroup.Name)' mixes $($frameworkGroup.Name) framework builds across repeats."
        }
        $frameworks[$frameworkGroup.Name] = [ordered]@{
            runCount = $frameworkRuns.Count
            repeats = @($frameworkRuns.Repeat)
            captureDirectories = @($frameworkRuns.CaptureDirectory)
            fixtureRasterSizes = @($frameworkRuns.FixtureRasterSignature | Where-Object { $_ } | Sort-Object -Unique)
            fixtureDevicePixelRatios = @($frameworkRuns.FixtureDevicePixelRatio | Where-Object { $null -ne $_ } | Sort-Object -Unique)
            metrics = Get-MetricMedians $frameworkRuns
        }
        if ($frameworkRuns.Count -lt $MinimumRepeats) {
            $warnings.Add("Condition '$($conditionGroup.Name)' has only $($frameworkRuns.Count) $($frameworkGroup.Name) repeat(s); minimum is $MinimumRepeats.")
        }
        if ($null -eq $frameworks[$frameworkGroup.Name].metrics.frameP95Milliseconds) {
            $warnings.Add("Condition '$($conditionGroup.Name)' has no PresentMon frame data for $($frameworkGroup.Name).")
        }
    }
    foreach ($expectedFramework in @('Baseline', 'OSFUI', 'CarbonUI')) {
        if (-not $frameworks.Contains($expectedFramework)) {
            $warnings.Add("Condition '$($conditionGroup.Name)' is missing $expectedFramework captures.")
        }
    }
    $frameworkRasters = [ordered]@{}
    foreach ($name in @($frameworks.Keys | Where-Object { $_ -ne 'Baseline' })) {
        $sizes = @($frameworks[$name].fixtureRasterSizes)
        $frameworkRasters[$name] = if ($sizes.Count) { $sizes -join ', ' } else { 'unrecorded' }
    }
    $distinctRasters = @($frameworkRasters.Values | Where-Object { $_ -ne 'unrecorded' } | Sort-Object -Unique)
    if ($conditionRuns[0].RasterizationPolicy -eq 'PixelMatched' -and $distinctRasters.Count -gt 1) {
        throw "Condition '$($conditionGroup.Name)' is PixelMatched but mixes effective fixture rasters: $($distinctRasters -join ', ')."
    }
    if ($conditionRuns[0].RasterizationPolicy -eq 'FrameworkDefault' -and $distinctRasters.Count -gt 1) {
        $rasterDescription = @($frameworkRasters.Keys | ForEach-Object {
            "$_=$($frameworkRasters[$_])"
        }) -join '; '
        $warnings.Add("Condition '$($conditionGroup.Name)' uses framework-default raster sizes ($rasterDescription); treat it as an end-to-end product comparison, not renderer efficiency per equal pixel.")
    }
    $repeatSets = @($frameworks.Keys | ForEach-Object {
        (@($frameworks[$_].repeats | Sort-Object) -join ',')
    } | Sort-Object -Unique)
    if ($repeatSets.Count -gt 1) {
        $warnings.Add("Condition '$($conditionGroup.Name)' uses different repeat IDs across frameworks.")
    }

    $baselineDeltas = [ordered]@{}
    $megapixels = $null
    if ($conditionRuns[0].Resolution -match '^(?<w>\d+)x(?<h>\d+)$') {
        $megapixels = ([double]$Matches.w * [double]$Matches.h) / 1000000.0
    }
    $baselineRuns = @($conditionRuns | Where-Object Framework -eq 'Baseline')
    if ($baselineRuns.Count) {
        foreach ($name in @($frameworks.Keys | Where-Object { $_ -ne 'Baseline' })) {
            $frameworkRuns = @($conditionRuns | Where-Object Framework -eq $name)
            $paired = Get-PairedDeltaSummary $frameworkRuns $baselineRuns $megapixels
            if ($null -ne $paired) { $baselineDeltas[$name] = $paired }
        }
    }

    $carbonMinusOSF = $null
    if ($frameworks.Contains('CarbonUI') -and $frameworks.Contains('OSFUI')) {
        $carbon = $frameworks.CarbonUI.metrics
        $osf = $frameworks.OSFUI.metrics
        $carbonMinusOSF = [ordered]@{
            systemCpuMeanPercent = Get-Difference $carbon.systemCpuMeanPercent $osf.systemCpuMeanPercent
            trackedCpuMeanPercent = Get-Difference $carbon.trackedCpuMeanPercent $osf.trackedCpuMeanPercent
            trackedPrivateMaxMiB = Get-Difference $carbon.trackedPrivateMaxMiB $osf.trackedPrivateMaxMiB
            trackedDedicatedVramMaxMiB = Get-Difference $carbon.trackedDedicatedVramMaxMiB $osf.trackedDedicatedVramMaxMiB
            adapterGpuMeanPercent = Get-Difference $carbon.adapterGpuMeanPercent $osf.adapterGpuMeanPercent
            adapterPowerMeanWatts = Get-Difference $carbon.adapterPowerMeanWatts $osf.adapterPowerMeanWatts
            adapterVramMaxMiB = Get-Difference $carbon.adapterVramMaxMiB $osf.adapterVramMaxMiB
            averageFps = Get-Difference $carbon.averageFps $osf.averageFps
            displayedFps = Get-Difference $carbon.displayedFps $osf.displayedFps
            onePercentLowFps = Get-Difference $carbon.onePercentLowFps $osf.onePercentLowFps
            frameP95Milliseconds = Get-Difference $carbon.frameP95Milliseconds $osf.frameP95Milliseconds
            frameP99Milliseconds = Get-Difference $carbon.frameP99Milliseconds $osf.frameP99Milliseconds
            gpuTimeP95Milliseconds = Get-Difference $carbon.gpuTimeP95Milliseconds $osf.gpuTimeP95Milliseconds
            trackedCpuCoreMsPerSecond = Get-Difference $carbon.trackedCpuCoreMsPerSecond $osf.trackedCpuCoreMsPerSecond
            trackedCpuCoreMsPerFrame = Get-Difference $carbon.trackedCpuCoreMsPerFrame $osf.trackedCpuCoreMsPerFrame
            fixtureRafFps = Get-Difference $carbon.fixtureRafFps $osf.fixtureRafFps
            baselineAdjustedCpuCoreMsPerFrame = if ($baselineDeltas.Contains('CarbonUI') -and $baselineDeltas.Contains('OSFUI')) {
                Get-Difference $baselineDeltas.CarbonUI.trackedCpuCoreMsPerApplicationFrame $baselineDeltas.OSFUI.trackedCpuCoreMsPerApplicationFrame
            } else { $null }
            baselineAdjustedCpuCoreMsPerUiUpdate = if ($baselineDeltas.Contains('CarbonUI') -and $baselineDeltas.Contains('OSFUI')) {
                Get-Difference $baselineDeltas.CarbonUI.trackedCpuCoreMsPerUiUpdate $baselineDeltas.OSFUI.trackedCpuCoreMsPerUiUpdate
            } else { $null }
        }
    }

    $conditions.Add([ordered]@{
        scenario = $conditionRuns[0].Scenario
        resolution = $conditionRuns[0].Resolution
        frameGeneration = $conditionRuns[0].FrameGeneration
        frameRateMode = $conditionRuns[0].FrameRateMode
        renderPreset = $conditionRuns[0].RenderPreset
        rasterizationPolicy = $conditionRuns[0].RasterizationPolicy
        frameworkRasters = $frameworkRasters
        fixtureHash = if ($fixtureHashes.Count) { $fixtureHashes[0] } else { $null }
        resolutionMegapixels = $megapixels
        carbonTheoreticalBgraMiBPerFrame = if ($megapixels) { $megapixels * 1000000.0 * 4.0 / 1MB } else { $null }
        carbonFullSurfaceBgraMiBPerSecondAtAppFps = if ($megapixels -and $frameworks.Contains('CarbonUI') -and
            $frameworks.CarbonUI.metrics.averageFps) {
            $megapixels * 1000000.0 * 4.0 / 1MB * [double]$frameworks.CarbonUI.metrics.averageFps
        } else { $null }
        environmentSignature = $environments[0]
        frameworks = $frameworks
        baselineDeltas = $baselineDeltas
        carbonMinusOSF = $carbonMinusOSF
    })
}

$report = [ordered]@{
    schemaVersion = 3
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    inputRoot = $InputRoot
    selectedRuns = $runs.Count
    minimumRepeats = $MinimumRepeats
    warnings = @($warnings)
    conditions = @($conditions)
}
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding utf8NoBOM

$markdown = [Collections.Generic.List[string]]::new()
$markdown.Add('# UI framework benchmark comparison')
$markdown.Add('')
$markdown.Add("- Selected captures: $($runs.Count)")
$markdown.Add('- Aggregation: median of per-run metrics; duplicate repeat IDs resolve to the newest completed capture')
$markdown.Add('- Carbon minus OSF: positive CPU, memory, power, or frame-time values favor OSF; positive FPS values favor Carbon')
if ($warnings.Count) {
    $markdown.Add("- Warnings: $($warnings.Count) (see comparison.json)")
}

foreach ($condition in $conditions) {
    $markdown.Add('')
    $markdown.Add("## $($condition.scenario) — $($condition.resolution), $($condition.frameRateMode), FG $($condition.frameGeneration), $($condition.renderPreset)")
    if ($condition.fixtureHash) {
        $markdown.Add('')
        $markdown.Add("- Identical fixture SHA-256: $($condition.fixtureHash)")
        $markdown.Add("- Rasterization policy: $($condition.rasterizationPolicy)")
        foreach ($name in $condition.frameworkRasters.Keys) {
            $markdown.Add("- ${name} effective fixture raster: $($condition.frameworkRasters[$name])")
        }
        if (@($condition.frameworkRasters.Values | Sort-Object -Unique).Count -gt 1) {
            $markdown.Add('- **Renderer pixel workloads differ:** use this condition for end-to-end product overhead, not equal-pixel renderer-efficiency claims.')
        }
        $markdown.Add("- Carbon visible BGRA surface volume: $(Format-Number $condition.carbonTheoreticalBgraMiBPerFrame) MiB/frame theoretical (not a measured bandwidth counter)")
        $markdown.Add("- Full-surface volume at measured Carbon app FPS: $(Format-Number $condition.carbonFullSurfaceBgraMiBPerSecondAtAppFps) MiB/s theoretical if uploaded every application frame")
    }
    $markdown.Add('')
    $markdown.Add('| Framework | Runs | System CPU mean (%) | Tracked CPU mean (%) | Tracked private max (MiB) | Tracked VRAM max (MiB) | GPU mean (%) | Power mean (W) | Adapter VRAM max (MiB) |')
    $markdown.Add('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    foreach ($name in $condition.frameworks.Keys) {
        $item = $condition.frameworks[$name]
        $metric = $item.metrics
        $markdown.Add('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |' -f @(
            $name, $item.runCount,
            (Format-Number $metric.systemCpuMeanPercent),
            (Format-Number $metric.trackedCpuMeanPercent),
            (Format-Number $metric.trackedPrivateMaxMiB),
            (Format-Number $metric.trackedDedicatedVramMaxMiB),
            (Format-Number $metric.adapterGpuMeanPercent),
            (Format-Number $metric.adapterPowerMeanWatts),
            (Format-Number $metric.adapterVramMaxMiB)
        ))
    }

    $markdown.Add('')
    $markdown.Add('| Framework | App FPS | Displayed FPS | 1% low FPS | UI RAF FPS | Generated rows | Frame p95 (ms) | Frame p99 (ms) | GPU time p95 (ms) |')
    $markdown.Add('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    foreach ($name in $condition.frameworks.Keys) {
        $item = $condition.frameworks[$name]
        $metric = $item.metrics
        $markdown.Add('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |' -f @(
            $name,
            (Format-Number $metric.averageFps),
            (Format-Number $metric.displayedFps),
            (Format-Number $metric.onePercentLowFps),
            (Format-Number $metric.fixtureRafFps),
            (Format-Number $metric.generatedFrameRows 0),
            (Format-Number $metric.frameP95Milliseconds),
            (Format-Number $metric.frameP99Milliseconds),
            (Format-Number $metric.gpuTimeP95Milliseconds)
        ))
    }

    if ($condition.baselineDeltas.Count) {
        $markdown.Add('')
        $markdown.Add('### Delta from baseline')
        $markdown.Add('')
        $markdown.Add('| Framework | System CPU (%) | Tracked CPU (%) | Private (MiB) | Tracked VRAM (MiB) | GPU (%) | Power (W) | Frame p95 (ms) | Frame p99 (ms) |')
        $markdown.Add('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
        foreach ($name in $condition.baselineDeltas.Keys) {
            $delta = $condition.baselineDeltas[$name]
            $markdown.Add('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |' -f @(
                $name,
                (Format-Delta $delta.systemCpuMeanPercent),
                (Format-Delta $delta.trackedCpuMeanPercent),
                (Format-Delta $delta.trackedPrivateMaxMiB),
                (Format-Delta $delta.trackedDedicatedVramMaxMiB),
                (Format-Delta $delta.adapterGpuMeanPercent),
                (Format-Delta $delta.adapterPowerMeanWatts),
                (Format-Delta $delta.frameP95Milliseconds),
                (Format-Delta $delta.frameP99Milliseconds)
            ))
        }
        $markdown.Add('')
        $markdown.Add('| Framework | CPU overhead (core-ms/s) | CPU overhead (core-ms/app frame) | CPU overhead (core-ms/UI update) | CPU overhead (core-ms/UI update/MP) |')
        $markdown.Add('|---|---:|---:|---:|---:|')
        foreach ($name in $condition.baselineDeltas.Keys) {
            $delta = $condition.baselineDeltas[$name]
            $markdown.Add('| {0} | {1} | {2} | {3} | {4} |' -f @(
                $name,
                (Format-Delta $delta.trackedCpuCoreMsPerSecond 2),
                (Format-Delta $delta.trackedCpuCoreMsPerApplicationFrame 3),
                (Format-Delta $delta.trackedCpuCoreMsPerUiUpdate 3),
                (Format-Delta $delta.trackedCpuCoreMsPerUiUpdatePerMegapixel 3)
            ))
        }
    }

    if ($null -ne $condition.carbonMinusOSF) {
        $delta = $condition.carbonMinusOSF
        $markdown.Add('')
        $markdown.Add('### Carbon minus OSF')
        $markdown.Add('')
        $markdown.Add("- Baseline-adjusted CPU difference: $(Format-Delta $delta.baselineAdjustedCpuCoreMsPerFrame 3) core-ms/application frame (positive favors OSF)")
        $markdown.Add("- Baseline-adjusted CPU difference: $(Format-Delta $delta.baselineAdjustedCpuCoreMsPerUiUpdate 3) core-ms/UI update (positive favors OSF)")
        $markdown.Add('| System CPU (%) | Tracked CPU (%) | Private (MiB) | Tracked VRAM (MiB) | GPU (%) | Power (W) | Average FPS | 1% low FPS | Frame p95 (ms) | Frame p99 (ms) |')
        $markdown.Add('|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
        $markdown.Add('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} |' -f @(
            (Format-Delta $delta.systemCpuMeanPercent),
            (Format-Delta $delta.trackedCpuMeanPercent),
            (Format-Delta $delta.trackedPrivateMaxMiB),
            (Format-Delta $delta.trackedDedicatedVramMaxMiB),
            (Format-Delta $delta.adapterGpuMeanPercent),
            (Format-Delta $delta.adapterPowerMeanWatts),
            (Format-Delta $delta.averageFps),
            (Format-Delta $delta.onePercentLowFps),
            (Format-Delta $delta.frameP95Milliseconds),
            (Format-Delta $delta.frameP99Milliseconds)
        ))
    }
}

$markdown.Add('')
$markdown.Add('## Interpretation boundary')
$markdown.Add('')
$markdown.Add('Controlled static/transforms/repaint/layout/text-scroll/canvas conditions use a recorded identical-document SHA-256 and exact CSS viewport. Effective raster sizes are reported separately: FrameworkDefault answers end-to-end product overhead, while renderer-efficiency claims require PixelMatched captures. Headline CPU claims should use baseline-adjusted core-ms/application-frame when PresentMon is valid. For controlled animated documents, core-ms/UI update normalizes CPU work by the page cadence actually delivered and must be reported beside that cadence. App FPS, displayed FPS, UI RAF cadence, p99 frame time, memory, and GPU/power remain separate quality dimensions. Carbon renderer/upload counters are unavailable from the stock public API, so theoretical BGRA surface volume is labeled rather than presented as measured bandwidth.')
$markdown | Set-Content -LiteralPath $OutputPath -Encoding utf8NoBOM

Write-Host "Comparison: $OutputPath" -ForegroundColor Green
Write-Host "Machine-readable data: $jsonPath"
