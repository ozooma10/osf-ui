[CmdletBinding()]
param([string]$Generator = 'Visual Studio 18 2026')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$json = Join-Path $repo 'tests/native/.deps'
if (-not (Test-Path (Join-Path $json 'nlohmann/json.hpp'))) {
    throw 'Run the native tests once to prepare the pinned JSON headers.'
}
$build = Join-Path $repo 'build/browser-audio-probe'
& cmake -S $PSScriptRoot -B $build -G $Generator -A x64 "-DJSON_INCLUDE=$json"
if ($LASTEXITCODE) { throw 'Audio probe configuration failed.' }
& cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Audio probe build failed.' }
Write-Host "Probe: $build/Release/osfui_browser_audio_probe.exe"
