#requires -Version 7.2
param(
    [string]$HarnessRoot='C:\Modding\Starfield\OSF Test Harness',
    [ValidateSet('webview2','cef')][string]$Backend='webview2',
    [string]$CoreData,
    [switch]$Interactive
)
$ErrorActionPreference='Stop'
$Scenario='WorldScreenAddons'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if(-not $CoreData){$CoreData=Join-Path $repo "build/package/staging/$Backend/Data"}
& (Join-Path $repo 'tests/package/test-package.ps1') -DataRoot $CoreData -ExpectedBackend $Backend
. (Join-Path $HarnessRoot 'scripts/Common.ps1')
. (Join-Path $HarnessRoot 'scripts/Environment.ps1')
. (Join-Path $HarnessRoot 'scripts/Reports.ps1')
$lock=[IO.File]::Open((Join-Path $script:StateRoot 'runner.lock'),'OpenOrCreate','ReadWrite','None')
try {
    if(@(Get-GameProcess).Count){throw 'An existing Starfield session blocks this test.'}
    $script:ArtifactRoot=Join-Path $repo 'build/world-addon-migration/game'
    $null=Start-RunArtifacts "addons-$Backend"
    $script:StateRoot=Join-Path $script:Run.path 'state'
    $script:ProbeDir=Join-Path $script:Run.path 'probe'
    $script:ProbeSession=[guid]::NewGuid().ToString('N')
    [IO.Directory]::CreateDirectory($script:ProbeDir)|Out-Null
    $source=Join-Path $script:Config.MO2 ('profiles/'+$script:Config.SourceProfile)
    $script:Config.TestProfile='OSF World Screens '+$script:Run.id
    $script:ProfileRoot=Assert-ChildPath (Join-Path $script:Config.MO2 'profiles') (Join-Path $script:Config.MO2 ('profiles/'+$script:Config.TestProfile))
    $script:HarnessModName=$script:Config.TestProfile; $script:AutoLoadModName=$script:HarnessModName
    $mod=Assert-ChildPath $script:ModRoot (Join-Path $script:ModRoot $script:HarnessModName)
    if((Test-Path $mod) -or (Test-Path $script:ProfileRoot)){throw 'Fresh test paths already exist.'}
    New-TemporaryMO2Directory profiles $script:ProfileRoot
    New-TemporaryMO2Directory mods $mod
    foreach($file in Get-ChildItem -LiteralPath $source -File){Copy-Item -LiteralPath $file.FullName -Destination $script:ProfileRoot}
    Write-JsonFile (Join-Path $script:ProfileRoot '.osf-test-harness.json') @{harnessRoot=$script:HarnessRoot;session=$script:ProbeSession}
    # Exact generated payloads; never copy the obsolete combined prototype DLL.
    foreach($payload in @(
        $CoreData,
        (Join-Path $script:ModRoot 'OSF UI - World Screens'),
        'C:\Modding\Starfield\X2357 Settled Systems Stock Exchange\Data',
        'C:\Modding\Starfield\ARCHIVE\Starcade\build\world-screens\Data',
        'C:\Modding\Starfield\SettledSystemsExchange\Build\market-display\Data',
        (Join-Path $script:ModRoot 'OSF Settings'),(Join-Path $script:ModRoot 'Auto Load'))){Copy-Tree $payload $mod}
    foreach($dll in @((Join-Path $HarnessRoot 'build/bin/OSFTestHarness.dll'),(Join-Path $repo 'build/windows/x64/releasedbg/OSFUIBackendTest.dll'))){Copy-Item $dll (Join-Path $mod 'SFSE/Plugins')}
    $compiler=Join-Path $script:Config.Game 'Tools/Papyrus Compiler/PapyrusCompiler.exe'
    Push-Location $PSScriptRoot
    try {
        foreach($probeSource in @('OSFWorldScreenProbe.psc','OSFWorldWorkshopProbe.psc')){
            & $compiler $probeSource "-i=$PSScriptRoot;C:\Modding\Starfield\ARCHIVE\Starcade\Web-and-Papyrus\Source;C:\Modding\Starfield\PapyrusSource" "-o=$(Join-Path $mod 'Scripts')" '-f=C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg'
            if($LASTEXITCODE){throw 'Private Papyrus probe compilation failed.'}
        }
    } finally {Pop-Location}
    # Observe DOM and real bridge replies only in this private copy of the addons.
    $views=Join-Path $mod 'SFSE/Plugins/OSF/UI/views'
    foreach($manifest in Get-ChildItem $views -Filter manifest.json -File -Recurse){
        $info=Get-Content $manifest.FullName -Raw|ConvertFrom-Json -AsHashtable
        if(-not $info.ContainsKey('kind') -or $info.kind -ne 'world'){continue}
        Copy-Item (Join-Path $PSScriptRoot 'observe.js') (Join-Path $manifest.DirectoryName 'world-test-observe.js')
        Add-Content (Join-Path $manifest.DirectoryName 'index.html') '<script type="module" src="./world-test-observe.js"></script>'
    }
    $lines=@(Get-Content (Join-Path $source 'modlist.txt') | ForEach-Object {
        $name=$_.TrimStart('+','-'); $plugins=Join-Path $script:ModRoot "$name/SFSE/Plugins"
        $owner=@('OSFUI.dll','OSFUIWorldScreens.dll','OSFSettings.dll','Starcade.dll') | Where-Object {Test-Path (Join-Path $plugins $_)}
        if($owner -or $name -match '^(OSF |Auto Load|CharacterStudio|CK Output|Luma |Starfield Shader Injector)' -or $name -eq 'Undelayed Launching (Skip Intro Screens)'){'-'+$name}else{$_}
    })
    [IO.File]::WriteAllLines((Join-Path $script:ProfileRoot 'modlist.txt'),[string[]](@('+'+$script:HarnessModName)+$lines))
    $esm=@('x2357starcade.esm','x2357ssse.esm','x2357starcade-world.esm','x2357ssse-displays.esm','x2357ssse-key-screens.esm')
    foreach($name in @('plugins.txt','loadorder.txt')){
        $file=Join-Path $script:ProfileRoot $name
        $old=@(Get-Content $file | Where-Object { $_ -notmatch 'OSFSettingsAcceptance|OSFTest|OSFUIBench|OSFSettingsTest|x2357starcade|x2357ssse'})
        $new=@($esm|ForEach-Object {if($name -eq 'plugins.txt'){'*'+$_}else{$_}})
        [IO.File]::WriteAllLines($file,[string[]]($old+$new))
    }
    $script:SaveRoot=Assert-ChildPath (Join-Path $script:GameDocuments 'OSFTestingFixture') (Join-Path $script:GameDocuments ('OSFTestingFixture/'+$script:Run.id))
    Write-JsonFile (Join-Path $script:SaveRoot '.osf-test-harness.json') @{harnessRoot=$script:HarnessRoot}
    foreach($name in @('Starfield.ini','StarfieldCustom.ini')){
        $ini=Join-Path $script:ProfileRoot $name
        Set-IniValue $ini General sLocalSavePath ([IO.Path]::GetRelativePath($script:GameDocuments,$script:SaveRoot)+'\')
        Set-IniValue $ini General bAlwaysActive 1
        Set-IniValue $ini Papyrus bEnableLogging 1
        Set-IniValue $ini Papyrus bEnableTrace 1
    }
    Set-IniValue (Join-Path $script:ProfileRoot 'StarfieldPrefs.ini') Display uiFrameGenerationTech 0
    foreach($key in @('bSaveOnPause','bSaveOnTravel','bSaveOnWait','bSaveOnRest')){Set-IniValue (Join-Path $script:ProfileRoot 'StarfieldPrefs.ini') GamePlay $key 0}
    $ini=Join-Path $script:ProfileRoot 'settings.ini'
    Set-IniValue $ini General LocalSaves false; Set-IniValue $ini General LocalSettings true
    Set-IniValue $ini custom_overwrites ($script:Config.Executable.Replace(' ','%20')) $script:HarnessModName
    $seed=$script:FixtureRoot; $script:FixtureRoot=Join-Path $script:Run.path 'fixture'
    foreach($folder in @('saves','settings-values')){Copy-Tree (Join-Path $seed $folder) (Join-Path $script:FixtureRoot $folder)}
    Copy-Item (Join-Path $seed 'fixture.json') $script:FixtureRoot
    $mappings=@()
    foreach($entry in @(@{name='settings';path=(Join-Path $script:GameDocuments 'OSF/Settings')},@{name='starcade';path=(Join-Path $script:GameDocuments 'Starcade')},@{name='legacy';path=(Join-Path $script:Config.Game 'Data/SFSE/Plugins/OSFUI/settings/values')})){
        $private=Join-Path $script:Run.path ('private-values/'+$entry.name)
        [IO.Directory]::CreateDirectory($private)|Out-Null
        if(Test-Path $entry.path){Copy-Tree $entry.path $private}
        Write-JsonFile (Join-Path $private '.osf-ui-storage.json') @{session=$script:ProbeSession}
        $mappings+=@{source=$private;destination=$entry.path}
    }
    $probe=@{directory=$script:ProbeDir;session=$script:ProbeSession;storage=@{game=$script:Config.Game;mo2=$script:Config.MO2;vfsInstance='mod_organizer_instance';expiresUnixMs=([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()+180000);mappings=$mappings}}
    $probeConfigPath=Join-Path $mod 'SFSE/Plugins/OSFUIBackendTest.json';Write-JsonFile $probeConfigPath $probe
    $controller=Join-Path $repo 'tests/game/private_storage.py'
    $process=Start-Process (Get-Command python).Source -ArgumentList ('"{0}" "{1}"' -f $controller,$probeConfigPath) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $script:Run.path 'storage.out') -RedirectStandardError (Join-Path $script:Run.path 'storage.err')
    $context=@{run=$script:Run;harness=$HarnessRoot;state=$script:StateRoot;probe=$script:ProbeDir;session=$script:ProbeSession;profile=$script:ProfileRoot;mod=$mod;save=$script:SaveRoot;fixture=$script:FixtureRoot;storageProcess=$process.Id;testProfile=$script:Config.TestProfile;interactive=[bool]$Interactive;backend=$Backend;coreData=$CoreData}
    Write-JsonFile (Join-Path $script:Run.path 'context.json') $context
    Write-JsonFile (Join-Path $repo 'build/world-addon-migration/active.json') $context
    $null=Start-TestGame -RequiredTestMods @($script:HarnessModName)
    $null=Wait-Observed 'Extension acquired and ready' {Read-JsonFile (Join-Path $script:ProbeDir 'consumer.json')} {param($c)$c -and $c.screensReady} 20
    Write-JsonFile (Join-Path $script:Run.path 'modules.json') @((Get-OwnedGame).Modules|ForEach-Object {@{name=$_.ModuleName;path=$_.FileName}})
    Write-Host "Owned world-screen test ready: $($script:Run.path)"
} catch {
    if($script:Session){try{Stop-TestGame}catch{Write-Warning $_}}
    Finish-RunArtifacts 'blocked' $_.Exception.Message
    throw
} finally {$lock.Dispose()}
