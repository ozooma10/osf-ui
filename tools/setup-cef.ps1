<# Downloads the pinned official x64 CEF binary distribution. No Chromium build. #>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cef_host\cef.lock.json') -Raw | ConvertFrom-Json
$sdk = Join-Path $repo 'external\cef'
$header = Join-Path $sdk 'include\cef_version.h'
if (Test-Path -LiteralPath $header) {
    if ((Get-Content -LiteralPath $header -Raw).Contains('"' + $pin.version + '"')) {
        Write-Host "CEF $($pin.version) already staged at $sdk"
        return
    }
    throw "A different CEF SDK exists at $sdk. Move it aside before running setup."
}
New-Item -ItemType Directory -Path (Join-Path $repo 'external') -Force | Out-Null
$archive = Join-Path $repo ('external\' + $pin.archive)
if (-not (Test-Path -LiteralPath $archive)) {
    $url = 'https://cef-builds.spotifycdn.com/' + [uri]::EscapeDataString($pin.archive)
    & curl.exe --fail --location --silent --show-error $url --output $archive
    if ($LASTEXITCODE -ne 0) { throw 'CEF download failed.' }
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $pin.sha256) {
    throw "CEF checksum mismatch: $archive"
}
New-Item -ItemType Directory -Path $sdk -Force | Out-Null
& tar.exe -xjf $archive -C $sdk --strip-components=1
if ($LASTEXITCODE -ne 0) { throw 'CEF extraction failed.' }
Write-Host "Verified and staged CEF $($pin.version) at $sdk"
