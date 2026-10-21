<# Build and stage the optional extension locally. Never deploys or publishes. #>
[CmdletBinding()]
param([string]$OutDir, [switch]$IncludePdb)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
# Unique staging preserves prior packages and cannot inherit stale core files.
$stage = Join-Path $repo ('build/package/world-screens-' + [Guid]::NewGuid().ToString('N'))
$data = Join-Path $stage 'Data'
$mods = $env:XSE_SF_MODS_PATH
$game = $env:XSE_SF_GAME_PATH
try {
    $env:XSE_SF_MODS_PATH = $null
    $env:XSE_SF_GAME_PATH = $null
    Push-Location $repo
    try {
        xmake build -P $repo -F (Join-Path $repo 'xmake.lua') -y -j4 'OSF UI - World Screens'
        if ($LASTEXITCODE) { throw 'World Screens build failed' }
        New-Item -ItemType Directory -Path $data -Force | Out-Null
        xmake install -P $repo -F (Join-Path $repo 'xmake.lua') -o $data 'OSF UI - World Screens'
        if ($LASTEXITCODE) { throw 'World Screens staging failed' }
        if (-not $IncludePdb) {
            $pdb = Join-Path $data 'SFSE/Plugins/OSFUIWorldScreens.pdb'
            if (Test-Path -LiteralPath $pdb) { Remove-Item -LiteralPath $pdb }
        }
        & (Join-Path $repo 'tests/package/test-world-screens-package.ps1') -DataRoot $data
        $files = @(Get-ChildItem -LiteralPath $data -Recurse -File | Sort-Object FullName | ForEach-Object {
            [ordered]@{path=[IO.Path]::GetRelativePath($data,$_.FullName).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
        })
        New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
        $archive = Join-Path $OutDir ('OSF-UI-World-Screens-v0.1.0-dev-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip')
        Compress-Archive -LiteralPath $data -DestinationPath $archive -CompressionLevel Optimal
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            $entries = @($zip.Entries | Where-Object Name)
            if ($entries.Count -ne $files.Count) { throw 'Archive file count differs from staging' }
            foreach ($file in $files) {
                $entry = @($entries | Where-Object { $_.FullName.Replace('\','/') -ceq ('Data/' + $file.path) })
                if ($entry.Count -ne 1) { throw "Missing or duplicated archive file: $($file.path)" }
                $stream = $entry[0].Open()
                try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
                finally { $stream.Dispose() }
                if ($hash -ne $file.sha256) { throw "Archive hash mismatch: $($file.path)" }
            }
        } finally { $zip.Dispose() }
        $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        [ordered]@{version='0.1.0-dev'; coreRequirement='matching source revision; surfaces ABI 1.3'; sourceCommit=(& git rev-parse HEAD); sourceDirty=[bool](& git status --porcelain --ignore-submodules=all); archiveSha256=$hash; files=$files} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($archive + '.manifest.json') -Encoding utf8NoBOM
        "$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath ($archive + '.sha256') -Encoding utf8NoBOM
        Write-Host "Verified local archive: $archive"
        Write-Host "Staging: $data"
    } finally { Pop-Location }
} finally {
    $env:XSE_SF_MODS_PATH = $mods
    $env:XSE_SF_GAME_PATH = $game
}
