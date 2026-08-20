#requires -Version 7.2
#requires -RunAsAdministrator
<#
.SYNOPSIS
  Capture UI-framework process, CPU, memory, GPU, and frame-pacing evidence.

.DESCRIPTION
  Samples Starfield and, for OSF UI, its browser host and only the WebView2
  processes descended from that host. Optionally records a CPU/GPU WPR trace
  and a PresentMon CSV. The default remains an OSF UI capture for backwards
  compatibility; Capture-UIBench.ps1 supplies comparison metadata and strict
  loaded-module validation for OSF UI, Carbon UI, and baseline runs.

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

    [ValidateSet('OSFUI', 'CarbonUI', 'Baseline')]
    [string] $Framework = 'OSFUI',

    [string] $Scenario = '',

    [ValidateRange(0, 1000)]
    [int] $Repeat = 0,

    [string] $Resolution = 'Unspecified',

    [ValidateSet('Off', 'On', 'Unknown')]
    [string] $FrameGeneration = 'Unknown',

    [ValidateSet('Fixed60', 'Fixed120', 'Uncapped', 'Unknown')]
    [string] $FrameRateMode = 'Unknown',

    [string] $RenderPreset = 'Unspecified',

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

    [switch] $ValidateFrameworkState,

    [switch] $RequireFixture,

    [switch] $RequireFixtureModule,

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

function Get-UIProcessInventory([int] $TargetGamePid, [string] $TargetFramework)
{
    $processes = @(Get-CimInstance Win32_Process -Filter (
        "Name='Starfield.exe' OR Name='osfui_webview2_host.exe' OR Name='msedgewebview2.exe'"))

    $roles = @{}
    $roles[$TargetGamePid] = 'Game'

    if ($TargetFramework -eq 'OSFUI') {
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
    }

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

function Get-LoadedUiModules([Diagnostics.Process] $GameProcess)
{
    $targets = @('OSFUI.dll', 'CarbonUI.dll', 'CarbonUICore.dll', 'UIBench.dll')
    try {
        $GameProcess.Refresh()
        return @($GameProcess.Modules | Where-Object ModuleName -In $targets | ForEach-Object {
            $module = $_
            $hash = try {
                if (Test-Path -LiteralPath $module.FileName -PathType Leaf) {
                    (Get-FileHash -LiteralPath $module.FileName -Algorithm SHA256).Hash
                } else { $null }
            } catch { $null }
            [ordered]@{
                name = $module.ModuleName
                path = $module.FileName
                moduleMemorySize = $module.ModuleMemorySize
                fileVersion = $module.FileVersionInfo.FileVersion
                productVersion = $module.FileVersionInfo.ProductVersion
                sha256 = $hash
            }
        })
    } catch {
        throw "Could not inspect loaded Starfield modules for framework validation: $($_.Exception.Message)"
    }
}

function Assert-FrameworkState([string] $TargetFramework, $LoadedModules, [bool] $FixtureModuleRequired = $false)
{
    $names = @($LoadedModules | ForEach-Object { [string]$_.name })
    $hasOSFUI = $names -icontains 'OSFUI.dll'
    $hasCarbonLoader = $names -icontains 'CarbonUI.dll'
    $hasCarbonCore = $names -icontains 'CarbonUICore.dll'
    $hasFixture = $names -icontains 'UIBench.dll'

    if ($FixtureModuleRequired -and -not $hasFixture) {
        throw "Controlled runs require UIBench.dll loaded in every profile. Loaded UI/fixture modules: $($names -join ', ')"
    }

    switch ($TargetFramework) {
        'OSFUI' {
            if (-not $hasOSFUI -or $hasCarbonLoader -or $hasCarbonCore) {
                throw "OSFUI run requires OSFUI.dll loaded and Carbon UI absent. Loaded UI modules: $($names -join ', ')"
            }
        }
        'CarbonUI' {
            if ($hasOSFUI -or -not $hasCarbonLoader -or -not $hasCarbonCore) {
                throw "CarbonUI run requires CarbonUI.dll and CarbonUICore.dll loaded and OSF UI absent. Loaded UI modules: $($names -join ', ')"
            }
        }
        'Baseline' {
            if ($hasOSFUI -or $hasCarbonLoader -or $hasCarbonCore) {
                throw "Baseline run requires both UI frameworks absent. Loaded UI modules: $($names -join ', ')"
            }
        }
    }
}

function Get-GameClientSize([Diagnostics.Process] $GameProcess)
{
    if ($null -eq ('UIBench.NativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace UIBench
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public static class NativeMethods
    {
        [DllImport("user32.dll")]
        public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

        [DllImport("user32.dll")]
        public static extern uint GetDpiForWindow(IntPtr window);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetClientRect(IntPtr window, out Rect rect);
    }
}
'@
    }

    $GameProcess.Refresh()
    $window = $GameProcess.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) {
        throw 'Starfield does not have a main window yet; wait until the game is fully loaded.'
    }
    $rect = [UIBench.Rect]::new()
    # PowerShell is normally DPI-unaware, which makes GetClientRect return
    # virtualized logical pixels on scaled displays (for example, 2560x1440
    # for a 3840x2160 monitor at 150%). Temporarily make this thread per-monitor
    # DPI-aware so resolution validation and recorded metadata use physical
    # pixels, matching the game's render target and the requested fixture size.
    $previousDpiContext = [UIBench.NativeMethods]::SetThreadDpiAwarenessContext([IntPtr](-4))
    $success = $false
    $code = 0
    try {
        $success = [UIBench.NativeMethods]::GetClientRect($window, [ref]$rect)
        if (-not $success) {
            $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        }
    } finally {
        if ($previousDpiContext -ne [IntPtr]::Zero) {
            [void][UIBench.NativeMethods]::SetThreadDpiAwarenessContext($previousDpiContext)
        }
    }
    if (-not $success) {
        throw "Could not read the Starfield client size (Win32 error $code)."
    }
    $windowDpi = [UIBench.NativeMethods]::GetDpiForWindow($window)
    return [ordered]@{
        width = $rect.Right - $rect.Left
        height = $rect.Bottom - $rect.Top
        coordinateSpace = 'physicalPixels'
        windowDpi = [int]$windowDpi
        windowScalePercent = [int][Math]::Round(($windowDpi / 96.0) * 100.0)
    }
}

function Get-GameDisplayMode([Diagnostics.Process] $GameProcess)
{
    if ($null -eq ('UIBench.DisplayNativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace UIBench
{
    [StructLayout(LayoutKind.Sequential)]
    public struct DisplayRect
    {
        public int Left, Top, Right, Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MonitorInfoEx
    {
        public int Size;
        public DisplayRect Monitor;
        public DisplayRect Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DevMode
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        public ushort SpecVersion, DriverVersion, Size, DriverExtra;
        public uint Fields;
        public int PositionX, PositionY;
        public uint DisplayOrientation, DisplayFixedOutput;
        public short Color, Duplex, YResolution, TTOption, Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
        public ushort LogPixels;
        public uint BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
        public uint ICMMethod, ICMIntent, MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
    }

    public static class DisplayNativeMethods
    {
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfoEx info);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool EnumDisplaySettings(string device, int mode, ref DevMode devMode);
    }
}
'@
    }

    $GameProcess.Refresh()
    $monitor = [UIBench.DisplayNativeMethods]::MonitorFromWindow($GameProcess.MainWindowHandle, 2)
    if ($monitor -eq [IntPtr]::Zero) { return $null }
    $info = [UIBench.MonitorInfoEx]::new()
    $info.Size = [Runtime.InteropServices.Marshal]::SizeOf([type][UIBench.MonitorInfoEx])
    if (-not [UIBench.DisplayNativeMethods]::GetMonitorInfo($monitor, [ref]$info)) { return $null }
    $mode = [UIBench.DevMode]::new()
    $mode.Size = [Runtime.InteropServices.Marshal]::SizeOf([type][UIBench.DevMode])
    if (-not [UIBench.DisplayNativeMethods]::EnumDisplaySettings($info.DeviceName, -1, [ref]$mode)) { return $null }
    return [ordered]@{
        deviceName = $info.DeviceName
        width = [int]$mode.PelsWidth
        height = [int]$mode.PelsHeight
        refreshHz = [int]$mode.DisplayFrequency
        bitsPerPixel = [int]$mode.BitsPerPel
    }
}

function Get-DefaultFixtureTelemetryPath
{
    $documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
    if (-not $documents) { return $null }
    return Join-Path $documents 'My Games\Starfield\SFSE\Logs\UIBench.telemetry.jsonl'
}

function Get-LatestFixtureRecord([string] $Path, [int] $TargetGamePid)
{
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $lines = @(Get-Content -LiteralPath $Path -Tail 500 -ErrorAction Stop)
    for ($index = $lines.Count - 1; $index -ge 0; --$index) {
        try {
            $record = $lines[$index] | ConvertFrom-Json -Depth 20 -ErrorAction Stop
            if ([int]$record.gamePid -eq $TargetGamePid) { return $record }
        } catch {}
    }
    return $null
}

function Assert-FixtureRecord($Record, [string] $TargetFramework, [string] $TargetScenario,
    [string] $TargetResolution, [string] $TargetRasterizationPolicy)
{
    if ($null -eq $Record) {
        throw 'No live UIBench telemetry was found for this Starfield process. Install/enable the fixture mod and leave its view open.'
    }
    $ageMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - [int64]$Record.receivedUnixMs
    if ($ageMs -lt 0 -or $ageMs -gt 5000) {
        throw "UIBench telemetry is stale ($ageMs ms old); leave the fixture visible and wait for its once-per-second report."
    }
    if ([string]$Record.provider -ne $TargetFramework -or [string]$Record.framework -ne $TargetFramework) {
        throw "UIBench reported provider '$($Record.provider)' / page '$($Record.framework)', expected '$TargetFramework'."
    }
    if ([string]$Record.scenario -ne $TargetScenario) {
        throw "UIBench is running scenario '$($Record.scenario)', expected '$TargetScenario'. Use keys 1-6 in the fixture or configure it before launch."
    }
    $actualResolution = "$([int]$Record.viewport.width)x$([int]$Record.viewport.height)"
    if ($actualResolution -ne $TargetResolution) {
        throw "UIBench viewport is $actualResolution, expected $TargetResolution. Reconfigure the fixture and restart Starfield."
    }
    $dpr = [double]$Record.viewport.devicePixelRatio
    if (-not [double]::IsFinite($dpr) -or $dpr -le 0) {
        throw "UIBench reported invalid devicePixelRatio '$dpr'."
    }
    if ($TargetRasterizationPolicy -eq 'PixelMatched' -and
        [math]::Abs($dpr - 1.0) -gt 0.01) {
        throw "UIBench devicePixelRatio is $dpr, expected 1.0 for $TargetFramework under rasterization policy 'PixelMatched'."
    }
    if ($TargetRasterizationPolicy -eq 'FrameworkDefault' -and
        [math]::Abs($dpr - 1.0) -gt 0.01) {
        $rasterWidth = [int][math]::Round([double]$Record.viewport.width * $dpr)
        $rasterHeight = [int][math]::Round([double]$Record.viewport.height * $dpr)
        Write-Warning "$TargetFramework FrameworkDefault rasterization is ${rasterWidth}x${rasterHeight} (DPR $dpr) inside the $TargetResolution output. This is an end-to-end product-default run, not a pixel-matched renderer run."
    }
    if ([string]$Record.fixtureHash -notmatch '^[A-Fa-f0-9]{64}$') {
        throw "UIBench did not report a valid fixture SHA-256 ('$($Record.fixtureHash)')."
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

function Get-ValidCounterSamples([string[]] $Paths)
{
    # GPU engine/memory instances can disappear between enumeration and the
    # PDH query when browser/helper processes start or stop. Get-Counter emits
    # a non-terminating "sample is not valid" error for that interval even
    # though the remaining samples are usable. Keep valid status-0 finite
    # samples and let this one interval contain nulls for missing counters.
    $counterSet = Get-Counter -Counter $Paths -ErrorAction SilentlyContinue
    if ($null -eq $counterSet) { return @() }
    return @($counterSet.CounterSamples | Where-Object {
        [uint32]$_.Status -eq 0 -and [double]::IsFinite([double]$_.CookedValue)
    })
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
$loadedUiModules = @(Get-LoadedUiModules $game)
if ($ValidateFrameworkState) {
    Assert-FrameworkState $Framework $loadedUiModules ([bool]$RequireFixtureModule)
}

function Export-WprModuleAttribution([string] $TracePath, [string] $CapturePath)
{
    $xperf = Get-Command xperf.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $xperf -or -not (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
        return [ordered]@{
            status = 'unavailable'
            tool = if ($xperf) { $xperf.Source } else { $null }
            report = $null
            focusReport = $null
        }
    }
    $report = Join-Path $CapturePath 'cpu-profile-by-module.txt'
    $focusReport = Join-Path $CapturePath 'cpu-profile-ui-modules.txt'
    & $xperf.Source -i $TracePath -o $report -target machine -a profile -detail
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $report -PathType Leaf)) {
        Write-Warning "xperf module attribution failed with exit code $LASTEXITCODE."
        return [ordered]@{
            status = 'failed'
            tool = $xperf.Source
            report = $report
            focusReport = $null
        }
    }
    $patterns = 'Starfield|OSFUI|CarbonUI|CarbonUICore|Ultralight|WebCore|osfui_webview2_host|msedgewebview2'
    $matches = @(Select-String -LiteralPath $report -Pattern $patterns -CaseSensitive:$false |
        ForEach-Object Line | Select-Object -Unique)
    @(
        'Relevant process/module rows selected from xperf sampled CPU attribution.'
        'The complete report remains cpu-profile-by-module.txt.'
        ''
        $matches
    ) | Set-Content -LiteralPath $focusReport -Encoding utf8NoBOM
    return [ordered]@{
        status = 'complete'
        tool = $xperf.Source
        report = $report
        focusReport = $focusReport
        matchingLines = $matches.Count
    }
}
$observedClientSize = if ($Scenario) { Get-GameClientSize $game } else { $null }
$observedDisplayMode = if ($Scenario) { Get-GameDisplayMode $game } else { $null }
if ($ValidateFrameworkState -and $observedClientSize) {
    $observedResolution = "$($observedClientSize.width)x$($observedClientSize.height)"
    if ($observedResolution -ne $Resolution) {
        $displayDescription = if ($observedDisplayMode) {
            "$($observedDisplayMode.width)x$($observedDisplayMode.height) @ $($observedDisplayMode.refreshHz) Hz"
        } else { 'unknown' }
        throw "Resolution metadata says $Resolution, but the Starfield physical client is $observedResolution (monitor mode: $displayDescription; Windows scale: $($observedClientSize.windowScalePercent)%). A 4K monitor mode does not make a decorated game window's client 3840x2160; use borderless fullscreen."
    }
}

if (-not $FixtureTelemetryPath -and $RequireFixture) {
    $FixtureTelemetryPath = Get-DefaultFixtureTelemetryPath
}
$fixtureRecord = if ($FixtureTelemetryPath) {
    Get-LatestFixtureRecord $FixtureTelemetryPath $GamePid
} else { $null }
if ($RequireFixture) {
    Assert-FixtureRecord $fixtureRecord $Framework $Scenario $Resolution $RasterizationPolicy
}
$observedFixtureRaster = if ($fixtureRecord) {
    $fixtureDpr = [double]$fixtureRecord.viewport.devicePixelRatio
    $fixtureRasterWidth = [int][math]::Round([double]$fixtureRecord.viewport.width * $fixtureDpr)
    $fixtureRasterHeight = [int][math]::Round([double]$fixtureRecord.viewport.height * $fixtureDpr)
    [ordered]@{
        cssViewportWidth = [int]$fixtureRecord.viewport.width
        cssViewportHeight = [int]$fixtureRecord.viewport.height
        devicePixelRatio = $fixtureDpr
        rasterWidth = $fixtureRasterWidth
        rasterHeight = $fixtureRasterHeight
        rasterMegapixels = ($fixtureRasterWidth * $fixtureRasterHeight) / 1000000.0
    }
} else { $null }

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
$presentMonIdentity = if ($presentMon) {
    $presentItem = Get-Item -LiteralPath $presentMon
    [ordered]@{
        path = $presentItem.FullName
        fileVersion = $presentItem.VersionInfo.FileVersion
        productVersion = $presentItem.VersionInfo.ProductVersion
        sha256 = (Get-FileHash -LiteralPath $presentItem.FullName -Algorithm SHA256).Hash
    }
} else { $null }
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
    schemaVersion = 3
    label = $Label
    safeLabel = $safeLabel
    framework = $Framework
    benchmark = if ($Scenario) {
        [ordered]@{
            scenario = $Scenario
            repeat = $Repeat
            resolution = $Resolution
            observedClientSize = $observedClientSize
            observedDisplayMode = $observedDisplayMode
            frameGeneration = $FrameGeneration
            frameRateMode = $FrameRateMode
            renderPreset = $RenderPreset
            rasterizationPolicy = $RasterizationPolicy
            observedFixtureRaster = $observedFixtureRaster
        }
    } else { $null }
    notes = $Notes
    captureDirectory = $captureDir
    requestedDurationSeconds = $DurationSeconds
    intervalSeconds = $IntervalSeconds
    countdownSeconds = $CountdownSeconds
    wprProfile = $WprProfile
    wprTrace = if ($WprProfile -eq 'None') { $null } else { $etl }
    presentMon = $presentMon
    presentMonIdentity = $presentMonIdentity
    presentMonMode = $presentMonMode
    presentMonCaptured = $false
    hardwareTelemetry = if ($nvidiaSmi) { 'nvidia-smi' } else { $null }
    gamePid = $GamePid
    gameStarted = $game.StartTime.ToUniversalTime().ToString('o')
    gameExecutable = [ordered]@{
        path = $game.MainModule.FileName
        fileVersion = $game.MainModule.FileVersionInfo.FileVersion
        productVersion = $game.MainModule.FileVersionInfo.ProductVersion
    }
    loadedUiModules = $loadedUiModules
    fixture = if ($fixtureRecord) {
        [ordered]@{
            required = [bool]$RequireFixture
            telemetrySource = $FixtureTelemetryPath
            fixtureHash = [string]$fixtureRecord.fixtureHash
            provider = [string]$fixtureRecord.provider
            scenario = [string]$fixtureRecord.scenario
            viewport = $fixtureRecord.viewport
            capture = $null
            postflight = $null
        }
    } else { $null }
    wprAttribution = $null
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

Write-Host "$Framework profile: $Label" -ForegroundColor Cyan
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
        Write-Progress -Activity "$Framework profiling" -Status "Starting in $remaining second(s)" `
            -PercentComplete ((($CountdownSeconds - $remaining) / $CountdownSeconds) * 100)
        Start-Sleep -Seconds 1
    }
    Write-Progress -Activity "$Framework profiling" -Completed
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
$wprSessionName = "UIBench-WPR-$GamePid"
$presentSessionName = "UIBench-Present-$GamePid"

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
            '--qpc_time_ms', '--v2_metrics', '--track_frame_type',
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
        $inventory = @(Get-UIProcessInventory $GamePid $Framework)
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

        $counterSamples = @(Get-ValidCounterSamples $counterPaths)
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
        $availableMemoryMib = Get-CounterValue $counterSamples '\memory\available mbytes'
        $systemRows.Add([pscustomobject]@{
            TimestampUtc = $sampleStarted.ToString('o')
            ElapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
            CpuPercent = Get-CounterValue $counterSamples '\processor(_total)\% processor time'
            AvailableMemoryBytes = if ($null -eq $availableMemoryMib) { $null } else { $availableMemoryMib * 1MB }
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
        Write-Progress -Activity "$Framework profiling: $Label" `
            -Status ('{0:N0} second(s) remaining' -f $remaining) `
            -PercentComplete ([math]::Min(100, $stopwatch.Elapsed.TotalSeconds / $DurationSeconds * 100))
        $sleepMs = [math]::Round($IntervalSeconds * 1000 - ([DateTime]::UtcNow - $sampleStarted).TotalMilliseconds)
        if ($sleepMs -gt 0) { Start-Sleep -Milliseconds $sleepMs }
    }
} catch {
    $captureError = $_
} finally {
    $stopwatch.Stop()
    Write-Progress -Activity "$Framework profiling: $Label" -Completed

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
            & $wpr.Source -stop $etl "$Framework profile: $safeLabel" -compress -instancename $wprSessionName
            if ($LASTEXITCODE -ne 0) {
                $captureError = [Management.Automation.ErrorRecord]::new(
                    [InvalidOperationException]::new("wpr -stop failed with exit code $LASTEXITCODE."),
                    'WprStopFailed',
                    [Management.Automation.ErrorCategory]::InvalidResult,
                    $etl)
            }
            if (-not $captureError) {
                $manifest.wprAttribution = Export-WprModuleAttribution $etl $captureDir
            }
        }
    }

    $manifest.actualDurationSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
    $manifest.completedUtc = [DateTime]::UtcNow.ToString('o')
    if ($FixtureTelemetryPath -and $manifest.startedUtc) {
        try {
            $fixtureCapturePath = Join-Path $captureDir 'fixture-telemetry.jsonl'
            $startMs = [DateTimeOffset]::Parse($manifest.startedUtc).ToUnixTimeMilliseconds()
            $endMs = [DateTimeOffset]::Parse($manifest.completedUtc).ToUnixTimeMilliseconds()
            $capturedLines = [Collections.Generic.List[string]]::new()
            foreach ($line in @(Get-Content -LiteralPath $FixtureTelemetryPath -Tail 10000 -ErrorAction Stop)) {
                try {
                    $record = $line | ConvertFrom-Json -Depth 20 -ErrorAction Stop
                    $received = [int64]$record.receivedUnixMs
                    if ([int]$record.gamePid -eq $GamePid -and $received -ge $startMs -and $received -le $endMs) {
                        $capturedLines.Add($line)
                    }
                } catch {}
            }
            if ($capturedLines.Count) {
                $capturedLines | Set-Content -LiteralPath $fixtureCapturePath -Encoding utf8NoBOM
            }
            $postflight = Get-LatestFixtureRecord $FixtureTelemetryPath $GamePid
            if ($manifest.fixture) {
                $manifest.fixture.capture = if ($capturedLines.Count) { $fixtureCapturePath } else { $null }
                $manifest.fixture.postflight = $postflight
            }
            if ($RequireFixture) {
                Assert-FixtureRecord $postflight $Framework $Scenario $Resolution
                if ([string]$postflight.fixtureHash -ne [string]$manifest.fixture.fixtureHash) {
                    throw 'UIBench fixture hash changed during the capture.'
                }
                if ($capturedLines.Count -lt [math]::Max(1, [math]::Floor($DurationSeconds / 3))) {
                    throw "Only $($capturedLines.Count) UIBench telemetry reports were captured; the fixture was not continuously observable."
                }
            }
        } catch {
            if (-not $captureError) { $captureError = $_ }
        }
    }
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

Write-Host "Saved $Framework profile to $captureDir" -ForegroundColor Green
Write-Host "WPA symbol path: $env:_NT_SYMBOL_PATH"
if ($OpenWpa -and $wpa -and (Test-Path -LiteralPath $etl)) {
    Start-Process -FilePath $wpa.Source -ArgumentList ('"{0}"' -f $etl)
}
