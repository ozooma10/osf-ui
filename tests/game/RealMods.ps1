# Dot-sourced by Test-Backend. Original consumers are copied without modification.
function Initialize-RealMods([string]$Mod) {
    $script:RealModNames=@("X-2357's Settled Systems Stock Exchange","X-2357's Starcade OS",'AISS - AI Settled Systems','SomaticCameraSF')
    if($ResumeRun){
        $script:ResumeRun=Assert-ChildPath (Join-Path $repo 'build\backend-validation\game') ([IO.Path]::GetFullPath($ResumeRun))
        $previous=Read-JsonFile (Join-Path $ResumeRun 'result.json')
        if(-not $previous -or $previous.outcome -ne 'passed' -or -not(Test-Path (Join-Path $ResumeRun 'review-complete.json'))){throw 'Resume requires a completed private real-mod review.'}
        $previousIdentities=Read-JsonFile (Join-Path $ResumeRun 'real-mods.json')
    }
    $identities=@()
    foreach($name in $script:RealModNames){
        $source=Join-Path $script:ModRoot $name
        if($name -eq 'SomaticCameraSF' -and $SomaticModRoot){$source=[IO.Path]::GetFullPath($SomaticModRoot)}
        if(-not(Test-Path $source)){throw "[blocked] Missing original installed consumer: $source"}
        if((Test-Path (Join-Path $source 'SFSE\Plugins\OSFUI.dll')) -or (Test-Path (Join-Path $source 'SFSE\Plugins\OSFSettings.dll'))){throw 'Consumer must not replace framework binaries.'}
        $hashes=Get-StorageHashes $source
        if($ResumeRun){
            $prior=@($previousIdentities | Where-Object name -eq $name)
            if($prior.Count -ne 1 -or $hashes -cne (ConvertTo-Json -InputObject @($prior[0].files) -Depth 4 -Compress)){
                throw "Edition switch must use the same original consumer payload: $name"
            }
        }
        Copy-Tree $source $Mod
        foreach($file in ($hashes | ConvertFrom-Json)){
            if((Get-FileHash (Join-Path $Mod $file.path)).Hash -cne $file.sha256){throw "Consumer staging changed $name/$($file.path)"}
        }
        $identities+=@{name=$name;source=$source;files=($hashes | ConvertFrom-Json)}
    }
    Write-JsonFile (Join-Path $script:Run.path 'real-mods.json') $identities
    # Separate diagnostic document: copy the original stylesheet unchanged and
    # measure the author's hidden/display interaction in the actual helper.
    $layout=Join-Path $Mod 'SFSE\Plugins\OSF\UI\views\osfui-backend-test\aiss-layout'
    [IO.Directory]::CreateDirectory($layout)|Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'aiss-layout-probe.html') -Destination (Join-Path $layout 'index.html')
    $style=Join-Path $Mod 'SFSE\Plugins\OSFUI\views\x2357aiss.companionlog\companionlog\style.css'
    Copy-Item -LiteralPath $style -Destination $layout
    Write-JsonFile (Join-Path $layout 'manifest.json') @{manifestVersion=1;title='AISS stylesheet diagnosis';entry='index.html';kind='menu';width=1920;height=1080;capturesInput=$true;pausesGame=$false}
    Write-JsonFile (Join-Path $script:Run.path 'aiss-layout-source.json') @{original=(Get-FileHash $style).Hash;copy=(Get-FileHash (Join-Path $layout 'style.css')).Hash}
}
function Enable-RealModPlugins {
    # Somatic's invisible headgear lives in its ESM. Copying only its DLL and
    # leaving this inherited profile entry disabled makes camera review invalid.
    foreach($plugin in @('x2357ssse.esm','x2357starcade.esm','x2357aiss.esm','ImprovedCameraSF.esm')){
        if(-not(Test-Path -LiteralPath (Join-Path $mod $plugin))){throw "Missing staged original consumer plugin: $plugin"}
        foreach($name in @('plugins.txt','loadorder.txt')){
            $path=Join-Path $script:ProfileRoot $name
            $lines=@(Get-Content $path | Where-Object {$_.TrimStart('*') -ne $plugin})
            $lines+=if($name -eq 'plugins.txt'){'*'+$plugin}else{$plugin}
            [IO.File]::WriteAllLines($path,[string[]]$lines)
        }
    }
}
function Invoke-RealModReview {
    foreach($name in @('Starcade.dll','X2357AISSCompanionLog.dll','SomaticCameraSF.dll')){
        $loaded=@((Get-OwnedGame).Modules | Where-Object ModuleName -eq $name)
        Assert-Test ($loaded.Count -eq 1 -and (Get-FileHash $loaded[0].FileName).Hash -eq (Get-FileHash (Join-Path $mod "SFSE\Plugins\$name")).Hash) "Original $name loaded unchanged"
    }
    if($ResumeRun){
        Copy-Item -LiteralPath (Join-Path $ResumeRun 'OSFUIRealMods.sfs') -Destination $script:SaveRoot
        $generation=(Read-Consumer).loadGeneration
        $null=Invoke-ConsoleCommand 'load OSFUIRealMods'
        $null=Wait-Observed 'Prior edition load completed a new game-load event' {Read-Consumer} {param($s)$s -and $s.loadGeneration -gt $generation} 60
        $null=Wait-Observed 'Prior edition private save loaded' {Get-GameState} {param($s)$s.loaded -and -not $s.snapshotStale -and 'LoadingMenu' -notin $s.menus} 60
    }
    Write-JsonFile (Join-Path $script:Run.path 'review-ready.json') @{session=$script:ProbeSession;harnessRoot=$HarnessRoot;saveRoot=$script:SaveRoot;mod=$mod;resume=$ResumeRun}
    # An operator uses the owned harness while this loop sends no desktop input.
    # A result is never inferred from elapsed time or a native command receipt.
    $reviewResult=Wait-Observed 'Explicit capture-reviewed real-mod result' {Read-JsonFile (Join-Path $script:Run.path 'review-complete.json')} {param($r)$r -and $r.session -eq $script:ProbeSession -and $r.checked.Count -gt 0} $ReviewSeconds
    Assert-Test ($reviewResult.outcome -eq 'passed') 'Capture-reviewed real-mod workflows passed' $reviewResult
    foreach($capture in $reviewResult.captures){
        $path=Assert-ChildPath $script:Run.path (Join-Path $script:Run.path $capture.file)
        Assert-Test ((Get-FileHash -LiteralPath $path).Hash -eq $capture.sha256 -and -not [string]::IsNullOrWhiteSpace($capture.observation)) 'Reviewed capture matches retained evidence' $capture
    }
    Assert-Test ($reviewResult.captures.Count -gt 0) 'Real-mod review includes inspected captures'
    if($diagnosticComparison){throw '[failed] Diagnostic comparison only; this run cannot qualify a release or edition-switch baseline.'}
    $hosts=@(Get-TestHosts)
    Assert-Test ($hosts.Count -eq 1) 'Real mods use the selected edition helper'
    $expectedHost=if($Backend -eq 'cef'){'SFSE\Plugins\OSF\UI\bin\cef\osfui_cef_host.exe'}else{'SFSE\Plugins\OSF\UI\bin\osfui_webview2_host.exe'}
    Assert-Test ((Get-FileHash $hosts[0].Path).Hash -eq (Get-FileHash (Join-Path $data $expectedHost)).Hash) 'Real-mod helper matches the final archive'
    $saveStarted=[DateTime]::UtcNow
    $null=Invoke-ConsoleCommand 'save OSFUIRealMods'
    $save=Join-Path $script:SaveRoot 'OSFUIRealMods.sfs'
    $null=Wait-Observed 'Real-mod private save completed' {
        $file=Get-Item -LiteralPath $save -ErrorAction SilentlyContinue
        if($file){@{path=$file.FullName;length=$file.Length;modified=$file.LastWriteTimeUtc}}
    } {param($f)$f -and $f.length -gt 0 -and $f.modified -ge $saveStarted} 30
    Copy-Item -LiteralPath $save -Destination $script:Run.path
    $legacyValues=Join-Path $script:Run.path 'private-values\legacy'
    $savedValues=Join-Path $script:Run.path 'legacy-values'
    [IO.Directory]::CreateDirectory($savedValues)|Out-Null
    if(Test-Path $legacyValues){Copy-Tree $legacyValues $savedValues}
}
