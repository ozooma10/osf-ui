<#
.SYNOPSIS
    Out-of-process resource sampler for the OSF UI renderer benchmark
    (docs/renderer-benchmark.md).

.DESCRIPTION
    Waits for Starfield.exe, then samples once per interval until it exits:
      - Starfield: CPU % (of whole machine), working set, private bytes,
        GPU engine utilization %, dedicated GPU memory
      - The OSF UI-owned msedgewebview2.exe process tree (user-data-dir
        contains \OSFUI\WebView2): same columns, summed over the tree
        (always 0/empty for the Ultralight backend — that cost is in-process)
    On game exit it snapshots "OSF UI.log" next to the CSV so the run's
    internal Bench: lines and the external samples stay paired.

.EXAMPLE
    .\Sample-OsfUiBench.ps1 -Label webview2
#>
[CmdletBinding()]
param(
    [string]$Label = 'run',
    [string]$OutDir = (Join-Path $PSScriptRoot 'results'),
    [double]$IntervalSec = 1.0,
    [int]$WaitForGameMinutes = 15
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csvPath = Join-Path $OutDir "$stamp-$Label.csv"
$logCopy = Join-Path $OutDir "$stamp-$Label.log"
$osfLog = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Starfield\SFSE\Logs\OSF UI.log'
$cores = [Environment]::ProcessorCount

Write-Host "Waiting for Starfield.exe (up to $WaitForGameMinutes min)..."
$deadline = (Get-Date).AddMinutes($WaitForGameMinutes)
$game = $null
while (-not $game -and (Get-Date) -lt $deadline) {
    $game = Get-Process -Name Starfield -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) { Start-Sleep -Milliseconds 500 }
}
if (-not $game) { throw 'Starfield.exe never appeared.' }
Write-Host "Sampling PID $($game.Id) -> $csvPath (Ctrl+C to stop early)"

# GPU perf-counter instances are keyed "pid_<id>_luid_..._<engine>".
function Get-GpuByPid {
    param([hashtable]$Table, [string]$CounterSet, [string]$CounterName)
    try {
        $samples = (Get-Counter "\$CounterSet(*)\$CounterName" -ErrorAction Stop).CounterSamples
    } catch { return }
    foreach ($s in $samples) {
        if ($s.InstanceName -match '^pid_(\d+)_') {
            $id = [int]$Matches[1]
            $Table[$id] = [double]$Table[$id] + $s.CookedValue
        }
    }
}

'time,phasePid,sfCpuPct,sfWsMB,sfPrivMB,sfGpuPct,sfGpuMemMB,wvCount,wvCpuPct,wvWsMB,wvPrivMB,wvGpuPct,wvGpuMemMB' |
    Set-Content $csvPath

$prev = @{}   # pid -> [TotalProcessorTime ticks, wall time]
$wvPids = @()
$wvRefreshed = [DateTime]::MinValue

while ($true) {
    $loopStart = Get-Date
    $game = Get-Process -Name Starfield -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) { break }

    # Refresh the OSF UI WebView2 process set every 5 s (CIM query is slow):
    # the out-of-process host exe plus every msedgewebview2 whose command line
    # references the OSFUI folders (excludes other apps' WebView2 runtimes).
    if (($loopStart - $wvRefreshed).TotalSeconds -ge 5) {
        $wvPids = @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe' OR Name='osfui_webview2_host.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -eq 'osfui_webview2_host.exe' -or $_.CommandLine -match 'OSFUI' } |
            Select-Object -ExpandProperty ProcessId)
        $wvRefreshed = $loopStart
    }

    $gpuUtil = @{}; $gpuMem = @{}
    Get-GpuByPid -Table $gpuUtil -CounterSet 'GPU Engine' -CounterName 'Utilization Percentage'
    Get-GpuByPid -Table $gpuMem  -CounterSet 'GPU Process Memory' -CounterName 'Dedicated Usage'

    $now = Get-Date
    function CpuPct([System.Diagnostics.Process]$p) {
        if (-not $p) { return 0 }
        try { $cpuTicks = $p.TotalProcessorTime.Ticks } catch { return 0 }
        $entry = $prev[$p.Id]
        $prev[$p.Id] = @($cpuTicks, $now)
        if (-not $entry) { return 0 }
        $wallTicks = ($now - $entry[1]).Ticks
        if ($wallTicks -le 0) { return 0 }
        [Math]::Round(100.0 * ($cpuTicks - $entry[0]) / $wallTicks / $cores, 2)
    }

    $sfCpu = CpuPct $game
    $sfWs = [Math]::Round($game.WorkingSet64 / 1MB, 1)
    $sfPriv = [Math]::Round($game.PrivateMemorySize64 / 1MB, 1)
    $sfGpu = [Math]::Round([double]$gpuUtil[$game.Id], 2)
    $sfGpuMem = [Math]::Round([double]$gpuMem[$game.Id] / 1MB, 1)

    $wvCpu = 0.0; $wvWs = 0.0; $wvPriv = 0.0; $wvGpu = 0.0; $wvGpuMem = 0.0; $wvCount = 0
    foreach ($id in $wvPids) {
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $p) { continue }
        $wvCount++
        $wvCpu += CpuPct $p
        $wvWs += $p.WorkingSet64 / 1MB
        $wvPriv += $p.PrivateMemorySize64 / 1MB
        $wvGpu += [double]$gpuUtil[$id]
        $wvGpuMem += [double]$gpuMem[$id] / 1MB
    }

    ('{0:yyyy-MM-dd HH:mm:ss.fff},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12}' -f
        $now, $game.Id, $sfCpu, $sfWs, $sfPriv, $sfGpu, $sfGpuMem, $wvCount,
        [Math]::Round($wvCpu, 2), [Math]::Round($wvWs, 1), [Math]::Round($wvPriv, 1),
        [Math]::Round($wvGpu, 2), [Math]::Round($wvGpuMem, 1)) | Add-Content $csvPath

    $sleepMs = [int](($IntervalSec * 1000) - ((Get-Date) - $loopStart).TotalMilliseconds)
    if ($sleepMs -gt 0) { Start-Sleep -Milliseconds $sleepMs }
}

Write-Host 'Game exited; snapshotting OSF UI.log'
if (Test-Path $osfLog) { Copy-Item $osfLog $logCopy }
# Orphan check: the host exe and OSF UI-owned WebView2 processes must die
# with the game.
Start-Sleep -Seconds 3
$orphans = @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe' OR Name='osfui_webview2_host.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -eq 'osfui_webview2_host.exe' -or $_.CommandLine -match 'OSFUI' })
Add-Content $csvPath ("# orphans_after_exit={0}" -f $orphans.Count)
Write-Host "Done: $csvPath (orphans after exit: $($orphans.Count))"
