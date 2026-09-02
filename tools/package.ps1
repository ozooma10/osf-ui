<# Build and package the OSF UI 2.x WebView add-on. #>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Tag = 'rc',
    [switch]$NoPdb,
    [string]$Mode = 'releasedbg',
    [switch]$SkipBuild,
    [string]$WebView2SdkDir,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$Staging = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build\package\staging'))
$StageData = Join-Path $Staging 'Data'

function Step($Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Fail($Message) { throw $Message }

if (-not $Staging.StartsWith($RepoRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    Fail 'Refusing to stage outside the repository'
}
if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) {
    Fail 'xmake was not found on PATH'
}

if (-not $Version) {
    $header = Join-Path $RepoRoot 'src\Core\Version.h'
    $match = Select-String -Path $header -Pattern 'kOsfuiReleaseVersion\s*=\s*"([^"]+)"' |
        Select-Object -First 1
    if (-not $match) { Fail 'Could not determine the OSF UI version' }
    $Version = $match.Matches[0].Groups[1].Value
}
$versionLabel = "v$Version"
if ($Tag) { $versionLabel += "-$Tag" }

if (-not $WebView2SdkDir) { $WebView2SdkDir = $env:WEBVIEW2_SDK_DIR }
if (-not $WebView2SdkDir) { $WebView2SdkDir = Join-Path $RepoRoot 'external\webview2' }
$webViewNative = Join-Path $WebView2SdkDir 'build\native'
if (-not (Test-Path -LiteralPath (Join-Path $webViewNative 'include\WebView2.h')) -or
    -not (Test-Path -LiteralPath (Join-Path $webViewNative 'x64\WebView2LoaderStatic.lib'))) {
    Fail "WebView2 SDK not found at '$WebView2SdkDir'"
}
$env:WEBVIEW2_SDK_DIR = $WebView2SdkDir

# Disable developer-machine auto-deploy while producing the isolated archive.
$env:XSE_SF_MODS_PATH = $null
$env:XSE_SF_GAME_PATH = $null

Push-Location $RepoRoot
try {
    if (-not $SkipBuild) {
        Step "Configuring $Mode"
        xmake f -P $RepoRoot -m $Mode -y
        if ($LASTEXITCODE -ne 0) { Fail 'xmake configuration failed' }
        Step 'Building native runtime and deterministic shared web assets'
        xmake build -P $RepoRoot -y 'OSF UI'
        if ($LASTEXITCODE -ne 0) { Fail 'build failed' }
    }

    if (Test-Path -LiteralPath $Staging) {
        Remove-Item -LiteralPath $Staging -Recurse -Force
    }
    New-Item -ItemType Directory -Path $StageData -Force | Out-Null

    Step 'Staging the Data tree'
    xmake install -P $RepoRoot -o $StageData 'OSF UI'
    if ($LASTEXITCODE -ne 0) { Fail 'xmake install failed' }

    $uiRoot = Join-Path $StageData 'SFSE\Plugins\OSF\UI'
    New-Item -ItemType Directory -Path $uiRoot -Force | Out-Null
    foreach ($doc in 'LICENSE', 'EXCEPTIONS', 'CREDITS.md') {
        $source = Join-Path $RepoRoot $doc
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $uiRoot $doc) -Force
        }
    }

    if ($NoPdb) {
        Get-ChildItem (Join-Path $StageData 'SFSE\Plugins') -Filter '*.pdb' -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }

    Step 'Verifying exact archive ownership'
    $required = @(
        'SFSE\Plugins\OSFUI.dll',
        'SFSE\Plugins\OSF\UI\bin\osfui_webview2_host.exe',
        'SFSE\Plugins\OSF\UI\views\shared\osfui.js',
        'SFSE\Plugins\OSF\UI\views\shared\osfui.css',
        'SFSE\Plugins\OSF\UI\views\shared\gamepadnav.js',
        'SFSE\Plugins\OSF\Settings\schemas\osfui.json',
        'Scripts\OSFUI.pex',
        'Scripts\OSFUI_View.pex'
    )
    $missing = $required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $StageData $_)) }
    if ($missing) { Fail ("Missing staged files:`n  " + ($missing -join "`n  ")) }

    $forbidden = @(
        'SFSE\Plugins\OSFUI',
        'SFSE\Plugins\OSFSettings.dll',
        'Scripts\OSFUI_Settings.pex'
    )
    foreach ($path in $forbidden) {
        if (Test-Path -LiteralPath (Join-Path $StageData $path)) {
            Fail "OSF UI archive contains a forbidden legacy/sibling path: Data\$path"
        }
    }
    $settingsRoot = Join-Path $StageData 'SFSE\Plugins\OSF\Settings'
    $ownedSchema = [IO.Path]::GetFullPath((Join-Path $settingsRoot 'schemas\osfui.json'))
    $unexpectedSettings = Get-ChildItem -LiteralPath $settingsRoot -Recurse -File |
        Where-Object { [IO.Path]::GetFullPath($_.FullName) -ne $ownedSchema }
    if ($unexpectedSettings) {
        Fail 'OSF UI archive owns files in the OSF Settings subtree other than schemas/osfui.json'
    }

    $viewsRoot = Join-Path $uiRoot 'views'
    Get-ChildItem $viewsRoot -Recurse -Filter 'manifest.json' -ErrorAction SilentlyContinue |
        ForEach-Object {
            $manifest = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
            if ($manifest.PSObject.Properties.Name -contains 'debugOnly' -and $manifest.debugOnly) {
                Fail "Debug-only view must not ship: $($_.FullName)"
            }
        }

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $archive = Join-Path $OutDir "OSF-UI-$versionLabel.zip"
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    Compress-Archive -Path (Join-Path $Staging '*') -DestinationPath $archive -CompressionLevel Optimal

    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    Write-Host "Created $archive" -ForegroundColor Green
    Write-Host "SHA256 $hash"
    Write-Host 'Requires OSF Settings >=1.0.0 <2.0.0, SFSE, and Address Library.'
}
finally {
    Pop-Location
}
