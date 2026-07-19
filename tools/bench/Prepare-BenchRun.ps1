<#
.SYNOPSIS
    Prepare a renderer benchmark run: pick the backend and the view, install
    the stress view if asked, and start the resource sampler.

.DESCRIPTION
    One command per run, so the two halves of an A/B can't drift apart by hand.
    It writes the DEPLOYED config (the MO2 mod folder), optionally installs the
    animation-heavy stress view (tools/bench/view — deliberately not part of
    data/OSFUI, so it never ships), verifies the deployed DLL is the freshly
    built one, and launches Sample-OsfUiBench.ps1 detached.

    Then launch Starfield via MO2 and play the session protocol in
    docs/renderer-benchmark.md.

.EXAMPLE
    .\Prepare-BenchRun.ps1 -Renderer webview2 -View stress
    .\Prepare-BenchRun.ps1 -Renderer ultralight -View stress
    .\Prepare-BenchRun.ps1 -Renderer ultralight -View settings
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('webview2', 'webview2-inproc', 'ultralight', 'mock')]
    [string]$Renderer,

    # stress = the animation-heavy benchmark view; settings = the shipped
    # mostly-static view (the idle-cost comparison).
    [ValidateSet('stress', 'settings')]
    [string]$View = 'stress',

    [string]$ModDir = 'C:\Modding\Starfield\MO2\mods\OSF UI',
    [string]$RepoDir = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [switch]$NoSampler
)

$ErrorActionPreference = 'Stop'

if (Get-Process -Name Starfield -ErrorAction SilentlyContinue) {
    throw 'Starfield is running — close it first; the deployed config is read at launch.'
}

$pluginDir = Join-Path $ModDir 'SFSE\Plugins'
$dataDir = Join-Path $pluginDir 'OSFUI'
$viewsDir = Join-Path $dataDir 'views'
$configPath = Join-Path $dataDir 'config.json'
if (-not (Test-Path $configPath)) { throw "deployed config not found: $configPath" }

# --- freshness: the deployed DLL must be the one just built -------------
$builtDll = Join-Path $RepoDir 'build\windows\x64\releasedbg\OSFUI.dll'
$deployedDll = Join-Path $pluginDir 'OSFUI.dll'
if ((Test-Path $builtDll) -and (Test-Path $deployedDll)) {
    $a = (Get-FileHash $builtDll).Hash
    $b = (Get-FileHash $deployedDll).Hash
    if ($a -ne $b) {
        Write-Warning "deployed OSFUI.dll differs from the last build — run 'xmake' first (build $($a.Substring(0,8)) vs deployed $($b.Substring(0,8)))"
    } else {
        Write-Host "DLL fresh ($($b.Substring(0,12)))"
    }
}

# --- stress view install ------------------------------------------------
$viewId = if ($View -eq 'stress') { 'osfui.bench/stress' } else { 'osfui/settings' }
if ($View -eq 'stress') {
    $src = Join-Path $PSScriptRoot 'view\osfui.bench'
    if (-not (Test-Path $src)) { throw "stress view source not found: $src" }
    $dst = Join-Path $viewsDir 'osfui.bench'
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    Copy-Item $src $dst -Recurse -Force
    Write-Host "installed stress view -> $dst"
} else {
    # Keep runs clean: a loaded-but-unused animating view would burn CPU in a
    # measurement that is supposed to be about the settings view.
    $stale = Join-Path $viewsDir 'osfui.bench'
    if (Test-Path $stale) { Remove-Item $stale -Recurse -Force; Write-Host 'removed stress view' }
}

# --- config -------------------------------------------------------------
$config = Get-Content $configPath -Raw | ConvertFrom-Json
$config.renderer = $Renderer
$config.view = $viewId
# Single-view backends cap this list anyway; keep it to exactly the subject
# view so neither backend loads extra pages the other doesn't.
$config.views = @($viewId)
$config | Add-Member -NotePropertyName benchStats -NotePropertyValue $true -Force
# WriteAllText with an explicit no-BOM encoding: Set-Content -Encoding UTF8
# emits a BOM on Windows PowerShell 5.1, and a BOM in front of a config the
# plugin parses is a needless risk.
$json = $config | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($configPath, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "config: renderer=$Renderer view=$viewId benchStats=true"

# --- sampler ------------------------------------------------------------
if (-not $NoSampler) {
    $label = "$Renderer-$View"
    $results = Join-Path $PSScriptRoot 'results'
    New-Item -ItemType Directory -Force $results | Out-Null
    $out = Join-Path $results "sampler-$label.out.txt"
    if (Test-Path $out) { Remove-Item $out -Force }
    # Built with -f, not interpolation: Windows PowerShell 5.1 cannot parse
    # same-type quotes nested inside a $() subexpression in a double-quoted
    # string, and this script has to run in whatever console the user has.
    $sampler = Join-Path $PSScriptRoot 'Sample-OsfUiBench.ps1'
    $command = "& '{0}' -Label {1} -WaitForGameMinutes 240 *> '{2}'" -f $sampler, $label, $out
    Start-Process pwsh -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command
    Start-Sleep -Seconds 2
    Write-Host "sampler armed (label $label) — log: $out"
}

Write-Host ''
Write-Host 'Ready. Launch Starfield (SFSE) via MO2 with +OSF UI enabled, then:'
Write-Host '  60s closed -> F10 -> 4 min open (2 full scene cycles) -> Esc -> quit'
