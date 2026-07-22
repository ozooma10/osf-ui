<#
.SYNOPSIS
    Build OSF UI and produce a mod-manager-installable release archive.

.DESCRIPTION
    Packaging is driven entirely by the xmake install step (the SAME rules that
    auto-deploy to MO2), so the archive layout can never drift from what the
    game actually loads. The flow is:

        1. (optionally) configure + build the release variant
        2. `xmake install -o <staging>`  -> staging/SFSE/Plugins/OSFUI(.dll|/...)
        3. copy the license files a distribution must carry into
           SFSE/Plugins/OSFUI/ (inside the plugin's own folder, so the game's
           Data root stays clean)
        4. verify the required files are present
        5. zip <staging> -> dist/OSF-UI-v<version>[-tag].zip

    The archive root contains `SFSE/` + `Scripts/`, which is exactly the
    structure MO2 / Vortex expect for a Starfield SFSE plugin: install it and it
    maps onto the game Data folder.

.PARAMETER Version
    Release version string for the archive name. Defaults to kPluginVersion
    parsed from src/core/Version.h.

.PARAMETER Tag
    Suffix appended after the version (e.g. "alpha" -> v1.0.0-alpha). "" omits it.
.PARAMETER Mode
    xmake build mode. Defaults to "releasedbg" (optimized + PDB for crash logs).

.PARAMETER SkipBuild
    Package the current build without reconfiguring/rebuilding. Use when you have
    just built the exact variant you want to ship.

.PARAMETER WebView2SdkDir
    Path to the unpacked Microsoft.Web.WebView2 NuGet package. Defaults to
    $env:WEBVIEW2_SDK_DIR, else external/webview2.

.PARAMETER OutDir
    Where the .zip is written. Defaults to <repo>/dist.

.EXAMPLE
    pwsh tools/package.ps1
    # release archive: dist/OSF-UI-v1.0.0-alpha.zip (WebView2, releasedbg)

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
    # Regenerate data/OSFUI/views from frontend/ and hard-fail if the committed
    # output was stale. Requires npm; off by default so packaging stays Node-free.
    [switch]$RebuildFrontend,
    [string]$WebView2SdkDir,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- paths -----------------------------------------------------------------
$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }
$Staging = Join-Path $RepoRoot 'build\package\staging'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "XX  $m" -ForegroundColor Red; exit 1 }

if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) {
    Die "xmake not found on PATH. Install xmake 3.0+ and retry."
}

# --- version ---------------------------------------------------------------
if (-not $Version) {
    $vh = Join-Path $RepoRoot 'src\core\Version.h'
    $m = Select-String -Path $vh -Pattern 'kPluginVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $m) { Die "Could not parse kPluginVersion from $vh; pass -Version explicitly." }
    $Version = $m.Matches[0].Groups[1].Value
}
$verLabel = "v$Version"
if ($Tag) { $verLabel += "-$Tag" }

Step "Packaging OSF UI $verLabel  (mode=$Mode, renderer=webview2)"

# --- WebView2 SDK sanity ---------------------------------------------------
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

# Neutralize auto-deploy: the commonlibsf.plugin rule sets installdir from these
# at config time, which would fight `xmake install -o`. Clear them for THIS
# process only (does not touch the user's shell).
$env:XSE_SF_MODS_PATH = $null
$env:XSE_SF_GAME_PATH  = $null

Push-Location $RepoRoot
try {
    # --- generated view output must be current -----------------------------
    # data/OSFUI/views is BUILD OUTPUT of frontend/ (Vite + TypeScript +
    # Preact) that is committed so packaging, xmake install and the MO2
    # redeploy can all consume it without Node. Shipping a stale bundle is
    # invisible until someone opens the overlay in game, so check it here.
    #
    # Deliberately advisory-by-default and Node-free: packaging must keep
    # working on a machine that has never run npm. Pass -RebuildFrontend to
    # regenerate and hard-fail on drift (what release builds should use).
    if ($RebuildFrontend) {
        Step "Rebuilding frontend (npm run build) and checking for drift"
        if (-not (Get-Command npm -ErrorAction SilentlyContinue)) { Die "-RebuildFrontend requires npm on PATH." }
        npm --prefix frontend ci
        if ($LASTEXITCODE -ne 0) { Die "npm ci failed." }
        npm --prefix frontend run build
        if ($LASTEXITCODE -ne 0) { Die "Frontend build failed." }
        npm --prefix frontend run check:dist
        if ($LASTEXITCODE -ne 0) { Die "Generated views under data/OSFUI/views are stale. Commit the rebuilt output." }
    } else {
        # Cheap Node-free approximation: if the generated tree is dirty in git,
        # someone edited output by hand or forgot to rebuild.
        $dirty = & git status --porcelain -- data/OSFUI/views 2>$null
        if ($LASTEXITCODE -eq 0 -and $dirty) {
            Warn ("data/OSFUI/views has uncommitted changes -- it is GENERATED from frontend/src." +
                  "`n    Run 'npm --prefix frontend run build' and commit, or re-run with -RebuildFrontend to verify.")
        }
    }

    # --- configure + build -------------------------------------------------
    if (-not $SkipBuild) {
        Step "xmake f -m $Mode --with_webview2=true"
        xmake f -m $Mode --with_webview2=true -y
        if ($LASTEXITCODE -ne 0) { Die "xmake config failed." }

        Step "xmake build"
        xmake build -y
        if ($LASTEXITCODE -ne 0) { Die "Build failed." }
    } else {
        Warn "SkipBuild: packaging whatever is already built (mode/variant not verified)."
    }

    # --- stage via xmake install ------------------------------------------
    if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }
    New-Item -ItemType Directory -Path $Staging -Force | Out-Null

    Step "xmake install -o $Staging"
    xmake install -o $Staging 'OSF UI'
    if ($LASTEXITCODE -ne 0) { Die "xmake install failed." }

    # --- deterministic data sync ------------------------------------------
    # xmake's add_installfiles("data/(OSFUI/**)") glob is CACHED and can go
    # stale: a view added/removed after the last clean reconfigure silently
    # won't match what's on disk (this is the deploy-race trap in docs). So we
    # do not trust install for the data folder -- mirror the authoritative
    # data/OSFUI/ over the staged tree while preserving the host executable
    # installed into bin/.
    $stagedData = Join-Path $Staging 'SFSE\Plugins\OSFUI'
    $srcData    = Join-Path $RepoRoot 'data\OSFUI'
    if (-not (Test-Path $srcData)) { Die "Source data folder not found: $srcData" }
    Step "Syncing data folder from data/OSFUI (authoritative; bypasses install-glob cache)"
    Get-ChildItem $stagedData -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne 'bin' } |
        Remove-Item -Recurse -Force
    Copy-Item (Join-Path $srcData '*') $stagedData -Recurse -Force

    # Same stale-glob trap applies to data/Scripts (the Papyrus surface):
    # mirror the authoritative folder over whatever install staged.
    $stagedScripts = Join-Path $Staging 'Scripts'
    $srcScripts    = Join-Path $RepoRoot 'data\Scripts'
    if (-not (Test-Path $srcScripts)) { Die "Source scripts folder not found: $srcScripts" }
    if (Test-Path $stagedScripts) { Remove-Item $stagedScripts -Recurse -Force }
    Copy-Item $srcScripts $stagedScripts -Recurse -Force

    # --- PDB (ships by default: crash loggers symbolicate with it) ---------
    if ($NoPdb) {
        Step "Stripping PDB (-NoPdb)"
        Get-ChildItem (Join-Path $Staging 'SFSE\Plugins') -Filter '*.pdb' -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }

    # --- license docs the distribution must carry -------------------------
    # LICENSE + EXCEPTIONS are load-bearing distribution terms.
    # CREDITS carries the attribution (incl. the "inspired by" credits).
    # They live INSIDE the plugin's own data folder -- the archive root maps
    # onto the game's Data\, and loose LICENSE/README files there would
    # clutter every install.
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

    # --- verify the payload ------------------------------------------------
    Step "Verifying staged payload"
    $required = @(
        'SFSE\Plugins\OSFUI.dll',
        'SFSE\Plugins\OSFUI\config.json',
        'SFSE\Plugins\OSFUI\bin\osfui_webview2_host.exe',
        'SFSE\Plugins\OSFUI\vanillakeys.json',       # vanilla-keybinds defaults table (runtime loads it at boot)
        'SFSE\Plugins\OSFUI\settings\osfui.json',    # OSF UI's own Mod Settings schema
        'SFSE\Plugins\OSFUI\LICENSE',                # GPL-3.0 text (required to distribute)
        'SFSE\Plugins\OSFUI\EXCEPTIONS',             # GPL 7 modding/linking exception
        'SFSE\Plugins\OSFUI\CREDITS.md',             # attribution
        # The shared asset kit is a FROZEN public contract: third-party views
        # link '../../shared/osfui.js' and '../../shared/osfui.css' by exact
        # path, so an archive missing them silently breaks every third-party
        # view while the built-ins keep working. padnav.js is private to the
        # osfui views but both of them <script src> it, so it is equally fatal.
        'SFSE\Plugins\OSFUI\views\shared\osfui.js',
        'SFSE\Plugins\OSFUI\views\shared\osfui.css',
        'SFSE\Plugins\OSFUI\views\osfui\padnav.js',
        'Scripts\OSFUI.pex'   # Papyrus surface (authoring-settings.md "From Papyrus")
    )
    $missing = $required | Where-Object { -not (Test-Path (Join-Path $Staging $_)) }
    if ($missing) {
        Die ("Staged archive is missing required files:`n    " + ($missing -join "`n    "))
    }
    # At least one view manifest must be present or the runtime has nothing to
    # host. Views live at views/<modId>/<viewName>/manifest.json (Ids.h grammar;
    # views/shared/ is the asset kit, not a view).
    $viewsRoot = Join-Path $Staging 'SFSE\Plugins\OSFUI\views'
    if (-not (Get-ChildItem $viewsRoot -Recurse -Filter 'manifest.json' -ErrorAction SilentlyContinue)) {
        Die "No view manifests found under SFSE\Plugins\OSFUI\views\<modId>\<viewName>\ -- nothing to render."
    }

    # --- content sanity checks ---------------------------------------------
    $cfgPath = Join-Path $Staging 'SFSE\Plugins\OSFUI\config.json'
    try {
        $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
        # HARD FAIL: every view the shipped config references must be in the
        # archive, or a fresh standalone install renders nothing on F10.
        # View ids are qualified '<modId>/<viewName>' and map onto
        # views/<modId>/<viewName>/manifest.json (src/runtime/Ids.h grammar).
        # External views (e.g. an 'ozooma10.almanac/planets') ship with their
        # own mod and must not be a standalone default.
        # Read the two keys defensively. Under `Set-StrictMode -Version Latest`
        # (line 71) touching a property that does not exist THROWS, and the
        # catch below downgrades that to a Warn -- so a config.json missing
        # `views` used to skip this entire hard-fail check and still produce a
        # zip. Probe the property names first so a missing key means "empty",
        # not "silently unvalidated".
        $names = $cfg.PSObject.Properties.Name
        $viewValue  = if ($names -contains 'view')  { $cfg.view }  else { $null }
        $viewsValue = if ($names -contains 'views') { $cfg.views } else { @() }
        $configuredViews = @(@($viewValue) + @($viewsValue) | Where-Object { $_ } | Select-Object -Unique)
        if ($configuredViews.Count -eq 0) {
            Die "config.json declares no views ('view' and 'views' are both absent/empty) -- a standalone release would render nothing on F10."
        }
        $missingViews = @($configuredViews | Where-Object {
            -not (Test-Path (Join-Path $viewsRoot ($_ -replace '/', '\') 'manifest.json'))
        })
        if ($missingViews.Count -gt 0) {
            $stagedViews = @(Get-ChildItem $viewsRoot -Directory | Where-Object Name -ne 'shared' | ForEach-Object {
                $mod = $_.Name
                Get-ChildItem $_.FullName -Directory |
                    Where-Object { Test-Path (Join-Path $_.FullName 'manifest.json') } |
                    ForEach-Object { "$mod/$($_.Name)" }
            })
            Die ("config.json references view(s) not shipped in this archive: " + ($missingViews -join ', ') + "`n    Shipped views: " + ($stagedViews -join ', ') + "`n    A standalone release must render out of the box -- default to 'osfui/settings'.")
        }
        if ($names -contains 'devMode' -and $cfg.devMode) { Warn "config.json has devMode=true (verbose logs). Turn OFF for release." }
    } catch {
        Warn "Could not parse staged config.json to sanity-check it: $($_.Exception.Message)"
    }

    # --- zip ---------------------------------------------------------------
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $zipPath = Join-Path $OutDir "OSF-UI-$verLabel.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

    Step "Compressing -> $zipPath"
    Compress-Archive -Path (Join-Path $Staging '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force

    # --- report ------------------------------------------------------------
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
