# Content-addressed local workflow evidence. Never caches a failed action.
function Get-WorkflowFiles([string[]]$Paths) {
    $unique = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($candidate in $Paths) {
        $fullPath = [IO.Path]::GetFullPath($candidate)
        if ([IO.Directory]::Exists($fullPath)) {
            foreach ($file in [IO.Directory]::EnumerateFiles($fullPath, '*', [IO.SearchOption]::AllDirectories)) {
                $null = $unique.Add($file)
            }
        } else { $null = $unique.Add($fullPath) }
    }
    [string[]]$sorted = @($unique)
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    $sorted
}

function Get-WorkflowFingerprint([string[]]$Paths = @(), [string[]]$Values = @()) {
    $parts = [Collections.Generic.List[string]]::new()
    $parts.AddRange([string[]]@('workflow-cache-v1', $PSVersionTable.PSVersion.ToString(), [Environment]::OSVersion.VersionString))
    foreach ($path in @(Get-WorkflowFiles $Paths)) {
        $hash = 'missing'
        if ([IO.File]::Exists($path)) {
            $stream = [IO.File]::OpenRead($path)
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
            finally { $stream.Dispose() }
        }
        $parts.Add("$path`0$hash")
    }
    $parts.AddRange($Values)
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $parts.ToArray() -Compress))))
}

function Read-WorkflowCache([string]$Stamp, [string]$InputHash, [string[]]$Outputs = @()) {
    try {
        if (-not (Test-Path -LiteralPath $Stamp -PathType Leaf)) { return $null }
        $receipt = Get-Content -LiteralPath $Stamp -Raw | ConvertFrom-Json -AsHashtable
        if ($receipt.version -ne 1 -or $receipt.inputHash -cne $InputHash) { return $null }
        if (-not $receipt.ContainsKey('completedUtc') -or -not $receipt.ContainsKey('outputs') -or
            (@($receipt.outputs) -join "`0") -cne ($Outputs -join "`0")) { return $null }
        foreach ($path in $Outputs) {
            if (-not [IO.File]::Exists($path)) { return $null }
        }
        if ($receipt.outputHash -cne (Get-WorkflowFingerprint -Paths $Outputs)) { return $null }
        return $receipt
    } catch { return $null } # Missing, corrupt or partially written evidence is a miss.
}

function Invoke-WorkflowCachedUnlocked {
    param([string]$Stamp, [scriptblock]$Fingerprint, [scriptblock]$Outputs = { @() },
        [scriptblock]$Action, [switch]$Force)
    $inputHash = & $Fingerprint
    $receipt = if (-not $Force) { Read-WorkflowCache $Stamp $inputHash @(& $Outputs) }
    if ($receipt) {
        Write-Host "Reusing verified inputs from $($receipt.completedUtc): $Stamp"
        return $receipt
    }
    # Invalidate before attempting work: a failure must not resurrect an older pass.
    if (Test-Path -LiteralPath $Stamp) { Remove-Item -LiteralPath $Stamp -Force }
    & $Action | Out-Host
    if ((& $Fingerprint) -cne $inputHash) { throw 'Workflow inputs changed during execution; no cached pass was recorded.' }
    $files = @(& $Outputs)
    foreach ($path in $files) {
        if (-not [IO.File]::Exists($path)) { throw "Missing workflow output: $path" }
    }
    $receipt = [ordered]@{version=1; inputHash=$inputHash; outputHash=(Get-WorkflowFingerprint -Paths $files)
        completedUtc=[DateTime]::UtcNow.ToString('o'); outputs=$files}
    [IO.Directory]::CreateDirectory((Split-Path $Stamp -Parent)) | Out-Null
    $temporary = "$Stamp.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
        Move-Item -LiteralPath $temporary -Destination $Stamp -Force
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
    return $receipt
}

function Invoke-WorkflowCached {
    param([string]$Stamp, [scriptblock]$Fingerprint, [scriptblock]$Outputs = { @() },
        [scriptblock]$Action, [switch]$Force)
    [IO.Directory]::CreateDirectory((Split-Path $Stamp -Parent)) | Out-Null
    $workflowCacheLock = [IO.File]::Open("$Stamp.lock", 'OpenOrCreate', 'ReadWrite', 'None')
    try { Invoke-WorkflowCachedUnlocked @PSBoundParameters }
    finally { $workflowCacheLock.Dispose() }
}
