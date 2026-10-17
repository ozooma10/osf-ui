[CmdletBinding()]
param(
    [ValidateSet('baseline','reuse','copyonly','discard','stock','webview2')]
    [string[]]$Cases = @('stock', 'baseline'),
    [int]$Repetitions = 3,
    [int]$Height = 1080,
    [double]$Seconds = 6,
    [double]$Warmup = 2,
    [switch]$Trace,
    [switch]$GpuQueries,
    [Parameter(Mandatory)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function RunningGame { @(Get-Process -Name Starfield -ErrorAction SilentlyContinue | Select-Object Id,StartTime) }
if (@(RunningGame).Count) { throw 'Close Starfield normally before profiling browser helpers.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $OutDir) { throw 'Use a fresh evidence directory.' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$hosts = @{
    baseline = (Join-Path $repo 'build\cef-audit\bin\runtime\osfui_cef_host.exe')
    reuse = (Join-Path $repo 'build\cef-audit\bin\runtime\osfui_cef_host.exe')
    copyonly = (Join-Path $repo 'build\cef-audit\bin\runtime\osfui_cef_host.exe')
    discard = (Join-Path $repo 'build\cef-audit\bin\runtime\osfui_cef_host.exe')
    stock = (Join-Path $repo 'build\cef-host\runtime\osfui_cef_host.exe')
    webview2 = (Join-Path $repo 'build\windows\x64\releasedbg\osfui_webview2_host.exe')
}
$exe = Join-Path $repo 'build\backend-bench\Release\osfui_backend_bench.exe'
$assets = Join-Path $OutDir 'views'
foreach ($view in 'main','second','third') {
    $path = Join-Path $assets "bench\$view"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\backend-bench\view.html') -Destination (Join-Path $path 'index.html')
}
$metadata = [ordered]@{
    started = (Get-Date).ToString('o'); cases = $Cases; height = $Height; seconds = $Seconds; warmup = $Warmup
    repetitions = $Repetitions; trace = [bool]$Trace; gpuQueries = [bool]$GpuQueries
    commit = (& git -C $repo rev-parse HEAD)
    hashes = @(Get-FileHash -LiteralPath $exe,(Join-Path $repo 'build\cef-audit\bin\runtime\osfui_cef_host.dll'),(Join-Path $repo 'build\cef-host\runtime\osfui_cef_host.dll'),$hosts.webview2,(Join-Path $assets 'bench\main\index.html') | Select-Object Path,Hash)
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDir 'metadata.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $repo 'build\cef-audit\source\source-hashes.json') -Destination $OutDir
$sourceEvidence = Join-Path $OutDir 'diagnostic-sources'
New-Item -ItemType Directory -Path $sourceEvidence | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repo 'build\cef-audit\source') -File | Copy-Item -Destination $sourceEvidence
Copy-Item -LiteralPath (Join-Path $repo 'build\cef-audit\bin\runtime\osfui_cef_host.dll') -Destination $OutDir
$oldMode = $env:OSFUI_CEF_AUDIT_MODE
$oldTrace = $env:OSFUI_CEF_AUDIT_TRACE
$oldGpu = $env:OSFUI_CEF_AUDIT_GPU
try {
    $env:OSFUI_CEF_AUDIT_TRACE = if ($Trace) { '1' } else { '0' }
    $env:OSFUI_CEF_AUDIT_GPU = if ($GpuQueries) { '1' } else { '0' }
    for ($repeat = 1; $repeat -le $Repetitions; $repeat++) {
        # Rotate the first variant each repetition to reduce ordering bias.
        for ($offset = 0; $offset -lt $Cases.Count; $offset++) {
            $case = $Cases[($offset + $repeat - 1) % $Cases.Count]
            if (@(RunningGame).Count) { throw 'Starfield started during this desktop audit.' }
            if (-not $hosts.ContainsKey($case)) { throw "Unknown case: $case" }
            $env:OSFUI_CEF_AUDIT_MODE = if ($case -in @('stock','webview2')) { $null } else { $case }
            $name = "$case-${Height}p-r$repeat"
            $run = Join-Path $OutDir $name
            New-Item -ItemType Directory -Path $run | Out-Null
            Write-Host "Running $name"
            $arguments = @('--backend', $(if ($case -eq 'webview2') { 'webview2' } else { 'cef' }), '--host', ('"' + $hosts[$case] + '"'), '--out', ('"' + $run + '"'), '--views', ('"' + $assets + '"'), '--profile', ('"' + (Join-Path $OutDir "profiles\$name") + '"'), '--width', ([int]($Height * 16 / 9)), '--height', $Height, '--seconds', $Seconds, '--warmup', $Warmup)
            if ($case -in @('copyonly','discard')) { $arguments += @('--allow-no-output', 'true') }
            $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'stdout.log') -RedirectStandardError (Join-Path $run 'stderr.log')
            $wait = [Diagnostics.Stopwatch]::StartNew()
            $contendingGames = @{}
            do {
                $exited = $process.WaitForExit(1000)
                foreach ($gameProcess in @(RunningGame)) { $contendingGames[$gameProcess.Id] = $gameProcess }
                if (-not $exited -and $wait.Elapsed.TotalSeconds -gt (90 + 4 * ($Seconds + $Warmup))) { $process.Kill(); throw "Timed out: $name" }
            } while (-not $exited)
            if ($contendingGames.Count) {
                @{reason='Starfield ran during the desktop audit.';processes=@($contendingGames.Values)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'contention.json')
                throw "Game contention invalidated $name; evidence retained."
            }
            if ($process.ExitCode -ne 0) { Get-Content -LiteralPath (Join-Path $run 'stderr.log'); throw "Failed: $name" }
            Get-Content -LiteralPath (Join-Path $run 'stdout.log')
        }
    }
} finally {
    $env:OSFUI_CEF_AUDIT_MODE = $oldMode
    $env:OSFUI_CEF_AUDIT_TRACE = $oldTrace
    $env:OSFUI_CEF_AUDIT_GPU = $oldGpu
}
Write-Host "Audit evidence: $OutDir"
