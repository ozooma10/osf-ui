[CmdletBinding()]
param([string]$Generator = 'Visual Studio 18 2026')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$build = Join-Path $repo 'build/audio-endpoint-meter'
$json = Join-Path $repo 'tests/native/.deps'
if (-not (Test-Path -LiteralPath (Join-Path $json 'nlohmann/json.hpp'))) { throw 'Pinned JSON headers missing.' }
$inputs = @('main.cpp','CMakeLists.txt','../webview2-tab-audio/AudioMeter.cpp','../webview2-tab-audio/AudioMeter.h' |
    ForEach-Object { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $_)) })
function Get-InputHashes {
    @($inputs | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash} })
}
$before = Get-InputHashes
& cmake -S $PSScriptRoot -B $build -G $Generator -A x64 "-DJSON_INCLUDE=$json"
if ($LASTEXITCODE) { throw 'Endpoint meter configuration failed.' }
& cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Endpoint meter build failed.' }
$after = Get-InputHashes
if (($before | ConvertTo-Json -Compress) -ne ($after | ConvertTo-Json -Compress)) { throw 'Sources changed during build. Rebuild.' }
$binary = Join-Path $build 'Release/osfui_audio_endpoint_meter.exe'
[pscustomobject]@{builtAtUtc=[DateTime]::UtcNow.ToString('o');sources=$after;
    binarySha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build 'build-inputs.json') -Encoding utf8
Write-Host "Endpoint meter: $binary"
