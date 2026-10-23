#requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../../tools/WorkflowCache.ps1')
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('osf-workflow-cache-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
function Check($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
try {
    $source = Join-Path $scratch 'source.txt'; $output = Join-Path $scratch 'output.txt'; $stamp = Join-Path $scratch 'pass.json'
    Set-Content -LiteralPath $source -Value 'one'
    $script:runs = 0
    $argsForCache = @{ Stamp=$stamp; Fingerprint={ Get-WorkflowFingerprint -Paths @($source) }
        Outputs={ @($output) }; Action={ $script:runs++; Set-Content -LiteralPath $output -Value 'result' } }
    $first = Invoke-WorkflowCached @argsForCache
    $null = Invoke-WorkflowCached @argsForCache
    Check ($script:runs -eq 1) 'Unchanged work ran twice'
    $mtime = (Get-Item -LiteralPath $source).LastWriteTimeUtc
    Set-Content -LiteralPath $source -Value 'two'
    (Get-Item -LiteralPath $source).LastWriteTimeUtc = $mtime
    $null = Invoke-WorkflowCached @argsForCache
    Check ($script:runs -eq 2) 'Same-size input edit with preserved timestamp was missed'
    Set-Content -LiteralPath $output -Value 'edited'
    $null = Invoke-WorkflowCached @argsForCache
    Check ($script:runs -eq 3) 'Corrupt output was reused'
    Remove-Item -LiteralPath $output
    $null = Invoke-WorkflowCached @argsForCache
    Check ($script:runs -eq 4) 'Missing output was reused'
    $null = Invoke-WorkflowCached @argsForCache -Force
    Check ($script:runs -eq 5) 'Force did not rerun work'
    Set-Content -LiteralPath $stamp -Value '{bad json'
    $null = Invoke-WorkflowCached @argsForCache
    Check ($script:runs -eq 6) 'Corrupt receipt was reused'
    $argsForCache.Action = { throw 'expected failure' }
    try { $null = Invoke-WorkflowCached @argsForCache -Force; throw 'Failure was swallowed' }
    catch { Check ($_.Exception.Message -eq 'expected failure') 'Unexpected failure' }
    Check (-not (Test-Path -LiteralPath $stamp)) 'Failed attempt retained a successful receipt'
    $argsForCache.Action = { Set-Content -LiteralPath $source -Value 'changed during action' }
    try { $null = Invoke-WorkflowCached @argsForCache; throw 'Mutation was accepted' }
    catch { Check ($_.Exception.Message -like 'Workflow inputs changed*') 'Mid-action source mutation was not rejected' }
    Check (-not (Test-Path -LiteralPath $stamp)) 'Mutated inputs received a cached pass'
    $tree = Join-Path $scratch 'tree'; [IO.Directory]::CreateDirectory($tree) | Out-Null
    $before = Get-WorkflowFingerprint -Paths @($tree)
    Set-Content -LiteralPath (Join-Path $tree 'added.txt') -Value 'new dependency'
    Check ($before -cne (Get-WorkflowFingerprint -Paths @($tree))) 'Added dependency was missed'
    # The helper must not shadow a caller variable referenced by its scriptblock.
    $lock = @{ value='caller' }
    $argsForCache.Fingerprint = { Get-WorkflowFingerprint -Values @($lock.value) }
    $argsForCache.Action = { Set-Content -LiteralPath $output -Value 'result' }
    $null = Invoke-WorkflowCached @argsForCache
    Write-Host 'PASS workflow cache: warm reuse, input/output edits, missing files, force, corrupt receipts, failure, mutation, additions and caller scope.'
} finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    if (-not $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) { throw 'Scratch path escaped temp directory.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
