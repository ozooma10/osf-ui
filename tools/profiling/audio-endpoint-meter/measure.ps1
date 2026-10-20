[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1000,30000)][int]$DurationMs = 3000,
    [ValidateRange(0,5000)][int]$WarmupMs = 500,
    [ValidateRange(1,20000)][double]$ToneHz = 997
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$build = Join-Path $repo 'build/audio-endpoint-meter'
$binary = Join-Path $build 'Release/osfui_audio_endpoint_meter.exe'
$inputsPath = Join-Path $build 'build-inputs.json'
if (-not (Test-Path -LiteralPath $binary) -or -not (Test-Path -LiteralPath $inputsPath)) { throw 'Build the endpoint meter first.' }
$inputs = Get-Content -Raw -LiteralPath $inputsPath | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash -ne $inputs.binarySha256) { throw 'Meter binary differs from build provenance.' }
foreach ($source in $inputs.sources) {
    if ((Get-FileHash -LiteralPath $source.path -Algorithm SHA256).Hash -ne $source.sha256) { throw "Meter source changed; rebuild: $($source.path)" }
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath (Join-Path $output 'result.json')) { throw 'Choose a phase directory without an existing result.json.' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$arguments = @('--output',('"{0}"' -f $output),'--duration-ms',$DurationMs,'--warmup-ms',$WarmupMs,
    '--tone-hz',$ToneHz.ToString([cultureinfo]::InvariantCulture))
$process = Start-Process -FilePath $binary -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $output 'meter.stdout.txt') -RedirectStandardError (Join-Path $output 'meter.stderr.txt')
$deadline = [DateTime]::UtcNow.AddSeconds(($DurationMs + $WarmupMs) / 1000 + 15)
while (-not $process.WaitForExit(500) -and [DateTime]::UtcNow -lt $deadline) {}
if (-not $process.HasExited) { throw "Owned meter PID $($process.Id) timed out. Evidence: $output" }
$process.Refresh()
$inputs | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'meter-provenance.json') -Encoding utf8
if ($process.ExitCode -ne 0) {
    $failure = Get-Content -Raw -LiteralPath (Join-Path $output 'meter.stderr.txt')
    throw "Endpoint meter failed (exit $($process.ExitCode)): $failure"
}
$result = Join-Path $output 'result.json'
if (-not (Test-Path -LiteralPath $result)) { throw 'Meter returned without a report.' }
Get-Content -Raw -LiteralPath $result
