[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RunDir = [IO.Path]::GetFullPath($RunDir)
$metadata = Get-Content -LiteralPath (Join-Path $RunDir 'metadata.json') -Raw | ConvertFrom-Json
$runs = @()
foreach ($directory in Get-ChildItem -LiteralPath $RunDir -Directory -Force) {
    if ($directory.Name -notmatch '^(cef|webview2)-(\d+)p-r(\d+)$') { continue }
    $backend = $Matches[1]
    if (Test-Path -LiteralPath (Join-Path $directory.FullName 'contention.json')) { throw "Game contention invalidated $($directory.Name)." }
    $file = Join-Path $directory.FullName 'result.json'
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing result: $file" }
    $data = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    if ($data.status -ne 'passed' -or -not $data.shutdown_clean -or $data.exit_code -ne 0) { throw "Failed run: $file" }
    if (@($data.phases | Where-Object { $_.invalid_markers -ne 0 }).Count) { throw "Invalid rendered frames: $file" }
    $runs += [pscustomobject]@{ backend=$backend; name=$directory.Name; data=$data }
}
$expected = $metadata.repetitions * @($metadata.heights).Count * @($metadata.backends).Count
if ($runs.Count -ne $expected) { throw "Expected $expected runs; found $($runs.Count)." }
function Stats($Values) {
    $sorted = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if (-not $sorted.Count) { return $null }
    $n = $sorted.Count
    $middle = [int][math]::Floor($n/2)
    $median = if ($n % 2) { $sorted[$middle] } else { ($sorted[$middle-1]+$sorted[$middle])/2 }
    [pscustomobject]@{ median=[double]$median; min=[double]$sorted[0]; max=[double]$sorted[-1]; n=$n }
}
$metrics = @('unique_fps','delivered_fps','cpu_core_percent','private_commit_mean_mib','private_commit_peak_mib','bridge_rtt_p50_ms','bridge_rtt_p95_ms','frame_interval_p95_ms','unique_interval_p95_ms','consumer_copy_p95_ms','gpu_engine_sum_mean_percent','processes')
$rows = @(); $startup = @()
foreach ($height in $metadata.heights) {
    foreach ($backend in $metadata.backends) {
        $selected = @($runs | Where-Object { $_.backend -eq $backend -and $_.data.height -eq $height })
        $startup += [pscustomobject]@{ height=$height; backend=$backend; runtime=$selected[0].data.runtime; first_content_ms=(Stats @($selected | ForEach-Object { $_.data.startup_ms.first_content })); shutdown_ms=(Stats @($selected | ForEach-Object { $_.data.shutdown_ms })) }
        foreach ($phase in 'idle','canvas','dom','three_views') {
            $values = @($selected | ForEach-Object { $_.data.phases | Where-Object { $_.name -eq $phase } })
            $row = [ordered]@{ height=$height; backend=$backend; phase=$phase }
            foreach ($metric in $metrics) { $row[$metric] = Stats @($values | ForEach-Object { $_.$metric }) }
            $rows += [pscustomobject]$row
        }
    }
}
$summary = [ordered]@{ metadata=$metadata; runs=$runs.Count; phases=$rows; startup=$startup }
$summary | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $RunDir 'summary.json') -Encoding utf8
function Encode([string]$Text) { [Net.WebUtility]::HtmlEncode($Text) }
function Number($Metric) { if ($null -eq $Metric) { return 'unavailable' }; '{0:N2}' -f $Metric.median }
function Cell($Metric) { if ($null -eq $Metric) { return '<td>unavailable</td>' }; '<td title="Range {0:N2}–{1:N2}; n={2}">{3}</td>' -f $Metric.min,$Metric.max,$Metric.n,(Number $Metric) }
$html = [Text.StringBuilder]::new()
$null = $html.Append(@'
<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OSF UI backend performance comparison</title><style>
body{max-width:1350px;margin:40px auto;padding:0 24px;font:16px/1.55 system-ui,sans-serif;color:#172538;background:#f5f7fa}h1{font-size:32px;margin-bottom:4px}h2{margin-top:32px}table{border-collapse:collapse;width:100%;background:white;font-variant-numeric:tabular-nums}th,td{padding:10px 12px;text-align:right;border-bottom:1px solid #dbe1e8}th{background:#e7edf5}td:first-child,td:nth-child(2),th:first-child,th:nth-child(2){text-align:left}tr.cef{background:#edf7f5}.note{background:white;border-left:4px solid #52749b;padding:14px 20px;margin:20px 0}.small{font-size:14px;color:#46596f}.scroll{overflow:auto}code{font-size:13px;overflow-wrap:anywhere}a{color:#225b9b}li{margin:7px 0}
</style><h1>OSF UI: WebView2 and CEF</h1>
<p>Measured desktop helper performance. This is not a Starfield FPS benchmark.</p>
'@)
$null = $html.Append("<p><b>Machine:</b> $(Encode $metadata.cpu[0].Name) · $(Encode $runs[0].data.adapter) · $(Encode $metadata.os.Caption)</p>")
$null = $html.Append("<p class='small'>Started $(Encode $metadata.started). $($runs.Count) successful sessions; $($metadata.repetitions) repetitions per backend/resolution. Each workload: $($metadata.warmupSeconds)s warmup + $($metadata.measureSeconds)s measured. Tables show the median; hover a cell for the observed range.</p>")
$null = $html.Append(@'
<div class="note"><b>Read the units carefully.</b> CPU 100% means one fully occupied logical CPU, not the whole machine. Memory is summed process-private committed memory, not physical resident RAM or VRAM. New frames/s counts changes in a marker read from completed output textures; output/s includes repeated markers. The static page should produce no new frames. Bridge RTT is a native-to-JavaScript echo, not input-to-photon latency.</div>
'@)
foreach ($height in $metadata.heights) {
    $null=$html.Append("<h2>$([int]($height*16/9)) × $height</h2><div class='scroll'><table><thead><tr><th>Workload</th><th>Backend</th><th>New frames/s</th><th>Output/s</th><th>CPU (% of one core)</th><th>Private commit (MiB)</th><th>Bridge p95 (ms)</th><th>New-frame gap p95 (ms)</th><th>GPU engine sum (%)</th></tr></thead><tbody>")
    foreach ($phase in 'idle','canvas','dom','three_views') {
        foreach ($backend in $metadata.backends) {
            $row=$rows | Where-Object { $_.height -eq $height -and $_.backend -eq $backend -and $_.phase -eq $phase }
            $label=@{idle='Static menu';canvas='Canvas animation';dom='Animated menu';three_views='Three canvas views'}[$phase]
            $null=$html.Append("<tr class='$backend'><td>$label</td><td>$backend</td>")
            foreach ($metric in 'unique_fps','delivered_fps','cpu_core_percent','private_commit_mean_mib','bridge_rtt_p95_ms','unique_interval_p95_ms','gpu_engine_sum_mean_percent') { $null=$html.Append((Cell $row.$metric)) }
            $null=$html.Append('</tr>')
        }
    }
    $null=$html.Append('</tbody></table></div>')
}
$variable = @($rows | Where-Object { $_.unique_fps.median -gt 0 -and $_.unique_fps.min -lt $_.unique_fps.median * 0.8 })
if ($variable.Count) {
    $null=$html.Append('<h2>Observed variation</h2><p>Some repetitions were substantially slower than their median. The measurements do not identify the cause; retain these ranges when judging consistency.</p><ul>')
    foreach ($row in $variable) { $null=$html.Append(('<li>{0} / {1}p / {2}: {3:N1}–{4:N1} new frames/s across repetitions.</li>' -f $row.backend,$row.height,$row.phase,$row.unique_fps.min,$row.unique_fps.max)) }
    $null=$html.Append('</ul>')
}
$null=$html.Append('<h2>Fresh-profile startup and shutdown</h2><table><thead><tr><th>Resolution</th><th>Backend/runtime</th><th>First content (ms)</th><th>Shutdown (ms)</th></tr></thead><tbody>')
foreach ($row in $startup) { $null=$html.Append("<tr class='$($row.backend)'><td>$($row.height)p</td><td>$(Encode $row.runtime)</td>$(Cell $row.first_content_ms)$(Cell $row.shutdown_ms)</tr>") }
$null=$html.Append(@'
</tbody></table><h2>Method and limits</h2><ul>
<li>Identical local pages, viewport size, profiles created fresh per session, and default production helpers. Order alternates by repetition. OS disk caches are warm; startup is not a cold-boot measurement. CEF 150 and installed WebView2 155 use different Chromium versions.</li>
<li>Canvas: 700 moving translucent circles at a target 60 updates/s. Animated menu: 160 DOM cards with transforms and bar-width changes. Three-view case renders three full-size overlapping canvas documents; visible frame counts describe the topmost output.</li>
<li>All phases include a 10 Hz bridge echo and a 1 Hz page statistics message. Static mode has no pixel changes but retains a lightweight requestAnimationFrame callback. Developer mode is off.</li>
<li>The consumer opens the real shared textures/fence, performs a GPU copy, reads one pixel after completion, then acknowledges the slot. That common consumer overhead is outside reported browser CPU. There is no Starfield scene, D3D12 blend, game contention, frame generation, or MO2 cache preparation in these numbers.</li>
<li>CPU and memory cover the helper and its discovered descendants. Shared DWM/system services are excluded. The GPU column sums per-process engine counters; it can exceed 100% and is not total GPU utilization. It does not capture all Windows composition work, so do not use it alone to rank total GPU cost.</li>
<li>Completed texture markers were valid and both helpers exited normally in every included run. These short synthetic runs do not establish long-session stability, real-mod performance, controller behavior, or multi-GPU compatibility.</li>
</ul><h2>Reproducibility</h2><p>Full samples, frame timing statistics, process counts, screenshots, and event logs are retained beside this report. <a href="summary.json">Aggregated data</a> · <a href="metadata.json">Machine and binary hashes</a></p><table><thead><tr><th>File</th><th>SHA-256</th></tr></thead><tbody>
'@)
foreach ($hash in $metadata.hashes) { $null=$html.Append("<tr><td>$(Encode $hash.Path)</td><td><code>$(Encode $hash.Hash)</code></td></tr>") }
$null=$html.Append('</tbody></table></html>')
$html.ToString() | Set-Content -LiteralPath (Join-Path $RunDir 'comparison.html') -Encoding utf8
$rows | Select-Object height,backend,phase,@{n='fps';e={$_.unique_fps.median}},@{n='cpu';e={$_.cpu_core_percent.median}},@{n='MiB';e={$_.private_commit_mean_mib.median}},@{n='bridgeP95';e={$_.bridge_rtt_p95_ms.median}} | Format-Table
Write-Host "Report: $(Join-Path $RunDir 'comparison.html')"
