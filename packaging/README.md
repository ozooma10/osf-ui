# OSF UI — launch & branding kit

Marketing/branding assets for the OSF UI mod page. This is **presentation
material only** — the installable release archive is produced separately by
[`tools/package.ps1`](../tools/package.ps1) (see [docs/PACKAGING.md](../docs/PACKAGING.md)).

Same NASA-punk mission-patch family as **OSF Animation**, re-themed from that
project's amber + playback-curve identity to OSF UI's **teal/cyan + floating
web-UI-window** identity, so the two read as siblings.

## Contents

| File | Size | Use |
|---|---|---|
| `branding/osf-ui-patch.{svg,png}` | 1024×1024 | **Main / thumbnail image** on Nexus (the mission patch) |
| `branding/osf-ui-header.{svg,png}` | 1600×520 | Banner at the top of the description (gallery image) |
| `branding/osf-ui-emblem.{svg,png}` | 512×512 | Standalone mark, transparent — icons/avatars |
| `section-headers/svg/*.svg` | 1300×130 | Section dividers used inside the description |
| `section-headers/*.png` | 1300×130 | Rendered dividers to upload to the gallery |
| `section-headers/_html/*.html` | — | Transparent render sources for the PNGs above |
| `nexus-page.bbcode` | — | Paste-ready Nexus description (BBCode) |

The **SVGs are the source of truth**; the PNGs are rendered from them. Nexus
only serves raster images it hosts, so upload the PNGs and paste their
Nexus-hosted URLs into the `[img]` tags in `nexus-page.bbcode`.

## Re-rendering the PNGs

Rendered with headless Edge/Chrome (transparent background). From this folder:

```bash
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
render(){ "$EDGE" --headless --disable-gpu --force-device-scale-factor=1 \
  --default-background-color=00000000 --window-size=$3 \
  --screenshot="$(pwd -W)/$2" "$(pwd -W)/$1"; }

render branding/osf-ui-patch.svg   branding/osf-ui-patch.png   1024,1024
render branding/osf-ui-header.svg  branding/osf-ui-header.png  1600,520
render branding/osf-ui-emblem.svg  branding/osf-ui-emblem.png  512,512
for f in 01-overview 02-features 03-get-started 04-for-authors 05-credits; do
  render "section-headers/_html/$f.html" "section-headers/$f.png" 1300,130
done
```

Section headers render from `_html/` (SVG wrapped in a transparent-background
HTML page); the branding pieces render straight from the `.svg`.

## Palette

- Brushed steel wordmark: `#d4dae1` → `#828a93`
- Teal accent (primary): `#63d1d8` → `#38a8bf` → `#2b7fa2`
- Ring / mid accent: `#2f9fb4` · `#3aa9c0` · light `#6fd0da`
- HUD brackets / eyebrow: `#4d9aa8` · `#46c7c2`
- Background: `#0b0e12` · panels `#0d151e`
