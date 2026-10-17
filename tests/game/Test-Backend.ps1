#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('webview2','cef')][string]$Backend,
    [ValidateSet('stock','luma')][string]$Graphics='stock',
    [switch]$Interactive,
    [ValidateRange(0,1800)][int]$SoakSeconds=120,
    [string]$Package,
    [string]$HarnessRoot='C:\Modding\Starfield\OSF Test Harness',
    [string]$SettingsTestDll='C:\Modding\Starfield\OSF Settings\build\windows\x64\releasedbg\OSFSettings.dll'
)
$ErrorActionPreference='Stop'
if(-not $Interactive){throw '[blocked] This suite tests keyboard/mouse paths; explicitly supply -Interactive.'}
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if(-not $Package){$Package=Join-Path $repo "dist\OSF-UI-v2.0.0-rc-$Backend.zip"}
$Scenario='OSFUIBackend'
. (Join-Path $HarnessRoot 'scripts\Common.ps1')
. (Join-Path $HarnessRoot 'scripts\Environment.ps1')
. (Join-Path $HarnessRoot 'scripts\Reports.ps1')
$script:ArtifactRoot=Join-Path $repo 'build\backend-validation\game'
$lock=$null; $outcome='blocked'; $message='Setup did not complete'; $exitCode=2
$script:PapyrusProbe=37

function Read-Consumer {
    $value=Read-JsonFile (Join-Path $script:ProbeDir 'consumer.json')
    if($value -and $value.session -eq $script:ProbeSession -and [Environment]::TickCount64-$value.tickMs -lt 3000){return $value}
}
function Read-Page([string]$View='panel') {
    $value=Read-Consumer
    if($value -and $value.pages.ContainsKey("osfui-backend-test/$View")){
        $page=$value.pages["osfui-backend-test/$View"]
        if([Environment]::TickCount64-$page.reportedTickMs -lt 3000){return $page}
    }
}
function Send-Probe([string]$Action,[hashtable]$Values=@{}) {
    $id=[guid]::NewGuid().ToString('N')
    $command=@{id=$id;session=$script:ProbeSession;action=$Action;deadline=([Environment]::TickCount64+10000)}
    foreach($key in $Values.Keys){$command[$key]=$Values[$key]}
    Write-JsonFile (Join-Path $script:ProbeDir 'command.json') $command
    $reply=Wait-Observed "Public API $Action returned" {Read-JsonFile (Join-Path $script:ProbeDir 'reply.json')} {param($r)$r -and $r.id -eq $id -and $r.session -eq $script:ProbeSession} 10
    Assert-Test $reply.accepted "Public API accepted $Action" $command
}
function Wait-Menu([bool]$Open) {
    $null=Wait-Observed "Game focus menu open=$Open" {Get-GameState} {param($s)-not $s.snapshotStale -and (('OSFUI_FocusMenu' -in $s.menus) -eq $Open)} 15
}
function Capture-Panel([string]$Name) {
    $path=Save-Capture $Name
    $output=& python (Join-Path $PSScriptRoot 'check_capture.py') $path
    Assert-Test ($LASTEXITCODE -eq 0) 'Actual game capture contains both browser markers' $output
}
function Open-Panel([string]$View='panel') {
    Send-Probe menu @{view="osfui-backend-test/$View";open=$true}
    $page=Wait-Observed "Page $View bridge completed" {Read-Page $View} {param($p)$p -and $p.complete} 45
    Assert-Test ($page.errors.Count -eq 0 -and $page.networkBlocked) "Page $View native replies/rejections and network boundary" $page
    if($View -ne 'hud'){Wait-Menu $true}
}
function Click-Element([string]$Id,[string[]]$Extra=@()) {
    $rect=(Read-Page).rects[$Id]; $window=Invoke-Input status
    $null=Invoke-Input click (@('--x',[string][int]($rect.x*$window.clientWidth),'--y',[string][int]($rect.y*$window.clientHeight))+$Extra)
}
function Get-TestHosts {
    if(-not $script:Session){return @()}
    $name=if($Backend -eq 'cef'){'osfui_cef_host.exe'}else{'osfui_webview2_host.exe'}
    return @(Get-CimInstance Win32_Process -Filter "Name='$name'" | Where-Object {
        $_.CommandLine -match ('--game-pid='+$script:Session.pid+'(?:\s|$)') -and $_.CommandLine -notmatch '--type='
    } | ForEach-Object {Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue})
}
function Check-Papyrus {
    $null=Invoke-ConsoleCommand 'cgf "OSFUIBackendPapyrus.Register"'
    $number=$script:PapyrusProbe++
    Send-Probe event @{name='papyrus';value=@{number=$number}}
    $null=Wait-Observed 'Actual Papyrus VM request and reply' {Read-Page} {param($p)$p -and $p.ContainsKey('papyrus') -and $p.papyrus -eq ($number+5)} 15
}
try {
    $lock=[IO.File]::Open((Join-Path $script:StateRoot 'runner.lock'),'OpenOrCreate','ReadWrite','None')
    if(@(Get-GameProcess).Count){throw '[blocked] Existing Starfield will not be interrupted.'}
    $null=Start-RunArtifacts "$Backend-$Graphics"
    $sources=@(Get-ChildItem $PSScriptRoot -File | ForEach-Object {@{name=$_.Name;sha256=(Get-FileHash $_.FullName).Hash}})
    Write-JsonFile (Join-Path $script:Run.path 'test-sources.json') $sources
    $script:ProbeSession=[guid]::NewGuid().ToString('N')
    $script:ProbeDir=Join-Path $script:Run.path 'probe'
    $script:StateRoot=Join-Path $script:Run.path 'state'
    $script:SaveRoot=Assert-ChildPath (Join-Path $script:GameDocuments 'OSFTestingFixture') (Join-Path $script:GameDocuments ('OSFTestingFixture\'+$script:Run.id))
    Write-JsonFile (Join-Path $script:SaveRoot '.osf-test-harness.json') @{harnessRoot=$script:HarnessRoot}
    $sourceProfile=Join-Path $script:Config.MO2 ('profiles\'+$script:Config.SourceProfile)
    $script:Config.TestProfile='OSF UI Backend '+$script:Run.id
    $script:ProfileRoot=Assert-ChildPath (Join-Path $script:Config.MO2 'profiles') (Join-Path $script:Config.MO2 ('profiles\'+$script:Config.TestProfile))
    if(Test-Path $script:ProfileRoot){throw 'Fresh private profile already exists.'}
    [IO.Directory]::CreateDirectory($script:ProfileRoot)|Out-Null
    foreach($file in Get-ChildItem -LiteralPath $sourceProfile -File){Copy-Item -LiteralPath $file.FullName -Destination $script:ProfileRoot}
    Write-JsonFile (Join-Path $script:ProfileRoot '.osf-test-harness.json') @{harnessRoot=$script:HarnessRoot;session=$script:ProbeSession}
    $script:HarnessModName=$script:Config.TestProfile; $script:AutoLoadModName=$script:HarnessModName
    $mod=Assert-ChildPath $script:ModRoot (Join-Path $script:ModRoot $script:HarnessModName)
    if(Test-Path $mod){throw 'Fresh private mod already exists.'}
    $expanded=Join-Path $script:Run.path 'package'
    Expand-Archive -LiteralPath $Package -DestinationPath $expanded
    $data=if(Test-Path (Join-Path $expanded 'Data')){Join-Path $expanded 'Data'}else{$expanded}
    & (Join-Path $repo 'tests\package\test-package.ps1') -DataRoot $data -ExpectedBackend $Backend
    Copy-Tree $data $mod
    Copy-Tree (Join-Path $script:ModRoot 'OSF Settings') $mod
    # Private values require the companion's test build; OSF UI itself is the exact packaged production DLL.
    $settingsBytes=[IO.File]::ReadAllBytes($SettingsTestDll)
    if(-not [Text.Encoding]::ASCII.GetString($settingsBytes).Contains('OSFSettingsTestHarness.json')){throw '[blocked] Settings candidate lacks private values support.'}
    Copy-Item -LiteralPath $SettingsTestDll -Destination (Join-Path $mod 'SFSE\Plugins\OSFSettings.dll') -Force
    Copy-Tree (Join-Path $script:ModRoot 'Auto Load') $mod
    foreach($file in @((Join-Path $HarnessRoot 'build\bin\OSFTestHarness.dll'),(Join-Path $repo 'build\windows\x64\releasedbg\OSFUIBackendTest.dll'))){Copy-Item -LiteralPath $file -Destination (Join-Path $mod 'SFSE\Plugins')}
    Copy-Item -LiteralPath (Join-Path $repo 'build\papyrus\OSFUIBackendPapyrus.pex') -Destination (Join-Path $mod 'Scripts')
    Write-JsonFile (Join-Path $mod 'SFSE\Plugins\OSFUIBackendTest.json') @{directory=$script:ProbeDir;session=$script:ProbeSession}
    Write-JsonFile (Join-Path $mod 'SFSE\Plugins\OSFSettingsTestHarness.json') @{valuesDir=(Join-Path $script:StateRoot 'settings-values');translationRegistration=$false}
    foreach($view in @('panel','paused','hud')) {
        $viewRoot=Join-Path $mod "SFSE\Plugins\OSF\UI\views\osfui-backend-test\$view"
        [IO.Directory]::CreateDirectory($viewRoot)|Out-Null
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'index.html'),(Join-Path $PSScriptRoot 'test.js') -Destination $viewRoot
        Write-JsonFile (Join-Path $viewRoot 'manifest.json') @{manifestVersion=1;title="Backend test $view";entry='index.html';kind=$(if($view -eq 'hud'){'hud'}else{'menu'});width=1920;height=1080;capturesInput=($view -ne 'hud');pausesGame=($view -eq 'paused');launcher=($view -eq 'panel')}
    }
    $legacyRoot=Join-Path $mod 'SFSE\Plugins\OSFUI\views\osfui-backend-test\legacy'
    [IO.Directory]::CreateDirectory($legacyRoot)|Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'legacy.html') -Destination (Join-Path $legacyRoot 'index.html')
    Write-JsonFile (Join-Path $legacyRoot 'manifest.json') @{title='Legacy backend acceptance';entry='index.html';width=1920;height=1080;capturesInput=$true;pausesGame=$false}
    # Retain fixture dependencies, disable all other frameworks and previous tests.
    $lines=@(Get-Content (Join-Path $sourceProfile 'modlist.txt') | ForEach-Object {
        $name=$_.TrimStart('+','-')
        # Release installations may have arbitrary MO2 folder names (for example
        # OSF-UI-v2.0.0 or OSFSettings). Disable competing payload owners too.
        $plugins=Join-Path $script:ModRoot "$name\SFSE\Plugins"
        $ownsFramework=(Test-Path -LiteralPath (Join-Path $plugins 'OSFUI.dll')) -or (Test-Path -LiteralPath (Join-Path $plugins 'OSFSettings.dll'))
        if($ownsFramework -or $name -match '^(OSF |Auto Load|CharacterStudio|CK Output)' -or $name -eq 'Undelayed Launching (Skip Intro Screens)'){'-'+$name}
        elseif($name -in @('Luma - Native HDR and more','Starfield Shader Injector and ReShade Helper')){$(if($Graphics -eq 'luma'){'+'}else{'-'})+$name}
        else{$_}
    })
    [IO.File]::WriteAllLines((Join-Path $script:ProfileRoot 'modlist.txt'),[string[]](@('+'+$script:HarnessModName)+$lines))
    foreach($file in @('plugins.txt','loadorder.txt')) {
        $path=Join-Path $script:ProfileRoot $file
        if(Test-Path $path){[IO.File]::WriteAllLines($path,[string[]]@(Get-Content $path | Where-Object {$_ -notmatch 'OSFSettingsAcceptance|OSFTest|OSFUIBench|OSFSettingsTest'}))}
    }
    foreach($ini in @('Starfield.ini','StarfieldCustom.ini')) {
        Set-IniValue (Join-Path $script:ProfileRoot $ini) 'General' 'sLocalSavePath' ([IO.Path]::GetRelativePath($script:GameDocuments,$script:SaveRoot)+'\')
        Set-IniValue (Join-Path $script:ProfileRoot $ini) 'General' 'bAlwaysActive' '1'
    }
    $settingsIni=Join-Path $script:ProfileRoot 'settings.ini'
    Set-IniValue $settingsIni 'General' 'LocalSaves' 'false'
    Set-IniValue $settingsIni 'General' 'LocalSettings' 'true'
    Set-IniValue $settingsIni 'custom_overwrites' ($script:Config.Executable.Replace(' ','%20')) $script:HarnessModName
    $prefs=Join-Path $script:ProfileRoot 'StarfieldPrefs.ini'
    Set-IniValue $prefs 'Display' 'uiFrameGenerationTech' '0'
    foreach($key in @('bSaveOnPause','bSaveOnTravel','bSaveOnWait','bSaveOnRest')){Set-IniValue $prefs 'GamePlay' $key '0'}
    if($Graphics -eq 'luma') {
        $luma=Join-Path $script:ModRoot 'Luma - Native HDR and more\SFSE\Plugins\Luma.toml'
        $text=(Get-Content $luma -Raw) -replace '(?m)^DisplayMode\s*=.*$','DisplayMode = 0' -replace '(?m)^EnforceUserDisplayMode\s*=.*$','EnforceUserDisplayMode = true'
        [IO.File]::WriteAllText((Join-Path $mod 'SFSE\Plugins\Luma.toml'),$text)
    }
    $seed=$script:FixtureRoot; $script:FixtureRoot=Join-Path $script:Run.path 'fixture'
    foreach($folder in @('saves','settings-values')){Copy-Tree (Join-Path $seed $folder) (Join-Path $script:FixtureRoot $folder)}
    Copy-Item -LiteralPath (Join-Path $seed 'fixture.json') -Destination $script:FixtureRoot
    Write-JsonFile (Join-Path $script:FixtureRoot 'settings-values\osfui.json') @{formatVersion=1;values=@{developerMode=$false;cefPrototype=($Backend -eq 'webview2')}}
    Write-JsonFile (Join-Path $script:Run.path 'candidate.json') @{sourceCommit=(& git -C $repo rev-parse HEAD);package=(Get-FileHash $Package);settingsTestDll=(Get-FileHash $SettingsTestDll);backend=$Backend;graphics=$Graphics}
    $null=Start-TestGame -RequiredTestMods @($script:HarnessModName) -RefocusAfterLoad
    $consumer=Wait-Observed 'Public SDK consumer initialized' {Read-Consumer} {param($c)$c -and $c.pid -eq $script:Session.pid -and $c.initialized -and $c.registered} 20
    $modules=@((Get-OwnedGame).Modules | ForEach-Object {@{name=$_.ModuleName;path=$_.FileName}})
    Write-JsonFile (Join-Path $script:Run.path 'modules.json') $modules
    Assert-Test ((@($modules | Where-Object name -eq 'Luma.dll').Count -gt 0) -eq ($Graphics -eq 'luma')) 'Selected graphics integration loaded'
    Open-Panel
    $hosts=@(Get-TestHosts); Assert-Test ($hosts.Count -eq 1) 'Package automatically selected expected browser helper'
    Add-Evidence 'browser-host' $Backend (Get-ProcessIdentity $hosts[0])
    $expectedHost=if($Backend -eq 'cef'){'SFSE\Plugins\OSF\UI\bin\cef\osfui_cef_host.exe'}else{'SFSE\Plugins\OSF\UI\bin\osfui_webview2_host.exe'}
    Assert-Test ((Get-FileHash $hosts[0].Path).Hash -eq (Get-FileHash (Join-Path $data $expectedHost)).Hash) 'Running helper matches package bytes'
    if($Backend -eq 'cef') {
        foreach($name in @('osfui_cef_host.dll','libcef.dll')) {
            $loaded=@($hosts[0].Modules | Where-Object ModuleName -eq $name)
            Assert-Test ($loaded.Count -eq 1 -and (Get-FileHash $loaded[0].FileName).Hash -eq (Get-FileHash (Join-Path $data "SFSE\Plugins\OSF\UI\bin\cef\$name")).Hash) "Loaded $name matches package bytes"
        }
    }
    Capture-Panel 'panel'
    Click-Element text
    Send-Key 65; Send-Key 66; Send-Key 8; Send-Key 67
    $null=Wait-Observed 'Keyboard text and Backspace reach browser' {Read-Page} {param($p)$p -and $p.text -eq 'ac'} 10
    Send-Key 9; Send-Key 32
    $null=Wait-Observed 'Tab and Space activate button' {Read-Page} {param($p)$p -and $p.clicks -ge 1} 10
    Click-Element button
    $null=Wait-Observed 'Mouse click reaches browser' {Read-Page} {param($p)$p -and $p.clicks -ge 2} 10
    Click-Element select
    Start-Sleep -Milliseconds 300
    Capture-Panel 'select-popup'
    Send-Key 40; Send-Key 13
    $null=Wait-Observed 'Native select popup commits selection' {Read-Page} {param($p)$p -and $p.ContainsKey('selection') -and $p.selection -eq 'two'} 10
    Click-Element scroll
    $null=Invoke-Input wheel @('--dy','-120')
    $null=Wait-Observed 'Mouse wheel reaches browser' {Read-Page} {param($p)$p -and $p.wheel -gt 0} 10
    Click-Element drag @('--dx','100','--dy','35','--hold-ms','500')
    $null=Wait-Observed 'Relative pointer begins, moves, and releases' {Read-Consumer} {param($c)$c.pointer.ContainsKey('2') -and $c.pointer['2'] -gt 0 -and $c.pointer.distance -gt 0} 10
    $null=Invoke-Input focus-cycle
    Click-Element text; Send-Key 68
    $null=Wait-Observed 'Typing resumes after focus cycle' {Read-Page} {param($p)$p -and $p.text -eq 'acd'} 10
    $capturedBefore=Get-GameState
    Send-Key 87 400
    $capturedAfter=Get-GameState
    Assert-Test (([Math]::Abs($capturedAfter.position.x-$capturedBefore.position.x)+[Math]::Abs($capturedAfter.position.y-$capturedBefore.position.y)) -lt 0.01) 'Captured keyboard input does not move the player'
    Send-Probe state @{value=@{revision=7}}
    $null=Wait-Observed 'Retained native state reaches page' {Read-Page} {param($p)$p -and $p.state -and $p.state.revision -eq 7} 10
    Send-Probe event @{name='probe';value=@{number=42}}
    $null=Wait-Observed 'Native event reaches page' {Read-Page} {param($p)$p -and @($p.events | Where-Object number -eq 42).Count -eq 1} 10
    Check-Papyrus
    $old=(Read-Page).document; Send-Probe event @{name='reload';value=@{}}
    $null=Wait-Observed 'Fresh document replays retained state' {Read-Page} {param($p)$p -and $p.document -ne $old -and $p.complete -and $p.state -and $p.state.revision -eq 7} 20
    Capture-Panel 'input-and-reload'
    Send-Key 27; Wait-Menu $false
    Assert-Test ((Read-Consumer).legacyVersion -eq 0x10008) 'Frozen native ABI 1.8 acquired'
    Send-Probe legacyMenu @{open=$true}
    $legacyPage=Wait-Observed 'Legacy document typed native reply' {Read-Page legacy} {param($p)$p -and $p.complete} 20
    Assert-Test ($legacyPage.errors.Count -eq 0) 'Legacy helper readiness and native request/reply'
    Wait-Menu $true
    Send-Probe legacyState @{value=@{revision=8}}
    Send-Probe legacyEvent @{value=@{number=99}}
    $null=Wait-Observed 'Legacy retained state and event delivery' {Read-Page legacy} {param($p)$p -and $p.state -and $p.state.revision -eq 8 -and @($p.events | Where-Object number -eq 99).Count -eq 1} 10
    Capture-Panel 'legacy'
    Send-Probe legacyMenu @{open=$false}; Wait-Menu $false
    for($i=0;$i -lt 8;$i++){Open-Panel;Send-Probe menu @{open=$false};Wait-Menu $false}
    Open-Panel hud
    Wait-Menu $false
    $before=Get-GameState; Send-Key 87 400
    $null=Wait-Observed 'HUD permits gameplay movement' {Get-GameState} {param($s)[Math]::Abs($s.position.x-$before.position.x)+[Math]::Abs($s.position.y-$before.position.y) -gt 0.1} 10
    Capture-Panel 'hud'
    $pauseBefore=(Get-GameState).pauseRequestCount
    Open-Panel paused
    Assert-Test ((Get-GameState).pauseRequestCount -gt $pauseBefore) 'Paused menu owns an engine pause request'
    Capture-Panel 'paused-menu'
    Send-Probe menu @{view='osfui-backend-test/paused';open=$false}; Wait-Menu $false
    Assert-Test ((Get-GameState).pauseRequestCount -eq $pauseBefore) 'Closing paused menu releases engine pause request'
    Send-Probe menu @{view='osfui-backend-test/hud';open=$false}
    $null=Invoke-ConsoleCommand 'save OSFUIBackendTest'
    $save=Wait-Observed 'Disposable save completed' {
        $file=Get-ChildItem $script:SaveRoot -Filter 'OSFUIBackendTest.sfs' | Select-Object -First 1
        if($file){@{path=$file.FullName;length=$file.Length}}
    } {param($s)$s -and $s.length -gt 0} 20
    Open-Panel
    $null=Invoke-ConsoleCommand 'load OSFUIBackendTest'
    $null=Wait-Observed 'Load releases browser focus' {Get-GameState} {param($s)$s.loaded -and -not $s.snapshotStale -and 'LoadingMenu' -notin $s.menus -and 'OSFUI_FocusMenu' -notin $s.menus} 60
    Open-Panel; Check-Papyrus
    Capture-Panel 'after-load'
    $old=(Read-Page).document; $hosts=@(Get-TestHosts)
    Assert-Test ($hosts.Count -eq 1) 'Exactly one owned host before fault injection'
    $identity=Get-ProcessIdentity $hosts[0]; Add-Evidence 'fault' 'Terminate only owned browser helper' $identity
    Stop-Process -InputObject $hosts[0] -Force
    $null=Wait-Observed 'Browser recovery creates a new ready document' {Read-Page} {param($p)$p -and $p.document -ne $old -and $p.complete} 60
    Open-Panel; Capture-Panel 'after-host-recovery'
    $timer=[Diagnostics.Stopwatch]::StartNew(); $first=(Read-Page).frame
    while($timer.Elapsed.TotalSeconds -lt $SoakSeconds) {
        Start-Sleep -Seconds 5
        $page=Read-Page; Assert-Test ($page -and $page.errors.Count -eq 0 -and $page.frame -gt $first) 'Browser animation and bridge remain live during soak'
        $first=$page.frame
    }
    Send-Probe menu @{open=$false};Wait-Menu $false
    Send-Key 27
    $null=Wait-Observed 'Vanilla pause opens after browser closes' {Get-GameState} {param($s)'PauseMenu' -in $s.menus} 10
    Send-Key 27
    $null=Save-Capture 'final-gameplay'
    Write-JsonFile (Join-Path $script:Run.path 'consumer-final.json') (Read-Consumer)
    $log=Get-Content (Join-Path $script:Run.logs 'OSF UI.log') -Raw
    Assert-Test ($log -notmatch 'device removed|device lost|DXGI_ERROR_DEVICE|hook set incomplete|self-test FAILED') 'No device loss or hook failure'
    $outcome='passed';$message='Production package passed browser, native/Papyrus bridge, input, lifecycle and recovery scenarios.';$exitCode=0
} catch {
    $message=$_.Exception.Message
    if($message.StartsWith('[failed]')){$outcome='failed';$exitCode=1}
    Write-Warning $_.ScriptStackTrace
    try {if($script:Session){$null=Save-Capture 'failure'}}catch{Write-Warning "Failure capture: $_"}
} finally {
    try {if($script:Session){
        $finalHosts=@(Get-TestHosts)
        foreach($hostProcess in $finalHosts){$null=$hostProcess.Handle}
        $game=Get-OwnedGame
        if($game){$null=$game.CloseMainWindow();if(-not $game.WaitForExit(20000)){Stop-TestGame}}
        foreach($hostProcess in $finalHosts){
            Assert-Test ($hostProcess.WaitForExit(15000) -and $hostProcess.ExitCode -eq 0) 'Browser helper exits cleanly when the game closes'
        }
    }}catch{Write-Warning "Cleanup: $_";$message+="; cleanup: $_";$outcome='blocked';$exitCode=2}
    if($script:Run){Finish-RunArtifacts $outcome $message}
    if($lock){$lock.Dispose()}
}
exit $exitCode
