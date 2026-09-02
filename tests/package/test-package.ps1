[CmdletBinding()]
param([Parameter(Mandatory)][string]$DataRoot)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Staged Data root does not exist: $root"
}

$required = @(
    'SFSE\Plugins\OSFUI.dll',
    'SFSE\Plugins\OSF\UI\bin\osfui_webview2_host.exe',
    'SFSE\Plugins\OSF\UI\views\shared\osfui.js',
    'SFSE\Plugins\OSF\Settings\schemas\osfui.json',
    'Scripts\OSFUI.pex',
    'Scripts\OSFUI_View.pex'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative))) {
        throw "Missing OSF UI-owned package path: $relative"
    }
}

foreach ($relative in 'SFSE\Plugins\OSFUI', 'SFSE\Plugins\OSFSettings.dll',
        'Scripts\OSFUI_Settings.pex') {
    if (Test-Path -LiteralPath (Join-Path $root $relative)) {
        throw "OSF UI package contains legacy or sibling-owned path: $relative"
    }
}

$settings = Join-Path $root 'SFSE\Plugins\OSF\Settings'
$ownedSchema = [IO.Path]::GetFullPath((Join-Path $settings 'schemas\osfui.json'))
$unexpected = Get-ChildItem -LiteralPath $settings -Recurse -File |
    Where-Object { [IO.Path]::GetFullPath($_.FullName) -ne $ownedSchema }
if ($unexpected) {
    throw 'OSF UI package owns Settings files other than schemas/osfui.json'
}

# Guard the synchronizer itself: it may replace OSF/UI/views but must never
# delete the shared OSF parent or any OSF Settings directory.
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$sync = Get-Content -LiteralPath (Join-Path $repo 'tools\xmake\runtime_payload.lua') -Raw
if ($sync -match 'os\.rm\(\s*(?:uidata|pluginsdir|settings)\s*\)') {
    throw 'runtime_payload.lua can delete a shared or sibling-owned directory'
}
if ($sync -notmatch 'os\.rm\(views\)') {
    throw 'runtime_payload.lua must synchronize only OSF UI-owned views'
}

Write-Host 'OSF UI package ownership test passed'
