# Background-only production CEF acceptance. Uses public SDK events and game
# captures; does not enable debugging or modify the shipped helper or game DLL.
$script:CefProfileLease=$null
function Initialize-RuntimeFixtures([string]$Mod) {
    foreach($view in @('runtime-main','runtime-second')) {
        $root=Join-Path $Mod "SFSE/Plugins/OSF/UI/views/osfui-backend-test/$view"
        [IO.Directory]::CreateDirectory($root)|Out-Null
        foreach($entry in @('index.html','slow.html','missed.html')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime.html') -Destination (Join-Path $root $entry)}
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime.js') -Destination $root
        Copy-Item -LiteralPath $VideoFixture -Destination (Join-Path $root 'seek.webm')
        Write-JsonFile (Join-Path $root 'manifest.json') @{manifestVersion=1;entry='index.html';kind='hud';width=1920;height=1080;capturesInput=$false;pausesGame=$false;openOnStart=$false;order=$(if($view -eq 'runtime-main'){10}else{20})}
    }
    Write-JsonFile (Join-Path $script:Run.path 'video-fixture.json') (Get-FileHash -LiteralPath $VideoFixture)
}
function Enter-PrivateCefProfile {
    if(@(Get-Process Starfield,osfui_cef_host -ErrorAction SilentlyContinue).Count){throw '[blocked] Browser profile isolation requires all game and CEF processes stopped.'}
    $root=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'OSFUI'
    $profile=Assert-ChildPath $root (Join-Path $root 'CEF')
    $backup=Assert-ChildPath $root (Join-Path $root ('CEF.saved-'+$script:ProbeSession))
    if(Test-Path -LiteralPath $backup){throw 'CEF backup path already exists.'}
    $existed=Test-Path -LiteralPath $profile
    $script:CefProfileLease=@{profile=$profile;backup=$backup;existed=$existed;before=(Get-StorageHashes $profile)}
    Write-JsonFile (Join-Path $script:Run.path 'browser-profile-lease.json') $script:CefProfileLease
    if($existed){Move-Item -LiteralPath $profile -Destination $backup}
    [IO.Directory]::CreateDirectory($profile)|Out-Null
    Write-JsonFile (Join-Path $profile '.osfui-runtime-test.json') @{session=$script:ProbeSession}
}
function Exit-PrivateCefProfile {
    if(-not $script:CefProfileLease){return}
    if(@(Get-Process Starfield,osfui_cef_host -ErrorAction SilentlyContinue).Count){throw 'Processes remain; private profile retained and original backup preserved. Restore only after they exit.'}
    $lease=$script:CefProfileLease
    $marker=Read-JsonFile (Join-Path $lease.profile '.osfui-runtime-test.json')
    if(-not $marker -or $marker.session -ne $script:ProbeSession){throw 'Private CEF profile ownership marker mismatch.'}
    $destination=Assert-ChildPath $script:Run.path (Join-Path $script:Run.path 'browser-profile')
    $profile=Assert-ChildPath (Split-Path $lease.profile -Parent) $lease.profile
    if(Test-Path -LiteralPath $destination){throw 'Browser evidence destination already exists.'}
    Move-Item -LiteralPath $profile -Destination $destination
    if($lease.existed){Move-Item -LiteralPath $lease.backup -Destination $profile}
    Assert-Test ((Get-StorageHashes $profile) -ceq $lease.before) 'Original CEF profile restored byte-for-byte'
    $script:CefProfileLease=$null
}
function Runtime-Command([string]$Test,[hashtable]$Values=@{},[string]$View='runtime-main') {
    $id=[guid]::NewGuid().ToString('N');$Values.test=$Test;$Values.id=$id
    Send-Probe event @{view="osfui-backend-test/$View";name='runtime';value=$Values}
    if($Test -eq 'navigate'){return}
    $page=Wait-Observed "Page runtime $Test completed" {Read-Page $View} {param($p)$p -and $p.results.ContainsKey($id)} 45
    $result=$page.results[$id]
    Add-Evidence 'runtime' $Test $result
    Assert-Test $result.passed "Page runtime $Test passed" $result
    return $result
}
function Open-RuntimeView([string]$View) {
    Send-Probe menu @{view="osfui-backend-test/$View";open=$true}
    $null=Wait-Observed "$View document ready" {Read-Page $View} {param($p)$p -and $p.complete} 30
}
function Capture-RuntimeProgress([string]$Phase) {
    $observed=@()
    for($i=0;$i -lt 2;$i++){
        $path=Save-Capture "runtime-$Phase-$i"
        $raw=& python (Join-Path $PSScriptRoot 'check_runtime_capture.py') $path
        Assert-Test ($LASTEXITCODE -eq 0) "Healthy HUD pixels present during $Phase" $raw
        $observed+=($raw|ConvertFrom-Json -AsHashtable)
        Start-Sleep -Milliseconds 250
    }
    Assert-Test ($observed[0].frame -ne $observed[1].frame) "Healthy HUD delivers distinct game frames during $Phase" $observed
}
function Get-OwnedRenderers {
    $hosts=@(Get-TestHosts)
    if($hosts.Count -ne 1){throw 'Expected one owned CEF host.'}
    @(Get-CimInstance Win32_Process -Filter "Name='osfui_cef_host.exe'" | Where-Object {$_.ParentProcessId -eq $hosts[0].Id -and $_.CommandLine -match '--type=renderer(?:\s|$)'})
}
function Close-RuntimeGame([string]$Phase) {
    $hosts=@(Get-TestHosts);Assert-Test ($hosts.Count -eq 1) 'One owned helper at normal game shutdown'
    foreach($p in $hosts){$null=$p.Handle}
    $game=Get-OwnedGame;$null=$game.Handle;$timer=[Diagnostics.Stopwatch]::StartNew()
    Assert-Test ($game.CloseMainWindow() -and $game.WaitForExit(20000)) 'Owned game exits normally'
    foreach($p in $hosts){Assert-Test ($p.WaitForExit(6000) -and $p.ExitCode -eq 0) 'CEF helper exits normally after game closure'}
    Add-Evidence 'runtime-shutdown' $Phase @{gamePid=$game.Id;helperPid=$hosts[0].Id;helperExit=$hosts[0].ExitCode;elapsedMs=$timer.ElapsedMilliseconds}
    $logs=Join-Path $script:Run.path "$Phase-logs";[IO.Directory]::CreateDirectory($logs)|Out-Null
    foreach($name in @('OSF UI.log','osfui-cef-host.log')){if(Test-Path (Join-Path $script:Run.logs $name)){Copy-Item -LiteralPath (Join-Path $script:Run.logs $name) -Destination $logs}}
}
function Invoke-CefGameRuntimeChecks {
    Open-RuntimeView runtime-main
    $hosts=@(Get-TestHosts);Assert-Test ($hosts.Count -eq 1) 'Production CEF helper selected in Starfield'
    foreach($name in @('osfui_cef_host.dll','libcef.dll')) {
        $loaded=@($hosts[0].Modules|Where-Object ModuleName -eq $name)
        Assert-Test ($loaded.Count -eq 1 -and (Get-FileHash $loaded[0].FileName).Hash -eq (Get-FileHash (Join-Path $data "SFSE/Plugins/OSF/UI/bin/cef/$name")).Hash) "Loaded $name matches tested package"
    }
    $gpu=Runtime-Command gpu
    $gpuProcess=@(Get-CimInstance Win32_Process -Filter "Name='osfui_cef_host.exe'"|Where-Object {$_.ParentProcessId -eq $hosts[0].Id -and $_.CommandLine -match '--type=gpu-process'})
    Assert-Test ($gpuProcess.Count -eq 1 -and $gpuProcess[0].CommandLine -match '--use-adapter-luid=([^\s]+)') 'Chromium GPU process carries the game adapter LUID'
    Add-Evidence 'gpu-process' 'CEF GPU selection' $gpuProcess[0].CommandLine
    Send-Probe graphics
    $native=Wait-Observed 'Game D3D12 adapter LUID read directly' {Read-Consumer} {param($c)$c -and $c.ContainsKey('graphics')} 5
    Assert-Test ($gpu.renderer -match 'ANGLE.*Direct3D11' -and $gpu.renderer -notmatch 'SwiftShader|Basic Render') 'CEF uses hardware ANGLE rendering' $gpu
    $high=$native.graphics.adapterLuidHigh;$low=$native.graphics.adapterLuidLow
    Assert-Test ($gpuProcess[0].CommandLine.Contains("--use-adapter-luid=$high,$low")) 'Chromium LUID equals the game-selected shared-frame LUID'
    Capture-RuntimeProgress baseline
    $null=Runtime-Command media
    Capture-RuntimeProgress video-seeked
    $mainDocument=(Read-Page runtime-main).document
    Open-RuntimeView runtime-second
    Runtime-Command navigate @{entry='slow.html'} runtime-second
    $null=Wait-Observed 'Sibling entered parser stall' {Read-Page runtime-second} {param($p)$p -and $p.ContainsKey('phase') -and $p.phase -eq 'slow-start'} 10
    # Chromium may prelaunch spare renderers and replace a renderer on failed
    # navigation. Identify the process executing our known parser CPU loop,
    # instead of guessing from process count or an earlier navigation's PID.
    $samples=@(Get-OwnedRenderers|ForEach-Object {
        $p=Get-Process -Id $_.ProcessId
        @{process=$p;identity=(Get-ProcessIdentity $p);cpu=$p.TotalProcessorTime.TotalMilliseconds}
    })
    Start-Sleep -Milliseconds 1000
    $busy=@($samples|Where-Object {
        $_.process.Refresh()
        $_.delta=$_.process.TotalProcessorTime.TotalMilliseconds-$_.cpu
        $_.delta -gt 600
    })
    Add-Evidence 'renderer-identification' 'Known sibling parser CPU loop' @($samples|ForEach-Object {@{identity=$_.identity;cpuDeltaMs=$_.delta}})
    Assert-Test ($busy.Count -eq 1) 'Exactly one owned renderer executes the sibling parser loop'
    $sibling=$busy[0].process;$siblingIdentity=$busy[0].identity
    Capture-RuntimeProgress slow-load
    $page=Wait-Observed 'Sibling parser resumed' {Read-Page runtime-second} {param($p)$p -and $p.frame -gt 0} 20
    $oldDocument=$page.document
    $current=@(Get-OwnedRenderers|Where-Object ProcessId -eq $sibling.Id)
    Assert-Test ($current.Count -eq 1 -and (Get-ProcessIdentity (Get-Process -Id $sibling.Id)).processStartFileTime -eq $siblingIdentity.processStartFileTime) 'Fault target is still the owned sibling renderer'
    Add-Evidence 'fault' 'Terminate only sibling CEF renderer' $siblingIdentity
    Stop-Process -InputObject $sibling -Force
    $null=Wait-Observed 'Sibling renderer termination reported' {Get-Content (Join-Path $script:Run.logs 'OSF UI.log') -Raw} {param($s)$s -match "view 'osfui-backend-test/runtime-second' FAILED to load.*CEF renderer terminated"} 10
    Capture-RuntimeProgress renderer-crash
    Assert-Test ((Read-Page runtime-main).document -eq $mainDocument) 'Healthy HUD document survives sibling renderer crash'
    $null=Wait-Observed 'Sibling automatically recovers after renderer crash' {Read-Page runtime-second} {param($p)$p -and $p.document -ne $oldDocument -and $p.frame -gt 0} 30
    Capture-RuntimeProgress recovered
    Runtime-Command navigate @{entry='missing.html'} runtime-second
    $null=Wait-Observed 'Sibling failed navigation observed by game' {Get-Content (Join-Path $script:Run.logs 'OSF UI.log') -Raw} {param($s)$s -match "view 'osfui-backend-test/runtime-second' FAILED to load \(https://osfui.example/osfui-backend-test/runtime-second/missing.html"} 10
    Capture-RuntimeProgress failed-load
    $null=Wait-Observed 'Game automatically retries failed sibling' {Read-Page runtime-second} {param($p)$p -and $p.path.EndsWith('/index.html')} 30
    Runtime-Command navigate @{entry='missed.html'} runtime-second
    $null=Wait-Observed 'Sibling loaded without reveal callback' {Read-Page runtime-second} {param($p)$p -and $p.path.EndsWith('/missed.html') -and $p.frame -eq 0} 10
    Capture-RuntimeProgress missed-reveal
    Runtime-Command navigate @{entry='index.html'} runtime-second
    $page=Wait-Observed 'Sibling reveal restored' {Read-Page runtime-second} {param($p)$p -and $p.path.EndsWith('/index.html') -and $p.frame -gt 0} 15
    $script:RuntimeStorageToken=[guid]::NewGuid().ToString('N')
    $null=Runtime-Command storage-write @{token=$script:RuntimeStorageToken}
    Write-JsonFile (Join-Path $script:Run.path 'runtime-write-consumer.json') (Read-Consumer)
    Close-RuntimeGame storage-write
    foreach($name in @('storage-ready.json','storage-mapped.json','storage-verified.json','consumer.json')){
        $file=Assert-ChildPath $script:ProbeDir (Join-Path $script:ProbeDir $name)
        if(Test-Path $file){Remove-Item -LiteralPath $file}
    }
    $probeConfig.storage.expiresUnixMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()+180000
    Write-JsonFile $probeConfigPath $probeConfig
    $script:storageController=Start-Process -FilePath (Get-Command python).Source -ArgumentList $controllerArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $script:Run.path 'storage-restart.out') -RedirectStandardError (Join-Path $script:Run.path 'storage-restart.err')
    $null=Start-TestGame -RequiredTestMods @($script:HarnessModName)
    $null=Wait-Observed 'Fresh game consumer ready' {Read-Consumer} {param($c)$c -and $c.pid -eq $script:Session.pid -and $c.initialized -and $c.registered} 20
    Open-RuntimeView runtime-main
    $null=Runtime-Command storage-read @{token=$script:RuntimeStorageToken}
    Capture-RuntimeProgress fresh-game-storage
    Write-JsonFile (Join-Path $script:Run.path 'runtime-read-consumer.json') (Read-Consumer)
}
