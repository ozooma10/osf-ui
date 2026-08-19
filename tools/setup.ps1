<#
.SYNOPSIS
    First-time setup for building OSF UI on a fresh machine.
.DESCRIPTION
    Downloads and verifies the pinned WebView2 SDK; reruns are no-ops unless -Force is passed.
.PARAMETER Version
    Microsoft.Web.WebView2 NuGet version to fetch.
.PARAMETER Dest
    SDK destination; defaults to external/webview2.
.PARAMETER Force
    Re-download and overwrite an existing SDK.
.EXAMPLE
    pwsh tools/setup.ps1
.EXAMPLE
    pwsh tools/setup.ps1 -Force
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
