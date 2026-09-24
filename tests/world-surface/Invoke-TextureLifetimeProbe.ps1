#requires -Version 7.2
[CmdletBinding()]
param([ValidateSet('Away','Return','Purge','Cycle')][string]$Action = 'Cycle', [ValidateRange(1,8)][int]$Cycles = 3)
$ErrorActionPreference = 'Stop'
. 'C:\Modding\Starfield\OSF Test Harness\scripts\Common.ps1'
. (Join-Path $script:HarnessRoot 'scripts\Reports.ps1')
$lock = [IO.File]::Open((Join-Path $script:StateRoot 'runner.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $owner = Read-JsonFile (Join-Path $script:StateRoot 'world-surface-owner.json')
    if (-not $owner -or $owner.source -ne $PSScriptRoot) { throw 'This checkout does not own the retained world fixture.' }
    $script:Run = $owner.run
    $null = Get-OwnedGame
    $null = Invoke-Input 'focus'
    function Send-Console([string]$command) {
        $null = Invoke-Input 'focus'
        $state = Get-GameState
        if ('Console' -notin $state.menus -and 'ConsoleMenu' -notin $state.menus) { Send-Key 192 40 }
        $null = Wait-Observed 'console opened for texture-lifetime test' { Get-GameState } { param($s) 'Console' -in $s.menus -or 'ConsoleMenu' -in $s.menus } 5
        foreach ($character in $command.ToCharArray()) {
            $vk = if ($character -eq ' ') { 32 } else { [int][char][char]::ToUpperInvariant($character) }
            Send-Key $vk 40
        }
        Send-Key 13 40
        Send-Key 192 40
        $null = Wait-Observed 'console closed after texture-lifetime command' { Get-GameState } { param($s) -not $s.snapshotStale -and 'Console' -notin $s.menus -and 'ConsoleMenu' -notin $s.menus } 10
    }
    function Read-Surfaces {
        $line = @(Get-Content -LiteralPath (Join-Path $script:Run.logs 'OSF UI.log') -Tail 250 | Select-String 'WorldSurface snapshot (\[.*\])')[-1]
        return @($line.Matches[0].Groups[1].Value | ConvertFrom-Json -AsHashtable)
    }
    function Read-Cache {
        $line = @(Get-Content -LiteralPath (Join-Path $script:Run.logs 'OSF UI.log') -Tail 250 | Select-String 'WorldSurface cache (\{.*\})')[-1]
        return $line.Matches[0].Groups[1].Value | ConvertFrom-Json -AsHashtable
    }
    function Leave-Fixture {
        if ((Get-GameState).cellFormId -ne 0x2BE3A9) { throw 'Away requires the owned QASmoke fixture.' }
        Send-Console 'coc CityNewAtlantisLodgeInt'
        $null = Wait-Observed 'left QASmoke to unload private world boards' { Get-GameState } { param($s) -not $s.snapshotStale -and $s.cellFormId -ne 0 -and $s.cellFormId -ne 0x2BE3A9 -and 'LoadingMenu' -notin $s.menus } 45
        Send-Console 'pcb'
    }
    function Return-Fixture {
        Send-Console 'coc QASmoke'
        $null = Wait-Observed 'reloaded QASmoke with private world boards' { Get-GameState } { param($s) -not $s.snapshotStale -and $s.cellFormId -eq 0x2BE3A9 -and 'LoadingMenu' -notin $s.menus } 45
        $owner.cameraAimed = $false
        Write-JsonFile (Join-Path $script:StateRoot 'world-surface-owner.json') $owner
    }
    if ($Action -eq 'Cycle') {
        $cyclesEvidence = @()
        for ($cycle=1; $cycle -le $Cycles; ++$cycle) {
            $before = @(Wait-Observed 'all four named outputs ready before eviction' { @(Read-Surfaces) } {
                param($s) $s.Count -eq 4 -and @($s | Where-Object { $_.engineOwners -ne 1 -or $_.lastCompletedFrame -eq 0 }).Count -eq 0
            } 30)
            $cacheBefore = Read-Cache
            $request = [Guid]::NewGuid().ToString('N')
            $requestPath = Join-Path $script:ModRoot ($owner.testModName + '\SFSE\Plugins\OSF\UI\views\osfui-world-test\evict.request')
            [IO.File]::WriteAllText($requestPath, $request)
            $held = @(Wait-Observed 'eviction waits for engine ownership' { @(Read-Surfaces) } {
                param($s) @($s | Where-Object { -not $_.evictionPending }).Count -eq 0
            } 10)
            Assert-Test (@($held | Where-Object { $_.residentBytes -eq 0 -or $_.engineOwners -eq 0 }).Count -eq 0) "Cycle $cycle`: visible materials pin every output during eviction" $held
            Leave-Fixture
            $empty = Wait-Observed 'engine resources and GPU copies retire before releasing outputs' { Read-Cache } {
                param($c) $c.residentBytes -eq 0 -and $c.retiredOutputs -ge ($cacheBefore.retiredOutputs + 4)
            } 30
            Assert-Test ($empty.residentBytes -eq 0) "Cycle $cycle`: all output allocations released after real cell unload" $empty
            Return-Fixture
            $after = @(Wait-Observed 'same asset IDs bind new output generations on reload' { @(Read-Surfaces) } {
                param($s)
                if ($s.Count -ne 4) { return $false }
                foreach ($old in $before) {
                    $new = @($s | Where-Object id -eq $old.id)[0]
                    if ($new.outputGeneration -le $old.outputGeneration -or $new.lastCompletedFrame -eq 0 -or $new.engineOwners -ne 1) { return $false }
                }
                return $true
            } 30)
            Assert-Test (@($after | Where-Object gpuFailed).Count -eq 0) "Cycle $cycle`: reloaded feeds resume without GPU failure" $after
            $cacheAfter = Read-Cache
            Assert-Test ($cacheAfter.residentBytes -eq $cacheBefore.residentBytes -and $cacheAfter.residentBytes -le $cacheAfter.budgetBytes) "Cycle $cycle`: residency returns to the same bounded size" $cacheAfter
            $cyclesEvidence += @{ cycle=$cycle; before=$before; held=$held; empty=$empty; after=$after; cacheAfter=$cacheAfter }
            Write-JsonFile (Join-Path $script:Run.path 'texture-lifetime-cycles.json') $cyclesEvidence
        }
    } elseif ($Action -eq 'Away') { Leave-Fixture }
    elseif ($Action -eq 'Return') { Return-Fixture }
    else { Send-Console 'pcb' }
    $state = Get-GameState
    Add-Evidence 'texture-lifetime' $Action $state
    $state | ConvertTo-Json -Depth 5
} finally { $lock.Dispose() }