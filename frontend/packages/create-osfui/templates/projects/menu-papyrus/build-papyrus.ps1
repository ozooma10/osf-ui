[CmdletBinding()]
param(
    [string]$StarfieldRoot,
    [string]$PapyrusCompiler,
    [string]$PapyrusSource,
    [string]$Mo2Mods
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$modRoot = Join-Path $projectRoot 'mod'
$sourceRoot = Join-Path $modRoot 'Scripts/Source'
$scriptOutput = Join-Path $modRoot 'Scripts'
$osfuiApis = Join-Path $projectRoot 'tools/papyrus'

function Resolve-StarfieldRoot {
    $steam = ${env:ProgramFiles(x86)}
    foreach ($candidate in @(
        $StarfieldRoot,
        $env:STARFIELD_ROOT,
        $env:STARFIELD_PATH,
        $(if ($steam) { Join-Path $steam 'Steam/steamapps/common/Starfield' }),
        'C:/XboxGames/Starfield/Content'
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Container)) { return $candidate }
    }
    return $null
}

$root = Resolve-StarfieldRoot
if (-not $PapyrusCompiler) {
    if ($env:PAPYRUS_COMPILER) {
        $PapyrusCompiler = $env:PAPYRUS_COMPILER
    } elseif ($root) {
        $PapyrusCompiler = Join-Path $root 'Tools/Papyrus Compiler/PapyrusCompiler.exe'
    }
}
if (-not $PapyrusSource) {
    if ($env:PAPYRUS_IMPORTS) {
        $PapyrusSource = $env:PAPYRUS_IMPORTS
    } elseif ($root) {
        $PapyrusSource = Join-Path $root 'Data/Scripts/Source'
    }
}

if (-not $PapyrusCompiler -or -not (Test-Path -LiteralPath $PapyrusCompiler -PathType Leaf)) {
    throw 'PapyrusCompiler.exe not found. Install the Starfield Creation Kit (Steam > Library > Tools), or pass -PapyrusCompiler.'
}
$flagsFile = if ($PapyrusSource) { Join-Path $PapyrusSource 'Starfield_Papyrus_Flags.flg' } else { $null }
if (-not $PapyrusSource -or -not (Test-Path -LiteralPath (Join-Path $PapyrusSource 'Quest.psc') -PathType Leaf) -or -not (Test-Path -LiteralPath $flagsFile -PathType Leaf)) {
    throw "Creation Kit script sources not found at '$PapyrusSource'. Unpack Tools/ContentResources.zip, or pass -PapyrusSource."
}

New-Item -ItemType Directory -Force -Path $scriptOutput | Out-Null
Push-Location $sourceRoot
try {
    # The three files under tools/papyrus are compiler declarations only. The
    # installed OSF UI mod supplies their matching PEX files at runtime.
    & $PapyrusCompiler `
        '__OSFUI_SCRIPT_NAME__.psc' `
        "-i=$sourceRoot;$osfuiApis;$PapyrusSource" `
        "-o=$scriptOutput" `
        "-f=$flagsFile"
    if ($LASTEXITCODE -ne 0) { throw "Papyrus compiler exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "[osfui] Compiled __OSFUI_SCRIPT_NAME__.pex"

if (-not $Mo2Mods) {
    Write-Host '[osfui] Pass -Mo2Mods "path-to-MO2-mods" to also deploy the Papyrus backend and settings.'
    exit 0
}
$deployRoot = Join-Path $Mo2Mods '__OSFUI_DISPLAY_NAME__ Backend'
New-Item -ItemType Directory -Force -Path $deployRoot | Out-Null
Copy-Item -Path (Join-Path $modRoot '*') -Destination $deployRoot -Recurse -Force
Write-Host "[osfui] Deployed backend and settings to $deployRoot"
