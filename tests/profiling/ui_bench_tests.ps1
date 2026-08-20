#requires -Version 7.2
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Near([double] $Expected, [double] $Actual, [string] $Message)
{
    if ([math]::Abs($Expected - $Actual) -gt 0.000001) {
        throw "$Message. Expected $Expected, got $Actual."
    }
}

function New-SyntheticSummary(
    [string] $Framework,
    [datetime] $CompletedUtc,
    [double] $SystemCpu,
    [double] $TrackedCpu,
    [double] $PrivateMiB,
    [double] $VramMiB,
    [double] $FrameP95,
    [int] $Repeat = 1)
{
    return [ordered]@{
        schemaVersion = 2
        captureDirectory = "synthetic-$Framework-$($CompletedUtc.Ticks)"
        framework = $Framework
        manifest = [ordered]@{
            status = 'complete'
            framework = $Framework
            completedUtc = $CompletedUtc.ToUniversalTime().ToString('o')
            benchmark = [ordered]@{
                scenario = 'loaded-hidden'
                repeat = $Repeat
                resolution = '2560x1440'
                frameGeneration = 'Off'
                frameRateMode = 'Fixed60'
                renderPreset = 'Ultra-DLSSQuality'
            }
            gameExecutable = [ordered]@{ fileVersion = '1.16.244.0' }
            presentMonIdentity = [ordered]@{ sha256 = 'presentmon-test' }
            loadedUiModules = if ($Framework -eq 'Baseline') { @() } else {
                @([ordered]@{ name = "$Framework.dll"; sha256 = "$Framework-test" })
            }
            machine = [ordered]@{
                computerName = 'SYNTHETIC'
                osVersion = 'test'
                displayAdapters = @([ordered]@{ name = 'GPU'; driverVersion = '1' })
            }
        }
        trackedProcesses = [ordered]@{
            cpuPercentMachine = [ordered]@{ mean = $TrackedCpu }
            workingSetBytes = [ordered]@{ max = ($PrivateMiB + 50) * 1MB }
            privateBytes = [ordered]@{ max = $PrivateMiB * 1MB }
            dedicatedGpuBytes = [ordered]@{ max = $VramMiB * 1MB }
        }
        system = [ordered]@{ cpuPercent = [ordered]@{ mean = $SystemCpu } }
        hardware = @([ordered]@{
            gpuUtilizationPercent = [ordered]@{ mean = 40 + $SystemCpu }
            powerWatts = [ordered]@{ mean = 100 + $SystemCpu }
            vramUsedMiB = [ordered]@{ max = 3000 + $VramMiB }
        })
        presentMon = [ordered]@{
            averageFps = 60 - $SystemCpu
            onePercentLowFps = 55 - $SystemCpu
            metricsMilliseconds = [ordered]@{
                FrameTime = [ordered]@{ p95 = $FrameP95; p99 = $FrameP95 + 2 }
                GPUTime = [ordered]@{ p95 = $FrameP95 - 3 }
            }
        }
        derived = [ordered]@{
            trackedCpuCoreMillisecondsPerSecond = $TrackedCpu * 100
            trackedCpuCoreMillisecondsPerApplicationFrame = ($TrackedCpu * 100) / (60 - $SystemCpu)
            trackedCpuCoreMillisecondsPerFramePerMegapixel = (($TrackedCpu * 100) / (60 - $SystemCpu)) / 3.6864
        }
        fixture = if ($Framework -eq 'Baseline') { $null } else {
            [ordered]@{ rafFps = [ordered]@{ mean = 60.0 } }
        }
    }
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$fixtureSource = Get-Content -LiteralPath (Join-Path $repo 'tools\profiling\fixture\src\main.cpp') -Raw
@(
    'RE::MenuOpenCloseEvent'
    'desc.visible = 0;'
    'FixtureTick'
    '!a_event.opening && name == "LoadingMenu"'
    'tasks->AddTask(InitializeAfterDataLoad);'
    'tasks->AddPermanentTask(FixtureTick);'
    'TrySubmitThreadpoolCallback(&WriteTelemetry'
    'file:///Data/SFSE/Plugins/UIBench/index.html?scenario='
    'UIBench: loading menu closed; no-framework baseline ready'
    'desc.focusable = 0;'
) | ForEach-Object {
    if (-not $fixtureSource.Contains($_, [StringComparison]::Ordinal)) {
        throw "UIBench fixture is missing the post-load presentation contract: $_"
    }
}
$fixtureDocument = Get-Content -LiteralPath (Join-Path $repo 'tools\profiling\fixture\content\index.html') -Raw
if ($fixtureDocument.Contains('inset:', [StringComparison]::Ordinal)) {
    throw 'UIBench fixture uses the CSS inset shorthand, which is unsupported by the Carbon UI renderer.'
}
foreach ($unsupportedCss in @('display: grid', 'isolation:', 'translate3d(', 'will-change:', 'perspective:', 'color-scheme:')) {
    if ($fixtureDocument.Contains($unsupportedCss, [StringComparison]::Ordinal)) {
        throw "UIBench fixture retains an avoidable Ultralight 1.3 compatibility risk: $unsupportedCss"
    }
}
if ($fixtureDocument -match '#[0-9A-Fa-f]{8}(?![0-9A-Fa-f])') {
    throw 'UIBench fixture uses eight-digit hexadecimal colors instead of legacy rgba() syntax.'
}
if (-not $fixtureDocument.Contains('.transform-field {', [StringComparison]::Ordinal) -or
    -not $fixtureDocument.Contains('width: 100%; height: 100%;', [StringComparison]::Ordinal)) {
    throw 'UIBench transform field is missing its explicit full-viewport sizing contract.'
}
@(
    '<p id="ui-rate">UI RAF: measuring</p>'
    'UI RAF: intentionally idle'
    'setInterval(function () { report(performance.now()); }, 1000);'
    'fpsThisInterval: intervalFps'
) | ForEach-Object {
    if (-not $fixtureDocument.Contains($_, [StringComparison]::Ordinal)) {
        throw "UIBench fixture is missing its visible RAF cadence contract: $_"
    }
}
$matrixRunnerSource = Get-Content -LiteralPath (Join-Path $repo 'tools\profiling\Invoke-UIBenchMatrix.ps1') -Raw
if (-not $matrixRunnerSource.Contains("`$marker = 'UIBench: loading menu closed; no-framework baseline ready'", [StringComparison]::Ordinal)) {
    throw 'Matrix runner is missing its baseline post-load readiness marker.'
}
if ($matrixRunnerSource.Contains('$marker = "[$($Game.Id)] [I] UIBench: loading menu closed; no-framework baseline ready"', [StringComparison]::Ordinal)) {
    throw 'Matrix runner incorrectly treats the SFSE logger thread ID as a process ID.'
}
$captureSource = Get-Content -LiteralPath (Join-Path $repo 'tools\profiling\Capture-OSFUI.ps1') -Raw
@(
    'function Get-ValidCounterSamples'
    'Get-Counter -Counter $Paths -ErrorAction SilentlyContinue'
    '[uint32]$_.Status -eq 0'
) | ForEach-Object {
    if (-not $captureSource.Contains($_, [StringComparison]::Ordinal)) {
        throw "Capture script is missing invalid performance-counter resilience: $_"
    }
}
$testParent = Join-Path $repo 'build\test-tmp'
$testRoot = Join-Path $testParent ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

try {
    $compareRoot = Join-Path $testRoot 'compare'
    New-Item -ItemType Directory -Force -Path $compareRoot | Out-Null
    $now = [DateTime]::UtcNow
    $summaries = @(
        New-SyntheticSummary 'Baseline' $now.AddMinutes(-3) 10 5 100 200 18
        New-SyntheticSummary 'OSFUI' $now.AddMinutes(-2) 11 6 120 230 19
        New-SyntheticSummary 'CarbonUI' $now.AddMinutes(-1) 13 8 150 270 21
        # Same OSF repeat, but older. The comparison must discard this outlier.
        New-SyntheticSummary 'OSFUI' $now.AddMinutes(-10) 99 99 999 999 99
    )
    for ($index = 0; $index -lt $summaries.Count; ++$index) {
        $directory = Join-Path $compareRoot "capture-$index"
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        $summaries[$index] | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $directory 'summary.json') -Encoding utf8NoBOM
    }

    $comparisonPath = Join-Path $compareRoot 'comparison.md'
    & (Join-Path $repo 'tools\profiling\Compare-UIBench.ps1') `
        -InputRoot $compareRoot -OutputPath $comparisonPath -MinimumRepeats 1
    $comparison = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($comparisonPath, '.json')) -Raw |
        ConvertFrom-Json
    if ($comparison.conditions.Count -ne 1) { throw 'Expected exactly one comparison condition.' }
    if ($comparison.selectedRuns -ne 3) { throw 'Expected duplicate repeat to resolve to three selected runs.' }
    if ($comparison.warnings.Count -ne 1) { throw 'Expected one duplicate-repeat warning.' }
    $condition = $comparison.conditions[0]
    Assert-Near 1 $condition.baselineDeltas.OSFUI.systemCpuMeanPercent 'OSF baseline CPU delta'
    Assert-Near 2 $condition.carbonMinusOSF.systemCpuMeanPercent 'Carbon-minus-OSF CPU delta'
    Assert-Near 2 $condition.carbonMinusOSF.frameP95Milliseconds 'Carbon-minus-OSF frame p95 delta'
    Assert-Near ((800 - 500) / 47 - (600 - 500) / 49) $condition.carbonMinusOSF.baselineAdjustedCpuCoreMsPerFrame 'Baseline-adjusted core-ms/frame delta'
    Assert-Near ((800 - 500) / 60 - (600 - 500) / 60) $condition.carbonMinusOSF.baselineAdjustedCpuCoreMsPerUiUpdate 'Baseline-adjusted core-ms/UI-update delta'

    $pairedRoot = Join-Path $testRoot 'paired'
    New-Item -ItemType Directory -Force -Path $pairedRoot | Out-Null
    $pairedSummaries = @(
        New-SyntheticSummary 'Baseline' $now.AddMinutes(1) 1 1 100 200 18 1
        New-SyntheticSummary 'OSFUI' $now.AddMinutes(2) 2 2 120 230 19 1
        New-SyntheticSummary 'Baseline' $now.AddMinutes(3) 4 4 100 200 18 2
        New-SyntheticSummary 'OSFUI' $now.AddMinutes(4) 3 3 120 230 19 2
        New-SyntheticSummary 'Baseline' $now.AddMinutes(5) 5 5 100 200 18 3
        New-SyntheticSummary 'OSFUI' $now.AddMinutes(6) 6 6 120 230 19 3
    )
    for ($index = 0; $index -lt $pairedSummaries.Count; ++$index) {
        $directory = Join-Path $pairedRoot "capture-$index"
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        $pairedSummaries[$index] | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $directory 'summary.json') -Encoding utf8NoBOM
    }
    $pairedPath = Join-Path $pairedRoot 'comparison.md'
    & (Join-Path $repo 'tools\profiling\Compare-UIBench.ps1') -InputRoot $pairedRoot -OutputPath $pairedPath -MinimumRepeats 3
    $pairedComparison = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($pairedPath, '.json')) -Raw |
        ConvertFrom-Json
    # Paired deltas are +100, -100, +100 core-ms/s: median +100. The
    # difference of independent medians would incorrectly report -100.
    Assert-Near 100 $pairedComparison.conditions[0].baselineDeltas.OSFUI.trackedCpuCoreMsPerSecond 'Paired baseline delta must be aggregated after repeat matching'
    Assert-Near 0 $pairedComparison.conditions[0].baselineDeltas.OSFUI.systemCpuMeanPercentMad 'Paired headline delta MAD'

    $captureRoot = Join-Path $testRoot 'summarize'
    New-Item -ItemType Directory -Force -Path $captureRoot | Out-Null
    $manifest = [ordered]@{
        schemaVersion = 2
        label = 'synthetic-osf'
        framework = 'OSFUI'
        status = 'complete'
        requestedDurationSeconds = 60
        actualDurationSeconds = 60
        wprProfile = 'None'
        notes = ''
        benchmark = [ordered]@{
            scenario = 'static'
            repeat = 1
            resolution = '1920x1080'
            frameGeneration = 'On'
            frameRateMode = 'Fixed60'
            renderPreset = 'Synthetic'
        }
    }
    $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $captureRoot 'manifest.json') -Encoding utf8NoBOM
    @(
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:00Z'; Role = 'Game'; CpuPercentMachine = 5; CpuPercentOneCore = 50; WorkingSetBytes = 200MB; PrivateBytes = 100MB; VirtualBytes = 1GB; HandleCount = 100; ThreadCount = 20; ReadBytesPerSecond = 0; WriteBytesPerSecond = 0 }
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:00Z'; Role = 'OSFUIHost'; CpuPercentMachine = 1; CpuPercentOneCore = 10; WorkingSetBytes = 40MB; PrivateBytes = 20MB; VirtualBytes = 100MB; HandleCount = 10; ThreadCount = 5; ReadBytesPerSecond = 0; WriteBytesPerSecond = 0 }
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:01Z'; Role = 'Game'; CpuPercentMachine = 6; CpuPercentOneCore = 60; WorkingSetBytes = 210MB; PrivateBytes = 110MB; VirtualBytes = 1GB; HandleCount = 101; ThreadCount = 20; ReadBytesPerSecond = 0; WriteBytesPerSecond = 0 }
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:01Z'; Role = 'OSFUIHost'; CpuPercentMachine = 2; CpuPercentOneCore = 20; WorkingSetBytes = 45MB; PrivateBytes = 25MB; VirtualBytes = 100MB; HandleCount = 11; ThreadCount = 5; ReadBytesPerSecond = 0; WriteBytesPerSecond = 0 }
    ) | Export-Csv -LiteralPath (Join-Path $captureRoot 'process-samples.csv') -NoTypeInformation
    @(
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:00Z'; Role = 'Game'; DedicatedBytes = 300MB; SharedBytes = 10MB }
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:00Z'; Role = 'OSFUIHost'; DedicatedBytes = 30MB; SharedBytes = 2MB }
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:01Z'; Role = 'Game'; DedicatedBytes = 320MB; SharedBytes = 10MB }
        [pscustomobject]@{ TimestampUtc = '2026-01-01T00:00:01Z'; Role = 'OSFUIHost'; DedicatedBytes = 35MB; SharedBytes = 2MB }
    ) | Export-Csv -LiteralPath (Join-Path $captureRoot 'gpu-memory-samples.csv') -NoTypeInformation
    @(
        [pscustomobject]@{ SwapChainAddress = '0xGAME'; FrameType = 'Application'; MsBetweenPresents = 16.6667; MsCPUBusy = 8; MsGPUTime = 9; DisplayedTime = 8.3333 }
        [pscustomobject]@{ SwapChainAddress = '0xGAME'; FrameType = 'AMD AFMF'; MsBetweenPresents = 8.3333; MsCPUBusy = 0; MsGPUTime = 0; DisplayedTime = 8.3333 }
        [pscustomobject]@{ SwapChainAddress = '0xGAME'; FrameType = 'Application'; MsBetweenPresents = 16.6667; MsCPUBusy = 8; MsGPUTime = 9; DisplayedTime = 8.3333 }
        [pscustomobject]@{ SwapChainAddress = '0xGAME'; FrameType = 'AMD AFMF'; MsBetweenPresents = 8.3333; MsCPUBusy = 0; MsGPUTime = 0; DisplayedTime = 8.3333 }
        [pscustomobject]@{ SwapChainAddress = '0xAUX'; FrameType = 'Application'; MsBetweenPresents = 99; MsCPUBusy = 90; MsGPUTime = 90; DisplayedTime = 'NA' }
    ) | Export-Csv -LiteralPath (Join-Path $captureRoot 'presentmon.csv') -NoTypeInformation
    @(
        [ordered]@{
            framework = 'OSFUI'; provider = 'OSFUI'; scenario = 'static'; fixtureHash = ('A' * 64)
            receivedUnixMs = 2000
            viewport = [ordered]@{ width = 1920; height = 1080; devicePixelRatio = 1 }
            raf = [ordered]@{ fpsThisInterval = 60; callbacks = 60; intervalsOver20ms = 0; intervalsOver33ms = 0; intervalsOver50ms = 0 }
            workload = [ordered]@{ ticks = 60; workP95Milliseconds = 0.2; workP99Milliseconds = 0.3 }
            longTasks = [ordered]@{ count = 0; totalMilliseconds = 0 }
            frameworkCounters = [ordered]@{ rendered = $null; uploaded = $null; published = $null; consumed = $null; stale = $null; dropped = $null }
        }
        [ordered]@{
            framework = 'OSFUI'; provider = 'OSFUI'; scenario = 'static'; fixtureHash = ('A' * 64)
            receivedUnixMs = 1000
            viewport = [ordered]@{ width = 1920; height = 1080; devicePixelRatio = 1 }
            raf = [ordered]@{ fpsThisInterval = 60; callbacks = 120; intervalsOver20ms = 0; intervalsOver33ms = 0; intervalsOver50ms = 0 }
            workload = [ordered]@{ ticks = 120; workP95Milliseconds = 0.2; workP99Milliseconds = 0.3 }
            longTasks = [ordered]@{ count = 0; totalMilliseconds = 0 }
            frameworkCounters = [ordered]@{ rendered = $null; uploaded = $null; published = $null; consumed = $null; stale = $null; dropped = $null }
        }
    ) | ForEach-Object { $_ | ConvertTo-Json -Depth 8 -Compress } |
        Set-Content -LiteralPath (Join-Path $captureRoot 'fixture-telemetry.jsonl') -Encoding utf8NoBOM

    & (Join-Path $repo 'tools\profiling\Summarize-OSFUI.ps1') -CaptureDirectory $captureRoot
    $summarized = Get-Content -LiteralPath (Join-Path $captureRoot 'summary.json') -Raw |
        ConvertFrom-Json
    Assert-Near 135 ($summarized.trackedProcesses.privateBytes.max / 1MB) 'Tracked private-byte total'
    Assert-Near 355 ($summarized.trackedProcesses.dedicatedGpuBytes.max / 1MB) 'Tracked VRAM total'
    Assert-Near 7 $summarized.trackedProcesses.cpuPercentMachine.mean 'Tracked CPU mean'
    Assert-Near (1000 / 16.6667) $summarized.presentMon.application.averageFps 'Application FPS excludes generated rows'
    Assert-Near 120.00048 $summarized.presentMon.displayedAverageFps 'Displayed FPS includes generated rows'
    Assert-Near 2 $summarized.presentMon.generated.rows 'Generated row count'
    if ($summarized.presentMon.primarySwapChain -ne '0xGAME') { throw 'Primary displayed swapchain selection failed.' }
    Assert-Near 5 $summarized.presentMon.allRows 'All swapchain row count'
    Assert-Near 1 $summarized.presentMon.auxiliaryRows 'Auxiliary swapchain row count'
    Assert-Near 9 $summarized.presentMon.application.metricsMilliseconds.GPUTime.p95 'PresentMon v2 MsGPUTime alias'
    Assert-Near (700 / (1000 / 16.6667)) $summarized.derived.trackedCpuCoreMillisecondsPerApplicationFrame 'Tracked core-ms per app frame'
    Assert-Near 60 $summarized.fixture.rafCallbacks 'Newest async telemetry must be selected by timestamp'
    if (-not $summarized.frameRateValidation.valid) { throw 'Synthetic fixed-60 run should validate.' }

    $matrixPath = Join-Path $testRoot 'matrix.csv'
    & (Join-Path $repo 'tools\profiling\New-UIBenchMatrix.ps1') -OutputPath $matrixPath
    $matrix = @(Import-Csv -LiteralPath $matrixPath)
    Assert-Near 27 $matrix.Count 'Default screening matrix size'
    if (@($matrix | Where-Object { $_.CaptureCommand -notmatch '-WprProfile None' }).Count) {
        throw 'Default screening matrix must keep WPR disabled.'
    }
    $runnerPlan = & (Join-Path $repo 'tools\profiling\Invoke-UIBenchMatrix.ps1') `
        -MatrixPath $matrixPath -FixtureModPath $testRoot -PlanOnly *>&1 | Out-String
    if (-not $runnerPlan.Contains('0 complete; 27 pending/failed.', [StringComparison]::Ordinal)) {
        throw 'Matrix runner plan did not recognize all generated rows.'
    }
    if (-not $runnerPlan.Contains('Minimum timed work remaining', [StringComparison]::Ordinal)) {
        throw 'Matrix runner plan is missing its duration estimate.'
    }

    Write-Host 'UI benchmark tests: PASS' -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = (Resolve-Path -LiteralPath $testRoot).Path
        $resolvedParent = [IO.Path]::GetFullPath($testParent).TrimEnd('\') + '\'
        if (-not $resolvedTestRoot.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean unexpected test path: $resolvedTestRoot"
        }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
