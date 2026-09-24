#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Run', 'Start', 'Status', 'Capture', 'Stop')][string]$Action = 'Run',
    [ValidateSet('Baseline', 'Live')][string]$Mode = 'Live',
    [string]$HarnessRoot = 'C:\Modding\Starfield\OSF Test Harness',
    [string]$RuntimeMod = 'C:\Modding\Starfield\MO2\mods\OSF UI',
    [string]$SettingsModName = 'OSF Settings',
    [string]$Python = 'python',
    [switch]$KeepGame,
    [switch]$AimAtScreen,
    [switch]$RestartBrowser,
    [switch]$Overlay,
    [switch]$BudgetPressure,
    [ValidateRange(1, 4)][int]$Screens = 1,
    [ValidatePattern('^OSF Testing - [A-Za-z0-9 -]+$')][string]$TestModName = 'OSF Testing - World Surface',
    [string]$CaptureName = 'inspection'
)

# Keep the scenario in its owning mod. The shared harness remains unchanged.
$ErrorActionPreference = 'Stop'
if ($RestartBrowser -and ($Mode -ne 'Live' -or $Action -ne 'Run')) { throw 'RestartBrowser requires a fresh Live Run.' }
if ($Overlay -and ($Mode -ne 'Live' -or $Action -notin @('Run', 'Start'))) { throw 'Overlay requires a fresh Live Run or Start.' }
$fixtureSource = $PSScriptRoot
$TranslationRegistration = $false
. (Join-Path $HarnessRoot 'scripts\Common.ps1')
. (Join-Path $HarnessRoot 'scripts\Environment.ps1')
. (Join-Path $HarnessRoot 'scripts\Reports.ps1')
$testMod = Join-Path $script:ModRoot $testModName
$ownerFile = Join-Path $script:StateRoot 'world-surface-owner.json'
$pluginName = 'OSFUIWorldSurfaceTest.esp'
$viewId = 'osfui-world-test/screen'
$lock = $null
$script:worldOwner = $null
$preserveSession = $Action -in @('Status', 'Capture')
$cleanupOwned = $false
$exitCode = 0

function Close-WorldTestMO2 {
    if (@(Get-GameProcess).Count) { throw '[blocked] Cannot change the private profile while Starfield is running.' }
    $moExe = Join-Path $script:Config.MO2 'ModOrganizer.exe'
    foreach ($mo in @(Get-Process ModOrganizer -ErrorAction SilentlyContinue | Where-Object Path -eq $moExe)) {
        $null = $mo.CloseMainWindow()
        if (-not $mo.WaitForExit(10000)) { throw '[blocked] Idle MO2 did not close normally.' }
    }
}

function Save-WorldTestProfile {
    if (Test-Path -LiteralPath $ownerFile) { throw '[blocked] A retained world-surface test owns the private profile. Run -Action Stop first.' }
    Close-WorldTestMO2
    $profileMarker = Read-JsonFile (Join-Path $script:ProfileRoot '.osf-test-harness.json')
    if (-not $profileMarker -or $profileMarker.harnessRoot -ne $script:HarnessRoot) {
        throw '[blocked] Initialize the standard harness once before this scenario.'
    }
    $backup = Join-Path $script:Run.path 'profile-before'
    [IO.Directory]::CreateDirectory($backup) | Out-Null
    $files = @('modlist.txt', 'plugins.txt', 'loadorder.txt', 'settings.ini', 'StarfieldCustom.ini', 'Starfield.ini', 'StarfieldPrefs.ini')
    $existing = @()
    foreach ($name in $files) {
        $path = Join-Path $script:ProfileRoot $name
        if (Test-Path -LiteralPath $path) {
            Copy-Item -LiteralPath $path -Destination (Join-Path $backup $name)
            $existing += $name
        }
    }
    $record = @{
        source = $fixtureSource; profileRoot = $script:ProfileRoot; backup = $backup
        files = $files; existing = $existing; run = $script:Run; mode = $Mode; screens = $Screens; testModName = $TestModName; budgetPressure = [bool]$BudgetPressure
    }
    Write-JsonFile $ownerFile $record
    return $record
}

function Restore-WorldTestProfile {
    if (-not $script:worldOwner) { return }
    Close-WorldTestMO2
    if ($script:worldOwner.source -ne $fixtureSource -or $script:worldOwner.profileRoot -ne $script:ProfileRoot) {
        throw '[blocked] World test profile ownership differs from this runner.'
    }
    $backup = Assert-ChildPath $script:ArtifactRoot $script:worldOwner.backup
    foreach ($name in $script:worldOwner.files) {
        $path = Assert-ChildPath $script:ProfileRoot (Join-Path $script:ProfileRoot $name)
        if ($name -in $script:worldOwner.existing) {
            Copy-Item -LiteralPath (Join-Path $backup $name) -Destination $path -Force
            if ((Get-FileHash -LiteralPath $path).Hash -ne (Get-FileHash -LiteralPath (Join-Path $backup $name)).Hash) {
                throw "[blocked] Profile restoration hash differs: $name"
            }
        } elseif (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    Remove-Item -LiteralPath $ownerFile -Force
    Add-Evidence 'cleanup' 'Restored exact private profile files' @{ backup = $backup }
    $script:worldOwner = $null
}

function Stage-WorldTest {
    $safeMod = Assert-ChildPath $script:ModRoot $testMod
    $markerPath = Join-Path $safeMod '.osf-world-surface-test.json'
    if (Test-Path -LiteralPath $safeMod) {
        $marker = Read-JsonFile $markerPath
        if (-not $marker -or $marker.source -ne $fixtureSource) { throw "[blocked] Unowned test mod: $safeMod" }
        foreach ($item in Get-ChildItem -LiteralPath $safeMod -Force) {
            $checked = Assert-ChildPath $safeMod $item.FullName
            if ($item.Name -ne '.osf-world-surface-test.json') { Remove-Item -LiteralPath $checked -Recurse -Force }
        }
    } else {
        [IO.Directory]::CreateDirectory($safeMod) | Out-Null
    }
    Write-JsonFile $markerPath @{ source = $fixtureSource; run = $script:Run.id }
    if ($Mode -eq 'Live') {
        foreach ($relative in @('SFSE\Plugins\OSFUI.dll', 'SFSE\Plugins\OSF\UI\bin\osfui_webview2_host.exe')) {
            if (-not (Test-Path -LiteralPath (Join-Path $RuntimeMod $relative))) {
                throw "[blocked] Build and deploy the current OSF UI runtime first: $relative"
            }
        }
        Copy-Tree $RuntimeMod $safeMod
    }
    & $Python (Join-Path $fixtureSource 'make-fixture.py') --out $safeMod --screens $Screens
    [IO.File]::WriteAllText((Join-Path $testMod 'SFSE\Plugins\OSF\UI\views\osfui-world-test\output-budget-mib.txt'), $(if ($BudgetPressure) { '8' } else { '256' }))
    if ($LASTEXITCODE) { throw '[blocked] World-screen fixture generation failed.' }
    $viewDir = Join-Path $safeMod 'SFSE\Plugins\OSF\UI\views\osfui-world-test\screen'
    [IO.Directory]::CreateDirectory($viewDir) | Out-Null
    if ($Screens -eq 1) {
        foreach ($name in @('manifest.json', 'index.html')) {
            Copy-Item -LiteralPath (Join-Path $fixtureSource $name) -Destination $viewDir -Force
        }
    }
    if ($Overlay) {
        Copy-Tree (Join-Path $fixtureSource 'overlay') (Join-Path $safeMod 'SFSE\Plugins\OSF\UI\views\osfui-world-test\overlay')
    }

    $profileFile = Join-Path $script:ProfileRoot 'modlist.txt'
    $enable = @($testModName, $SettingsModName, 'OSF Test Harness', 'OSF Testing - Auto Load', 'OSF Testing Output')
    $disable = @('OSF UI', 'OSF-UI-v1.6.0', 'OSF Testing - UI Smoke', 'OSF Testing - UI 1.6.0',
        'OSF Testing - Settings', 'OSF Testing - Character Studio', 'CharacterStudio', 'Character Studio',
        'OSF RE', 'Auto Load', 'Luma - Native HDR and more', 'Starfield Shader Injector and ReShade Helper')
    if (-not (Test-Path -LiteralPath (Join-Path $script:ModRoot ($SettingsModName + '\SFSE\Plugins\OSFSettings.dll')))) {
        throw '[blocked] The configured Settings dependency DLL is missing.'
    }
    $lines = @(Get-Content -LiteralPath $profileFile | Where-Object { $_ -and $_.TrimStart('+', '-') -notin $enable } | ForEach-Object {
        $name = $_.TrimStart('+', '-')
        if ($name -in $disable -or $name -like 'OSF Testing - World*') { '-' + $name } else { $_ }
    })
    [IO.File]::WriteAllLines($profileFile, @($enable | ForEach-Object { '+' + $_ }) + $lines, [Text.UTF8Encoding]::new($false))
    foreach ($name in @('plugins.txt', 'loadorder.txt')) {
        $path = Join-Path $script:ProfileRoot $name
        $lines = if (Test-Path -LiteralPath $path) { @(Get-Content -LiteralPath $path | Where-Object { $_.TrimStart('*') -notin @($pluginName, 'OSFUI.esp') }) } else { @() }
        $line = if ($name -eq 'plugins.txt') { '*' + $pluginName } else { $pluginName }
        [IO.File]::WriteAllLines($path, $lines + @($line), [Text.UTF8Encoding]::new($false))
    }
    $hashes = @(Get-ChildItem -LiteralPath $safeMod -File -Recurse | ForEach-Object {
        @{ file = [IO.Path]::GetRelativePath($safeMod, $_.FullName); sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash; bytes = $_.Length }
    })
    Write-JsonFile (Join-Path $script:Run.path 'world-fixture-hashes.json') $hashes
    foreach ($name in @('modlist.txt', 'plugins.txt', 'loadorder.txt')) {
        Copy-Item -LiteralPath (Join-Path $script:ProfileRoot $name) -Destination (Join-Path $script:Run.path ('staged-' + $name)) -Force
    }
    foreach ($name in @('Invoke-WorldSurfaceTest.ps1', 'make-fixture.py', 'manifest.json', 'index.html')) {
        Copy-Item -LiteralPath (Join-Path $fixtureSource $name) -Destination $script:Run.path -Force
    }
    if ($Overlay) { Copy-Tree (Join-Path $fixtureSource 'overlay') (Join-Path $script:Run.path 'overlay') }
    if ($Screens -gt 1) {
        Copy-Item -LiteralPath (Join-Path $fixtureSource 'world_boards.py') -Destination $script:Run.path
        Copy-Tree (Join-Path $safeMod 'SFSE\Plugins\OSF\UI\views') (Join-Path $script:Run.path 'views')
    }
    Add-Evidence 'fixture' 'Private QASmoke screen fixture staged' @{
        mode = $Mode; view = $viewId; mod = $safeMod; plugin = $pluginName; overlay = [bool]$Overlay; restartBrowser = [bool]$RestartBrowser; screens = $Screens
        cellFormId = '0x002BE3A9'; referenceLocalFormId = '0x81D'; runtimeSource = $RuntimeMod
    }
}

function Read-WorldSnapshot {
    param([switch]$All)
    $path = Join-Path $script:Run.logs 'OSF UI.log'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $matches = @(Get-Content -LiteralPath $path -Tail 300 | Select-String -Pattern 'WorldSurface snapshot (\[.*\])')
    if (-not $matches.Count) { return $null }
    $records = $matches[-1].Matches[0].Groups[1].Value | ConvertFrom-Json -AsHashtable
    if ($All) { return @($records | Where-Object { $_.id -eq $viewId -or $_.id -match '^(osfui-world-test|z-world-test)/screen(-2)?$' }) }
    return @($records | Where-Object id -eq $viewId) | Select-Object -First 1
}

function Get-WorldTestHelpers {
    $game = Get-OwnedGame
    if (-not $game) { throw '[blocked] Owned game exited while identifying browser helpers.' }
    $gameIdentity = Get-ProcessIdentity $game
    $gameStart = [DateTime]::FromFileTimeUtc([long]$gameIdentity.processStartFileTime)
    $expectedName = 'osfui_webview2_host.exe'
    $expectedHash = (Get-FileHash -LiteralPath (Join-Path $testMod 'SFSE\Plugins\OSF\UI\bin\osfui_webview2_host.exe')).Hash
    $mirrorRoot = Join-Path $env:LOCALAPPDATA 'OSFUI\bin'
    foreach ($candidate in @(Get-CimInstance Win32_Process -Filter "Name='$expectedName'" | Where-Object {
        $_.CommandLine -match ('(?:^|\s)--game-pid=' + $game.Id + '(?:\s|$)')
    })) {
        if (-not $candidate.ExecutablePath -or [IO.Path]::GetFileName($candidate.ExecutablePath) -cne $expectedName) {
            throw '[blocked] Browser helper executable identity is unavailable or unexpected.'
        }
        $executable = Assert-ChildPath $mirrorRoot $candidate.ExecutablePath
        if ((Get-FileHash -LiteralPath $executable).Hash -ne $expectedHash) { throw '[blocked] Browser helper differs from this run staged executable.' }
        $process = Get-Process -Id $candidate.ProcessId -ErrorAction Stop
        $identity = Get-ProcessIdentity $process
        if ($process.Path -ne $executable -or $process.StartTime.ToUniversalTime() -lt $gameStart -or $candidate.CreationDate.ToUniversalTime() -lt $gameStart) {
            throw '[blocked] Browser helper path or creation time does not belong to this game session.'
        }
        $instanceMatch = [regex]::Match($candidate.CommandLine, '(?:^|\s)--instance=([^\s]+)(?:\s|$)')
        @{
            pid = $identity.pid; processStartFileTime = $identity.processStartFileTime
            path = $executable; sha256 = $expectedHash; commandLine = $candidate.CommandLine
            instance = if ($instanceMatch.Success) { $instanceMatch.Groups[1].Value } else { '' }
            gamePid = $gameIdentity.pid; gameStartFileTime = $gameIdentity.processStartFileTime
        }
    }
}

function Invoke-WorldBrowserRestart {
    $beforeAll = @(Read-WorldSnapshot -All)
    $before = Read-WorldSnapshot
    if (-not $before -or $before.ringGeneration -eq 0) { throw '[blocked] No live browser ring before the restart test.' }
    $helpers = @(Get-WorldTestHelpers)
    $world = @($helpers | Where-Object instance -ceq 'world-0')
    Assert-Test ($world.Count -eq 1) 'Exactly one verified world-0 helper belongs to the owned game' $helpers
    $target = $world[0]
    # Re-read both PID/start identities and the helper's command line immediately
    # before fault injection. A PID recycled after inspection must not be stopped.
    $game = Get-OwnedGame
    $identity = Get-ProcessIdentity $game
    if ($identity.pid -ne $target.gamePid -or $identity.processStartFileTime -ne $target.gameStartFileTime) { throw '[blocked] Game identity changed before browser restart.' }
    $rechecked = @(Get-WorldTestHelpers | Where-Object { $_.pid -eq $target.pid -and $_.processStartFileTime -eq $target.processStartFileTime -and $_.instance -ceq 'world-0' })
    if ($rechecked.Count -ne 1) { throw '[blocked] World browser identity changed before restart.' }
    $process = Get-Process -Id $target.pid -ErrorAction Stop
    if ((Get-ProcessIdentity $process).processStartFileTime -ne $target.processStartFileTime) { throw '[blocked] Browser PID was reused before restart.' }
    Add-Evidence 'fault-injection' 'Stop only the verified world-0 helper to exercise browser-loss recovery' @{ helper = $target; before = $before }
    Stop-Process -InputObject $process -Force
    if (-not $process.WaitForExit(10000)) { throw '[blocked] The owned world browser did not stop before the deadline.' }
    $replacement = Wait-Observed 'replacement world-0 browser process' {
        @(Get-WorldTestHelpers | Where-Object { $_.instance -ceq 'world-0' -and $_.processStartFileTime -ne $target.processStartFileTime })
    } { param($found) $found -and @($found).Count -eq 1 } 45
    $recovered = Wait-Observed 'new browser ring completes fresh GPU frame copies' { Read-WorldSnapshot } {
        param($snapshot) $snapshot -and $snapshot.ringGeneration -gt $before.ringGeneration -and $snapshot.completedFrames -gt ($before.completedFrames + 2) -and $snapshot.lastCompletedFrame -gt 0
    } 45
    $continued = Wait-Observed 'recovered browser keeps producing completed world frames' { Read-WorldSnapshot } {
        param($snapshot) $snapshot -and $snapshot.ringGeneration -eq $recovered.ringGeneration -and $snapshot.completedFrames -ge ($recovered.completedFrames + 15) -and $snapshot.lastCompletedFrame -gt $recovered.lastCompletedFrame
    } 20
    Assert-Test (-not $continued.gpuFailed -and $continued.ringOpenFailures -eq 0 -and $continued.boundDescriptors -eq $before.boundDescriptors) 'Browser restart preserves material bindings and resumes healthy GPU copies' @{ before = $before; recovered = $continued; helper = $replacement }
    foreach ($peer in @($beforeAll | Where-Object id -ne $viewId)) {
        $after = Wait-Observed ('uninterrupted frames on ' + $peer.id) { @(Read-WorldSnapshot -All | Where-Object id -eq $peer.id) | Select-Object -First 1 } {
            param($snapshot) $snapshot -and $snapshot.completedFrames -gt ($peer.completedFrames + 15)
        } 20
        Assert-Test ($after.ringGeneration -eq $peer.ringGeneration -and $after.boundDescriptors -eq $peer.boundDescriptors -and $after.producerDisconnects -eq $peer.producerDisconnects -and -not $after.gpuFailed) ('Restart leaves the independent texture healthy: ' + $peer.id) @{ before = $peer; after = $after }
    }
    $afterHelpers = @(Get-WorldTestHelpers)
    foreach ($peer in @($helpers | Where-Object { $_.instance -match '^world-[1-3]$' })) {
        Assert-Test (@($afterHelpers | Where-Object { $_.pid -eq $peer.pid -and $_.processStartFileTime -eq $peer.processStartFileTime }).Count -eq 1) ('Independent browser survives: ' + $peer.instance) $peer
    }
    if ($Overlay) {
        $originalOverlay = @($helpers | Where-Object instance -ceq '')
        $afterHelpers = @(Get-WorldTestHelpers)
        Assert-Test ($originalOverlay.Count -eq 1 -and @($afterHelpers | Where-Object { $_.pid -eq $originalOverlay[0].pid -and $_.processStartFileTime -eq $originalOverlay[0].processStartFileTime }).Count -eq 1) 'The independent overlay browser survives world-browser restart' $afterHelpers
    }
    $null = Save-Capture 'live-screen-browser-recovered'
}

function Aim-WorldTestCamera {
    if ($script:worldOwner.ContainsKey('cameraAimed') -and $script:worldOwner.cameraAimed) { return }
    $state = Get-GameState
    Assert-Test ($state.cellFormId -eq 0x2BE3A9) 'Camera fixture positioning is limited to the disposable QASmoke cell'
    $null = Invoke-Input 'focus'
    # Only bounded scan-code events to the already claimed foreground game.
    # The pinned save's third-person camera faces north. The screen faces south,
    # so place the player south of it while retaining that initial camera yaw.
    $cameraX = if ($Screens -gt 1) { -5.7 } else { -4.8 }
    $cameraY = if ($Screens -gt 1) { 1.0 } else { 4.2 }
    foreach ($command in @("player.setpos x $cameraX", "player.setpos y $cameraY", 'player.setangle x 0', 'player.setangle z 0')) {
        $null = Invoke-Input 'focus'
        Send-Key 192 40
        $null = Wait-Observed 'console opened for camera positioning' { Get-GameState } {
            param($state) 'Console' -in $state.menus -or 'ConsoleMenu' -in $state.menus
        } 5
        foreach ($character in $command.ToCharArray()) {
            $vk = if ($character -eq '.') { 190 } elseif ($character -eq '-') { 189 } elseif ($character -eq ' ') { 32 } else { [int][char][char]::ToUpperInvariant($character) }
            Send-Key $vk 40
        }
        Send-Key 13 40
        Send-Key 192 40
        $null = Wait-Observed 'console closed after camera positioning' { Get-GameState } {
            param($state) -not $state.snapshotStale -and 'Console' -notin $state.menus -and 'ConsoleMenu' -notin $state.menus
        } 5
    }
    $null = Wait-Observed 'fresh gameplay after camera commands' { Get-GameState } {
        param($state) -not $state.snapshotStale -and 'Console' -notin $state.menus -and 'ConsoleMenu' -notin $state.menus
    } 10
    $pitch = if ($Screens -gt 1) { '60' } else { '180' }
    $null = Invoke-Input 'move' @('--dx', '0', '--dy', $pitch)
    $tick = (Get-GameState).tickCount
    $positioned = Wait-Observed 'camera framing reaches rendered gameplay' { Get-GameState } {
        param($state) -not $state.snapshotStale -and $state.tickCount -gt ($tick + 3)
    } 10
    Assert-Test ([Math]::Abs($positioned.position.x - $cameraX) -lt 0.05 -and [Math]::Abs($positioned.position.y - $cameraY) -lt 0.05) 'Native player position confirms the screen-camera fixture' $positioned.position
    $script:worldOwner.cameraAimed = $true
    Write-JsonFile $ownerFile $script:worldOwner
}

function Invoke-MultipleWorldAssertions {
    $first = @(Wait-Observed 'all independent textures bound and copying' { @(Read-WorldSnapshot -All) } {
        param($records) @($records).Count -eq $Screens -and @($records | Where-Object { $_.boundDescriptors -eq 0 -or $_.completedFrames -lt 3 -or $_.lastCompletedFrame -eq 0 }).Count -eq 0
    } 45)
    $second = @(Wait-Observed 'every independent texture continues updating' { @(Read-WorldSnapshot -All) } {
        param($records)
        if (@($records).Count -ne $Screens) { return $false }
        foreach ($before in $first) {
            $after = @($records | Where-Object id -eq $before.id) | Select-Object -First 1
            if (-not $after -or $after.completedFrames -lt ($before.completedFrames + 15) -or $after.lastCompletedFrame -le $before.lastCompletedFrame) { return $false }
        }
        return $true
    } 25)
    Assert-Test (@($second.texture | Sort-Object -Unique).Count -eq $Screens) 'Each world feed binds its generated asset independently of dimensions' $second
    foreach ($before in $first) {
        $after = @($second | Where-Object id -eq $before.id)[0]
        Assert-Test ($after.boundDescriptors -eq $before.boundDescriptors -and -not $after.gpuFailed -and $after.ringOpenFailures -eq 0) ('Independent GPU copies remain healthy: ' + $after.id) @{ before = $before; after = $after }
    }
    $helpers = @(Get-WorldTestHelpers | Where-Object { $_.instance -match '^world-[0-3]$' })
    Assert-Test ($helpers.Count -eq $Screens -and @($helpers.instance | Sort-Object -Unique).Count -eq $Screens) 'Each independent display has its own verified browser process' $helpers
    $null = Save-Capture 'multiple-world-boards'
}

function Invoke-WorldAssertions {
    $state = Get-GameState
    Assert-Test ($state.cellFormId -eq 0x2BE3A9) 'Pinned fixture is the QASmoke cell containing the screen' $state
    Assert-Test ('OSFUI_FocusMenu' -notin $state.menus) 'World screen does not own the overlay focus menu'
    if ($AimAtScreen) { Aim-WorldTestCamera }
    if ($Mode -eq 'Baseline') {
        $null = Save-Capture 'baseline-screen'
        Add-Evidence 'visual-review' 'Baseline fixture screenshot requires inspection' @{ expected = 'Visible checkerboard on the screen; ordinary world remains visible.' }
        return
    }
    if ($BudgetPressure) {
        if ($Screens -ne 4) { throw 'BudgetPressure requires four private screens.' }
        $records = @(Wait-Observed 'budget admits two outputs and defers two' { @(Read-WorldSnapshot -All) } {
            param($s) $s.Count -eq 4 -and @($s | Where-Object engineOwners -gt 0).Count -eq 2 -and @($s | Where-Object allocationDeferrals -gt 0).Count -eq 2
        } 45)
        $resident = ($records | Measure-Object residentBytes -Sum).Sum
        Assert-Test ($resident -le 8MB) 'Output allocation stays within the forced 8 MiB budget' $records
        Assert-Test (@($records | Where-Object { $_.allocationDeferrals -gt 0 -and ($_.engineOwners -ne 0 -or $_.boundDescriptors -ne 0) }).Count -eq 0) 'Budget-deferred assets retain their own placeholders without redirecting another feed' $records
        $null = Save-Capture 'named-assets-budget-pressure'
        return
    }
    $first = Wait-Observed 'world texture binding and first GPU copies' { Read-WorldSnapshot } {
        param($snapshot) $snapshot -and $snapshot.boundDescriptors -gt 0 -and $snapshot.completedFrames -gt 2 -and $snapshot.lastCompletedFrame -gt 0
    } 45
    if ($Screens -gt 1) {
        Invoke-MultipleWorldAssertions
        $first = Read-WorldSnapshot
    }
    $null = Save-Capture 'live-screen-a'
    $requiredFrames = if ($Screens -gt 1) { 10 } else { 30 }
    $second = Wait-Observed 'world texture continues receiving distinct browser frames' { Read-WorldSnapshot } {
        param($snapshot) $snapshot -and $snapshot.completedFrames -ge ($first.completedFrames + $requiredFrames) -and $snapshot.lastCompletedFrame -gt $first.lastCompletedFrame
    } 20
    $null = Save-Capture 'live-screen-b'
    Assert-Test ($second.boundDescriptors -eq $first.boundDescriptors) 'Bound material descriptors remain stable while browser frames advance' @{ first = $first; second = $second }
    $null = Invoke-Input 'focus-transfer'
    $null = Wait-Observed 'fresh game rendering after focus return' { Get-GameState } { param($s) $s.foreground -and -not $s.snapshotStale } 10
    $third = Wait-Observed 'world frame copies resume after focus return' { Read-WorldSnapshot } {
        param($snapshot) $snapshot -and $snapshot.completedFrames -gt $second.completedFrames -and $snapshot.lastCompletedFrame -gt $second.lastCompletedFrame
    } 20
    $null = Save-Capture 'live-screen-focus-return'
    Assert-Test (-not $third.gpuFailed -and $third.ringOpenFailures -eq 0) 'GPU copy completion reports no device failure or shared-ring open failure' $third
    Assert-Test ('OSFUI_FocusMenu' -notin (Get-GameState).menus) 'World-only view leaves gameplay input ownership available'
    if ($Overlay) {
        $null = Wait-Observed 'test HUD requested through the native API after loaded gameplay' {
            Get-Content -LiteralPath (Join-Path $script:Run.logs 'OSF UI.log') -Raw
        } { param($text) $text -match 'WorldSurface test: passive HUD requested after loaded QASmoke frames' } 15
        $helpers = @(Wait-Observed 'independent world and overlay browser helpers' { @(Get-WorldTestHelpers) } {
            param($found) $found -and @($found | Where-Object instance -ceq 'world-0').Count -eq 1 -and @($found | Where-Object instance -ceq '').Count -eq 1
        } 30)
        Assert-Test (@($helpers | Where-Object instance -ceq 'world-0').Count -eq 1 -and @($helpers | Where-Object instance -ceq '').Count -eq 1) 'World screen and passive HUD use independent verified browser helpers' $helpers
        $null = Wait-Observed 'ordinary overlay compositor draws beside the live world screen' {
            Get-Content -LiteralPath (Join-Path $script:Run.logs 'OSF UI.log') -Raw
        } { param($text) $text -match 'FIRST UI-PASS OVERLAY DRAW' } 15
        $null = Save-Capture 'live-screen-with-overlay'
    }
    if ($RestartBrowser) { Invoke-WorldBrowserRestart }
    $log = Get-Content -LiteralPath (Join-Path $script:Run.logs 'OSF UI.log') -Raw
    Assert-Test ($log -notmatch 'device removed|device lost|DXGI_ERROR_DEVICE|hook set incomplete|seam draw hook self-test FAILED') 'No render-hook or device-loss signature in the current OSF UI log'
    Add-Evidence 'visual-review' 'Live captures require inspection for animated content on the physical screen' @{ first = $first; second = $second; afterFocus = $third }
}

try {
    [IO.Directory]::CreateDirectory($script:StateRoot) | Out-Null
    try { $lock = [IO.File]::Open((Join-Path $script:StateRoot 'runner.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { throw '[blocked] Another harness runner owns the test session.' }
    $script:worldOwner = Read-JsonFile $ownerFile
    if ($Action -in @('Status', 'Capture', 'Stop')) {
        if (-not $script:worldOwner) { throw '[blocked] No retained world-surface test session.' }
        if ($script:worldOwner.source -ne $fixtureSource) { throw '[blocked] Another checkout owns this world-surface session.' }
        if ($script:worldOwner.ContainsKey('screens')) { $Screens = $script:worldOwner.screens }
        if ($script:worldOwner.ContainsKey('testModName')) { $TestModName = $script:worldOwner.testModName; $testMod = Join-Path $script:ModRoot $TestModName }
        $script:Run = $script:worldOwner.run
        if ($Action -eq 'Status') {
            @{ game = Get-GameState; world = @(Read-WorldSnapshot -All) } | ConvertTo-Json -Depth 20
            $preserveSession = $true
        } elseif ($Action -eq 'Capture') {
            if ($AimAtScreen) { Aim-WorldTestCamera }
            $null = Save-Capture $CaptureName
            $preserveSession = $true
        } else {
            $cleanupOwned = $true
            $priorOutcome = $script:Run.outcome
            $priorMessage = if ($script:Run.ContainsKey('message')) { $script:Run.message } else { '' }
            Stop-TestGame
            Restore-WorldTestProfile
            if ($priorOutcome -in @('failed', 'blocked')) {
                Finish-RunArtifacts $priorOutcome ($priorMessage + ' Retained session stopped and private profile restored.')
            } else {
                Finish-RunArtifacts 'passed' 'Retained world-screen session stopped and private profile restored.'
            }
        }
    } else {
        $null = Get-OwnedGame
        if (@(Get-GameProcess).Count) { throw '[blocked] Stop the existing owned game before a fresh world-screen run.' }
        $null = Start-RunArtifacts ('WorldSurface-' + $Mode)
        $script:worldOwner = Save-WorldTestProfile
        $cleanupOwned = $true
        Initialize-Harness
        Stage-WorldTest
        $script:worldOwner.run = $script:Run
        Write-JsonFile $ownerFile $script:worldOwner
        $null = Start-TestGame
        $preserveSession = $KeepGame -or $Action -eq 'Start'
        $modules = @((Get-OwnedGame).Modules | Where-Object ModuleName -match 'OSF|Luma|Character')
        Add-Evidence 'modules' 'Loaded native modules' @($modules | ForEach-Object { @{name=$_.ModuleName;path=$_.FileName} })
        Assert-Test ((@($modules | Where-Object ModuleName -eq 'OSFUI.dll').Count -gt 0) -eq ($Mode -eq 'Live')) 'OSF UI module presence matches the requested test mode'
        if ($Action -eq 'Run') { Invoke-WorldAssertions }
        if (-not $preserveSession) { Stop-TestGame; Restore-WorldTestProfile }
        Finish-RunArtifacts 'passed' $(if ($Action -eq 'Start') { 'Owned world-screen session ready for inspection.' } else { 'World-screen structured assertions completed; inspect the retained PNGs for visual acceptance.' })
        if ($preserveSession) {
            $script:worldOwner.run = $script:Run
            Write-JsonFile $ownerFile $script:worldOwner
        }
    }
} catch {
    $exitCode = 1
    $failure = $_
    try { if ($script:Run -and (Get-OwnedGame)) { $null = Save-Capture 'failure' } } catch { Write-Warning "Failure capture unavailable: $_" }
    if ($script:Run) {
        $outcome = if ($failure.Exception.Message.StartsWith('[failed]')) { 'failed' } else { 'blocked' }
        if ($Action -in @('Status', 'Capture')) {
            $name = 'inspection-failure-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '.json'
            Write-JsonFile (Join-Path $script:Run.path $name) @{ action = $Action; outcome = $outcome; error = $failure.Exception.Message }
        } else {
            Finish-RunArtifacts $outcome $failure.Exception.Message
        }
        if ($preserveSession -and $script:worldOwner) {
            $script:worldOwner.run = $script:Run
            Write-JsonFile $ownerFile $script:worldOwner
        }
    }
    Write-Error -Message $failure -ErrorAction Continue
} finally {
    if ($lock) {
        if ($cleanupOwned -and -not $preserveSession -and $script:worldOwner) {
            try { Stop-TestGame; Restore-WorldTestProfile }
            catch { $exitCode = 1; Write-Warning "World test cleanup incomplete; retain ownership file for -Action Stop: $_" }
        }
        $lock.Dispose()
    }
}
exit $exitCode
