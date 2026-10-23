<# Build WebView2 and CEF editions with a compiled backend choice and shared public API. Never deploys or publishes. #>
[CmdletBinding()]
param(
    [ValidateSet('both', 'webview2', 'cef')][string]$Backend = 'both',
    [string]$Version,
    [string]$Tag = 'rc',
    [switch]$NoPdb,
    [switch]$IncludePdb,
    [string]$Mode = 'releasedbg',
    [switch]$SkipBuild,
    [switch]$RequireClean,
    [string]$WebView2SdkDir,
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($NoPdb -and $IncludePdb) { throw 'Choose either -IncludePdb or -NoPdb, not both.' }
$RepoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$sourceCommit = & git -C $RepoRoot rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot determine package source revision' }
function Assert-SourceUnchanged {
    $current = & git -C $RepoRoot rev-parse HEAD
    if ($LASTEXITCODE -or $current -cne $sourceCommit) {
        throw 'Source revision changed during packaging; discard this attempt and rebuild both editions.'
    }
    if ($RequireClean) {
        $changes = @(& git -C $RepoRoot status --porcelain=v1 --untracked-files=all --ignore-submodules=none)
        if ($LASTEXITCODE -or $changes.Count) { throw 'Release packaging requires a clean source tree, including submodules.' }
    }
}
if ($RequireClean -and $SkipBuild) { throw '-RequireClean cannot use -SkipBuild; release binaries must be built from the recorded source.' }
Assert-SourceUnchanged
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$Staging = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build/package/staging'))
if (-not $Staging.StartsWith($RepoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to stage outside the repository'
}
if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) { throw 'xmake was not found on PATH' }
$match = Select-String -LiteralPath (Join-Path $RepoRoot 'src/Core/Version.h') -Pattern 'kOsfuiReleaseVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
if (-not $match) { throw 'Could not determine OSF UI version' }
$sourceVersion = $match.Matches[0].Groups[1].Value
if ($Version -and $Version -cne $sourceVersion) { throw "Package version $Version differs from source version $sourceVersion. Update and rebuild the source first." }
$Version = $sourceVersion
$versionLabel = "v$Version"
if ($Tag) { $versionLabel += "-$Tag" }
$editions = @(if ($Backend -eq 'both') { 'webview2'; 'cef' } else { $Backend })
$previousSdk = $env:WEBVIEW2_SDK_DIR
$previousMods = $env:XSE_SF_MODS_PATH
$previousGame = $env:XSE_SF_GAME_PATH
function FileManifest([string]$Root) {
    @(Get-ChildItem -LiteralPath $Root -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
function VerifyArchive([string]$Archive, $Files) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $entries = @{}
        foreach ($entry in $zip.Entries) {
            if (-not $entry.Name) { continue }
            $name = $entry.FullName.Replace('\', '/')
            if ($entries.ContainsKey($name)) { throw "Duplicate ZIP entry: $name" }
            $entries[$name] = $entry
        }
        if ($entries.Count -ne $Files.Count) { throw 'Archive contents differ from the verified staging tree' }
        foreach ($file in $Files) {
            $name = 'Data/' + $file.path
            if (-not $entries.ContainsKey($name)) { throw "Missing ZIP entry: $name" }
            $stream = $entries[$name].Open()
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
            finally { $stream.Dispose() }
            if ($hash -ne $file.sha256) { throw "ZIP content hash mismatch: $name" }
        }
    } finally { $zip.Dispose() }
}
try {
    if ($WebView2SdkDir) { $env:WEBVIEW2_SDK_DIR = $WebView2SdkDir }
    $env:XSE_SF_MODS_PATH = $null
    $env:XSE_SF_GAME_PATH = $null
    Push-Location $RepoRoot
    try {
        if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
        $commonHashes = $null
        $commonPex = $null
        $archiveRecords = @()
        foreach ($edition in $editions) {
            Assert-SourceUnchanged
            # Keep both backend object/dependency caches across packaging runs.
            $nativeOutput = Join-Path $RepoRoot "build/package/native/$edition"
            xmake f -P $RepoRoot -F (Join-Path $RepoRoot 'xmake.lua') -o $nativeOutput -m $Mode "--browser_backend=$edition" -y
            if ($LASTEXITCODE) { throw 'XMake configuration failed' }
            if (-not $SkipBuild) {
                xmake build -P $RepoRoot -F (Join-Path $RepoRoot 'xmake.lua') -j4 -y 'OSF UI'
                if ($LASTEXITCODE) { throw "OSF UI $edition build failed" }
            }
            $editionRoot = Join-Path $Staging $edition
            $data = Join-Path $editionRoot 'Data'
            New-Item -ItemType Directory -Path $data -Force | Out-Null
            xmake install -P $RepoRoot -F (Join-Path $RepoRoot 'xmake.lua') -o $data 'OSF UI'
            if ($LASTEXITCODE) { throw "OSF UI $edition installation failed" }
            $ui = Join-Path $data 'SFSE/Plugins/OSF/UI'
            foreach ($doc in 'LICENSE', 'EXCEPTIONS') {
                Copy-Item -LiteralPath (Join-Path $RepoRoot $doc) -Destination (Join-Path $ui $doc)
            }
            # Release archives omit symbols and their account-specific source/temp paths.
            # Keep -NoPdb accepted for existing release commands.
            if (-not $IncludePdb) {
                Get-ChildItem -LiteralPath (Join-Path $data 'SFSE/Plugins') -Filter '*.pdb' -File | Remove-Item -Force
            }
            # PEX embeds compilation timestamps; share the first edition's script bytes.
            $pex = Join-Path $data 'Scripts/OSFUI.pex'
            if ($commonPex) {
                Copy-Item -LiteralPath $commonPex -Destination $pex -Force
            } else {
                $commonPex = $pex
            }
            & (Join-Path $RepoRoot 'tests/package/test-package.ps1') -DataRoot $data -ExpectedBackend $edition
            Assert-SourceUnchanged
            $files = FileManifest $data
            $shared = @($files | Where-Object {
                $_.path -notin @('SFSE/Plugins/OSFUI.dll', 'SFSE/Plugins/OSFUI.pdb') -and
                (-not $_.path.StartsWith('SFSE/Plugins/OSF/UI/bin/') -or $_.path.StartsWith('SFSE/Plugins/OSF/UI/bin/broker/'))
            })
            $sharedJson = ConvertTo-Json -InputObject $shared -Depth 4 -Compress
            if ($null -eq $commonHashes) { $commonHashes = $sharedJson }
            elseif ($commonHashes -ne $sharedJson) { throw 'The editions do not contain identical common files' }
            New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
            $archive = Join-Path $OutDir "OSF-UI-$versionLabel-$edition.zip"
            Compress-Archive -LiteralPath $data -DestinationPath $archive -CompressionLevel Optimal -Force
            VerifyArchive $archive $files
            Assert-SourceUnchanged
            $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
            "$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath ($archive + '.sha256') -Encoding utf8NoBOM
            [ordered]@{
                backend = $edition; version = $Version; sourceCommit = $sourceCommit
                sourceDirty = [bool](& git status --porcelain --untracked-files=normal --ignore-submodules)
                archiveSha256 = $hash; files = $files
            } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($archive + '.manifest.json') -Encoding utf8NoBOM
            $archiveRecords += [pscustomobject]@{ backend = $edition; archive = $archive; sha256 = $hash }
            Write-Host "Created $archive"
            Write-Host "SHA256 $hash"
        }
        if ($editions.Count -eq 2) { Write-Host 'Both editions contain byte-identical scripts and shared assets, with edition-specific DLLs and browser hosts.' }
        Write-Host 'WebView2: recommended default for most users; requires the WebView2 Evergreen Runtime.'
        Write-Host 'CEF: bundled-runtime alternative; includes Chromium and does not require WebView2.'
        Write-Host 'Install one edition. Both require OSF Settings, SFSE, and Address Library. Only the WebView2 edition requires Evergreen.'
        $archiveRecords
    } finally { Pop-Location }
} finally {
    $env:WEBVIEW2_SDK_DIR = $previousSdk
    $env:XSE_SF_MODS_PATH = $previousMods
    $env:XSE_SF_GAME_PATH = $previousGame
}
