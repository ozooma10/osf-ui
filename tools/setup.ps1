<#
.SYNOPSIS
    First-time setup for building OSF UI on a fresh machine.

.DESCRIPTION
    Fetches the build-time dependencies that are NOT checked into the repo, so a
    fresh clone can build. Currently that is one thing: the Microsoft.Web.WebView2
    SDK package, which the WebView2 renderer backend links its static loader from.
    `external/` is gitignored (a local SDK drop, not source), so every new clone
    starts without it and `xmake build` fails with:

        OSFUI WebView2 host: unpack Microsoft.Web.WebView2 into external/webview2
        or set WEBVIEW2_SDK_DIR to the NuGet package root

    This script downloads that NuGet package (the SAME version CI pins) and
    unpacks it to external/webview2, then verifies the header and static loader
    are present. It is idempotent: re-running with the SDK already in place is a
    no-op unless -Force is passed.

    What this does NOT do (intentionally): install xmake, the Edge WebView2
    Evergreen runtime, or Node. Those are documented in the README; this script
    only stages the repo-local SDK drop.

.PARAMETER Version
    Microsoft.Web.WebView2 NuGet version to fetch. Defaults to the version CI
    pins; override only to match a different toolchain.

.PARAMETER Dest
    Where to unpack the SDK. Defaults to external/webview2 (what the build looks
    for by default). If you keep the SDK in a shared location instead, unpack it
    there and set WEBVIEW2_SDK_DIR rather than using this script.

.PARAMETER Force
    Re-download and overwrite even if the SDK already appears present.

.EXAMPLE
    pwsh tools/setup.ps1
    # fetch + unpack the WebView2 SDK into external/webview2

.EXAMPLE
    pwsh tools/setup.ps1 -Force
    # re-fetch even if already present
#>
[CmdletBinding()]
param(
    [string]$Version = '1.0.4078.44',
    [string]$Dest,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Dest) { $Dest = Join-Path $RepoRoot 'external\webview2' }

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "XX  $m" -ForegroundColor Red; exit 1 }

Step "OSF UI setup: staging build-time dependencies"

# --- WebView2 SDK ----------------------------------------------------------
$native    = Join-Path $Dest 'build\native'
$header    = Join-Path $native 'include\WebView2.h'
$staticLib = Join-Path $native 'x64\WebView2LoaderStatic.lib'

if (-not $Force -and (Test-Path $header) -and (Test-Path $staticLib)) {
    Step "WebView2 SDK already present at $Dest (use -Force to re-fetch)"
} else {
    Step "Fetching Microsoft.Web.WebView2 $Version"
    $archive = Join-Path ([System.IO.Path]::GetTempPath()) "webview2-$Version.zip"
    $uri = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"
    try {
        Invoke-WebRequest -Uri $uri -OutFile $archive
    } catch {
        Die "Download failed from $uri`n    $($_.Exception.Message)"
    }
    Step "Unpacking to $Dest"
    Expand-Archive -Path $archive -DestinationPath $Dest -Force
    Remove-Item $archive -ErrorAction SilentlyContinue

    if (-not (Test-Path $header) -or -not (Test-Path $staticLib)) {
        Die "SDK unpacked but expected files are missing under $native. Check the package version."
    }
}

Write-Host ""
Step "Setup complete. Build with:  xmake build"
Warn "Runtime deps not handled here: xmake 3.0+, the Edge WebView2 Evergreen runtime, and (for the frontend) Node."
