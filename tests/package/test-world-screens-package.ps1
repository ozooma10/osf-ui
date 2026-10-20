[CmdletBinding()]
param([Parameter(Mandatory)][string]$DataRoot)
$ErrorActionPreference = 'Stop'
$required = @(
    'SFSE/Plugins/OSFUIWorldScreens.dll',
    'Scripts/OSFUIWorldScreens.pex', 'Scripts/Source/OSFUIWorldScreens.psc',
    'SFSE/Plugins/OSF/WorldScreens/README.md',
    'SFSE/Plugins/OSF/WorldScreens/LICENSE', 'SFSE/Plugins/OSF/WorldScreens/EXCEPTIONS'
)
$allowed = $required + 'SFSE/Plugins/OSFUIWorldScreens.pdb'
foreach ($name in $required) {
    $file = Get-Item -LiteralPath (Join-Path $DataRoot $name)
    if (-not $file.Length) { throw "Empty extension payload: $name" }
}
foreach ($file in Get-ChildItem -LiteralPath $DataRoot -File -Recurse) {
    $relative = [IO.Path]::GetRelativePath($DataRoot, $file.FullName).Replace('\','/')
    if ($relative -notin $allowed) { throw "Unexpected file in optional extension: $relative" }
}
Write-Host 'World Screens package: required payload present; no core or browser files.'
