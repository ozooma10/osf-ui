[CmdletBinding()]
param([switch]$Allowlist, [switch]$RemoteVideo, [switch]$NativeSourceMute, [switch]$ProductionRoute)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$exe = Join-Path $repo 'build/webview2-tab-audio-probe/Release/osfui_webview2_tab_audio_probe.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Run build.ps1 first.' }
$buildInputsPath = Join-Path $repo 'build/webview2-tab-audio-probe/build-inputs.json'
if (-not (Test-Path -LiteralPath $buildInputsPath)) { throw 'Run build.ps1 to record compiled input hashes first.' }
$buildInputs = Get-Content -Raw -LiteralPath $buildInputsPath | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $buildInputs.binarySha256) {
    throw 'Probe binary differs from build provenance. Rebuild first.'
}
foreach ($inputFile in $buildInputs.sources) {
    if ((Get-FileHash -LiteralPath $inputFile.path -Algorithm SHA256).Hash -ne $inputFile.sha256) {
        throw "Compiled source changed after build: $($inputFile.path). Rebuild first."
    }
}
$label = if ($Allowlist) { 'allowlist' } else { 'normal' }
if ($RemoteVideo) { $label += '-video' }
if ($NativeSourceMute) { $label += '-native-mute' }
if ($ProductionRoute) { $label += '-production' }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$runs = Join-Path $repo 'build/webview2-tab-audio-probe/runs'
New-Item -ItemType Directory -Path $runs -Force | Out-Null
$output = Join-Path $runs "$stamp-$label"
$arguments = @('--root', ('"{0}"' -f $PSScriptRoot), '--output', ('"{0}"' -f $output))
if ($Allowlist) { $arguments += '--allowlist' }
if ($RemoteVideo) { $arguments += '--remote-video' }
if ($NativeSourceMute) { $arguments += '--native-source-mute' }
if ($ProductionRoute) { $arguments += '--production-route' }
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$output.stdout.txt" -RedirectStandardError "$output.stderr.txt"
$timeoutSeconds = if ($ProductionRoute) { 120 } else { 60 }
$deadline = [DateTime]::UtcNow.AddSeconds($timeoutSeconds)
while (-not $process.WaitForExit(1000) -and [DateTime]::UtcNow -lt $deadline) {}
if (-not $process.HasExited) {
    # This process was created here; retain evidence and report instead of killing
    # any browser/service process that might belong to a different application.
    throw "Owned probe PID $($process.Id) exceeded $timeoutSeconds seconds. Evidence: $output"
}
$process.Refresh()
$result = Join-Path $output 'result.json'
if (-not (Test-Path -LiteralPath $result)) {
    throw "Probe exited $($process.ExitCode) without a report. See $output.stderr.txt"
}
$data = Get-Content -Raw -LiteralPath $result | ConvertFrom-Json
$files = @(Get-ChildItem -LiteralPath $PSScriptRoot -File -Recurse | Where-Object { $_.Name -ne 'README.md' } |
    ForEach-Object { [pscustomobject]@{ path=$_.FullName; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
[pscustomobject]@{ binarySha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash; sources=$files;
    compiledInputs=$buildInputs } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'provenance.json') -Encoding utf8
[pscustomobject]@{
    exitCode = $process.ExitCode
    result = $data.result
    runtime = $data.runtime
    captureState = $data.captureAttempt.state
    captureError = $data.captureAttempt.lastError
    report = $result
} | ConvertTo-Json -Depth 8
exit $process.ExitCode
