[CmdletBinding()]
param(
    [string]$Compiler = $env:PAPYRUS_COMPILER,
    [string]$Imports = $env:PAPYRUS_IMPORTS
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Compiler) {
    $Compiler = 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe'
}
if (-not $Imports) { $Imports = 'C:\Modding\Starfield\PapyrusSource' }
$source = Join-Path $repoRoot 'data\Scripts\Source'
$output = Join-Path $repoRoot 'build\papyrus'
$flags = Join-Path $Imports 'Starfield_Papyrus_Flags.flg'
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf) -or
    -not (Test-Path -LiteralPath $flags -PathType Leaf)) {
    throw 'Papyrus compiler or vanilla imports missing. Set PAPYRUS_COMPILER and PAPYRUS_IMPORTS, or pass -Compiler and -Imports.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
Push-Location $source
try {
    & $Compiler 'OSFUI.psc' "-i=$source;$Imports" "-o=$output" "-f=$flags"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $output 'OSFUI.pex'))) {
        throw 'OSFUI Papyrus compilation failed.'
    }
} finally {
    Pop-Location
}
