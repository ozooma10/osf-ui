#requires -Version 7.2
#requires -RunAsAdministrator
<#
.SYNOPSIS
  Capture OSF UI process, CPU, memory, GPU, and frame-pacing evidence.

.DESCRIPTION
  Samples Starfield, the OSF UI browser host, and only the WebView2 processes
  descended from that host. Optionally records a CPU/GPU WPR trace and a
  PresentMon CSV. Captures are written beneath build/profiles/osfui by default.

.PARAMETER Label
  Short scenario name such as baseline-never-opened, overlay-hidden, or
  settings-visible-1440p.

.PARAMETER WprProfile
  CpuGpu records symbolizable sampled CPU stacks plus GPU activity. General
  adds first-level triage providers. None is intended for long memory soaks.

.PARAMETER PresentMonPath
  Optional PresentMon console executable. When omitted, the script checks PATH
  and external/presentmon.

.EXAMPLE
  .\Capture-OSFUI.ps1 -Label baseline-never-opened -DurationSeconds 60

.EXAMPLE
  .\Capture-OSFUI.ps1 -Label lifecycle-soak -DurationSeconds 1800 `
    -IntervalSeconds 5 -WprProfile None
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Label,

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

    [string] $OutputRoot,

    [string] $Notes = '',

    [switch] $NoHardwareTelemetry,

    [switch] $OpenWpa
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-ToSafeLabel([string] $Value)
{
    $safe = ($Value -replace '[^A-Za-z0-9._-]', '-').Trim('-')
    if (-not $safe) { return 'capture' }
    return $safe
}

function Convert-ToUInt64($Value)
{
    if ($null -eq $Value) { return [uint64]0 }
    return [uint64]$Value
}

function Convert-ToDoubleOrNull($Value)
{
    if ($null -eq $Value) { return $null }
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

function Export-CaptureCsv([Collections.IEnumerable] $Rows, [string] $Path)
{
    $items = @($Rows)
    if ($items.Count -gt 0) {
        $items | Export-Csv -LiteralPath $Path -NoTypeInformation
    }
}

function Get-OSFUIProcessInventory([int] $TargetGamePid)
{
    $processes = @(Get-CimInstance Win32_Process -Filter (
        "Name='Starfield.exe' OR Name='osfui_webview2_host.exe' OR Name='msedgewebview2.exe'"))

    $roles = @{}
    $roles[$TargetGamePid] = 'Game'

    foreach ($process in $processes) {
        if ($process.Name -ieq 'osfui_webview2_host.exe' -and
            $process.CommandLine -match "(?:^|\s)--game-pid=$TargetGamePid(?:\s|$)") {
            $roles[[int]$process.ProcessId] = 'OSFUIHost'
        }
    }

    do {
        $added = $false
        foreach ($process in $processes) {
            $processId = [int]$process.ProcessId
            $parentPid = [int]$process.ParentProcessId
            if ($roles.ContainsKey($processId) -or -not $roles.ContainsKey($parentPid)) {
                continue
            }
            if ($process.Name -ieq 'msedgewebview2.exe') {
                $roles[$processId] = 'WebView2'
                $added = $true
            }
        }
    } while ($added)

    foreach ($process in $processes) {
        $processId = [int]$process.ProcessId
        if ($roles.ContainsKey($processId)) {
            [pscustomobject]@{
                Process = $process
                Role = $roles[$processId]
            }
        }
    }
}

function Get-PresentMonExecutable([string] $RequestedPath, [string] $Repository)
{
    if ($RequestedPath) {
        $resolved = Resolve-Path -LiteralPath $RequestedPath -ErrorAction Stop
        return $resolved.Path
    }

    $command = Get-Command 'PresentMon*.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) { return $command.Source }

    $external = Join-Path $Repository 'external\presentmon'
    if (Test-Path -LiteralPath $external) {
        $candidate = Get-ChildItem -LiteralPath $external -Filter 'PresentMon*.exe' -File |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($candidate) { return $candidate.FullName }
    }
    return $null
}

function Get-CounterValue($Samples, [string] $Suffix)
{
    $match = $Samples | Where-Object { $_.Path.EndsWith($Suffix, [StringComparison]::OrdinalIgnoreCase) } |
        Select-Object -First 1
    if ($match) { return [double]$match.CookedValue }
    return $null
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$safeLabel = Convert-ToSafeLabel $Label
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $repo 'build\profiles\osfui'
}

$games = @(Get-Process -Name Starfield -ErrorAction SilentlyContinue)
if ($GamePid -ne 0) {
    $games = @($games | Where-Object Id -eq $GamePid)
}
if ($games.Count -ne 1) {
    throw "Expected exactly one running Starfield process; found $($games.Count)."
}
$game = $games[0]
$GamePid = $game.Id

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$captureDir = Join-Path $OutputRoot "$timestamp-$safeLabel"
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null

$processCsv = Join-Path $captureDir 'process-samples.csv'
$gpuEngineCsv = Join-Path $captureDir 'gpu-engine-samples.csv'
$gpuMemoryCsv = Join-Path $captureDir 'gpu-memory-samples.csv'
$systemCsv = Join-Path $captureDir 'system-samples.csv'
$hardwareCsv = Join-Path $captureDir 'hardware-samples.csv'
$presentCsv = Join-Path $captureDir 'presentmon.csv'
$etl = Join-Path $captureDir 'trace.etl'
$manifestPath = Join-Path $captureDir 'manifest.json'

$wpr = if ($WprProfile -ne 'None') { Get-Command wpr.exe -ErrorAction Stop } else { $null }
$wpa = Get-Command wpa.exe -ErrorAction SilentlyContinue
$presentMon = Get-PresentMonExecutable $PresentMonPath $repo
$presentMonMode = if ($presentMon) { 'live' } else { $null }
$nvidiaSmi = if (-not $NoHardwareTelemetry) {
    Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue | Select-Object -First 1
} else { $null }

$logicalProcessors = [Environment]::ProcessorCount
$os = Get-CimInstance Win32_OperatingSystem
$processors = @(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name -Unique)
$adapters = @(Get-CimInstance Win32_VideoController | ForEach-Object {
    [ordered]@{
        name = $_.Name
        driverVersion = $_.DriverVersion
        adapterRamBytes = [uint64]$_.AdapterRAM
    }
})
$gitHead = (& git -C $repo rev-parse HEAD 2>$null | Select-Object -First 1)
$gitStatus = @(& git -C $repo status --short 2>$null)

$manifest = [ordered]@{
    schemaVersion = 1
    label = $Label
    safeLabel = $safeLabel
    notes = $Notes
    captureDirectory = $captureDir
    requestedDurationSeconds = $DurationSeconds
    intervalSeconds = $IntervalSeconds
    countdownSeconds = $CountdownSeconds
    wprProfile = $WprProfile
    wprTrace = if ($WprProfile -eq 'None') { $null } else { $etl }
    presentMon = $presentMon
    presentMonMode = $presentMonMode
    presentMonCaptured = $false
    hardwareTelemetry = if ($nvidiaSmi) { 'nvidia-smi' } else { $null }
    gamePid = $GamePid
    gameStarted = $game.StartTime.ToUniversalTime().ToString('o')
    startedUtc = $null
    completedUtc = $null
    actualDurationSeconds = $null
    status = 'prepared'
    git = [ordered]@{
        head = $gitHead
        status = $gitStatus
    }
    machine = [ordered]@{
        computerName = $env:COMPUTERNAME
        os = $os.Caption
        osVersion = $os.Version
        logicalProcessors = $logicalProcessors
        processors = $processors
        displayAdapters = $adapters
    }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM

Write-Host "OSF UI profile: $Label" -ForegroundColor Cyan
Write-Host "Starfield PID: $GamePid"
Write-Host "Output: $captureDir"
if ($presentMon) {
    Write-Host "PresentMon ($presentMonMode): $presentMon"
} else {
    Write-Warning 'PresentMon was not found; WPR still records GPU/present ETW, but no frame percentile CSV will be produced.'
}
if ($CountdownSeconds -gt 0) {
    Write-Host "Switch to Starfield and establish the scenario; capture starts in $CountdownSeconds second(s)." -ForegroundColor Green
    for ($remaining = $CountdownSeconds; $remaining -gt 0; --$remaining) {
        Write-Progress -Activity 'OSF UI profiling' -Status "Starting in $remaining second(s)" `
            -PercentComplete ((($CountdownSeconds - $remaining) / $CountdownSeconds) * 100)
        Start-Sleep -Seconds 1
    }
    Write-Progress -Activity 'OSF UI profiling' -Completed
}

$processRows = [Collections.Generic.List[object]]::new()
$gpuEngineRows = [Collections.Generic.List[object]]::new()
$gpuMemoryRows = [Collections.Generic.List[object]]::new()
$systemRows = [Collections.Generic.List[object]]::new()
$hardwareRows = [Collections.Generic.List[object]]::new()
$processState = @{}
$wprStarted = $false
$presentProcess = $null
$captureError = $null
$stopwatch = [Diagnostics.Stopwatch]::new()
$wprSessionName = "OSFUI-WPR-$GamePid"
$presentSessionName = "OSFUI-Present-$GamePid"

try {
    if ($wpr) {
        $status = (& $wpr.Source -status -instancename $wprSessionName 2>&1 | Out-String)
        if ($LASTEXITCODE -eq 0 -and $status -match 'recording is in progress') {
            throw "WPR session '$wprSessionName' is already running."
        }
        $startArgs = if ($WprProfile -eq 'General') {
            @('-start', 'GeneralProfile.Verbose', '-start', 'GPU.Verbose', '-filemode', '-instancename', $wprSessionName)
        } else {
            @('-start', 'CPU.Verbose', '-start', 'GPU.Verbose', '-filemode', '-instancename', $wprSessionName)
        }
        & $wpr.Source @startArgs
        if ($LASTEXITCODE -ne 0) { throw "wpr -start failed with exit code $LASTEXITCODE." }
        $wprStarted = $true
    }

    if ($presentMon) {
        $presentStdout = Join-Path $captureDir 'presentmon-live.stdout.log'
        $presentStderr = Join-Path $captureDir 'presentmon-live.stderr.log'
        $presentArgs = @(
            '--process_id', [string]$GamePid,
            '--output_file', ('"{0}"' -f $presentCsv),
            '--qpc_time_ms', '--v2_metrics',
            '--timed', [string]$DurationSeconds,
            '--terminate_after_timed', '--no_console_stats',
            '--session_name', $presentSessionName
        )
        $presentProcess = Start-Process -FilePath $presentMon -ArgumentList $presentArgs `
            -WindowStyle Hidden -RedirectStandardOutput $presentStdout `
            -RedirectStandardError $presentStderr -PassThru
    }

    $manifest.startedUtc = [DateTime]::UtcNow.ToString('o')
    $manifest.status = 'recording'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
    $stopwatch.Start()

    $counterPaths = @(
        '\Processor(_Total)\% Processor Time',
        '\Memory\Available MBytes',
        '\Memory\Committed Bytes',
        '\Memory\Commit Limit',
        '\Memory\Pages/sec',
        '\GPU Engine(*)\Utilization Percentage',
        '\GPU Process Memory(*)\Dedicated Usage',
        '\GPU Process Memory(*)\Shared Usage'
    )

    while ($stopwatch.Elapsed.TotalSeconds -lt $DurationSeconds) {
        $sampleStarted = [DateTime]::UtcNow
        $inventory = @(Get-OSFUIProcessInventory $GamePid)
        $roleByPid = @{}
        foreach ($entry in $inventory) {
            $process = $entry.Process
            $processId = [int]$process.ProcessId
            $roleByPid[$processId] = $entry.Role
            $creation = if ($process.CreationDate) { $process.CreationDate.ToUniversalTime().ToString('o') } else { '' }
            $stateKey = "$processId|$creation"
            $cpu100ns = (Convert-ToUInt64 $process.KernelModeTime) + (Convert-ToUInt64 $process.UserModeTime)
            $readBytes = Convert-ToUInt64 $process.ReadTransferCount
            $writeBytes = Convert-ToUInt64 $process.WriteTransferCount
            $otherBytes = Convert-ToUInt64 $process.OtherTransferCount
            $previous = $processState[$stateKey]
            $cpuOneCore = $null
            $cpuMachine = $null
            $readBps = $null
            $writeBps = $null
            $otherBps = $null
            if ($previous) {
                $elapsed = ($sampleStarted - $previous.Timestamp).TotalSeconds
                if ($elapsed -gt 0) {
                    $cpuOneCore = (($cpu100ns - $previous.Cpu100ns) / 10000000.0) / $elapsed * 100.0
                    $cpuMachine = $cpuOneCore / $logicalProcessors
                    $readBps = ($readBytes - $previous.ReadBytes) / $elapsed
                    $writeBps = ($writeBytes - $previous.WriteBytes) / $elapsed
                    $otherBps = ($otherBytes - $previous.OtherBytes) / $elapsed
                }
            }
            $processState[$stateKey] = [pscustomobject]@{
                Timestamp = $sampleStarted
                Cpu100ns = $cpu100ns
                ReadBytes = $readBytes
                WriteBytes = $writeBytes
                OtherBytes = $otherBytes
            }
            $processRows.Add([pscustomobject]@{
                TimestampUtc = $sampleStarted.ToString('o')
                ElapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
                Role = $entry.Role
                ProcessId = $processId
                ParentProcessId = [int]$process.ParentProcessId
                ProcessName = [IO.Path]::GetFileNameWithoutExtension([string]$process.Name)
                CpuPercentMachine = $cpuMachine
                CpuPercentOneCore = $cpuOneCore
                WorkingSetBytes = Convert-ToUInt64 $process.WorkingSetSize
                PrivateBytes = Convert-ToUInt64 $process.PrivatePageCount
                VirtualBytes = Convert-ToUInt64 $process.VirtualSize
                HandleCount = [uint32]$process.HandleCount
                ThreadCount = [uint32]$process.ThreadCount
                ReadBytesPerSecond = $readBps
                WriteBytesPerSecond = $writeBps
                OtherBytesPerSecond = $otherBps
            })
        }

        $counterSet = Get-Counter -Counter $counterPaths -ErrorAction Stop
        $counterSamples = @($counterSet.CounterSamples)
        $gpuTotals = @{}
        $gpuMemory = @{}

        foreach ($sample in $counterSamples) {
            $instance = [string]$sample.InstanceName
            if ($instance -match '^pid_(?<pid>\d+)_luid_(?<luidHigh>0x[0-9a-f]+)_(?<luidLow>0x[0-9a-f]+)_phys_(?<phys>\d+)_eng_(?<engine>\d+)_engtype_(?<type>.+)$') {
                $processId = [int]$Matches.pid
                $engineType = $Matches.type
                $engineKey = "$($Matches.luidHigh)|$($Matches.luidLow)|$($Matches.phys)|$($Matches.engine)|$engineType"
                if (-not $gpuTotals.ContainsKey($engineKey)) { $gpuTotals[$engineKey] = 0.0 }
                $gpuTotals[$engineKey] += [double]$sample.CookedValue
                if ($roleByPid.ContainsKey($processId)) {
                    $gpuEngineRows.Add([pscustomobject]@{
                        TimestampUtc = $sampleStarted.ToString('o')
                        ElapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
                        Role = $roleByPid[$processId]
                        ProcessId = $processId
                        EngineType = $engineType
                        EngineId = [int]$Matches.engine
                        PhysicalAdapter = [int]$Matches.phys
                        AdapterLuid = "$($Matches.luidHigh):$($Matches.luidLow)"
                        UtilizationPercent = [double]$sample.CookedValue
                    })
                }
                continue
            }

            if ($instance -match '^pid_(?<pid>\d+)_luid_(?<luidHigh>0x[0-9a-f]+)_(?<luidLow>0x[0-9a-f]+)_phys_(?<phys>\d+)$') {
                $processId = [int]$Matches.pid
                if (-not $roleByPid.ContainsKey($processId)) { continue }
                $memoryKey = "$processId|$($Matches.luidHigh)|$($Matches.luidLow)|$($Matches.phys)"
                if (-not $gpuMemory.ContainsKey($memoryKey)) {
                    $gpuMemory[$memoryKey] = [ordered]@{
                        TimestampUtc = $sampleStarted.ToString('o')
                        ElapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
                        Role = $roleByPid[$processId]
                        ProcessId = $processId
                        PhysicalAdapter = [int]$Matches.phys
                        AdapterLuid = "$($Matches.luidHigh):$($Matches.luidLow)"
                        DedicatedBytes = 0.0
                        SharedBytes = 0.0
                    }
                }
                if ($sample.Path.EndsWith('\Dedicated Usage', [StringComparison]::OrdinalIgnoreCase)) {
                    $gpuMemory[$memoryKey].DedicatedBytes = [double]$sample.CookedValue
                } elseif ($sample.Path.EndsWith('\Shared Usage', [StringComparison]::OrdinalIgnoreCase)) {
                    $gpuMemory[$memoryKey].SharedBytes = [double]$sample.CookedValue
                }
            }
        }
        foreach ($memory in $gpuMemory.Values) {
            $gpuMemoryRows.Add([pscustomobject]$memory)
        }

        $systemGpu3D = @($gpuTotals.GetEnumerator() | Where-Object Key -match '\|3D$' |
            ForEach-Object { [double]$_.Value } | Measure-Object -Maximum).Maximum
        $systemGpuCopy = @($gpuTotals.GetEnumerator() | Where-Object Key -match '\|Copy$' |
            ForEach-Object { [double]$_.Value } | Measure-Object -Maximum).Maximum
        $systemGpuAny = @($gpuTotals.Values | Measure-Object -Maximum).Maximum
        $systemRows.Add([pscustomobject]@{
            TimestampUtc = $sampleStarted.ToString('o')
            ElapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
            CpuPercent = Get-CounterValue $counterSamples '\processor(_total)\% processor time'
            AvailableMemoryBytes = (Get-CounterValue $counterSamples '\memory\available mbytes') * 1MB
            CommittedMemoryBytes = Get-CounterValue $counterSamples '\memory\committed bytes'
            CommitLimitBytes = Get-CounterValue $counterSamples '\memory\commit limit'
            PagesPerSecond = Get-CounterValue $counterSamples '\memory\pages/sec'
            Gpu3DMaxEnginePercent = $systemGpu3D
            GpuCopyMaxEnginePercent = $systemGpuCopy
            GpuAnyMaxEnginePercent = $systemGpuAny
        })

        if ($nvidiaSmi) {
            $query = 'index,name,utilization.gpu,utilization.memory,memory.used,power.draw,power.limit,temperature.gpu,clocks.current.graphics,clocks.current.memory'
            $lines = @(& $nvidiaSmi.Source "--query-gpu=$query" '--format=csv,noheader,nounits' 2>$null)
            foreach ($line in $lines) {
                $parts = @($line -split '\s*,\s*')
                if ($parts.Count -lt 10) { continue }
                $hardwareRows.Add([pscustomobject]@{
                    TimestampUtc = $sampleStarted.ToString('o')
                    ElapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
                    AdapterIndex = [int]$parts[0]
                    AdapterName = $parts[1]
                    GpuUtilizationPercent = Convert-ToDoubleOrNull $parts[2]
                    MemoryControllerUtilizationPercent = Convert-ToDoubleOrNull $parts[3]
                    VramUsedMiB = Convert-ToDoubleOrNull $parts[4]
                    PowerWatts = Convert-ToDoubleOrNull $parts[5]
                    PowerLimitWatts = Convert-ToDoubleOrNull $parts[6]
                    TemperatureC = Convert-ToDoubleOrNull $parts[7]
                    GraphicsClockMHz = Convert-ToDoubleOrNull $parts[8]
                    MemoryClockMHz = Convert-ToDoubleOrNull $parts[9]
                })
            }
        }

        $remaining = [math]::Max(0, $DurationSeconds - $stopwatch.Elapsed.TotalSeconds)
        Write-Progress -Activity "OSF UI profiling: $Label" `
            -Status ('{0:N0} second(s) remaining' -f $remaining) `
            -PercentComplete ([math]::Min(100, $stopwatch.Elapsed.TotalSeconds / $DurationSeconds * 100))
        $sleepMs = [math]::Round($IntervalSeconds * 1000 - ([DateTime]::UtcNow - $sampleStarted).TotalMilliseconds)
        if ($sleepMs -gt 0) { Start-Sleep -Milliseconds $sleepMs }
    }
} catch {
    $captureError = $_
} finally {
    $stopwatch.Stop()
    Write-Progress -Activity "OSF UI profiling: $Label" -Completed

    Export-CaptureCsv $processRows $processCsv
    Export-CaptureCsv $gpuEngineRows $gpuEngineCsv
    Export-CaptureCsv $gpuMemoryRows $gpuMemoryCsv
    Export-CaptureCsv $systemRows $systemCsv
    Export-CaptureCsv $hardwareRows $hardwareCsv

    if ($presentProcess) {
        try {
            $presentProcess | Wait-Process -Timeout 15 -ErrorAction Stop
            if ($presentProcess.ExitCode -ne 0) {
                Write-Warning "PresentMon live capture exited with code $($presentProcess.ExitCode); see $presentStderr"
            }
        } catch {
            $running = Get-Process -Id $presentProcess.Id -ErrorAction SilentlyContinue
            if ($running -and $running.ProcessName -like 'PresentMon*') {
                Stop-Process -Id $running.Id -Force
            }
            Write-Warning "PresentMon did not exit cleanly: $($_.Exception.Message)"
        }
    }

    if ($wprStarted) {
        if ($captureError) {
            & $wpr.Source -cancel -instancename $wprSessionName | Out-Null
        } else {
            & $wpr.Source -stop $etl "OSF UI profile: $safeLabel" -compress -instancename $wprSessionName
            if ($LASTEXITCODE -ne 0) {
                $captureError = [Management.Automation.ErrorRecord]::new(
                    [InvalidOperationException]::new("wpr -stop failed with exit code $LASTEXITCODE."),
                    'WprStopFailed',
                    [Management.Automation.ErrorCategory]::InvalidResult,
                    $etl)
            }
        }
    }

    $manifest.actualDurationSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
    $manifest.completedUtc = [DateTime]::UtcNow.ToString('o')
    $manifest.status = if ($captureError) { 'failed' } else { 'complete' }
    $manifest.presentMonCaptured = (Test-Path -LiteralPath $presentCsv -PathType Leaf)
    if ($captureError) { $manifest['error'] = $captureError.Exception.Message }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
}

if ($captureError) { throw $captureError }

$summarizer = Join-Path $PSScriptRoot 'Summarize-OSFUI.ps1'
if (Test-Path -LiteralPath $summarizer) {
    & $summarizer -CaptureDirectory $captureDir
}

$symbolPaths = [Collections.Generic.List[string]]::new()
if ($env:XSE_SF_MODS_PATH) {
    $symbolPaths.Add((Join-Path $env:XSE_SF_MODS_PATH 'OSF UI\SFSE\Plugins'))
}
foreach ($candidate in @(
    (Join-Path $repo 'build\windows\x64\releasedbg'),
    (Join-Path $repo 'build\windows\x64\debug')
)) {
    if (Test-Path -LiteralPath $candidate) { $symbolPaths.Add($candidate) }
}
$symbolCache = Join-Path $repo 'build\symbols'
New-Item -ItemType Directory -Force -Path $symbolCache | Out-Null
$symbolPaths.Add("srv*$symbolCache*https://msdl.microsoft.com/download/symbols")
$env:_NT_SYMBOL_PATH = $symbolPaths -join ';'

Write-Host "Saved OSF UI profile to $captureDir" -ForegroundColor Green
Write-Host "WPA symbol path: $env:_NT_SYMBOL_PATH"
if ($OpenWpa -and $wpa -and (Test-Path -LiteralPath $etl)) {
    Start-Process -FilePath $wpa.Source -ArgumentList ('"{0}"' -f $etl)
}
