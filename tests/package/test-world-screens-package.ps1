[CmdletBinding()]
param([Parameter(Mandatory)][string]$DataRoot)
$ErrorActionPreference = 'Stop'
$required = @(
    'SFSE/Plugins/OSFUIWorldScreens.dll',
    'Scripts/OSFUIWorldScreens.pex', 'Scripts/Source/OSFUIWorldScreens.psc',
    'SFSE/Plugins/OSF/WorldScreens/README.md',
    'SFSE/Plugins/OSF/WorldScreens/LICENSE', 'SFSE/Plugins/OSF/WorldScreens/EXCEPTIONS',
    'SFSE/Plugins/OSF/UI/views/osfui.browser/tv/manifest.json',
    'SFSE/Plugins/OSF/UI/views/osfui.browser/tv/chrome.html',
    'SFSE/Plugins/OSF/UI/views/osfui.browser/tv/chrome.css',
    'SFSE/Plugins/OSF/UI/views/osfui.browser/tv/chrome.js',
    'textures/osfui/feeds/a38d323ae4f0b194bbb4971eff370bd1/06b5e53e5a73da990f666b53e6057caf.dds',
    'textures/osfui/feeds/a38d323ae4f0b194bbb4971eff370bd1/normal.dds',
    'textures/osfui/feeds/a38d323ae4f0b194bbb4971eff370bd1/rough.dds',
    'textures/osfui/feeds/a38d323ae4f0b194bbb4971eff370bd1/black.dds',
    'textures/osfui/feeds/a38d323ae4f0b194bbb4971eff370bd1/white.dds',
    'materials/osfui/screens/display-screen5/v1/a38d323ae4f0b194bbb4971eff370bd1/06b5e53e5a73da990f666b53e6057caf.mat',
    'materials/osfui/screens/full-uv/v1/a38d323ae4f0b194bbb4971eff370bd1/06b5e53e5a73da990f666b53e6057caf.mat'
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
Write-Host 'World Screens package: extension and trusted browser view present; no core DLL or host runtime.'
