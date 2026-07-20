# Credits & acknowledgments

## Prisma UI

**OSF UI** exists because of **[Prisma UI](https://www.prismaui.dev/)**,
the Skyrim Special Edition web-UI framework by **StarkMP**. Prisma UI pioneered the approach this project is built
around - rendering modern HTML/CSS/JS interfaces in game using the
Ultralight engine - and the entire idea for a Starfield equivalent came from it.

- Prisma UI on Nexus: <https://www.nexusmods.com/skyrimspecialedition/mods/148718>

**This project is an independent implementation for Starfield.**
Starfield is a different game on a different engine (Direct3D 12 vs Prisma's
Direct3D 11), so OSF UI has its own architecture, renderer, input
handling, and native<->web bridge. 

## Technology

- **[Microsoft Edge WebView2](https://developer.microsoft.com/microsoft-edge/webview2/)**
  — the Chromium-based renderer used by OSF UI.
- **[Ultralight](https://ultralig.ht/)** (Ultralight, Inc.) — the renderer used
  by Prisma UI and by OSF UI's initial release.
- **[CommonLibSF](https://github.com/Starfield-Reverse-Engineering/CommonLibSF)**
  and **[commonlibsf-template](https://github.com/libxse/commonlibsf-template)**
  — the plugin framework and project scaffold this is built on (GPL-3.0).
- **[SFSE - Starfield Script Extender](https://sfse.silverlock.org/)** - the
  runtime this plugin loads under.