param(
    [int]$Port = 8766
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

Push-Location $root
try {
    Write-Host "Serving Nexus BBCode preview at http://127.0.0.1:$Port/bbcode-preview.html"
    Write-Host "Press Ctrl+C to stop."
    python -m http.server $Port
}
finally {
    Pop-Location
}
