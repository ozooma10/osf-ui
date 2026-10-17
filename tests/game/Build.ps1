[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$mods=$env:XSE_SF_MODS_PATH; $game=$env:XSE_SF_GAME_PATH
try {
    $env:XSE_SF_MODS_PATH=$null; $env:XSE_SF_GAME_PATH=$null
    Push-Location $repo
    try { & xmake build osfui-game-consumer; if($LASTEXITCODE){throw 'Consumer build failed.'} } finally { Pop-Location }
    $compiler=$env:PAPYRUS_COMPILER
    if(-not $compiler){$compiler='C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe'}
    $imports=$env:PAPYRUS_IMPORTS
    if(-not $imports){$imports='C:\Modding\Starfield\PapyrusSource'}
    Push-Location $PSScriptRoot
    try {
        & $compiler 'OSFUIBackendPapyrus.psc' "-i=$PSScriptRoot;$repo\data\Scripts\Source;$imports" "-o=$repo\build\papyrus" "-f=$imports\Starfield_Papyrus_Flags.flg"
        if($LASTEXITCODE){throw 'Papyrus consumer build failed.'}
    } finally { Pop-Location }
} finally { $env:XSE_SF_MODS_PATH=$mods; $env:XSE_SF_GAME_PATH=$game }
