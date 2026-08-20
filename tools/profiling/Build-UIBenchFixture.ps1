#requires -Version 7.2
<#
.SYNOPSIS
  Build and stage the dual-framework UIBench SFSE mod.

.DESCRIPTION
  Builds the tiny optional UIBench consumer plugin, stages a MO2-ready mod under
  build/profiles/ui-bench/fixture-mod, and verifies that both framework paths
  contain the same HTML bytes. It never writes to a live MO2 profile.
#>
[CmdletBinding()]
param(
    [ValidateSet('static', 'transforms', 'repaint', 'layout', 'text-scroll', 'canvas')]
    [string] $Scenario = 'static',

    [ValidatePattern('^\d{3,5}x\d{3,5}$')]
    [string] $Resolution = '1920x1080',

    [string] $OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$fixture = Join-Path $PSScriptRoot 'fixture'
if (-not $OutputPath) {
    $OutputPath = Join-Path $repo 'build\profiles\ui-bench\fixture-mod'
}
New-Item -ItemType Directory -Force -Path $OutputPath | Out-Null

$xmake = Get-Command xmake.exe -ErrorAction Stop | Select-Object -First 1
$priorModsPath = [Environment]::GetEnvironmentVariable('XSE_SF_MODS_PATH', 'Process')
$priorGamePath = [Environment]::GetEnvironmentVariable('XSE_SF_GAME_PATH', 'Process')
try {
    # The CommonLib rule auto-installs after build. Constrain that install to
    # fixture/build/install instead of honoring a developer's live deploy env.
    [Environment]::SetEnvironmentVariable('XSE_SF_MODS_PATH', $null, 'Process')
    [Environment]::SetEnvironmentVariable('XSE_SF_GAME_PATH', $null, 'Process')
    & $xmake.Source f -P $fixture -m releasedbg
    if ($LASTEXITCODE -ne 0) { throw "xmake configure failed with exit code $LASTEXITCODE" }
    & $xmake.Source build -P $fixture UIBench
    if ($LASTEXITCODE -ne 0) { throw "xmake build failed with exit code $LASTEXITCODE" }
} finally {
    [Environment]::SetEnvironmentVariable('XSE_SF_MODS_PATH', $priorModsPath, 'Process')
    [Environment]::SetEnvironmentVariable('XSE_SF_GAME_PATH', $priorGamePath, 'Process')
}

$builtDll = @(
    (Join-Path $fixture 'build\install\Data\SFSE\Plugins\UIBench.dll')
    (Join-Path $repo 'build\windows\x64\releasedbg\UIBench.dll')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $builtDll) { throw 'UIBench.dll was not produced by the build.' }

$pluginDirectory = Join-Path $OutputPath 'SFSE\Plugins'
$canonicalDirectory = Join-Path $OutputPath 'UIBench\fixture'
New-Item -ItemType Directory -Force -Path $pluginDirectory, $canonicalDirectory | Out-Null
Copy-Item -LiteralPath $builtDll -Destination (Join-Path $pluginDirectory 'UIBench.dll') -Force
$builtPdb = [IO.Path]::ChangeExtension($builtDll, '.pdb')
if (Test-Path -LiteralPath $builtPdb -PathType Leaf) {
    Copy-Item -LiteralPath $builtPdb -Destination (Join-Path $pluginDirectory 'UIBench.pdb') -Force
}
Copy-Item -LiteralPath (Join-Path $fixture 'content\index.html') -Destination (Join-Path $canonicalDirectory 'index.html') -Force

& (Join-Path $PSScriptRoot 'Set-UIBenchFixture.ps1') -ModPath $OutputPath -Scenario $Scenario -Resolution $Resolution

$archive = "$OutputPath.zip"
Compress-Archive -Path (Join-Path $OutputPath '*') -DestinationPath $archive -Force
Write-Host "MO2-ready fixture: $OutputPath" -ForegroundColor Green
Write-Host "Archive: $archive"
