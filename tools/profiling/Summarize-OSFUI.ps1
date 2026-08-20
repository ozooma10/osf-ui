#requires -Version 7.2
<#
.SYNOPSIS
  Summarize a UI-framework profiling capture into JSON and Markdown.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CaptureDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-CsvIfPresent([string] $Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    return @(Import-Csv -LiteralPath $Path)
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
    if ([double]::TryParse([string]$Value, [ref]$parsed)) { return $parsed }
    return $null
}

function Get-Percentile([double[]] $Values, [double] $Percentile)
{
    $sorted = @($Values | Where-Object { [double]::IsFinite($_) } | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    if ($sorted.Count -eq 1) { return [double]$sorted[0] }
    $position = ($sorted.Count - 1) * $Percentile
    $lower = [math]::Floor($position)
    $upper = [math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$sorted[$lower] }
    $weight = $position - $lower
    return [double]$sorted[$lower] * (1.0 - $weight) + [double]$sorted[$upper] * $weight
}

function Measure-Series($Values)
{
    $numbers = [double[]]@($Values | ForEach-Object { Convert-ToNumber $_ } |
        Where-Object { $null -ne $_ -and [double]::IsFinite($_) })
    if ($numbers.Count -eq 0) { return $null }
    $measure = $numbers | Measure-Object -Average -Minimum -Maximum
    return [ordered]@{
        count = $numbers.Count
        mean = [double]$measure.Average
        min = [double]$measure.Minimum
        p50 = Get-Percentile $numbers 0.50
        p95 = Get-Percentile $numbers 0.95
        p99 = Get-Percentile $numbers 0.99
        max = [double]$measure.Maximum
    }
}

function Format-Number($Value, [int] $Decimals = 2)
{
    if ($null -eq $Value) { return 'n/a' }
    return ([double]$Value).ToString("N$Decimals", [Globalization.CultureInfo]::InvariantCulture)
}

function Get-Statistic($Stats, [string] $Name)
{
    if ($null -eq $Stats) { return $null }
    if ($Stats -is [Collections.IDictionary]) { return $Stats[$Name] }
    return $Stats.$Name
}

function Get-PropertyValue($Object, [string] $Property)
{
    if ($null -eq $Object -or [string]::IsNullOrWhiteSpace($Property)) { return $null }
    if ($Object -is [Collections.IDictionary]) {
        if ($Object.Contains($Property)) { return $Object[$Property] }
        return $null
    }
    $member = $Object.PSObject.Properties[$Property]
    if ($null -eq $member) { return $null }
    return $member.Value
}

function Get-Sum($Rows, [string] $Property)
{
    return [double](($Rows | ForEach-Object { Convert-ToNumber $_.$Property } |
        Where-Object { $null -ne $_ } | Measure-Object -Sum).Sum)
}

function Get-SumOrNull($Rows, [string] $Property)
{
    $numbers = @($Rows | ForEach-Object { Convert-ToNumber $_.$Property } |
        Where-Object { $null -ne $_ })
    if ($numbers.Count -eq 0) { return $null }
    return [double](($numbers | Measure-Object -Sum).Sum)
}

function Measure-PresentRows($Rows, [string[]] $Headers)
{
    if ($null -eq $Rows) { return $null }
    $items = @($Rows | Where-Object { $null -ne $_ })
    if (-not $items.Count) { return $null }
    $aliases = [ordered]@{
        FrameTime = @('FrameTime', 'MsBetweenPresents')
        CPUBusy = @('CPUBusy', 'MsCPUBusy')
        CPUWait = @('CPUWait', 'MsCPUWait')
        GPULatency = @('GPULatency', 'MsGPULatency')
        GPUTime = @('GPUTime', 'MsGPUTime', 'MsGPUDuration')
        GPUBusy = @('GPUBusy', 'MsGPUBusy')
        GPUWait = @('GPUWait', 'MsGPUWait')
        DisplayLatency = @('DisplayLatency', 'MsDisplayLatency')
        DisplayedTime = @('DisplayedTime', 'MsDisplayedTime')
        BetweenDisplayChange = @('MsBetweenDisplayChange')
        RenderPresentLatency = @('MsRenderPresentLatency')
        UntilDisplayed = @('MsUntilDisplayed')
    }
    $resolvedColumns = [ordered]@{}
    foreach ($metric in $aliases.Keys) {
        $column = @($aliases[$metric] | Where-Object { $Headers -contains $_ } | Select-Object -First 1)
        if ($column.Count) { $resolvedColumns[$metric] = $column[0] }
    }
    $frameColumn = $resolvedColumns.FrameTime
    $frameStats = if ($frameColumn) {
        Measure-Series ($items | ForEach-Object { Get-PropertyValue $_ $frameColumn })
    } else { $null }
    $metrics = [ordered]@{}
    foreach ($metric in $resolvedColumns.Keys) {
        $column = $resolvedColumns[$metric]
        $metrics[$metric] = Measure-Series ($items | ForEach-Object { Get-PropertyValue $_ $column })
    }
    $dropped = if ($Headers -contains 'Dropped') {
        @($items | Where-Object { $_.Dropped -eq '1' -or $_.Dropped -eq 'true' }).Count
    } elseif ($resolvedColumns.Contains('DisplayedTime')) {
        $displayedColumn = $resolvedColumns.DisplayedTime
        @($items | Where-Object {
            $displayed = Convert-ToNumber (Get-PropertyValue $_ $displayedColumn)
            $null -eq $displayed -or $displayed -le 0
        }).Count
    } else { $null }
    return [ordered]@{
        rows = $items.Count
        dropped = $dropped
        averageFps = if ($frameStats -and $frameStats.mean -gt 0) { 1000.0 / $frameStats.mean } else { $null }
        onePercentLowFps = if ($frameStats -and $frameStats.p99 -gt 0) { 1000.0 / $frameStats.p99 } else { $null }
        metricsMilliseconds = $metrics
    }
}

function Import-JsonLines([string] $Path)
{
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    $result = [Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        try { $result.Add(($line | ConvertFrom-Json -Depth 20 -ErrorAction Stop)) } catch {}
    }
    return @($result)
}

$capture = (Resolve-Path -LiteralPath $CaptureDirectory).Path
$manifestPath = Join-Path $capture 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Capture manifest not found: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$benchmark = if ($manifest.PSObject.Properties['benchmark']) { $manifest.benchmark } else { $null }
$framework = if ($null -ne $manifest.PSObject.Properties['framework']) {
    [string]$manifest.framework
} else { 'OSFUI' }
$processRows = @(Import-CsvIfPresent (Join-Path $capture 'process-samples.csv'))
$gpuEngineRows = @(Import-CsvIfPresent (Join-Path $capture 'gpu-engine-samples.csv'))
$gpuMemoryRows = @(Import-CsvIfPresent (Join-Path $capture 'gpu-memory-samples.csv'))
$systemRows = @(Import-CsvIfPresent (Join-Path $capture 'system-samples.csv'))
$hardwareRows = @(Import-CsvIfPresent (Join-Path $capture 'hardware-samples.csv'))
$presentRows = @(Import-CsvIfPresent (Join-Path $capture 'presentmon.csv'))
$fixtureRows = @(Import-JsonLines (Join-Path $capture 'fixture-telemetry.jsonl') |
    Sort-Object { [int64]$_.receivedUnixMs })

$rolePoints = @()
foreach ($group in ($processRows | Group-Object { "$($_.TimestampUtc)|$($_.Role)" })) {
    $rows = @($group.Group)
    $rolePoints += [pscustomobject]@{
        TimestampUtc = $rows[0].TimestampUtc
        Role = $rows[0].Role
        ProcessCount = $rows.Count
        CpuPercentMachine = Get-SumOrNull $rows 'CpuPercentMachine'
        CpuPercentOneCore = Get-SumOrNull $rows 'CpuPercentOneCore'
        WorkingSetBytes = Get-Sum $rows 'WorkingSetBytes'
        PrivateBytes = Get-Sum $rows 'PrivateBytes'
        VirtualBytes = Get-Sum $rows 'VirtualBytes'
        HandleCount = Get-Sum $rows 'HandleCount'
        ThreadCount = Get-Sum $rows 'ThreadCount'
        ReadBytesPerSecond = Get-SumOrNull $rows 'ReadBytesPerSecond'
        WriteBytesPerSecond = Get-SumOrNull $rows 'WriteBytesPerSecond'
    }
}

$gpuMemoryPoints = @()
foreach ($group in ($gpuMemoryRows | Group-Object { "$($_.TimestampUtc)|$($_.Role)" })) {
    $rows = @($group.Group)
    $gpuMemoryPoints += [pscustomobject]@{
        TimestampUtc = $rows[0].TimestampUtc
        Role = $rows[0].Role
        DedicatedBytes = Get-Sum $rows 'DedicatedBytes'
        SharedBytes = Get-Sum $rows 'SharedBytes'
    }
}

$roleSummary = [ordered]@{}
foreach ($role in @($rolePoints.Role | Sort-Object -Unique)) {
    $points = @($rolePoints | Where-Object Role -eq $role | Sort-Object TimestampUtc)
    $gpuPoints = @($gpuMemoryPoints | Where-Object Role -eq $role | Sort-Object TimestampUtc)
    if ($points.Count -eq 0) { continue }
    $first = $points[0]
    $last = $points[-1]
    $firstGpu = if ($gpuPoints.Count) { $gpuPoints[0] } else { $null }
    $lastGpu = if ($gpuPoints.Count) { $gpuPoints[-1] } else { $null }
    $roleSummary[$role] = [ordered]@{
        samples = $points.Count
        maxProcessCount = [int](($points.ProcessCount | Measure-Object -Maximum).Maximum)
        cpuPercentMachine = Measure-Series $points.CpuPercentMachine
        cpuPercentOneCore = Measure-Series $points.CpuPercentOneCore
        workingSetBytes = [ordered]@{
            start = [double]$first.WorkingSetBytes
            end = [double]$last.WorkingSetBytes
            delta = [double]$last.WorkingSetBytes - [double]$first.WorkingSetBytes
            max = [double](($points.WorkingSetBytes | Measure-Object -Maximum).Maximum)
        }
        privateBytes = [ordered]@{
            start = [double]$first.PrivateBytes
            end = [double]$last.PrivateBytes
            delta = [double]$last.PrivateBytes - [double]$first.PrivateBytes
            max = [double](($points.PrivateBytes | Measure-Object -Maximum).Maximum)
        }
        handles = [ordered]@{
            start = [double]$first.HandleCount
            end = [double]$last.HandleCount
            delta = [double]$last.HandleCount - [double]$first.HandleCount
            max = [double](($points.HandleCount | Measure-Object -Maximum).Maximum)
        }
        threads = [ordered]@{
            start = [double]$first.ThreadCount
            end = [double]$last.ThreadCount
            delta = [double]$last.ThreadCount - [double]$first.ThreadCount
            max = [double](($points.ThreadCount | Measure-Object -Maximum).Maximum)
        }
        readBytesPerSecond = Measure-Series $points.ReadBytesPerSecond
        writeBytesPerSecond = Measure-Series $points.WriteBytesPerSecond
        dedicatedGpuBytes = if ($gpuPoints.Count) {
            [ordered]@{
                start = [double]$firstGpu.DedicatedBytes
                end = [double]$lastGpu.DedicatedBytes
                delta = [double]$lastGpu.DedicatedBytes - [double]$firstGpu.DedicatedBytes
                max = [double](($gpuPoints.DedicatedBytes | Measure-Object -Maximum).Maximum)
            }
        } else { $null }
        sharedGpuBytes = if ($gpuPoints.Count) {
            [ordered]@{
                start = [double]$firstGpu.SharedBytes
                end = [double]$lastGpu.SharedBytes
                delta = [double]$lastGpu.SharedBytes - [double]$firstGpu.SharedBytes
                max = [double](($gpuPoints.SharedBytes | Measure-Object -Maximum).Maximum)
            }
        } else { $null }
    }
}

$trackedPoints = @()
foreach ($group in ($rolePoints | Group-Object TimestampUtc)) {
    $rows = @($group.Group)
    $trackedPoints += [pscustomobject]@{
        TimestampUtc = $rows[0].TimestampUtc
        ProcessCount = Get-Sum $rows 'ProcessCount'
        CpuPercentMachine = Get-SumOrNull $rows 'CpuPercentMachine'
        CpuPercentOneCore = Get-SumOrNull $rows 'CpuPercentOneCore'
        WorkingSetBytes = Get-Sum $rows 'WorkingSetBytes'
        PrivateBytes = Get-Sum $rows 'PrivateBytes'
        HandleCount = Get-Sum $rows 'HandleCount'
        ThreadCount = Get-Sum $rows 'ThreadCount'
    }
}
$trackedGpuPoints = @()
foreach ($group in ($gpuMemoryPoints | Group-Object TimestampUtc)) {
    $rows = @($group.Group)
    $trackedGpuPoints += [pscustomobject]@{
        TimestampUtc = $rows[0].TimestampUtc
        DedicatedBytes = Get-Sum $rows 'DedicatedBytes'
        SharedBytes = Get-Sum $rows 'SharedBytes'
    }
}
$trackedSummary = if ($trackedPoints.Count) {
    $points = @($trackedPoints | Sort-Object TimestampUtc)
    $gpuPoints = @($trackedGpuPoints | Sort-Object TimestampUtc)
    $first = $points[0]
    $last = $points[-1]
    $firstGpu = if ($gpuPoints.Count) { $gpuPoints[0] } else { $null }
    $lastGpu = if ($gpuPoints.Count) { $gpuPoints[-1] } else { $null }
    [ordered]@{
        roles = @($roleSummary.Keys)
        samples = $points.Count
        maxProcessCount = [int](($points.ProcessCount | Measure-Object -Maximum).Maximum)
        cpuPercentMachine = Measure-Series $points.CpuPercentMachine
        cpuPercentOneCore = Measure-Series $points.CpuPercentOneCore
        workingSetBytes = [ordered]@{
            start = [double]$first.WorkingSetBytes
            end = [double]$last.WorkingSetBytes
            delta = [double]$last.WorkingSetBytes - [double]$first.WorkingSetBytes
            max = [double](($points.WorkingSetBytes | Measure-Object -Maximum).Maximum)
        }
        privateBytes = [ordered]@{
            start = [double]$first.PrivateBytes
            end = [double]$last.PrivateBytes
            delta = [double]$last.PrivateBytes - [double]$first.PrivateBytes
            max = [double](($points.PrivateBytes | Measure-Object -Maximum).Maximum)
        }
        handles = [ordered]@{
            start = [double]$first.HandleCount
            end = [double]$last.HandleCount
            delta = [double]$last.HandleCount - [double]$first.HandleCount
            max = [double](($points.HandleCount | Measure-Object -Maximum).Maximum)
        }
        threads = [ordered]@{
            start = [double]$first.ThreadCount
            end = [double]$last.ThreadCount
            delta = [double]$last.ThreadCount - [double]$first.ThreadCount
            max = [double](($points.ThreadCount | Measure-Object -Maximum).Maximum)
        }
        dedicatedGpuBytes = if ($gpuPoints.Count) {
            [ordered]@{
                start = [double]$firstGpu.DedicatedBytes
                end = [double]$lastGpu.DedicatedBytes
                delta = [double]$lastGpu.DedicatedBytes - [double]$firstGpu.DedicatedBytes
                max = [double](($gpuPoints.DedicatedBytes | Measure-Object -Maximum).Maximum)
            }
        } else { $null }
        sharedGpuBytes = if ($gpuPoints.Count) {
            [ordered]@{
                start = [double]$firstGpu.SharedBytes
                end = [double]$lastGpu.SharedBytes
                delta = [double]$lastGpu.SharedBytes - [double]$firstGpu.SharedBytes
                max = [double](($gpuPoints.SharedBytes | Measure-Object -Maximum).Maximum)
            }
        } else { $null }
    }
} else { $null }

$gpuEnginePoints = @()
foreach ($group in ($gpuEngineRows | Group-Object { "$($_.TimestampUtc)|$($_.Role)|$($_.EngineType)" })) {
    $rows = @($group.Group)
    $gpuEnginePoints += [pscustomobject]@{
        TimestampUtc = $rows[0].TimestampUtc
        Role = $rows[0].Role
        EngineType = $rows[0].EngineType
        MaxEngineUtilizationPercent = [double](($rows.UtilizationPercent | Measure-Object -Maximum).Maximum)
        SumEngineUtilizationPercent = Get-Sum $rows 'UtilizationPercent'
    }
}
$gpuEngineSummary = @()
foreach ($group in ($gpuEnginePoints | Group-Object { "$($_.Role)|$($_.EngineType)" })) {
    $rows = @($group.Group)
    $gpuEngineSummary += [ordered]@{
        role = $rows[0].Role
        engineType = $rows[0].EngineType
        maxSingleEnginePercent = Measure-Series $rows.MaxEngineUtilizationPercent
        summedEnginePercent = Measure-Series $rows.SumEngineUtilizationPercent
    }
}

$systemSummary = if ($systemRows.Count) {
    [ordered]@{
        cpuPercent = Measure-Series $systemRows.CpuPercent
        availableMemoryBytes = Measure-Series $systemRows.AvailableMemoryBytes
        committedMemoryBytes = Measure-Series $systemRows.CommittedMemoryBytes
        pagesPerSecond = Measure-Series $systemRows.PagesPerSecond
        gpu3DMaxEnginePercent = Measure-Series $systemRows.Gpu3DMaxEnginePercent
        gpuCopyMaxEnginePercent = Measure-Series $systemRows.GpuCopyMaxEnginePercent
        gpuAnyMaxEnginePercent = Measure-Series $systemRows.GpuAnyMaxEnginePercent
    }
} else { $null }

$hardwareSummary = @()
foreach ($group in ($hardwareRows | Group-Object AdapterIndex)) {
    $rows = @($group.Group)
    $hardwareSummary += [ordered]@{
        adapterIndex = [int]$rows[0].AdapterIndex
        adapterName = $rows[0].AdapterName
        gpuUtilizationPercent = Measure-Series $rows.GpuUtilizationPercent
        memoryControllerUtilizationPercent = Measure-Series $rows.MemoryControllerUtilizationPercent
        vramUsedMiB = Measure-Series $rows.VramUsedMiB
        powerWatts = Measure-Series $rows.PowerWatts
        temperatureC = Measure-Series $rows.TemperatureC
        graphicsClockMHz = Measure-Series $rows.GraphicsClockMHz
        memoryClockMHz = Measure-Series $rows.MemoryClockMHz
    }
}

$presentSummary = $null
if ($presentRows.Count) {
    $headers = @($presentRows[0].PSObject.Properties.Name)
    $allRows = $presentRows.Count
    $primarySwapChain = $null
    if ($headers -contains 'SwapChainAddress') {
        $displayedColumn = if ($headers -contains 'DisplayedTime') {
            'DisplayedTime'
        } elseif ($headers -contains 'MsDisplayedTime') {
            'MsDisplayedTime'
        } else { $null }
        $swapChains = @($presentRows | Group-Object SwapChainAddress | ForEach-Object {
            $rows = @($_.Group)
            $displayed = if ($displayedColumn) {
                @($rows | Where-Object {
                    $value = Convert-ToNumber (Get-PropertyValue $_ $displayedColumn)
                    $null -ne $value -and $value -gt 0
                }).Count
            } else { 0 }
            [pscustomobject]@{
                address = [string]$_.Name
                rows = $rows
                rowCount = $rows.Count
                displayedCount = $displayed
            }
        } | Sort-Object -Property @{ Expression = 'displayedCount'; Descending = $true },
            @{ Expression = 'rowCount'; Descending = $true }, @{ Expression = 'address'; Descending = $false })
        if ($swapChains.Count) {
            $primarySwapChain = $swapChains[0].address
            $presentRows = @($swapChains[0].rows)
        }
    }
    $overall = Measure-PresentRows -Rows $presentRows -Headers $headers
    $applicationRows = if ($headers -contains 'FrameType') {
        @($presentRows | Where-Object {
            [string]::IsNullOrWhiteSpace([string]$_.FrameType) -or
            $_.FrameType -in @('Application', 'Unknown', 'NotSet', 'Unspecified')
        })
    } else { $presentRows }
    $generatedRows = if ($headers -contains 'FrameType') {
        @($presentRows | Where-Object {
            -not [string]::IsNullOrWhiteSpace([string]$_.FrameType) -and
            $_.FrameType -notin @('Application', 'Unknown', 'NotSet', 'Unspecified')
        })
    } else { @() }
    $frameTypes = [ordered]@{}
    if ($headers -contains 'FrameType') {
        foreach ($group in ($presentRows | Group-Object FrameType)) {
            $name = if ([string]::IsNullOrWhiteSpace([string]$group.Name)) { 'Unspecified' } else { [string]$group.Name }
            $frameTypes[$name] = Measure-PresentRows -Rows @($group.Group) -Headers $headers
        }
    } else {
        $frameTypes['Unspecified'] = $overall
    }
    $displayedColumn = if ($headers -contains 'DisplayedTime') {
        'DisplayedTime'
    } elseif ($headers -contains 'MsDisplayedTime') {
        'MsDisplayedTime'
    } else { $null }
    $displayedStats = if ($displayedColumn) {
        Measure-Series ($presentRows | ForEach-Object { Get-PropertyValue $_ $displayedColumn })
    } else { $null }
    $application = Measure-PresentRows -Rows $applicationRows -Headers $headers
    $generated = Measure-PresentRows -Rows $generatedRows -Headers $headers
    $presentSummary = [ordered]@{
        rows = $overall.rows
        allRows = $allRows
        auxiliaryRows = $allRows - $overall.rows
        primarySwapChain = $primarySwapChain
        dropped = $overall.dropped
        averageFps = if ($application) { $application.averageFps } else { $null }
        onePercentLowFps = if ($application) { $application.onePercentLowFps } else { $null }
        displayedAverageFps = if ($displayedStats -and $displayedStats.mean -gt 0) { 1000.0 / $displayedStats.mean } else { $null }
        metricsMilliseconds = if ($application) { $application.metricsMilliseconds } else { [ordered]@{} }
        application = $application
        generated = $generated
        frameTypes = $frameTypes
    }
}

$fixtureSummary = if ($fixtureRows.Count) {
    $latest = $fixtureRows[-1]
    $fixtureDpr = Convert-ToNumber $latest.viewport.devicePixelRatio
    $fixtureRasterWidth = if ($null -ne $fixtureDpr) {
        [int][math]::Round([double]$latest.viewport.width * $fixtureDpr)
    } else { $null }
    $fixtureRasterHeight = if ($null -ne $fixtureDpr) {
        [int][math]::Round([double]$latest.viewport.height * $fixtureDpr)
    } else { $null }
    [ordered]@{
        reports = $fixtureRows.Count
        framework = [string]$latest.framework
        provider = [string]$latest.provider
        scenario = [string]$latest.scenario
        fixtureHash = [string]$latest.fixtureHash
        viewport = $latest.viewport
        effectiveRaster = if ($null -ne $fixtureRasterWidth) {
            [ordered]@{
                width = $fixtureRasterWidth
                height = $fixtureRasterHeight
                megapixels = ($fixtureRasterWidth * $fixtureRasterHeight) / 1000000.0
            }
        } else { $null }
        rafFps = Measure-Series ($fixtureRows | ForEach-Object { $_.raf.fpsThisInterval })
        workP95Milliseconds = Measure-Series ($fixtureRows | ForEach-Object { $_.workload.workP95Milliseconds })
        workP99Milliseconds = Measure-Series ($fixtureRows | ForEach-Object { $_.workload.workP99Milliseconds })
        rafCallbacks = $latest.raf.callbacks
        workloadTicks = $latest.workload.ticks
        intervalsOver20ms = $latest.raf.intervalsOver20ms
        intervalsOver33ms = $latest.raf.intervalsOver33ms
        intervalsOver50ms = $latest.raf.intervalsOver50ms
        longTasks = $latest.longTasks
        frameworkCounters = $latest.frameworkCounters
    }
} else { $null }

$derived = $null
if ($trackedSummary) {
    $cpuMean = Get-Statistic $trackedSummary.cpuPercentOneCore 'mean'
    $applicationFps = if ($presentSummary -and $presentSummary.application -and
        $presentSummary.application.averageFps -gt 0) {
        [double]$presentSummary.application.averageFps
    } else { $null }
    $resolutionText = if ($benchmark) { [string]$benchmark.resolution } else { '' }
    $megapixels = $null
    $pixelWidth = $null
    $pixelHeight = $null
    if ($resolutionText -match '^(?<w>\d+)x(?<h>\d+)$') {
        $pixelWidth = [double]$Matches.w
        $pixelHeight = [double]$Matches.h
        $megapixels = ($pixelWidth * $pixelHeight) / 1000000.0
    }
    $coreMillisecondsPerSecond = if ($null -ne $cpuMean) { [double]$cpuMean * 10.0 } else { $null }
    $coreMillisecondsPerFrame = if ($null -ne $coreMillisecondsPerSecond -and $null -ne $applicationFps) {
        $coreMillisecondsPerSecond / $applicationFps
    } else { $null }
    $derived = [ordered]@{
        applicationFps = $applicationFps
        trackedCpuCoreMillisecondsPerSecond = $coreMillisecondsPerSecond
        trackedCpuCoreMillisecondsPerApplicationFrame = $coreMillisecondsPerFrame
        resolutionMegapixels = $megapixels
        trackedCpuCoreMillisecondsPerFramePerMegapixel = if ($null -ne $coreMillisecondsPerFrame -and $megapixels -gt 0) {
            $coreMillisecondsPerFrame / $megapixels
        } else { $null }
        carbonTheoreticalVisibleBgraBytesPerFrame = if ($framework -eq 'CarbonUI' -and $null -ne $pixelWidth) {
            [uint64]($pixelWidth * $pixelHeight * 4.0)
        } else { $null }
    }
}

$frameRateValidation = $null
$frameRateProperty = if ($benchmark) {
    $benchmark.PSObject.Properties['frameRateMode']
} else { $null }
if ($frameRateProperty -and $presentSummary -and $presentSummary.application) {
    $mode = [string]$frameRateProperty.Value
    $fps = Convert-ToNumber $presentSummary.application.averageFps
    $target = switch ($mode) { 'Fixed60' { 60.0 } 'Fixed120' { 120.0 } default { $null } }
    $tolerance = if ($target) { [math]::Max(3.0, $target * 0.05) } else { $null }
    $frameRateValidation = [ordered]@{
        mode = $mode
        targetFps = $target
        measuredApplicationFps = $fps
        toleranceFps = $tolerance
        valid = if ($target -and $null -ne $fps) {
            [math]::Abs($fps - $target) -le $tolerance
        } elseif ($mode -eq 'Uncapped') { $true } else { $false }
    }
}

$summary = [ordered]@{
    schemaVersion = 3
    captureDirectory = $capture
    label = $manifest.label
    framework = $framework
    manifest = $manifest
    roles = $roleSummary
    trackedProcesses = $trackedSummary
    gpuEngines = $gpuEngineSummary
    system = $systemSummary
    hardware = $hardwareSummary
    presentMon = $presentSummary
    fixture = $fixtureSummary
    derived = $derived
    frameRateValidation = $frameRateValidation
}
$summaryPath = Join-Path $capture 'summary.json'
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $summaryPath -Encoding utf8NoBOM

$markdown = [Collections.Generic.List[string]]::new()
$markdown.Add("# $framework performance capture: $($manifest.label)")
$markdown.Add('')
$markdown.Add("- Capture: ``$capture``")
$markdown.Add("- Status: $($manifest.status)")
$markdown.Add("- Requested/actual duration: $($manifest.requestedDurationSeconds) s / $($manifest.actualDurationSeconds) s")
$markdown.Add("- WPR profile: $($manifest.wprProfile)")
$markdown.Add("- PresentMon: $(if ($presentSummary) { 'captured' } else { 'not captured' })")
if ($benchmark) {
    $rateMode = if ($benchmark.PSObject.Properties['frameRateMode']) { $benchmark.frameRateMode } else { 'Unknown' }
    $markdown.Add("- Condition: $($benchmark.scenario), $($benchmark.resolution), $rateMode, Frame Generation $($benchmark.frameGeneration)")
    if ($benchmark.PSObject.Properties['rasterizationPolicy']) {
        $markdown.Add("- Rasterization policy: $($benchmark.rasterizationPolicy)")
    }
}
if ($fixtureSummary) {
    $markdown.Add("- Fixture: $($fixtureSummary.fixtureHash) ($($fixtureSummary.viewport.width)x$($fixtureSummary.viewport.height), $($fixtureSummary.reports) live reports)")
    if ($fixtureSummary.effectiveRaster) {
        $markdown.Add("- Effective fixture raster: $($fixtureSummary.effectiveRaster.width)x$($fixtureSummary.effectiveRaster.height) (DPR $(Format-Number $fixtureSummary.viewport.devicePixelRatio 3))")
    }
}
if ($manifest.notes) { $markdown.Add("- Notes: $($manifest.notes)") }
$markdown.Add('')
$markdown.Add('## Process roles')
$markdown.Add('')
$markdown.Add('| Role | CPU mean / p95 (% machine) | Working set max / delta (MiB) | Private bytes max / delta (MiB) | VRAM max / delta (MiB) | Handles delta | Threads delta |')
$markdown.Add('|---|---:|---:|---:|---:|---:|---:|')
foreach ($role in $roleSummary.Keys) {
    $item = $roleSummary[$role]
    $vram = $item.dedicatedGpuBytes
    $markdown.Add('| {0} | {1} / {2} | {3} / {4} | {5} / {6} | {7} / {8} | {9} | {10} |' -f @(
        $role
        (Format-Number (Get-Statistic $item.cpuPercentMachine 'mean'))
        (Format-Number (Get-Statistic $item.cpuPercentMachine 'p95'))
        (Format-Number ($item.workingSetBytes.max / 1MB))
        (Format-Number ($item.workingSetBytes.delta / 1MB))
        (Format-Number ($item.privateBytes.max / 1MB))
        (Format-Number ($item.privateBytes.delta / 1MB))
        $(if ($vram) { Format-Number ($vram.max / 1MB) } else { 'n/a' })
        $(if ($vram) { Format-Number ($vram.delta / 1MB) } else { 'n/a' })
        (Format-Number $item.handles.delta 0)
        (Format-Number $item.threads.delta 0)
    ))
}
$markdown.Add('')
if ($framework -eq 'OSFUI') {
    $markdown.Add('`Game` includes Starfield and the in-process OSFUI.dll. `OSFUIHost` is the native WGC/D3D11 transport host; `WebView2` contains only its descendant browser processes.')
} elseif ($framework -eq 'CarbonUI') {
    $markdown.Add('CarbonUI.dll, CarbonUICore.dll, Ultralight, and WebCore run inside `Game`; process sampling cannot split their resource use from Starfield.')
} else {
    $markdown.Add('`Game` is the no-framework Starfield baseline. OSF UI and Carbon UI were both rejected by capture preflight if their DLLs were loaded.')
}
if ($trackedSummary) {
    $markdown.Add(('Tracked total: {0} process(es) max, {1} MiB private bytes max, {2} MiB dedicated GPU memory max.' -f @(
        $trackedSummary.maxProcessCount
        (Format-Number ($trackedSummary.privateBytes.max / 1MB))
        $(if ($trackedSummary.dedicatedGpuBytes) { Format-Number ($trackedSummary.dedicatedGpuBytes.max / 1MB) } else { 'n/a' })
    )))
}

if ($derived) {
    $markdown.Add('')
    $markdown.Add('## Normalized CPU cost')
    $markdown.Add('')
    $markdown.Add("- Tracked CPU: $(Format-Number $derived.trackedCpuCoreMillisecondsPerSecond) core-ms/s")
    $markdown.Add("- Tracked CPU per application frame: $(Format-Number $derived.trackedCpuCoreMillisecondsPerApplicationFrame 3) core-ms/frame")
    $markdown.Add("- Tracked CPU per application frame per megapixel: $(Format-Number $derived.trackedCpuCoreMillisecondsPerFramePerMegapixel 3) core-ms/frame/MP")
    $markdown.Add('')
    $markdown.Add('These are absolute Starfield-plus-framework values. The comparison report subtracts the matched no-framework baseline before making a framework claim.')
    if ($null -ne $derived.carbonTheoreticalVisibleBgraBytesPerFrame) {
        $markdown.Add("Carbon full-surface BGRA size: $(Format-Number ($derived.carbonTheoreticalVisibleBgraBytesPerFrame / 1MB)) MiB/frame (theoretical path volume, not a measured bandwidth counter).")
    }
}

if ($fixtureSummary) {
    $markdown.Add('')
    $markdown.Add('## Fixture cadence')
    $markdown.Add('')
    $markdown.Add("- Page RAF mean / p95: $(Format-Number (Get-Statistic $fixtureSummary.rafFps 'mean')) / $(Format-Number (Get-Statistic $fixtureSummary.rafFps 'p95')) FPS")
    $markdown.Add("- RAF intervals over 20 / 33 / 50 ms: $($fixtureSummary.intervalsOver20ms) / $($fixtureSummary.intervalsOver33ms) / $($fixtureSummary.intervalsOver50ms)")
    $markdown.Add("- Work function p95 / p99: $(Format-Number (Get-Statistic $fixtureSummary.workP95Milliseconds 'mean') 3) / $(Format-Number (Get-Statistic $fixtureSummary.workP99Milliseconds 'mean') 3) ms")
    $markdown.Add('')
    $markdown.Add('RAF/workload values are directly observed inside the identical page. Renderer/upload/publish/consume counters remain unavailable where a stock framework public API does not expose them.')
}

if ($gpuEngineSummary.Count) {
    $markdown.Add('')
    $markdown.Add('## GPU engines')
    $markdown.Add('')
    $markdown.Add('| Role | Engine | Mean / p95 / max single-engine occupancy (%) |')
    $markdown.Add('|---|---|---:|')
    foreach ($item in $gpuEngineSummary | Sort-Object role, engineType) {
        $stats = $item.maxSingleEnginePercent
        $markdown.Add('| {0} | {1} | {2} / {3} / {4} |' -f @(
            $item.role
            $item.engineType
            (Format-Number (Get-Statistic $stats 'mean'))
            (Format-Number (Get-Statistic $stats 'p95'))
            (Format-Number (Get-Statistic $stats 'max'))
        ))
    }
    $markdown.Add('')
    $markdown.Add('Single-engine occupancy is used because summing independent GPU engines can exceed 100%. Inspect the ETL GPU queues to separate WGC copy work from the Starfield compositor pass.')
}

if ($presentSummary) {
    $markdown.Add('')
    $markdown.Add('## Frame pacing')
    $markdown.Add('')
    $markdown.Add("- Captured rows: $($presentSummary.rows)$(if ($null -ne $presentSummary.dropped) { "; not displayed: $($presentSummary.dropped)" } else { '' })")
    $markdown.Add("- Application FPS / approximate 1% low: $(Format-Number $presentSummary.application.averageFps) / $(Format-Number $presentSummary.application.onePercentLowFps)")
    $markdown.Add("- Displayed FPS: $(Format-Number $presentSummary.displayedAverageFps)")
    $markdown.Add("- Generated-frame rows: $(if ($presentSummary.generated) { $presentSummary.generated.rows } else { 0 })")
    if ($frameRateValidation -and -not $frameRateValidation.valid) {
        $markdown.Add("- **Invalid fixed-rate run:** measured $(Format-Number $frameRateValidation.measuredApplicationFps) FPS; expected $($frameRateValidation.targetFps) ± $(Format-Number $frameRateValidation.toleranceFps) FPS.")
    }
    $markdown.Add('')
    $markdown.Add('| Frame type | Rows | Average FPS |')
    $markdown.Add('|---|---:|---:|')
    foreach ($frameType in $presentSummary.frameTypes.Keys) {
        $item = $presentSummary.frameTypes[$frameType]
        $markdown.Add("| $frameType | $($item.rows) | $(Format-Number $item.averageFps) |")
    }
    $markdown.Add('')
    $markdown.Add('| Metric (ms) | p50 | p95 | p99 | max |')
    $markdown.Add('|---|---:|---:|---:|---:|')
    foreach ($metric in $presentSummary.metricsMilliseconds.Keys) {
        $stats = $presentSummary.metricsMilliseconds[$metric]
        if (-not $stats) { continue }
        $markdown.Add('| {0} | {1} | {2} | {3} | {4} |' -f @(
            $metric
            (Format-Number (Get-Statistic $stats 'p50'))
            (Format-Number (Get-Statistic $stats 'p95'))
            (Format-Number (Get-Statistic $stats 'p99'))
            (Format-Number (Get-Statistic $stats 'max'))
        ))
    }
}

if ($hardwareSummary.Count) {
    $markdown.Add('')
    $markdown.Add('## Hardware telemetry')
    $markdown.Add('')
    $markdown.Add('| Adapter | GPU mean / p95 / max (%) | VRAM max (MiB) | Power p95 / max (W) | Max temp (C) |')
    $markdown.Add('|---|---:|---:|---:|---:|')
    foreach ($item in $hardwareSummary) {
        $markdown.Add('| {0} | {1} / {2} / {3} | {4} | {5} / {6} | {7} |' -f @(
            $item.adapterName
            (Format-Number (Get-Statistic $item.gpuUtilizationPercent 'mean'))
            (Format-Number (Get-Statistic $item.gpuUtilizationPercent 'p95'))
            (Format-Number (Get-Statistic $item.gpuUtilizationPercent 'max'))
            (Format-Number (Get-Statistic $item.vramUsedMiB 'max'))
            (Format-Number (Get-Statistic $item.powerWatts 'p95'))
            (Format-Number (Get-Statistic $item.powerWatts 'max'))
            (Format-Number (Get-Statistic $item.temperatureC 'max') 0)
        ))
    }
}

$markdown.Add('')
$markdown.Add('## Interpretation boundary')
$markdown.Add('')
$markdown.Add('Process sampling can isolate OSF UI''s out-of-process browser path, but both OSFUI.dll and all Carbon UI engine modules contribute inside Starfield. Use matched baseline deltas for headline comparisons. When WPR is enabled, cpu-profile-by-module.txt and cpu-profile-ui-modules.txt are exported automatically; WPA remains available for stack- and time-range-level investigation.')
$markdown.Add('')
$markdown.Add('Build, deployment, and this script itself do not establish an in-game performance result; the capture is the evidence.')

$markdownPath = Join-Path $capture 'summary.md'
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8NoBOM
Write-Host "Summary: $markdownPath" -ForegroundColor Green
