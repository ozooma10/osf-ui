[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataRoot,
    [ValidateSet('webview2', 'cef')][string]$ExpectedBackend
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Staged Data root does not exist: $root"
}

$required = @(
    'SFSE\Plugins\OSFUI.dll',
    'SFSE\Plugins\OSF\UI\bin\broker\osfui_browser_broker.exe',
    'SFSE\Plugins\OSF\UI\views\shared\osfui.js',
    'SFSE\Plugins\OSF\UI\views\shared\osfui.css',
    'SFSE\Plugins\OSF\UI\views\shared\gamepadnav.js',
    'SFSE\Plugins\OSFUI\views\shared\osfui.js',
    'SFSE\Plugins\OSFUI\views\shared\osfui.css',
    'SFSE\Plugins\OSF\Settings\schemas\osfui.json',
    'Scripts\OSFUI.pex',
    'Scripts\Source\OSFUI.psc'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
        throw "Missing OSF UI-owned package path: $relative"
    }
}

$ui = Join-Path $root 'SFSE/Plugins/OSF/UI'
$description = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $root 'SFSE/Plugins/OSFUI.dll')).FileDescription
if ($description -notmatch '^Web Interface Framework for Starfield \((webview2|cef)\)$') {
    throw 'DLL is missing its compiled browser edition identity'
}
$backend = $Matches[1]
if ($ExpectedBackend -and $backend -ne $ExpectedBackend) { throw 'DLL selects the wrong backend' }
if (Test-Path -LiteralPath (Join-Path $ui 'backend.json')) { throw 'Obsolete backend.json must not ship' }
$bin = Join-Path $ui 'bin'
$brokerFiles = @(Get-ChildItem -LiteralPath (Join-Path $bin 'broker') -Recurse -File)
if ($brokerFiles.Count -ne 1) { throw 'Shared broker directory contains unexpected payload' }
if ($backend -eq 'webview2') {
    $runtimeFiles = @(Get-ChildItem -LiteralPath $bin -Recurse -File | Where-Object DirectoryName -ne (Join-Path $bin 'broker'))
    if ($runtimeFiles.Count -ne 1 -or $runtimeFiles[0].FullName -ne (Join-Path $bin 'osfui_webview2_host.exe')) {
        throw 'WebView2 edition must contain exactly its selected browser helper'
    }
} else {
    $cef = [IO.Path]::GetFullPath((Join-Path $bin 'cef'))
    if (@(Get-ChildItem -LiteralPath $bin).Count -ne 2) { throw 'CEF edition contains another browser payload' }
    foreach ($name in 'osfui_cef_host.exe', 'osfui_cef_host.dll', 'libcef.dll', 'icudtl.dat', 'resources.pak',
            'cef.lock.json', 'runtime-manifest.json', 'CEF-LICENSE.txt', 'CREDITS.html', 'locales/en-US.pak') {
        if (-not (Test-Path -LiteralPath (Join-Path $cef $name) -PathType Leaf)) { throw "Incomplete CEF runtime: $name" }
    }
    $pinPath = Join-Path $PSScriptRoot '../../tools/cef_host/cef.lock.json'
    if ((Get-FileHash -LiteralPath $pinPath).Hash -ne (Get-FileHash -LiteralPath (Join-Path $cef 'cef.lock.json')).Hash) {
        throw 'Packaged CEF distribution does not match the pinned SDK'
    }
    $manifest = Get-Content -LiteralPath (Join-Path $cef 'runtime-manifest.json') -Raw | ConvertFrom-Json
    $pin = Get-Content -LiteralPath $pinPath -Raw | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 1 -or $manifest.cefVersion -ne $pin.version) { throw 'Invalid CEF runtime manifest' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest.files) {
        $path = [IO.Path]::GetFullPath((Join-Path $cef $entry.path))
        if ($entry.path.Contains(':') -or -not $path.StartsWith($cef + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
                -not $seen.Add($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Invalid CEF runtime manifest entry: $($entry.path)"
        }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "CEF runtime hash mismatch: $($entry.path)" }
    }
    $actual = @(Get-ChildItem -LiteralPath $cef -Recurse -File | Where-Object Name -ne 'runtime-manifest.json')
    if ($actual.Count -ne $seen.Count) { throw 'CEF runtime contains files outside its manifest' }
}

$schema = Get-Content -LiteralPath (Join-Path $root 'SFSE/Plugins/OSF/Settings/schemas/osfui.json') -Raw | ConvertFrom-Json
if (@($schema.groups.Developer | Where-Object key -eq 'cefPrototype').Count) { throw 'Obsolete renderer toggle must not ship' }
Get-ChildItem -LiteralPath (Join-Path $ui 'views') -Recurse -Filter 'manifest.json' | ForEach-Object {
    $view = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
    if ($view.PSObject.Properties.Name -contains 'debugOnly' -and $view.debugOnly) { throw 'Debug-only view must not ship' }
}

foreach ($relative in 'SFSE\Plugins\OSFUI\settings', 'SFSE\Plugins\OSFUI\views\osfui',
        'SFSE\Plugins\OSFSettings.dll',
        'SFSE\Plugins\OSFUISettingsExample.dll', 'SFSE\Plugins\OSF\UI\views\osfui-example',
        'SFSE\Plugins\OSFUIBackendTest.dll', 'SFSE\Plugins\OSF\UI\views\osfui-backend-test',
        'Scripts\OSFUIBackendPapyrus.pex',
        'Scripts\OSFUI_Settings.pex', 'Scripts\OSFUI_View.pex',
        'Scripts\Source\OSFUI_Settings.psc', 'Scripts\Source\OSFUI_View.psc') {
    if (Test-Path -LiteralPath (Join-Path $root $relative)) {
        throw "OSF UI package contains legacy or sibling-owned path: $relative"
    }
}

$legacyRoot = Join-Path $root 'SFSE\Plugins\OSFUI'
$legacyOwned = @('views\shared\osfui.js', 'views\shared\osfui.css')
foreach ($file in Get-ChildItem -LiteralPath $legacyRoot -Recurse -File -Force) {
    if ([IO.Path]::GetRelativePath($legacyRoot, $file.FullName) -notin $legacyOwned) {
        throw "Unexpected legacy compatibility payload: $($file.FullName)"
    }
}

$settings = Join-Path $root 'SFSE\Plugins\OSF\Settings'
$ownedSchema = [IO.Path]::GetFullPath((Join-Path $settings 'schemas\osfui.json'))
$unexpected = Get-ChildItem -LiteralPath $settings -Recurse -File |
    Where-Object { [IO.Path]::GetFullPath($_.FullName) -ne $ownedSchema }
if ($unexpected) {
    throw 'OSF UI package owns Settings files other than schemas/osfui.json'
}

# The synchronizer may replace OSF/UI/views/shared but must never delete the OSF parent or OSF Settings dirs.
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$sync = Get-Content -LiteralPath (Join-Path $repo 'tools\xmake\runtime_payload.lua') -Raw
if ($sync -match 'os\.rm\(\s*(?:views|uidata|pluginsdir|settings)\s*\)') {
    throw 'runtime_payload.lua can delete a shared or sibling-owned directory'
}
if ($sync -notmatch 'os\.rm\(path\.join\(views, "shared"\)\)') {
    throw 'runtime_payload.lua must synchronize only OSF UI-owned views'
}

& python (Join-Path $PSScriptRoot 'verify_binary.py') $root --expected-backend $backend
if ($LASTEXITCODE -ne 0) { throw 'OSF UI compiled binary contract verification failed' }
Write-Host 'OSF UI package ownership and compiled binary tests passed'
