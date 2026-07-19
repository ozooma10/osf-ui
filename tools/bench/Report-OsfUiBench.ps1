<#
.SYNOPSIS
    Summarize one benchmark run (docs/renderer-benchmark.md) from the paired
    sampler CSV + "OSF UI.log" snapshot produced by Sample-OsfUiBench.ps1.

.DESCRIPTION
    Emits a markdown summary:
      - run metadata (renderer, output size, view)
      - internal Bench: channels aggregated per overlay state (n-weighted avg;
        window percentiles are n-weighted means of per-window values — an
        approximation, exact enough at 5 s windows; max is exact)
      - produced/uploaded frame rates
      - external CSV columns split into overlay-closed vs overlay-open phases
        using the log's visibility edges
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Csv,
    [Parameter(Mandatory)][string]$Log,
    [string]$OutMd
)

$ErrorActionPreference = 'Stop'

# ---- parse log ----
$renderer = '?'; $outputSize = '?'; $view = '?'
$edges = @()   # @{ Time=<datetime-of-day>; Visible=<bool> }
$bench = @()   # channel windows
$rates = @()
foreach ($line in Get-Content $Log) {
    if ($line -match '^\[(\d\d:\d\d:\d\d\.\d\d\d)\]') { $tod = $Matches[1] } else { continue }
    if ($line -match 'Config: loaded .*\(renderer=([^,]+),') { $renderer = $Matches[1] }
    if ($line -match 'Runtime: output (\d+x\d+)') { $outputSize = $Matches[1] }
    if ($line -match "loaded \d+ view\(s\); default menu = '([^']+)'") { $view = $Matches[1] }
    if ($line -match 'Runtime: overlay visibility -> (true|false)') {
        $edges += @{ Tod = $tod; Visible = ($Matches[1] -eq 'true') }
    }
    if ($line -match 'Bench: ch=(\w+) vis=(\d) win=([\d.]+)s n=(\d+) avg=([\d.]+) p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+)') {
        $bench += [pscustomobject]@{
            Ch = $Matches[1]; Vis = [int]$Matches[2]; Win = [double]$Matches[3]
            N = [long]$Matches[4]; Avg = [double]$Matches[5]; P50 = [double]$Matches[6]
            P95 = [double]$Matches[7]; P99 = [double]$Matches[8]; Max = [double]$Matches[9]
        }
    }
    if ($line -match 'Bench: rates vis=(\d) win=([\d.]+)s produced=(\d+) \(([\d.]+)/s\) uploaded=(\d+) \(([\d.]+)/s\)') {
        $rates += [pscustomobject]@{
            Vis = [int]$Matches[1]; Win = [double]$Matches[2]
            Produced = [long]$Matches[3]; Uploaded = [long]$Matches[5]
        }
    }
}

# ---- parse csv ----
$rows = Import-Csv $Csv | Where-Object { $_.time -and $_.time -notlike '#*' }
if (-not $rows) { throw "no data rows in $Csv" }
$runDate = ([datetime]$rows[0].time).Date

function TodToDateTime([string]$tod) { $runDate + [TimeSpan]::Parse($tod) }

# Tag every CSV row with overlay state from the log edges.
$edgeTimes = @($edges | ForEach-Object { [pscustomobject]@{ At = (TodToDateTime $_.Tod); Visible = $_.Visible } })
foreach ($r in $rows) {
    $t = [datetime]$r.time
    $vis = $false
    foreach ($e in $edgeTimes) { if ($e.At -le $t) { $vis = $e.Visible } else { break } }
    $r | Add-Member -NotePropertyName vis -NotePropertyValue ([int]$vis)
}

function Stat($set, $name) {
    $vals = @($set | ForEach-Object { [double]$_.$name })
    if (-not $vals) { return $null }
    [pscustomobject]@{
        Mean = [Math]::Round(($vals | Measure-Object -Average).Average, 2)
        Max  = [Math]::Round(($vals | Measure-Object -Maximum).Maximum, 2)
    }
}

$md = [System.Collections.Generic.List[string]]::new()
$md.Add("# OSF UI renderer benchmark run — $renderer")
$md.Add('')
$md.Add("- csv: ``$(Split-Path -Leaf $Csv)``  log: ``$(Split-Path -Leaf $Log)``")
$md.Add("- renderer: **$renderer** · output: $outputSize · view: $view")
$openSecs = ($rows | Where-Object vis -eq 1).Count
$closedSecs = ($rows | Where-Object vis -eq 0).Count
$md.Add("- samples: $($rows.Count) (~1/s) — overlay open ~${openSecs}s, closed ~${closedSecs}s")
$md.Add('')

$md.Add('## Internal timing (Bench: channels, ms)')
$md.Add('')
$md.Add('| channel | overlay | n | avg | p50 | p95 | p99 | max |')
$md.Add('|---|---|---|---|---|---|---|---|')
foreach ($vis in 0, 1) {
    foreach ($ch in 'frame', 'tick', 'present', 'produce') {
        $set = @($bench | Where-Object { $_.Ch -eq $ch -and $_.Vis -eq $vis })
        if (-not $set) { continue }
        $n = ($set | Measure-Object N -Sum).Sum
        $w = { param($prop) [Math]::Round((($set | ForEach-Object { $_.$prop * $_.N } | Measure-Object -Sum).Sum / $n), 3) }
        $mx = [Math]::Round(($set | Measure-Object Max -Maximum).Maximum, 3)
        $md.Add("| $ch | $(if ($vis) {'open'} else {'closed'}) | $n | $(& $w 'Avg') | $(& $w 'P50') | $(& $w 'P95') | $(& $w 'P99') | $mx |")
    }
}
$md.Add('')

$openRates = @($rates | Where-Object Vis -eq 1)
if ($openRates) {
    $win = ($openRates | Measure-Object Win -Sum).Sum
    $prod = ($openRates | Measure-Object Produced -Sum).Sum
    $upl = ($openRates | Measure-Object Uploaded -Sum).Sum
    $md.Add("Overlay open: web frames produced **$([Math]::Round($prod/$win,1))/s**, compositor uploads **$([Math]::Round($upl/$win,1))/s** (over $([Math]::Round($win,0)) s)")
    $md.Add('')
}

$md.Add('## External resources (per-second samples)')
$md.Add('')
$md.Add('| metric | closed mean | closed max | open mean | open max |')
$md.Add('|---|---|---|---|---|')
$cols = @(
    @('sfCpuPct',   'Starfield CPU % (of machine)'),
    @('sfWsMB',     'Starfield working set MB'),
    @('sfPrivMB',   'Starfield private MB'),
    @('sfGpuPct',   'Starfield GPU util %'),
    @('sfGpuMemMB', 'Starfield GPU dedicated MB'),
    @('wvCount',    'WebView2 process count'),
    @('wvCpuPct',   'WebView2 tree CPU % (of machine)'),
    @('wvWsMB',     'WebView2 tree working set MB'),
    @('wvPrivMB',   'WebView2 tree private MB'),
    @('wvGpuPct',   'WebView2 tree GPU util %'),
    @('wvGpuMemMB', 'WebView2 tree GPU dedicated MB'))
$closed = @($rows | Where-Object vis -eq 0)
$open = @($rows | Where-Object vis -eq 1)
foreach ($c in $cols) {
    $sc = Stat $closed $c[0]; $so = Stat $open $c[0]
    $md.Add("| $($c[1]) | $($sc.Mean) | $($sc.Max) | $($so.Mean) | $($so.Max) |")
}
$md.Add('')
$md.Add('Combined open-overlay footprint (Starfield + WebView2 tree): ' +
    "CPU mean $([Math]::Round((Stat $open 'sfCpuPct').Mean + (Stat $open 'wvCpuPct').Mean, 2))%, " +
    "working set mean $([Math]::Round((Stat $open 'sfWsMB').Mean + (Stat $open 'wvWsMB').Mean, 0)) MB, " +
    "GPU mean $([Math]::Round((Stat $open 'sfGpuPct').Mean + (Stat $open 'wvGpuPct').Mean, 2))%")

$orphanLine = Get-Content $Csv | Where-Object { $_ -like '# orphans_after_exit=*' } | Select-Object -Last 1
if ($orphanLine) { $md.Add(''); $md.Add("WebView2 orphans after game exit: $($orphanLine -replace '.*=','')") }

$text = $md -join "`n"
if (-not $OutMd) { $OutMd = [IO.Path]::ChangeExtension($Csv, 'md') }
Set-Content $OutMd $text
Write-Host $text
Write-Host "`nwritten: $OutMd"
