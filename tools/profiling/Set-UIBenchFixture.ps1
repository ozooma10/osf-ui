#requires -Version 7.2
<#
.SYNOPSIS
  Configure and verify an installed or staged UIBench fixture mod.

.DESCRIPTION
  Copies one canonical HTML file into the OSF UI and Carbon UI content
  locations, writes the OSF UI view manifest at the requested pixel size, and
  writes the Carbon-side bootstrap config. Run while Starfield is stopped.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ModPath,

    [Parameter(Mandatory)]
    [ValidateSet('static', 'transforms', 'repaint', 'layout', 'text-scroll', 'canvas')]
    [string] $Scenario,

    [Parameter(Mandatory)]
    [ValidatePattern('^\d{3,5}x\d{3,5}$')]
    [string] $Resolution
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedMod = (Resolve-Path -LiteralPath $ModPath -ErrorAction Stop).Path
$canonical = Join-Path $resolvedMod 'UIBench\fixture\index.html'
if (-not (Test-Path -LiteralPath $canonical -PathType Leaf)) {
    throw "Canonical UIBench document not found: $canonical"
}

$parts = $Resolution -split 'x'
$width = [int]$parts[0]
$height = [int]$parts[1]
if ($width -lt 640 -or $height -lt 480 -or $width -gt 16384 -or $height -gt 16384) {
    throw "Unsupported benchmark resolution: $Resolution"
}

$hash = (Get-FileHash -LiteralPath $canonical -Algorithm SHA256).Hash
$osfDirectory = Join-Path $resolvedMod 'SFSE\Plugins\OSFUI\views\uibench\shootout'
$configDirectory = Join-Path $resolvedMod 'SFSE\Plugins\UIBench'
$carbonDirectory = $configDirectory
foreach ($directory in @($osfDirectory, $carbonDirectory, $configDirectory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

$osfHtml = Join-Path $osfDirectory 'index.html'
$carbonHtml = Join-Path $carbonDirectory 'index.html'
Copy-Item -LiteralPath $canonical -Destination $osfHtml -Force
Copy-Item -LiteralPath $canonical -Destination $carbonHtml -Force

$query = "scenario=$Scenario&width=$width&height=$height&fixtureHash=$hash"
$manifest = [ordered]@{
    manifestVersion = 1
    mod = 'uibench'
    title = 'UIBench renderer shootout'
    description = 'Identical deterministic document used by the OSF UI / Carbon UI performance toolbench.'
    entry = "index.html?$query"
    hub = $false
    width = $width
    height = $height
    transparent = $false
    kind = 'menu'
    capturesInput = $false
    pausesGame = $false
    openOnStart = $false
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $osfDirectory 'manifest.json') -Encoding utf8NoBOM

$config = [ordered]@{
    schemaVersion = 1
    scenario = $Scenario
    width = $width
    height = $height
    fixtureHash = $hash
}
$config | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $configDirectory 'config.json') -Encoding utf8NoBOM

$hashes = @(@($canonical, $osfHtml, $carbonHtml) | ForEach-Object {
    (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
} | Select-Object -Unique)
if ($hashes.Count -ne 1 -or $hashes[0] -ne $hash) {
    throw 'Fixture verification failed: the OSF UI and Carbon UI documents are not byte-identical.'
}

[pscustomobject]@{
    ModPath = $resolvedMod
    Scenario = $Scenario
    Resolution = $Resolution
    FixtureSha256 = $hash
    OSFDocument = $osfHtml
    CarbonDocument = $carbonHtml
} | Format-List

Write-Host 'UIBench configured. Restart Starfield before capturing this resolution.' -ForegroundColor Green
