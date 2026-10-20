[CmdletBinding()]
param([string]$Generator = 'Visual Studio 18 2026')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$json = Join-Path $repo 'tests/native/.deps'
$sdk = if ($env:WEBVIEW2_SDK_DIR) { $env:WEBVIEW2_SDK_DIR } else { Join-Path $repo 'external/webview2' }
if (-not (Test-Path -LiteralPath (Join-Path $json 'nlohmann/json.hpp'))) {
    throw 'Pinned native-test JSON headers are missing.'
}
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'build/native/include/WebView2.h'))) {
    throw 'Run tools/setup.ps1 to prepare the WebView2 SDK.'
}
$build = Join-Path $repo 'build/webview2-tab-audio-probe'
$compiledInputs = @('main.cpp', 'ProductionRoute.inl', 'AudioMeter.cpp', 'AudioMeter.h', 'CMakeLists.txt' |
    ForEach-Object { Join-Path $PSScriptRoot $_ })
$compiledInputs += @('SpatialAudio.cpp', 'SpatialAudio.h', 'EmbeddedAudioExtension.cpp', 'EmbeddedAudioExtension.h',
    'audio-extension/manifest.json', 'audio-extension/collector.html', 'audio-extension/collector.js' |
    ForEach-Object { Join-Path $repo "tools/webview2_host/$_" })
$compiledInputs += @('tools/browser_shared/BrowserAudio.h', 'tools/browser_shared/Win32Util.h', 'sdk/OSFUI_SurfaceAudio.h' |
    ForEach-Object { Join-Path $repo $_ })
function Get-CompiledInputHashes {
    @($compiledInputs | ForEach-Object {
        [pscustomobject]@{ path=[IO.Path]::GetFullPath($_); sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    })
}
$before = Get-CompiledInputHashes
& cmake -S $PSScriptRoot -B $build -G $Generator -A x64 "-DJSON_INCLUDE=$json" "-DWEBVIEW2_SDK=$sdk"
if ($LASTEXITCODE) { throw 'Tab audio probe configuration failed.' }
& cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Tab audio probe build failed.' }
$after = Get-CompiledInputHashes
if (($before | ConvertTo-Json -Compress) -ne ($after | ConvertTo-Json -Compress)) {
    throw 'Compiled inputs changed during the build; rebuild to bind evidence to stable sources.'
}
$binary = Join-Path $build 'Release/osfui_webview2_tab_audio_probe.exe'
[pscustomobject]@{ builtAtUtc=[DateTime]::UtcNow.ToString('o'); sources=$after;
    binarySha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build 'build-inputs.json') -Encoding utf8
Write-Host "Probe: $build/Release/osfui_webview2_tab_audio_probe.exe"
