<# Negative checks against the packager's disposable staging trees. Every mutation is restored. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$StagingRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath($StagingRoot)
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $root.StartsWith((Join-Path $repo 'build') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Negative checks require disposable repository build staging'
}
$validator = Join-Path $PSScriptRoot 'test-package.ps1'
$cef = Join-Path $root 'cef/Data'
$wv = Join-Path $root 'webview2/Data'
$uiRelative = 'SFSE/Plugins/OSF/UI'
$script:checks = 0
function ExpectFailure([string]$Data, [string]$Backend, [string]$Message) {
    $caught = $null
    try { & $validator -DataRoot $Data -ExpectedBackend $Backend | Out-Null }
    catch { $caught = $_.Exception.Message }
    if (-not $caught -or $caught -notlike "*$Message*") { throw "Expected '$Message'; got '$caught'" }
    $script:checks++
}
ExpectFailure $wv 'cef' 'wrong backend'
$broker = Join-Path $wv "$uiRelative/bin/broker/osfui_browser_broker.exe"
$brokerBytes = [IO.File]::ReadAllBytes($broker)
try {
    Remove-Item -LiteralPath $broker
    ExpectFailure $wv 'webview2' 'Missing OSF UI-owned package path'
} finally { [IO.File]::WriteAllBytes($broker, $brokerBytes) }
$selection = Join-Path $cef "$uiRelative/backend.json"
$original = [IO.File]::ReadAllBytes($selection)
try {
    '{"schemaVersion":1,"backend":"unknown"}' | Set-Content -LiteralPath $selection
    ExpectFailure $cef 'cef' 'Invalid package backend'
} finally { [IO.File]::WriteAllBytes($selection, $original) }
$extra = Join-Path $cef "$uiRelative/bin/osfui_webview2_host.exe"
if (Test-Path -LiteralPath $extra) { throw 'Unexpected mixed fixture before test' }
try {
    [IO.File]::WriteAllText($extra, 'unexpected helper')
    ExpectFailure $cef 'cef' 'another browser payload'
} finally { Remove-Item -LiteralPath $extra -Force }
$license = Join-Path $cef "$uiRelative/bin/cef/CEF-LICENSE.txt"
$original = [IO.File]::ReadAllBytes($license)
try {
    [IO.File]::WriteAllText($license, 'tampered')
    ExpectFailure $cef 'cef' 'hash mismatch'
    Remove-Item -LiteralPath $license -Force
    ExpectFailure $cef 'cef' 'Incomplete CEF runtime'
} finally { [IO.File]::WriteAllBytes($license, $original) }
$manifestPath = Join-Path $cef "$uiRelative/bin/cef/runtime-manifest.json"
$original = [IO.File]::ReadAllBytes($manifestPath)
try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.files[0].path = '../escape.dll'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    ExpectFailure $cef 'cef' 'Invalid CEF runtime manifest entry'
} finally { [IO.File]::WriteAllBytes($manifestPath, $original) }
Write-Host "$script:checks package rejection checks passed; staging restored."
