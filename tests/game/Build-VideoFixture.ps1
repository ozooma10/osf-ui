[CmdletBinding()]
param([Parameter(Mandatory)][string]$FFmpeg, [string]$Output="$PSScriptRoot/../../build/cef-game/fixtures/seek.webm")
$ErrorActionPreference='Stop'
$Output=[IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory((Split-Path $Output -Parent))|Out-Null
# Moving background forces real decoding. A large color patch identifies time
# segments independently of currentTime; non-keyframe seeks exercise VP8 decode.
& $FFmpeg -hide_banner -y -f lavfi -i 'testsrc2=s=1280x720:r=30:d=20' -vf "drawbox=x=0:y=0:w=120:h=120:color=red:t=fill:enable='lt(t,6)',drawbox=x=0:y=0:w=120:h=120:color=lime:t=fill:enable='between(t,6,12)',drawbox=x=0:y=0:w=120:h=120:color=blue:t=fill:enable='gte(t,12)'" -c:v libvpx -b:v 4M -g 120 -an $Output
if($LASTEXITCODE){throw 'Video fixture encoding failed.'}
Get-FileHash -LiteralPath $Output
