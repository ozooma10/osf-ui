# The real host resolves this path through Windows' known-folder API. Lease it
# while all game hosts are stopped, and restore original cookies/history exactly.
function ConvertTo-AudioProfileHashManifest([string]$Json) {
    # Lease records may come from another PowerShell process, whose ordinary
    # hashtable property enumeration order is not stable. Compare file identities
    # and hashes after canonicalizing both old and newly generated manifests.
    $records=ConvertFrom-Json -InputObject $Json -AsHashtable -NoEnumerate -ErrorAction Stop
    if($records -isnot [array]){throw 'Audio profile hash manifest is not an array.'}
    $hashes=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    foreach($record in $records){
        if($record -isnot [Collections.IDictionary] -or $record.Count -ne 2 -or
            $record.path -isnot [string] -or [string]::IsNullOrEmpty($record.path) -or
            $record.sha256 -isnot [string] -or $record.sha256 -cnotmatch '^[0-9A-Fa-f]{64}$'){
            throw 'Audio profile hash manifest contains an invalid file record.'
        }
        if(-not $hashes.TryAdd($record.path,$record.sha256.ToUpperInvariant())){
            throw 'Audio profile hash manifest contains a duplicate path.'
        }
    }
    $paths=[string[]]@($hashes.Keys)
    [Array]::Sort($paths,[StringComparer]::Ordinal)
    $canonical=@(foreach($path in $paths){[ordered]@{path=$path;sha256=$hashes[$path]}})
    ConvertTo-Json -InputObject $canonical -Depth 4 -Compress
}
function Get-AudioProfileHashes([string]$Root) {
    $files=@(if(Test-Path -LiteralPath $Root){Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($Root,$_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    }})
    ConvertTo-AudioProfileHashManifest (ConvertTo-Json -InputObject $files -Depth 4 -Compress)
}
function Enter-AudioProfile {
    if(@(Get-Process Starfield,osfui_webview2_host -ErrorAction SilentlyContinue).Count){throw 'A live game/browser host blocks profile isolation.'}
    $root=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'OSFUI/browser/webview2'
    $profile=Assert-ChildPath $root (Join-Path $root 'surface-osfui.browser!tv')
    $backup=Assert-ChildPath $root (Join-Path $root ('surface-osfui.browser!tv.saved-'+$script:ProbeSession))
    if(Test-Path -LiteralPath $backup){throw 'Audio profile backup already exists.'}
    $lease=@{profile=$profile;backup=$backup;existed=(Test-Path -LiteralPath $profile);before=(Get-AudioProfileHashes $profile)}
    Write-JsonFile (Join-Path $script:Run.path 'browser-profile-lease.json') $lease
    if($lease.existed){Move-Item -LiteralPath $profile -Destination $backup}
    [IO.Directory]::CreateDirectory($profile)|Out-Null
    Write-JsonFile (Join-Path $profile '.osfui-audio-test.json') @{session=$script:ProbeSession}
}
function Exit-AudioProfile {
    $lease=Read-JsonFile (Join-Path $script:Run.path 'browser-profile-lease.json')
    if(-not $lease){return}
    if($lease -isnot [Collections.IDictionary]){throw 'Audio profile lease is not an object.'}
    if($lease.ContainsKey('restored') -and $lease['restored']){return}
    foreach($key in @('before','profile','backup','existed')){
        if(-not $lease.ContainsKey($key)){throw "Audio profile lease is missing required field '$key'."}
    }
    if($lease['existed'] -isnot [bool]){throw 'Audio profile lease existed field is not boolean.'}
    $expected=ConvertTo-AudioProfileHashManifest $lease.before
    if(@(Get-Process Starfield,osfui_webview2_host -ErrorAction SilentlyContinue).Count){throw 'Processes remain; leave private profile and original backup intact.'}
    $root=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'OSFUI/browser/webview2'
    $profile=Assert-ChildPath $root $lease.profile
    $backup=Assert-ChildPath $root $lease.backup
    $marker=Read-JsonFile (Join-Path $profile '.osfui-audio-test.json')
    if(-not $marker -or $marker -isnot [Collections.IDictionary] -or
        -not $marker.ContainsKey('session') -or $marker['session'] -ne $script:ProbeSession){throw 'Audio profile ownership marker mismatch.'}
    $destination=Assert-ChildPath $script:Run.path (Join-Path $script:Run.path 'browser-profile')
    if(Test-Path -LiteralPath $destination){throw 'Audio profile evidence destination already exists.'}
    Move-Item -LiteralPath $profile -Destination $destination
    if($lease.existed){Move-Item -LiteralPath $backup -Destination $profile}
    if((Get-AudioProfileHashes $profile) -cne $expected){throw 'Original audio profile hashes differ after restoration.'}
    $lease.restored=$true
    Write-JsonFile (Join-Path $script:Run.path 'browser-profile-lease.json') $lease
}
