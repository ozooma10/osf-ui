# Credits & acknowledgments

## The idea: Prisma UI

**StarfieldWebUI** exists because of **[Prisma UI](https://www.prismaui.dev/)**,
the Skyrim Special Edition web-UI framework by **StarkMP** (with contributors
including **langfod**). Prisma UI pioneered the approach this project is built
around — rendering modern HTML/CSS/JS interfaces over a Bethesda game using the
Ultralight engine — and the entire idea for a Starfield equivalent came from it.

StarkMP graciously gave permission to reference Prisma UI's branding and public
API. Thank you.

- Prisma UI on Nexus: <https://www.nexusmods.com/skyrimspecialedition/mods/148718>
- Prisma UI docs: <https://www.prismaui.dev/>
- Prisma UI source: <https://github.com/PrismaUI-SKSE>

**This project is an independent, from-scratch implementation for Starfield.**
Starfield is a different game on a different engine (Direct3D 12 vs Prisma's
Direct3D 11), so StarfieldWebUI has its own architecture, renderer, input
handling, and native↔web bridge. It is **not affiliated with or endorsed by the
Prisma UI project, and contains no Prisma UI code.**

## Technology

- **[Ultralight](https://ultralig.ht/)** (Ultralight, Inc.) — the lightweight,
  WebKit-based HTML renderer behind every view. Used under the Ultralight Free
  License Agreement; the full notices ship in
  `StarfieldWebUI/ultralight/license/`.
- **[CommonLibSF](https://github.com/Starfield-Reverse-Engineering/CommonLibSF)**
  and **[commonlibsf-template](https://github.com/libxse/commonlibsf-template)**
  — the plugin framework and project scaffold this is built on (GPL-3.0).
- **[SFSE — Starfield Script Extender](https://sfse.silverlock.org/)** — the
  runtime this plugin loads under.

## Short blurb (for the Nexus / mod page)

> Inspired by **Prisma UI** by StarkMP — the Skyrim web-UI framework that
> pioneered rendering HTML/CSS/JS over a Bethesda game with Ultralight.
> StarfieldWebUI is an independent, from-scratch Starfield implementation of
> that idea, used with StarkMP's kind permission. Not affiliated with or
> endorsed by Prisma UI, and contains no Prisma UI code. Rendering by
> Ultralight.
