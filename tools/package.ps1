<#
.SYNOPSIS
    Build OSF UI and produce a mod-manager-installable release archive.
.DESCRIPTION
    Builds, stages through xmake install, verifies, and zips the SFSE/Scripts layout.
.PARAMETER Version
    Archive version; defaults to kOsfuiReleaseVersion.
.PARAMETER Tag
    Optional archive-version suffix.
.PARAMETER Mode
    xmake build mode; defaults to releasedbg.
.PARAMETER SkipBuild
    Package the existing build without rebuilding.
.PARAMETER WebView2SdkDir
    Unpacked WebView2 SDK path.
.PARAMETER OutDir
    Archive output directory; defaults to dist.
.EXAMPLE
    pwsh tools/package.ps1
.EXAMPLE
    pwsh tools/package.ps1 -Version 1.0.0 -Tag beta
.EXAMPLE
    pwsh tools/package.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Tag = 'alpha',
    [switch]$NoPdb,
    [string]$Mode = 'releasedbg',
    [switch]$SkipBuild,
    [string]$WebView2SdkDir,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }
$Staging = Join-Path $RepoRoot 'build\package\staging'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "XX  $m" -ForegroundColor Red; exit 1 }

if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) {
    Die "xmake not found on PATH. Install xmake 3.0+ and retry."
}

if (-not $Version) {
    $vh = Join-Path $RepoRoot 'src\core\Version.h'
    $m = Select-String -Path $vh -Pattern 'kOsfuiReleaseVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $m) { Die "Could not parse kOsfuiReleaseVersion from $vh; pass -Version explicitly." }
    $Version = $m.Matches[0].Groups[1].Value
}
$verLabel = "v$Version"
if ($Tag) { $verLabel += "-$Tag" }

Step "Packaging OSF UI $verLabel  (mode=$Mode, renderer=webview2)"

if (-not $WebView2SdkDir) {
    $WebView2SdkDir = $env:WEBVIEW2_SDK_DIR
}
if (-not $WebView2SdkDir) {
    $WebView2SdkDir = Join-Path $RepoRoot 'external\webview2'
}
$webView2Native = Join-Path $WebView2SdkDir 'build\native'
if (-not (Test-Path (Join-Path $webView2Native 'include\WebView2.h')) -or
    -not (Test-Path (Join-Path $webView2Native 'x64\WebView2LoaderStatic.lib'))) {
    Die "WebView2 SDK not found at '$WebView2SdkDir'. Unpack Microsoft.Web.WebView2 there or set -WebView2SdkDir."
}
$env:WEBVIEW2_SDK_DIR = $WebView2SdkDir
Write-Host "    WebView2 SDK: $WebView2SdkDir"

# Prevent commonlibsf auto-deploy from overriding the explicit staging directory.
$env:XSE_SF_MODS_PATH = $null
$env:XSE_SF_GAME_PATH  = $null

Push-Location $RepoRoot
try {
    # xmake generates built-in views; install their locked dependencies first.
    Step "Installing dependencies (locked)"
    if (-not (Get-Command npm -ErrorAction SilentlyContinue)) { Die "Packaging requires npm on PATH." }
    npm ci
    if ($LASTEXITCODE -ne 0) { Die "npm ci failed." }

    if (-not $SkipBuild) {
        Step "xmake f -m $Mode"
        xmake f -m $Mode -y
        if ($LASTEXITCODE -ne 0) { Die "xmake config failed." }

        Step "xmake build"
        xmake build -y
        if ($LASTEXITCODE -ne 0) { Die "Build failed." }
    } else {
        Warn "SkipBuild: packaging the existing build."
    }

    if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }
    New-Item -ItemType Directory -Path $Staging -Force | Out-Null

    Step "xmake install -o $Staging"
    xmake install -o $Staging 'OSF UI'
    if ($LASTEXITCODE -ne 0) { Die "xmake install failed." }

    # Mirror authored data explicitly because xmake caches its glob; preserve bin and views.
    $stagedData = Join-Path $Staging 'SFSE\Plugins\OSFUI'
    $srcData    = Join-Path $RepoRoot 'data\OSFUI'
    if (-not (Test-Path $srcData)) { Die "Source data folder not found: $srcData" }
    Step "Syncing authored data from data/OSFUI (bypasses install-glob cache)"
    Get-ChildItem $stagedData -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notin @('bin', 'views') } |
        Remove-Item -Recurse -Force
    Copy-Item (Join-Path $srcData '*') $stagedData -Recurse -Force

    # Mirror data/Scripts explicitly to avoid the same cached-glob issue.
    $stagedScripts = Join-Path $Staging 'Scripts'
    $srcScripts    = Join-Path $RepoRoot 'data\Scripts'
    if (-not (Test-Path $srcScripts)) { Die "Source scripts folder not found: $srcScripts" }
    if (Test-Path $stagedScripts) { Remove-Item $stagedScripts -Recurse -Force }
    Copy-Item $srcScripts $stagedScripts -Recurse -Force

    if ($NoPdb) {
        Step "Stripping PDB (-NoPdb)"
        Get-ChildItem (Join-Path $Staging 'SFSE\Plugins') -Filter '*.pdb' -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }

    # Keep distribution terms and attribution inside the plugin data folder.
    $docDest = Join-Path $Staging 'SFSE\Plugins\OSFUI'
    Step "Adding license docs (LICENSE, EXCEPTIONS, CREDITS.md -> SFSE\Plugins\OSFUI\)"
    foreach ($doc in 'LICENSE', 'EXCEPTIONS', 'CREDITS.md') {
        $src = Join-Path $RepoRoot $doc
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $docDest $doc) -Force
        } else {
            Warn "doc '$doc' not found -- omitted from the archive."
        }
    }

    Step "Verifying staged payload"
    $required = @(
        'SFSE\Plugins\OSFUI.dll',
        'SFSE\Plugins\OSFUI\bin\osfui_webview2_host.exe',
        'SFSE\Plugins\OSFUI\settings\osfui.json',    # OSF UI's own Mod Settings schema
        'SFSE\Plugins\OSFUI\views\osfui\settings\manifest.json', # hard-coded default menu
        'SFSE\Plugins\OSFUI\LICENSE',                # GPL-3.0 text (required to distribute)
        'SFSE\Plugins\OSFUI\EXCEPTIONS',             # GPL 7 modding/linking exception
        'SFSE\Plugins\OSFUI\CREDITS.md',             # attribution
        # Third-party views depend on the shared asset paths exactly.
        'SFSE\Plugins\OSFUI\views\shared\osfui.js',
        'SFSE\Plugins\OSFUI\views\shared\osfui.css',
        'SFSE\Plugins\OSFUI\views\shared\gamepadnav.js',
        'Scripts\OSFUI.pex',          # Runtime/version API
        'Scripts\OSFUI_Settings.pex', # Settings/hotkey API
        'Scripts\OSFUI_View.pex'      # View bridge/presentation API
    )
    $missing = $required | Where-Object { -not (Test-Path (Join-Path $Staging $_)) }
    if ($missing) {
        Die ("Staged archive is missing required files:`n    " + ($missing -join "`n    "))
    }

    # Require at least one views/<modId>/<viewName>/manifest.json.
    $viewsRoot = Join-Path $Staging 'SFSE\Plugins\OSFUI\views'
    if (-not (Get-ChildItem $viewsRoot -Recurse -Filter 'manifest.json' -ErrorAction SilentlyContinue)) {
        Die "No view manifests found under SFSE\Plugins\OSFUI\views\<modId>\<viewName>\ -- nothing to render."
    }
    Get-ChildItem $viewsRoot -Recurse -Filter 'manifest.json' | ForEach-Object {
        $manifest = Get-Content $_.FullName -Raw | ConvertFrom-Json
        if ($manifest.PSObject.Properties.Name -contains 'debugOnly' -and $manifest.debugOnly) {
            Die "Debug-only view must not ship in a release: $($_.FullName)"
        }
    }

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $zipPath = Join-Path $OutDir "OSF-UI-$verLabel.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

    Step "Compressing -> $zipPath"
    Compress-Archive -Path (Join-Path $Staging '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force

    $zip = Get-Item $zipPath
    $sizeMB = [math]::Round($zip.Length / 1MB, 2)
    $sha = (Get-FileHash $zipPath -Algorithm SHA256).Hash
    $fileCount = (Get-ChildItem $Staging -Recurse -File).Count

    Write-Host ""
    Write-Host "OK  Release archive ready" -ForegroundColor Green
    Write-Host "    $zipPath"
    $pdbNote = if ($NoPdb) { 'no PDB' } else { 'PDB included (crash-log symbols; -NoPdb to omit)' }
    Write-Host "    $sizeMB MB, $fileCount files, $pdbNote"
    Write-Host "    SHA256 $sha"
    Write-Host ""
    Write-Host "    Archive root (drop-in for MO2 / Vortex; also unzips into the game folder):"
    Get-ChildItem $Staging | ForEach-Object {
        $suffix = if ($_.PSIsContainer) { '\' } else { '' }
        Write-Host "      $($_.Name)$suffix"
    }
    Write-Host ""
    Write-Host "    Install: add the .zip in your mod manager, or extract so that its"
    Write-Host "    'SFSE' folder lands in <Starfield>\Data\. Requires SFSE + Address Library."
}
finally {
    Pop-Location
}
