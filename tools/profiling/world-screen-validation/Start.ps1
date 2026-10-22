[CmdletBinding()]
param(
    [string]$RepositoryRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..')),
    [string]$HarnessRoot='',
    [Parameter(Mandatory)][ValidateSet('webview2','cef')][string]$Backend,
    [Parameter(Mandatory)][string]$CoreData,
    [Parameter(Mandatory)][string]$ExtensionData,
    [ValidatePattern('^[a-z0-9-]+$')][string]$Label='current',
    [string]$PapyrusSource=''
)
$ErrorActionPreference='Stop'
$Interactive=$false
$Scenario='WorldScreenValidation'
$repo=[IO.Path]::GetFullPath($RepositoryRoot)
$harness=if($HarnessRoot){[IO.Path]::GetFullPath($HarnessRoot)}else{Join-Path (Split-Path $repo -Parent) 'OSF Test Harness'}
$core=[IO.Path]::GetFullPath($CoreData)
$extension=[IO.Path]::GetFullPath($ExtensionData)
$script:ValidationBackend=$Backend
$fixtureTools=Join-Path $repo 'tools/profiling/webview2-game-audio'
$imports=if($PapyrusSource){[IO.Path]::GetFullPath($PapyrusSource)}else{Join-Path (Split-Path $repo -Parent) 'PapyrusSource'}
. (Join-Path $harness 'scripts/Common.ps1')
. (Join-Path $harness 'scripts/Environment.ps1')
. (Join-Path $harness 'scripts/Reports.ps1')
. (Join-Path $PSScriptRoot 'Profile.ps1')
$lock=[IO.File]::Open((Join-Path $harness 'state/runner.lock'),'OpenOrCreate','ReadWrite','None')
try {
    if(Get-GameProcess){throw 'Starfield is already running.'}
    if(@(Get-Process osfui_webview2_host,osfui_cef_host -ErrorAction SilentlyContinue).Count){throw 'A browser host is still running; finish its profile lease before another test.'}
    foreach($file in @((Join-Path $core 'SFSE/Plugins/OSFUI.dll'),(Join-Path $extension 'SFSE/Plugins/OSFUIWorldScreens.dll'))){if(-not (Test-Path -LiteralPath $file)){throw "Missing candidate payload: $file"}}
    # Lease the backend selected by the actual DLL, not merely a caller label.
    # A wrong label could otherwise expose the other edition's real profile.
    & python (Join-Path $repo 'tests/package/verify_binary.py') $core --expected-backend $Backend
    if($LASTEXITCODE){throw 'Candidate backend or compiled API contract does not match the requested fixture.'}
    $moProcesses=@(Get-Process ModOrganizer -ErrorAction SilentlyContinue)
    foreach($mo in $moProcesses){if($mo.Path -ne (Join-Path $script:Config.MO2 'ModOrganizer.exe')){throw 'Unexpected MO2 process.'}}
    foreach($mo in $moProcesses){
        # Finish reopens Default asynchronously. A following comparison may
        # arrive before MO2 has a window to receive the normal close request.
        $deadline=[DateTime]::UtcNow.AddSeconds(15)
        while(-not $mo.HasExited -and $mo.MainWindowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline){
            Start-Sleep -Milliseconds 100;$mo.Refresh()
        }
        if($mo.HasExited){continue}
        if(-not $mo.CloseMainWindow()){throw 'MO2 has no ready window for normal close.'}
        if(-not $mo.WaitForExit(10000)){throw 'MO2 did not close normally.'}
    }
    $script:ArtifactRoot=Join-Path $repo 'build/world-screen-validation/runs'
    $null=Start-RunArtifacts ($Label+'-'+$Backend)
    $script:StateRoot=Join-Path $script:Run.path 'state'
    $script:ProbeDir=Join-Path $script:Run.path 'probe'
    $script:ProbeSession=[guid]::NewGuid().ToString('N')
    $script:Config.TestProfile='OSF World Screen Validation '+$script:Run.id
    $script:ProfileRoot=Join-Path $script:Config.MO2 ('profiles/'+$script:Config.TestProfile)
    $script:HarnessModName=$script:Config.TestProfile
    $script:AutoLoadModName=$script:HarnessModName
    $mod=Join-Path $script:ModRoot $script:HarnessModName
    if((Test-Path -LiteralPath $mod) -or (Test-Path -LiteralPath $script:ProfileRoot)){throw 'Fresh test paths already exist.'}
    $default=Join-Path $script:Config.MO2 'profiles/Default'
    $backup=Join-Path $script:Run.path 'default-before'
    New-TemporaryMO2Directory profiles $script:ProfileRoot
    New-TemporaryMO2Directory mods $mod
    New-Item -ItemType Directory -Path $backup,$script:ProbeDir -Force|Out-Null
    foreach($file in @('modlist.txt','plugins.txt','loadorder.txt')){
        Copy-Item -LiteralPath (Join-Path $default $file) -Destination $backup
    }
    foreach($file in Get-ChildItem -LiteralPath $default -File){Copy-Item -LiteralPath $file.FullName -Destination $script:ProfileRoot}
    $tvPlugin=Join-Path $script:ModRoot 'OSF Browser TV/OSFBrowserTV.esm'
    if(-not (Test-Path -LiteralPath $tvPlugin)){throw 'Installed Browser TV content plugin is missing.'}
    # The Default profile may intentionally leave this content disabled. Enable
    # only its private copy; loading a DLL does not make GetFormFromFile resolve
    # the ACTI if its ESM remains disabled.
    $pluginList=Join-Path $script:ProfileRoot 'plugins.txt'
    $plugins=@(Get-Content -LiteralPath $pluginList|Where-Object {$_ -notmatch '^\*?OSFBrowserTV\.esm$'})
    [IO.File]::WriteAllLines($pluginList,[string[]]($plugins+'*OSFBrowserTV.esm'))
    $orderPath=Join-Path $script:ProfileRoot 'loadorder.txt'
    $order=@(Get-Content -LiteralPath $orderPath)
    if('OSFBrowserTV.esm' -notin $order){[IO.File]::WriteAllLines($orderPath,[string[]]($order+'OSFBrowserTV.esm'))}
    if(@(Get-Content -LiteralPath $pluginList|Where-Object {$_ -ceq '*OSFBrowserTV.esm'}).Count -ne 1){throw 'Browser TV ESM was not enabled exactly once in the private profile.'}
    $mods=@(('+'+$script:HarnessModName),'+OSF Browser TV','+OSF UI - World Screens','+OSF UI','+OSF Settings',
        '+Address Library for SFSE Plugins','+Baka Achievement Enabler (SFSE)','+Trainwreck - A Crash Logger')
    [IO.File]::WriteAllLines((Join-Path $script:ProfileRoot 'modlist.txt'),[string[]]$mods)
    $script:SaveRoot=Join-Path $script:GameDocuments ('OSFTestingFixture/'+$script:Run.id)
    Write-JsonFile (Join-Path $script:SaveRoot '.osf-test-harness.json') @{harnessRoot=$harness}
    Write-JsonFile (Join-Path $script:ProfileRoot '.osf-test-harness.json') @{harnessRoot=$harness}
    foreach($ini in @('Starfield.ini','StarfieldCustom.ini')){
        $path=Join-Path $script:ProfileRoot $ini
        Set-IniValue $path General sLocalSavePath ([IO.Path]::GetRelativePath($script:GameDocuments,$script:SaveRoot)+'\')
        Set-IniValue $path General bAlwaysActive 1
        Set-IniValue $path General SIntroSequence 0
        Set-IniValue $path General uMainMenuDelayBeforeAllowSkip 0
        Set-IniValue $path Archive bInvalidateOlderFiles 1
        Set-IniValue $path Archive sResourceDataDirsFinal ''
        Set-IniValue $path Papyrus bEnableLogging 1
        Set-IniValue $path Papyrus bEnableTrace 1
    }
    foreach($key in @('bSaveOnPause','bSaveOnTravel','bSaveOnWait','bSaveOnRest')){
        Set-IniValue (Join-Path $script:ProfileRoot 'StarfieldPrefs.ini') GamePlay $key 0
    }
    Set-IniValue (Join-Path $script:ProfileRoot 'settings.ini') General LocalSaves false
    Set-IniValue (Join-Path $script:ProfileRoot 'settings.ini') General LocalSettings true
    Set-IniValue (Join-Path $script:ProfileRoot 'settings.ini') custom_overwrites ($script:Config.Executable.Replace(' ','%20')) $script:HarnessModName
    $script:FixtureRoot=Join-Path $script:Run.path 'fixture'
    Copy-Tree (Join-Path $harness 'fixtures/saves') (Join-Path $script:FixtureRoot 'saves')
    New-Item -ItemType Directory -Path (Join-Path $script:FixtureRoot 'settings-values') -Force|Out-Null
    Copy-Item (Join-Path $harness 'fixtures/fixture.json') $script:FixtureRoot
    Copy-Tree $core $mod
    Copy-Tree $extension $mod
    $native=Join-Path $mod 'SFSE/Plugins'
    $scripts=Join-Path $mod 'Scripts'
    New-Item -ItemType Directory -Path $native,$scripts -Force|Out-Null
    Copy-Item (Join-Path $harness 'build/bin/OSFTestHarness.dll') $native
    Copy-Item (Join-Path $repo 'build/windows/x64/releasedbg/OSFUIBackendTest.dll') $native
    Copy-Item (Join-Path $script:ModRoot 'OSF Testing - Auto Load/SFSE/Plugins/AutoStartSFSE.dll') $native
    $compiler=Join-Path $script:Config.Game 'Tools/Papyrus Compiler/PapyrusCompiler.exe'
    Push-Location $fixtureTools
    try {
        & $compiler 'OSFBrowserTVProbe.psc' "-i=$fixtureTools;$repo/data/Scripts/Source;$imports" "-o=$scripts" "-f=$imports/Starfield_Papyrus_Flags.flg"
        if($LASTEXITCODE){throw 'Private setup script did not compile.'}
    } finally {Pop-Location}
    $view=Join-Path $mod 'SFSE/Plugins/OSF/UI/views/osfui.browser/tv'
    if(-not (Test-Path -LiteralPath (Join-Path $view 'chrome.html'))){throw 'ExtensionData must include the complete reserved browser view.'}
    Copy-Item (Join-Path $fixtureTools 'observe.js') $view
    Copy-Item (Join-Path $repo 'tools/profiling/browser-audio/tone.js') $view
    Add-Content (Join-Path $view 'chrome.html') '<script type="module" src="./tone.js"></script>'
    Add-Content (Join-Path $view 'chrome.html') '<script type="module" src="./observe.js"></script>'
    # Map only the owned process's Settings folder before production Settings initializes.
    $private=Join-Path $script:Run.path 'private-values/settings'
    Write-JsonFile (Join-Path $private '.osf-ui-storage.json') @{session=$script:ProbeSession}
    Write-JsonFile (Join-Path $private 'osfui.json') @{formatVersion=1;values=@{developerMode=$true}}
    # Browsing consent is declared by the World Screens extension, so it stores under its own mod id.
    Write-JsonFile (Join-Path $private 'osfuiworldscreens.json') @{formatVersion=1;values=@{allowWebBrowsing=$true}}
    $probe=@{directory=$script:ProbeDir;session=$script:ProbeSession;realViews=@('osfui.browser/tv');
        storage=@{game=$script:Config.Game;mo2=$script:Config.MO2;vfsInstance='mod_organizer_instance';
            expiresUnixMs=([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()+180000);
            mappings=@(@{source=$private;destination=(Join-Path $script:GameDocuments 'OSF/Settings')})}}
    $probeConfig=Join-Path $native 'OSFUIBackendTest.json'
    Write-JsonFile $probeConfig $probe
    $payload=@(Get-ChildItem -LiteralPath $mod -Recurse -File | ForEach-Object {
        @{path=[IO.Path]::GetRelativePath($mod,$_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    Write-JsonFile (Join-Path $script:Run.path 'private-payload-sha256.json') $payload
    $context=@{run=$script:Run;harness=$harness;state=$script:StateRoot;probe=$script:ProbeDir;session=$script:ProbeSession;
        profile=$script:ProfileRoot;mod=$mod;save=$script:SaveRoot;fixture=$script:FixtureRoot;testProfile=$script:Config.TestProfile;
        interactive=$false;previousProfile='Default';repository=$repo;coreData=$core;extensionData=$extension;backend=$Backend;label=$Label}
    Write-JsonFile (Join-Path $script:Run.path 'context.json') $context
    Write-JsonFile (Join-Path $repo 'build/world-screen-validation/active.json') $context
    Enter-AudioProfile
    $controller=Join-Path $repo 'tests/game/private_storage.py'
    Start-Process (Get-Command python).Source -ArgumentList ('"{0}" "{1}"' -f $controller,$probeConfig) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $script:Run.path 'storage.out') -RedirectStandardError (Join-Path $script:Run.path 'storage.err')|Out-Null
    $state=Start-TestGame -RequiredTestMods @($script:HarnessModName,'OSF Browser TV','OSF UI - World Screens','OSF UI') -AllowProductionSettings
    $state|ConvertTo-Json -Depth 8
} catch {
    $failure=$_
    if($script:Session){try{Stop-TestGame}catch{Write-Warning $_}}
    if($script:Run){
        try{Exit-AudioProfile}catch{Write-Warning $_}
        try{Finish-RunArtifacts blocked $failure.Exception.Message}catch{Write-Warning $_}
    }
    throw $failure
} finally {$lock.Dispose()}
