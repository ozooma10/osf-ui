[CmdletBinding()]
param(
    [string]$Compiler = $env:PAPYRUS_COMPILER,
    [string]$Imports = $env:PAPYRUS_IMPORTS,
    [switch]$WorldScreens,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'WorkflowCache.ps1')
if (-not $Compiler) {
    $Compiler = 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe'
}
if (-not $Imports) { $Imports = 'C:\Modding\Starfield\PapyrusSource' }
$source = Join-Path $repoRoot 'data\Scripts\Source'
$scriptName = 'OSFUI'
if ($WorldScreens) {
    $source = Join-Path $repoRoot 'extensions\world-screens\data\Scripts\Source'
    $scriptName = 'OSFUIWorldScreens'
}
$output = Join-Path $repoRoot 'build\papyrus'
$flags = Join-Path $Imports 'Starfield_Papyrus_Flags.flg'
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf) -or
    -not (Test-Path -LiteralPath $flags -PathType Leaf)) {
    throw 'Papyrus compiler or vanilla imports missing. Set PAPYRUS_COMPILER and PAPYRUS_IMPORTS, or pass -Compiler and -Imports.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$pex = Join-Path $output "$scriptName.pex"
$null = Invoke-WorkflowCached -Stamp (Join-Path $output "$scriptName.cache.json") -Force:$Force -Fingerprint {
    Get-WorkflowFingerprint -Paths @($source, $Imports, (Split-Path $Compiler -Parent), $PSCommandPath,
        (Join-Path $PSScriptRoot 'WorkflowCache.ps1')) -Values @($Compiler, $scriptName)
} -Outputs { @($pex) } -Action {
    Push-Location $source
    try {
        # A failed compiler must not leave an older PEX eligible for a new receipt.
        if (Test-Path -LiteralPath $pex) { Remove-Item -LiteralPath $pex -Force }
        & $Compiler "$scriptName.psc" "-i=$source;$Imports" "-o=$output" "-f=$flags"
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $pex)) {
            throw "$scriptName Papyrus compilation failed."
        }
    } finally {
        Pop-Location
    }
}
