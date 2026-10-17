<# Build two OSF UI editions around one shared DLL and public API. Never deploys or publishes. #>
[CmdletBinding()]
param(
    [ValidateSet('both', 'webview2', 'cef')][string]$Backend = 'both',
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
$Staging = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'build/package/staging'))
if (-not $Staging.StartsWith($RepoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to stage outside the repository'
}
if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) { throw 'xmake was not found on PATH' }
if (-not $Version) {
    $match = Select-String -LiteralPath (Join-Path $RepoRoot 'src/Core/Version.h') -Pattern 'kOsfuiReleaseVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $match) { throw 'Could not determine OSF UI version' }
    $Version = $match.Matches[0].Groups[1].Value
}
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
        # Build and install the common payload exactly once, including Papyrus compilation.
        xmake f -P $RepoRoot -m $Mode "--browser_backend=$($editions[0])" -y
        if ($LASTEXITCODE) { throw 'XMake configuration failed' }
        if (-not $SkipBuild) {
            xmake build -P $RepoRoot -j4 -y 'OSF UI'
            if ($LASTEXITCODE) { throw 'OSF UI build failed' }
            if ($Backend -eq 'both') {
                xmake build -P $RepoRoot -j4 -y osfui-cef-host
                if ($LASTEXITCODE) { throw 'CEF host build failed' }
            }
        }
        if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
        $baseData = Join-Path $Staging 'common/Data'
        New-Item -ItemType Directory -Path $baseData -Force | Out-Null
        xmake install -P $RepoRoot -o $baseData 'OSF UI'
        if ($LASTEXITCODE) { throw 'Common payload installation failed' }
        $baseUi = Join-Path $baseData 'SFSE/Plugins/OSF/UI'
        foreach ($doc in 'LICENSE', 'EXCEPTIONS') {
            Copy-Item -LiteralPath (Join-Path $RepoRoot $doc) -Destination (Join-Path $baseUi $doc)
        }
        if ($NoPdb) {
            Get-ChildItem -LiteralPath (Join-Path $baseData 'SFSE/Plugins') -Filter '*.pdb' -File | Remove-Item -Force
        }
        $commonHashes = $null
        $archiveRecords = @()
        foreach ($edition in $editions) {
            $editionRoot = Join-Path $Staging $edition
            $data = Join-Path $editionRoot 'Data'
            New-Item -ItemType Directory -Path $editionRoot -Force | Out-Null
            Copy-Item -LiteralPath $baseData -Destination $data -Recurse
            $ui = Join-Path $data 'SFSE/Plugins/OSF/UI'
            $bin = [IO.Path]::GetFullPath((Join-Path $ui 'bin'))
            if (-not $bin.StartsWith($Staging + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Refusing to replace a browser payload outside package staging'
            }
            Remove-Item -LiteralPath $bin -Recurse -Force
            New-Item -ItemType Directory -Path $bin | Out-Null
            Copy-Item -LiteralPath (Join-Path $baseUi 'bin/broker') -Destination (Join-Path $bin 'broker') -Recurse
            if ($edition -eq 'cef') {
                Copy-Item -LiteralPath (Join-Path $RepoRoot 'build/cef-host/runtime') -Destination (Join-Path $bin 'cef') -Recurse
            } else {
                Copy-Item -LiteralPath (Join-Path $baseUi 'bin/osfui_webview2_host.exe') -Destination $bin
            }
            Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging/backends/$edition.json") -Destination (Join-Path $ui 'backend.json') -Force
            & (Join-Path $RepoRoot 'tests/package/test-package.ps1') -DataRoot $data -ExpectedBackend $edition
            $files = FileManifest $data
            $shared = @($files | Where-Object { $_.path -ne 'SFSE/Plugins/OSF/UI/backend.json' -and (-not $_.path.StartsWith('SFSE/Plugins/OSF/UI/bin/') -or $_.path.StartsWith('SFSE/Plugins/OSF/UI/bin/broker/')) })
            $sharedJson = ConvertTo-Json -InputObject $shared -Depth 4 -Compress
            if ($null -eq $commonHashes) { $commonHashes = $sharedJson }
            elseif ($commonHashes -ne $sharedJson) { throw 'The editions do not contain identical common files' }
            New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
            $archive = Join-Path $OutDir "OSF-UI-$versionLabel-$edition.zip"
            Compress-Archive -LiteralPath $data -DestinationPath $archive -CompressionLevel Optimal -Force
            VerifyArchive $archive $files
            $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
            "$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath ($archive + '.sha256') -Encoding utf8NoBOM
            [ordered]@{
                backend = $edition; version = $Version; sourceCommit = (& git rev-parse HEAD)
                sourceDirty = [bool](& git status --porcelain --untracked-files=normal --ignore-submodules)
                archiveSha256 = $hash; files = $files
            } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($archive + '.manifest.json') -Encoding utf8NoBOM
            $archiveRecords += [pscustomobject]@{ backend = $edition; archive = $archive; sha256 = $hash }
            Write-Host "Created $archive"
            Write-Host "SHA256 $hash"
        }
        if ($editions.Count -eq 2) { Write-Host 'Both editions contain byte-identical common files, including OSFUI.dll and OSFUI.pex.' }
        Write-Host 'Install one edition. Both require OSF Settings, SFSE, and Address Library. Only the WebView2 edition requires Evergreen.'
        $archiveRecords
    } finally { Pop-Location }
} finally {
    $env:WEBVIEW2_SDK_DIR = $previousSdk
    $env:XSE_SF_MODS_PATH = $previousMods
    $env:XSE_SF_GAME_PATH = $previousGame
}
