[CmdletBinding()]
param([string]$JsonInclude, [string]$Generator = 'Visual Studio 18 2026', [switch]$AudioProbe)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $repo 'external\cef'
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cef_host\cef.lock.json') -Raw | ConvertFrom-Json
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'include\cef_version.h'))) {
    throw 'Run tools/setup-cef.ps1 first.'
}
if (-not (Get-Content -LiteralPath (Join-Path $sdk 'include\cef_version.h') -Raw).Contains('"' + $pin.version + '"')) {
    throw 'The CEF SDK does not match tools/cef_host/cef.lock.json.'
}
if (-not $JsonInclude) {
    $packageRoot = Join-Path $env:LOCALAPPDATA '.xmake\packages\n\nlohmann_json\v3.11.3'
    $match = Get-ChildItem -Path "$packageRoot\*\include\nlohmann\json.hpp" -Force -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $match) { throw 'Pass -JsonInclude with the nlohmann_json 3.11.3 include directory.' }
    $JsonInclude = Split-Path (Split-Path $match.FullName -Parent) -Parent
}
$build = Join-Path $repo $(if($AudioProbe){'build\cef-audio-host'}else{'build\cef-host'})
& cmake -S (Join-Path $PSScriptRoot 'cef_host') -B $build -G $Generator -A x64 "-DCEF_ROOT=$sdk" "-DJSON_INCLUDE=$JsonInclude" "-DOSFUI_CEF_AUDIO_PROBE=$(if($AudioProbe){'ON'}else{'OFF'})"
if ($LASTEXITCODE -ne 0) { throw 'CEF CMake configure failed.' }
& cmake --build $build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'CEF host build failed.' }
$runtime = Join-Path $build 'runtime'
$files = @(Get-ChildItem -LiteralPath $runtime -Recurse -File | Where-Object Name -ne 'runtime-manifest.json' |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($runtime, $_.FullName).Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    })
[ordered]@{ schemaVersion = 1; cefVersion = $pin.version; files = $files } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runtime 'runtime-manifest.json') -Encoding utf8NoBOM
Write-Host "CEF runtime ready at $build\runtime"
