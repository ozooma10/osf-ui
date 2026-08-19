#requires -Version 7.2
<#
.SYNOPSIS
  Summarize an OSF UI profiling capture into JSON and Markdown.
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

$capture = (Resolve-Path -LiteralPath $CaptureDirectory).Path
$manifestPath = Join-Path $capture 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Capture manifest not found: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$processRows = @(Import-CsvIfPresent (Join-Path $capture 'process-samples.csv'))
$gpuEngineRows = @(Import-CsvIfPresent (Join-Path $capture 'gpu-engine-samples.csv'))
$gpuMemoryRows = @(Import-CsvIfPresent (Join-Path $capture 'gpu-memory-samples.csv'))
$systemRows = @(Import-CsvIfPresent (Join-Path $capture 'system-samples.csv'))
$hardwareRows = @(Import-CsvIfPresent (Join-Path $capture 'hardware-samples.csv'))
$presentRows = @(Import-CsvIfPresent (Join-Path $capture 'presentmon.csv'))

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
    $frameColumn = if ($headers -contains 'FrameTime') { 'FrameTime' } elseif ($headers -contains 'MsBetweenPresents') { 'MsBetweenPresents' } else { $null }
    $frameStats = if ($frameColumn) { Measure-Series ($presentRows | ForEach-Object { $_.$frameColumn }) } else { $null }
    $metrics = [ordered]@{}
    foreach ($metric in @('FrameTime', 'CPUBusy', 'CPUWait', 'GPULatency', 'GPUTime', 'GPUBusy', 'GPUWait', 'DisplayLatency', 'DisplayedTime', 'MsBetweenPresents', 'MsBetweenDisplayChange', 'MsRenderPresentLatency', 'MsUntilDisplayed')) {
        if ($headers -contains $metric) {
            $metrics[$metric] = Measure-Series ($presentRows | ForEach-Object { $_.$metric })
        }
    }
    $dropped = if ($headers -contains 'Dropped') {
        @($presentRows | Where-Object { $_.Dropped -eq '1' -or $_.Dropped -eq 'true' }).Count
    } else { $null }
    $presentSummary = [ordered]@{
        rows = $presentRows.Count
        dropped = $dropped
        averageFps = if ($frameStats -and $frameStats.mean -gt 0) { 1000.0 / $frameStats.mean } else { $null }
        onePercentLowFps = if ($frameStats -and $frameStats.p99 -gt 0) { 1000.0 / $frameStats.p99 } else { $null }
        metricsMilliseconds = $metrics
    }
}

$summary = [ordered]@{
    schemaVersion = 1
    captureDirectory = $capture
    label = $manifest.label
    manifest = $manifest
    roles = $roleSummary
    gpuEngines = $gpuEngineSummary
    system = $systemSummary
    hardware = $hardwareSummary
    presentMon = $presentSummary
}
$summaryPath = Join-Path $capture 'summary.json'
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $summaryPath -Encoding utf8NoBOM

$markdown = [Collections.Generic.List[string]]::new()
$markdown.Add("# OSF UI performance capture: $($manifest.label)")
$markdown.Add('')
$markdown.Add("- Capture: ``$capture``")
$markdown.Add("- Status: $($manifest.status)")
$markdown.Add("- Requested/actual duration: $($manifest.requestedDurationSeconds) s / $($manifest.actualDurationSeconds) s")
$markdown.Add("- WPR profile: $($manifest.wprProfile)")
$markdown.Add("- PresentMon: $(if ($presentSummary) { 'captured' } else { 'not captured' })")
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
$markdown.Add('`Game` includes Starfield and the in-process OSFUI.dll. `OSFUIHost` is the native WGC/D3D11 transport host; `WebView2` contains only its descendant browser processes.')

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
    $markdown.Add("- Frames: $($presentSummary.rows)$(if ($null -ne $presentSummary.dropped) { "; dropped: $($presentSummary.dropped)" } else { '' })")
    $markdown.Add("- Average FPS: $(Format-Number $presentSummary.averageFps)")
    $markdown.Add("- Approximate 1% low FPS: $(Format-Number $presentSummary.onePercentLowFps)")
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
$markdown.Add('Process sampling can isolate the out-of-process browser path, but it cannot subtract OSFUI.dll from Starfield. Use the matching ETL in WPA (`CPU Usage (Sampled)` grouped by Process > Module > Stack, plus GPU Usage queues) and compare the same scene with only one OSF UI state changed.')
$markdown.Add('')
$markdown.Add('Build, deployment, and this script itself do not establish an in-game performance result; the capture is the evidence.')

$markdownPath = Join-Path $capture 'summary.md'
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8NoBOM
Write-Host "Summary: $markdownPath" -ForegroundColor Green
