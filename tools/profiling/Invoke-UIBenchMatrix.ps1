#requires -Version 7.2
<#
.SYNOPSIS
  Run a resumable, guided UIBench capture matrix.

.DESCRIPTION
  Reads a matrix produced by New-UIBenchMatrix.ps1, configures the installed
  fixture between scenarios, launches the matching MO2 profile, validates the
  loaded save/fixture, warms the scene, captures one row, and checkpoints the
  CSV after every attempt. Completed rows are skipped when the script resumes.

  Automatic MO2 launching requires MO2 to be closed before the runner starts.
  By default the user confirms that the benchmark save has loaded. Pass
  -AutoDetectReady only when the save/load flow is itself deterministic.

.EXAMPLE
  pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\profiling\Invoke-UIBenchMatrix.ps1 `
    -MatrixPath .\build\profiles\ui-bench-real-4k60\matrix.csv `
    -FixtureModPath 'C:\Modding\Starfield\MO2\mods\UIBench' `
    -ModOrganizerPath 'C:\Modding\Starfield\MO2\ModOrganizer.exe' `
    -AutoCloseGame
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $MatrixPath,

    [string] $SessionRoot,

    [Parameter(Mandatory)]
    [string] $FixtureModPath,

    [string] $ModOrganizerPath,

    [string] $Mo2Executable = 'Starfield (SFSE)',

    [string] $BaselineProfile = 'UI Bench - Baseline',

    [string] $OSFUIProfile = 'UI Bench - OSFUI',

    [string] $CarbonUIProfile = 'UI Bench - CarbonUI',

    [ValidateRange(5, 86400)]
    [int] $DurationSeconds = 60,

    [ValidateRange(0, 3600)]
    [int] $WarmupSeconds = 30,

    [ValidateRange(0, 3600)]
    [int] $CooldownSeconds = 15,

    [ValidateRange(30, 3600)]
    [int] $LaunchTimeoutSeconds = 300,

    [ValidateRange(30, 3600)]
    [int] $ReadyTimeoutSeconds = 600,

    [ValidateSet('CpuGpu', 'General', 'None')]
    [string] $WprProfile = 'None',

    [string] $PresentMonPath,

    [ValidateRange(0, 10000)]
    [int] $MaxCaptures = 0,

    [ValidateRange(1, 1000)]
    [int] $MinimumRepeats = 3,

    [switch] $ManualLaunch,

    [switch] $AutoDetectReady,

    [switch] $AutoCloseGame,

    [switch] $NoHardwareTelemetry,

    [switch] $SkipComparison,

    [switch] $PlanOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator
{
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ProfileName([string] $Framework)
{
    switch ($Framework) {
        'Baseline' { return $BaselineProfile }
        'OSFUI' { return $OSFUIProfile }
        'CarbonUI' { return $CarbonUIProfile }
        default { throw "Unsupported framework in matrix: $Framework" }
    }
}

function Add-MatrixTrackingFields($Rows)
{
    foreach ($row in $Rows) {
        foreach ($field in @('Attempts', 'LastAttemptUtc', 'LastError', 'CaptureDirectory')) {
            if ($null -eq $row.PSObject.Properties[$field]) {
                $row | Add-Member -MemberType NoteProperty -Name $field -Value ''
            }
        }
    }
}

function Save-Matrix($Rows, [string] $Path)
{
    $temporary = "$Path.$PID.tmp"
    try {
        $Rows | Export-Csv -LiteralPath $temporary -NoTypeInformation -Encoding utf8NoBOM
        [IO.File]::Move($temporary, $Path, $true)
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Get-StarfieldProcess
{
    $games = @(Get-Process -Name Starfield -ErrorAction SilentlyContinue)
    if ($games.Count -gt 1) {
        throw "Multiple Starfield processes are running: $($games.Id -join ', ')"
    }
    if ($games.Count -eq 1) { return $games[0] }
    return $null
}

function Start-Mo2Game([string] $Profile)
{
    if ($ManualLaunch) {
        Write-Host "Launch '$Mo2Executable' from MO2 profile '$Profile'." -ForegroundColor Cyan
        Read-Host 'Press Enter after starting Starfield' | Out-Null
        return $null
    }

    $existingMo2 = @(Get-Process -Name ModOrganizer -ErrorAction SilentlyContinue)
    if ($existingMo2.Count) {
        throw "MO2 is already running (PID $($existingMo2.Id -join ', ')). Close it before using automatic profile launching, or use -ManualLaunch."
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:ResolvedModOrganizerPath
    $startInfo.WorkingDirectory = Split-Path $script:ResolvedModOrganizerPath -Parent
    $startInfo.UseShellExecute = $true
    foreach ($argument in @('-p', $Profile, 'run', '-e', $Mo2Executable)) {
        [void]$startInfo.ArgumentList.Add($argument)
    }
    Write-Host "Launching MO2 profile '$Profile' -> '$Mo2Executable'..." -ForegroundColor Cyan
    return [Diagnostics.Process]::Start($startInfo)
}

function Wait-ForStarfield($Launcher, [int] $TimeoutSeconds)
{
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $game = Get-StarfieldProcess
        if ($game) { return $game }
        if ($Launcher -and $Launcher.HasExited) {
            throw "MO2 exited with code $($Launcher.ExitCode) before Starfield started."
        }
        Start-Sleep -Seconds 1
    }
    throw "Starfield did not start within $TimeoutSeconds seconds."
}

function Get-LatestFixtureRecord([string] $Path, [int] $GamePid)
{
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $lines = @(Get-Content -LiteralPath $Path -Tail 300 -ErrorAction SilentlyContinue)
    for ($index = $lines.Count - 1; $index -ge 0; --$index) {
        try {
            $record = $lines[$index] | ConvertFrom-Json -Depth 20 -ErrorAction Stop
            if ([int]$record.gamePid -eq $GamePid) { return $record }
        } catch {}
    }
    return $null
}

function Test-RowReady($Row, [Diagnostics.Process] $Game)
{
    if ([string]$Row.Framework -eq 'Baseline') {
        if (-not (Test-Path -LiteralPath $script:FixtureLogPath -PathType Leaf)) { return $false }
        $log = Get-Item -LiteralPath $script:FixtureLogPath
        if ($log.LastWriteTimeUtc -lt $Game.StartTime.ToUniversalTime()) { return $false }
        # SFSE's bracketed logger value is a thread ID, not the process ID.
        # The log is recreated for each launch, and LastWriteTimeUtc above ties
        # this marker to the current Starfield process lifetime.
        $marker = 'UIBench: loading menu closed; no-framework baseline ready'
        return (Get-Content -LiteralPath $script:FixtureLogPath -Tail 200 -ErrorAction SilentlyContinue |
            Select-String -SimpleMatch $marker -Quiet)
    }

    $record = Get-LatestFixtureRecord $script:FixtureTelemetryPath $Game.Id
    if ($null -eq $record) { return $false }
    $ageMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - [int64]$record.receivedUnixMs
    if ($ageMs -lt 0 -or $ageMs -gt 5000) { return $false }
    if ([string]$record.provider -ne [string]$Row.Framework -or
        [string]$record.framework -ne [string]$Row.Framework -or
        [string]$record.scenario -ne [string]$Row.Scenario) {
        return $false
    }
    $viewport = "$([int]$record.viewport.width)x$([int]$record.viewport.height)"
    if ($viewport -ne [string]$Row.Resolution) { return $false }
    if ([string]$Row.RasterizationPolicy -eq 'PixelMatched' -and
        [math]::Abs([double]$record.viewport.devicePixelRatio - 1.0) -gt 0.01) {
        return $false
    }
    return $true
}

function Wait-ForRowReady($Row, [Diagnostics.Process] $Game, [int] $TimeoutSeconds)
{
    Write-Host "Waiting for validated $($Row.Framework) '$($Row.Scenario)' fixture readiness..." -ForegroundColor Cyan
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not (Get-Process -Id $Game.Id -ErrorAction SilentlyContinue)) {
            throw 'Starfield exited before the benchmark scene became ready.'
        }
        if (Test-RowReady $Row $Game) { return }
        Start-Sleep -Seconds 1
    }
    throw "The benchmark scene did not report readiness within $TimeoutSeconds seconds."
}

function Set-StarfieldForeground([Diagnostics.Process] $Game)
{
    try {
        $Game.Refresh()
        if ($Game.MainWindowHandle -eq [IntPtr]::Zero) { return $false }
        [void][UIBenchMatrixNative]::ShowWindowAsync($Game.MainWindowHandle, 9)
        return [UIBenchMatrixNative]::SetForegroundWindow($Game.MainWindowHandle)
    } catch {
        return $false
    }
}

function Wait-Countdown([string] $Label, [int] $Seconds)
{
    if ($Seconds -le 0) { return }
    Write-Host "$Label ($Seconds seconds)..." -ForegroundColor Cyan
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
}

function Wait-ForGameExit([int] $GamePid)
{
    while (Get-Process -Id $GamePid -ErrorAction SilentlyContinue) {
        Start-Sleep -Seconds 1
    }
}

function Complete-GameLaunch([Diagnostics.Process] $Game, $Launcher)
{
    if ($AutoCloseGame) {
        Write-Host 'Capture complete; requesting a graceful Starfield shutdown...' -ForegroundColor Cyan
        try {
            $Game.Refresh()
            [void]$Game.CloseMainWindow()
        } catch {}
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while ([DateTime]::UtcNow -lt $deadline -and
            (Get-Process -Id $Game.Id -ErrorAction SilentlyContinue)) {
            Start-Sleep -Seconds 1
        }
    }
    if (Get-Process -Id $Game.Id -ErrorAction SilentlyContinue) {
        try { [Console]::Beep(880, 500) } catch {}
        Write-Host 'Capture complete. Exit Starfield to continue to the next matrix row.' -ForegroundColor Yellow
        Wait-ForGameExit $Game.Id
    }

    if ($Launcher -and -not $Launcher.HasExited) {
        [void]$Launcher.WaitForExit(60000)
    }
    if ($Launcher -and -not $Launcher.HasExited) {
        throw 'The MO2 command-line launcher did not exit after Starfield closed. Close MO2 and resume the matrix.'
    }
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$resolvedMatrixPath = (Resolve-Path -LiteralPath $MatrixPath -ErrorAction Stop).Path
if (-not $SessionRoot) { $SessionRoot = Split-Path $resolvedMatrixPath -Parent }
$resolvedSessionRoot = [IO.Path]::GetFullPath($SessionRoot)
$rows = @(Import-Csv -LiteralPath $resolvedMatrixPath)
if (-not $rows.Count) { throw "Matrix is empty: $resolvedMatrixPath" }

$requiredColumns = @('Id', 'Status', 'Framework', 'Scenario', 'Repeat', 'Resolution',
    'FrameRateMode', 'FrameGeneration', 'RenderPreset', 'RasterizationPolicy')
foreach ($column in $requiredColumns) {
    if ($null -eq $rows[0].PSObject.Properties[$column]) {
        throw "Matrix is missing required column '$column': $resolvedMatrixPath"
    }
}
Add-MatrixTrackingFields $rows
$pending = @($rows | Where-Object { [string]$_.Status -notin @('Complete', 'Skipped') })

Write-Host "Matrix: $resolvedMatrixPath" -ForegroundColor Green
Write-Host "Session: $resolvedSessionRoot"
Write-Host "$($rows.Count - $pending.Count) complete; $($pending.Count) pending/failed."
if ($pending.Count) {
    $pending | Select-Object -First 30 Id, Framework, Scenario, Repeat, Status,
        @{ Name = 'MO2Profile'; Expression = { Get-ProfileName $_.Framework } } |
        Format-Table -AutoSize | Out-Host
}
$estimatedSeconds = $pending.Count * ($DurationSeconds + $WarmupSeconds + $CooldownSeconds)
Write-Host "Minimum timed work remaining (excluding launches/save loads): $([TimeSpan]::FromSeconds($estimatedSeconds))"
if ($PlanOnly) { return }

if (-not (Test-IsAdministrator)) {
    throw 'Run the matrix runner from an elevated PowerShell 7 terminal.'
}
$resolvedFixtureModPath = (Resolve-Path -LiteralPath $FixtureModPath -ErrorAction Stop).Path
if (-not $ManualLaunch) {
    if (-not $ModOrganizerPath) { throw '-ModOrganizerPath is required unless -ManualLaunch is used.' }
    $script:ResolvedModOrganizerPath = (Resolve-Path -LiteralPath $ModOrganizerPath -ErrorAction Stop).Path
}
New-Item -ItemType Directory -Force -Path $resolvedSessionRoot | Out-Null

$documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
$script:FixtureTelemetryPath = Join-Path $documents 'My Games\Starfield\SFSE\Logs\UIBench.telemetry.jsonl'
$script:FixtureLogPath = Join-Path $documents 'My Games\Starfield\SFSE\Logs\UIBench.log'

if (-not ('UIBenchMatrixNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UIBenchMatrixNative
{
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
}
'@
}

$captureScript = Join-Path $PSScriptRoot 'Capture-UIBench.ps1'
$fixtureScript = Join-Path $PSScriptRoot 'Set-UIBenchFixture.ps1'
$compareScript = Join-Path $PSScriptRoot 'Compare-UIBench.ps1'
$completedThisRun = 0

foreach ($row in $rows) {
    if ([string]$row.Status -in @('Complete', 'Skipped')) { continue }
    if ($MaxCaptures -gt 0 -and $completedThisRun -ge $MaxCaptures) { break }

    $game = Get-StarfieldProcess
    if ($game) {
        throw "Starfield is already running (PID $($game.Id)). Exit it before resuming matrix row $($row.Id)."
    }

    Write-Host ''
    Write-Host "=== Row $($row.Id)/$($rows.Count): $($row.Framework), $($row.Scenario), repeat $($row.Repeat) ===" -ForegroundColor Green
    $configPath = Join-Path $resolvedFixtureModPath 'SFSE\Plugins\UIBench\config.json'
    $needsConfiguration = $true
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        try {
            $currentConfig = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
            $currentResolution = "$([int]$currentConfig.width)x$([int]$currentConfig.height)"
            $needsConfiguration = ([string]$currentConfig.scenario -ne [string]$row.Scenario -or
                $currentResolution -ne [string]$row.Resolution)
        } catch {}
    }
    if ($needsConfiguration) {
        & $fixtureScript -ModPath $resolvedFixtureModPath -Scenario ([string]$row.Scenario) `
            -Resolution ([string]$row.Resolution)
    }

    $row.Status = 'InProgress'
    $row.Attempts = ([int]$row.Attempts + 1).ToString()
    $row.LastAttemptUtc = [DateTime]::UtcNow.ToString('o')
    $row.LastError = ''
    Save-Matrix $rows $resolvedMatrixPath

    $launcher = $null
    try {
        $profile = Get-ProfileName ([string]$row.Framework)
        $launcher = Start-Mo2Game $profile
        $game = Wait-ForStarfield $launcher $LaunchTimeoutSeconds
        Write-Host "Starfield PID $($game.Id). Load the benchmark save and leave the game focused." -ForegroundColor Yellow
        if (-not $AutoDetectReady) {
            Read-Host 'After the save and fixture are fully visible, press Enter here' | Out-Null
            if (-not (Set-StarfieldForeground $game)) {
                Write-Warning 'Could not automatically restore Starfield focus; switch to it now.'
            }
        }
        Wait-ForRowReady $row $game $ReadyTimeoutSeconds
        if (-not (Set-StarfieldForeground $game)) {
            Write-Warning 'Could not confirm Starfield foreground focus before warm-up.'
        }
        Wait-Countdown 'Warming the validated scene' $WarmupSeconds
        [void](Set-StarfieldForeground $game)

        $safeFramework = ([string]$row.Framework -replace '[^A-Za-z0-9._-]', '-')
        $safeScenario = ([string]$row.Scenario -replace '[^A-Za-z0-9._-]', '-')
        $outputRoot = Join-Path $resolvedSessionRoot "runs\$safeFramework\$safeScenario"
        $captureParams = @{
            Framework = [string]$row.Framework
            Scenario = [string]$row.Scenario
            Repeat = [int]$row.Repeat
            Resolution = [string]$row.Resolution
            FrameGeneration = [string]$row.FrameGeneration
            FrameRateMode = [string]$row.FrameRateMode
            RenderPreset = [string]$row.RenderPreset
            RasterizationPolicy = [string]$row.RasterizationPolicy
            DurationSeconds = $DurationSeconds
            CountdownSeconds = 0
            WprProfile = $WprProfile
            GamePid = $game.Id
            OutputRoot = $outputRoot
            Notes = "Matrix row $($row.Id); automated session $resolvedSessionRoot"
        }
        if ($PresentMonPath) { $captureParams.PresentMonPath = $PresentMonPath }
        if ($NoHardwareTelemetry) { $captureParams.NoHardwareTelemetry = $true }
        & $captureScript @captureParams

        $latestSummary = Get-ChildItem -LiteralPath $outputRoot -Filter summary.json -File -Recurse |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if (-not $latestSummary) { throw "Capture completed without a summary below $outputRoot" }
        $row.Status = 'Complete'
        $row.CaptureDirectory = $latestSummary.DirectoryName
        $row.LastError = ''
        Save-Matrix $rows $resolvedMatrixPath
        ++$completedThisRun
        Complete-GameLaunch $game $launcher
        Wait-Countdown 'Cooling down before the next launch' $CooldownSeconds
    } catch {
        $row.Status = 'Failed'
        $row.LastError = $_.Exception.Message
        Save-Matrix $rows $resolvedMatrixPath
        try { [Console]::Beep(440, 900) } catch {}
        throw "Matrix row $($row.Id) failed and was checkpointed for retry: $($_.Exception.Message)"
    }
}

$remaining = @($rows | Where-Object { [string]$_.Status -notin @('Complete', 'Skipped') })
if ($remaining.Count) {
    Write-Host "$($remaining.Count) row(s) remain. Re-run the same command to resume." -ForegroundColor Yellow
    return
}

if (-not $SkipComparison) {
    $comparisonPath = Join-Path $resolvedSessionRoot 'comparison.md'
    & $compareScript -InputRoot $resolvedSessionRoot -OutputPath $comparisonPath `
        -MinimumRepeats $MinimumRepeats
    Write-Host "Completed comparison: $comparisonPath" -ForegroundColor Green
}
