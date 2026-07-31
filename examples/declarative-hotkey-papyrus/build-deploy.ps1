[CmdletBinding()]
param(
    [string]$PapyrusCompiler = 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe',
    [string]$PapyrusSource = 'C:\Modding\Starfield\PapyrusSource',
    [string]$Mo2Mods = 'C:\Modding\Starfield\MO2\mods'
)

$ErrorActionPreference = 'Stop'
$sampleRoot = $PSScriptRoot
$modRoot = Join-Path $sampleRoot 'mod'
$sourceRoot = Join-Path $modRoot 'Scripts\Source'
$scriptOutput = Join-Path $modRoot 'Scripts'
$flagsFile = Join-Path $PapyrusSource 'Starfield_Papyrus_Flags.flg'
$deployRoot = Join-Path $Mo2Mods 'OSF UI Declarative Hotkey Sample'

foreach ($required in @($PapyrusCompiler, $PapyrusSource, $flagsFile)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required Papyrus path not found: $required"
    }
}

New-Item -ItemType Directory -Force -Path $scriptOutput | Out-Null
Push-Location $sourceRoot
try {
    & $PapyrusCompiler `
        'OSFUIHotkeySample.psc' `
        "-i=$sourceRoot;$PapyrusSource" `
        "-o=$scriptOutput" `
        "-f=$flagsFile"
    if ($LASTEXITCODE -ne 0) {
        throw "Papyrus compiler exited with code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

New-Item -ItemType Directory -Force -Path $deployRoot | Out-Null
Copy-Item -Path (Join-Path $modRoot '*') -Destination $deployRoot -Recurse -Force
Copy-Item -LiteralPath (Join-Path $sampleRoot 'README.md') -Destination $deployRoot -Force

Write-Host "Compiled and deployed to $deployRoot"
Write-Host 'Refresh MO2 (F5), enable the sample mod, load a save, close all menus, and press F8.'
