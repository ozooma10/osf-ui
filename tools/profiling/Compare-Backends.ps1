<# Runs isolated desktop helpers. Never launches Starfield or edits player settings. #>
[CmdletBinding()]
param(
    [int]$Repetitions = 3,
    [double]$Seconds = 10,
    [double]$Warmup = 3,
    [int[]]$Heights = @(1080, 1440),
    [string]$JsonInclude,
    [string]$OutDir,
    [switch]$SkipBuild,
    [switch]$Lifecycle,
    [string[]]$Backends = @('webview2', 'cef')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function RunningGame { @(Get-Process -Name Starfield -ErrorAction SilentlyContinue | Select-Object Id,StartTime) }
if (@(RunningGame).Count) { throw 'Close Starfield normally before this isolated desktop comparison.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $OutDir) { $OutDir = Join-Path $repo ('build\backend-comparison\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $OutDir) { throw 'Choose a new output directory to preserve previous measurements.' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
if (-not $JsonInclude) {
    $packageRoot = Join-Path $env:LOCALAPPDATA '.xmake\packages\n\nlohmann_json\v3.11.3'
    $match = Get-ChildItem -Path "$packageRoot\*\include\nlohmann\json.hpp" -Force | Select-Object -First 1
    if (-not $match) { throw 'Pass -JsonInclude with the nlohmann_json 3.11.3 include directory.' }
    $JsonInclude = Split-Path (Split-Path $match.FullName -Parent) -Parent
}
$build = Join-Path $repo 'build\backend-bench'
if (-not $SkipBuild) {
    & cmake -S (Join-Path $PSScriptRoot 'backend-bench') -B $build -G 'Visual Studio 18 2026' -A x64 "-DJSON_INCLUDE=$JsonInclude" *> (Join-Path $OutDir 'configure.log')
    if ($LASTEXITCODE) { throw 'Benchmark configure failed; see configure.log.' }
    & cmake --build $build --config Release --parallel 4 *> (Join-Path $OutDir 'build.log')
    if ($LASTEXITCODE) { throw 'Benchmark build failed; see build.log.' }
}
$exe = Join-Path $build 'Release\osfui_backend_bench.exe'
$hosts = @{
    webview2 = Join-Path $repo 'build\windows\x64\releasedbg\osfui_webview2_host.exe'
    cef = Join-Path $repo 'build\cef-host\runtime\osfui_cef_host.exe'
}
$assets = Join-Path $OutDir 'views'
foreach ($view in 'main', 'second', 'third') {
    $destination = Join-Path $assets "bench\$view"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'backend-bench\view.html') -Destination (Join-Path $destination 'index.html')
}
$metadata = [ordered]@{
    started = (Get-Date).ToString('o'); repetitions = $Repetitions; measureSeconds = $Seconds; warmupSeconds = $Warmup
    heights = $Heights; backends = $Backends; lifecycle = [bool]$Lifecycle; requireStarfieldClosed = $true; commit = (& git -C $repo rev-parse HEAD)
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate)
    os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber
    hashes = @()
}
foreach ($file in @($exe, $hosts.webview2, $hosts.cef, (Join-Path $repo 'build\cef-host\runtime\osfui_cef_host.dll'), (Join-Path $repo 'build\cef-host\runtime\libcef.dll'), (Join-Path $PSScriptRoot 'backend-bench\view.html'), (Join-Path $PSScriptRoot 'backend-bench\main.cpp'), (Join-Path $repo 'tools\cef_host\Host.cpp'), (Join-Path $repo 'tools\cef_host\FramePacer.h'), (Join-Path $repo 'tools\cef_host\FrameTimer.h'))) {
    $metadata.hashes += Get-FileHash -LiteralPath $file -Algorithm SHA256 | Select-Object Path,Hash
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDir 'metadata.json') -Encoding utf8
foreach ($height in $Heights) {
    $width = [int]($height * 16 / 9)
    for ($repeat = 1; $repeat -le $Repetitions; $repeat++) {
        $order = @($Backends)
        if ($repeat % 2 -eq 0) { [array]::Reverse($order) }
        foreach ($backend in $order) {
            if (-not $hosts.ContainsKey($backend)) { throw "Unknown backend: $backend" }
            if (@(RunningGame).Count) { throw 'Starfield started; close it normally and use a new comparison directory.' }
            $name = "$backend-${height}p-r$repeat"
            $run = Join-Path $OutDir $name
            New-Item -ItemType Directory -Path $run | Out-Null
            $profile = Join-Path $OutDir "profiles\$name"
            Write-Host "Running $name ($Seconds seconds per workload)"
            $arguments = @('--host', ('"' + $hosts[$backend] + '"'), '--out', ('"' + $run + '"'), '--views', ('"' + $assets + '"'), '--profile', ('"' + $profile + '"'), '--width', $width, '--height', $height, '--seconds', $Seconds, '--warmup', $Warmup)
            if ($Lifecycle) { $arguments += @('--lifecycle', 'true') }
            $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'stdout.log') -RedirectStandardError (Join-Path $run 'stderr.log')
            $wait = [Diagnostics.Stopwatch]::StartNew()
            $contendingGames = @{}
            do {
                $exited = $process.WaitForExit(1000)
                foreach ($gameProcess in @(RunningGame)) { $contendingGames[$gameProcess.Id] = $gameProcess }
                if (-not $exited -and $wait.ElapsedMilliseconds -gt 240000) { $process.Kill(); throw "Benchmark consumer exceeded timeout: $name" }
            } while (-not $exited)
            Get-Content -LiteralPath (Join-Path $run 'stdout.log')
            if ($contendingGames.Count) {
                @{ reason='Starfield ran during this isolated comparison; performance results are invalid.'; processes=@($contendingGames.Values) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'contention.json') -Encoding utf8
                throw "Starfield ran during $name. Evidence retained; close the game normally and use a new comparison directory."
            }
            if ($process.ExitCode -ne 0) { Get-Content -LiteralPath (Join-Path $run 'stderr.log'); throw "Benchmark failed: $name. Evidence retained in $run" }
        }
    }
}
Write-Host "Comparison complete: $OutDir"
