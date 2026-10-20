<# Build the Papyrus-only TV into a fresh staging folder; optionally deploy its owned files. #>
[CmdletBinding()]
param(
    [string]$Game = 'C:/Program Files (x86)/Steam/steamapps/common/Starfield',
    [string]$Compiler = $env:PAPYRUS_COMPILER,
    [string]$Imports = $env:PAPYRUS_IMPORTS,
    [string]$ModsPath = $env:XSE_SF_MODS_PATH,
    [switch]$Deploy
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$addon = Join-Path $repo 'extensions/browser-tv'
if (Get-Process Starfield -ErrorAction SilentlyContinue) { throw 'Close Starfield before building Browser TV.' }
if (-not $Compiler) { $Compiler = Join-Path $Game 'Tools/Papyrus Compiler/PapyrusCompiler.exe' }
if (-not $Imports) { $Imports = 'C:/Modding/Starfield/PapyrusSource' }
if ($Deploy -and -not $ModsPath) { throw 'Set XSE_SF_MODS_PATH or pass -ModsPath for deployment.' }
$flags = Join-Path $Imports 'Starfield_Papyrus_Flags.flg'
foreach ($inputFile in @($Compiler, $flags, (Join-Path $Game 'Data/Starfield.esm'))) {
    if (-not (Test-Path -LiteralPath $inputFile -PathType Leaf)) { throw "Missing build input: $inputFile" }
}
# A fresh directory prevents stale PEX files or unrelated payloads from being deployed.
$stage = Join-Path $repo ('build/browser-tv/' + [Guid]::NewGuid().ToString('N'))
$data = Join-Path $stage 'Data'
$source = Join-Path $addon 'data/Scripts/Source'
$compiled = Join-Path $data 'Scripts'
New-Item -ItemType Directory -Path (Join-Path $compiled 'Source') -Force | Out-Null
Push-Location $source
try {
    & $Compiler 'OSFBrowserTV.psc' "-i=$source;$repo/extensions/world-screens/data/Scripts/Source;$Imports" "-o=$compiled" "-f=$flags"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $compiled 'OSFBrowserTV.pex'))) {
        throw 'Browser TV Papyrus compilation failed.'
    }
} finally { Pop-Location }
& python (Join-Path $addon 'tools/build_plugin.py') --game-data (Join-Path $Game 'Data') --out $data
if ($LASTEXITCODE) { throw 'Browser TV content build failed.' }
Copy-Item -LiteralPath (Join-Path $source 'OSFBrowserTV.psc') -Destination (Join-Path $compiled 'Source')
$notices = Join-Path $data 'OSF Browser TV'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $addon 'README.md') -Destination $notices
foreach ($name in @('LICENSE', 'EXCEPTIONS')) {
    Copy-Item -LiteralPath (Join-Path $repo $name) -Destination $notices
}
& python (Join-Path $addon 'tests/test_plugin.py') --data $data --game-data (Join-Path $Game 'Data')
if ($LASTEXITCODE) { throw 'Browser TV content validation failed.' }
$files = @(Get-ChildItem -LiteralPath $data -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = [IO.Path]::GetRelativePath($data, $_.FullName).Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
})
$destination = $null
if ($Deploy) {
    if (Get-Process Starfield -ErrorAction SilentlyContinue) { throw 'Starfield started during the build; deployment stopped.' }
    $destination = Join-Path $ModsPath 'OSF Browser TV'
    foreach ($file in $files) {
        $target = Join-Path $destination $file.path
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $data $file.path) -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target).Hash -ne $file.sha256) { throw "Deployed hash mismatch: $($file.path)" }
    }
}
$evidence = [ordered]@{ data = $data; deployedTo = $destination; runtimeValidated = $false; files = $files }
$evidence | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'payload.json') -Encoding utf8NoBOM
$evidence | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $repo 'build/browser-tv/latest-build.json') -Encoding utf8NoBOM
Write-Host "Built Browser TV: $data ($($files.Count) files)."
if ($Deploy) { Write-Host "Deployed with matching SHA-256 hashes: $destination" }
