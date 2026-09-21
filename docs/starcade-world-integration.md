# Starcade world-screen integration

Updated 2026-09-21 after inspecting the user-downloaded **Starcade OS 1.9.0
Complete Source**, copied and hash-verified at `C:\Modding\Starfield\Starcade`.
Its untouched baseline is commit `92f5dad` on `osf/starcade`. Earlier evaluation
of the older 1.7.3 snapshot is superseded by this source.

The world-view implementation and Starcade integration are on matching
`osf/world-cabinet` branches in their respective repositories.

## Recommended ownership

Reuse Starcade's existing browser game host inside a world-view presentation.
OSF UI owns the browser, texture transport, exclusive input session, and browser
recovery. Starcade owns game selection, scores, saves, credits, the physical
cabinet, activation eligibility, and any camera positioning.

The ordinary portable launcher now uses the OSF UI 2 Views SDK, current shared
JavaScript helper, retained profile state, current view layout/manifests, and
OSF Settings Slim hotkeys. Six Papyrus scripts build against current imports.
The native state format and normal save location are preserved.

Live migration checks and the repeatable harness live in the Starcade repository
under `tests/`. They use private browser storage and native state, an owned game
process, and the pinned disposable QASmoke save. See that repository's
`docs/osf-ui-2-migration.md` for final results and artifact paths.

## First cabinet prototype

The private Starcade cabinet scenario copies the same launcher controller and
game assets into a `kind: "world"` view, with a fixed Orbital Paddle (Pong) selection and
screen-sized presentation. It now uses Starcade's own cabinet NIF, seven
material groups, independent textures, and a full-UV 16:9 screen. A private
placement plugin puts the cabinet into QASmoke without vanilla texture overrides.
The test-only F8 action calls the Views service's new `BeginInteraction`; Escape
or controller Back returns gameplay control. Starcade's existing score/save
commands work from the world document as well as the portable document.

Starcade also starts an OSF Animation scene for the player at the cabinet. Its
standing-console reach is sourced from the game archive, with camera/equipment
cleanup owned by the scene. The animation's lifetime follows the UI interaction
token, including focus loss, browser failure, and portable-menu takeover. OSF UI
continues to own the game's input session; animation policy remains in Starcade.

The source game files are not forked. The prototype closes its iframe when the
session ends. It does not attempt to move a running browser game between the
portable screen and a cabinet, or resume a live game automatically after failure.
Native persisted state remains shared and is replayed to replacement documents.

The generic API is described in [world-surfaces.md](world-surfaces.md).
It supports one input owner, ownership tokens, bounded admission,
activation-release handling, and explicit cancellation. Menus and browser
failure revoke ownership; a recovered screen requires reactivation.

## Release work after the prototype

1. Add static collision to the authored cabinet. The independent mesh/material
   and full-screen UVs are complete under Starcade's `assets/cabinet` and `data`.
   Its BGRA8/1000 placeholder signature is reserved for this world view.
2. Wire the cabinet reference's activation to Starcade, with native/Papyrus
   distance, location, loading, and gameplay eligibility checks. The test F8
   shortcut deliberately supplies no production placement/interaction policy.
3. Expand from Orbital Paddle to the browser game catalog, with controls and
   presentation chosen for each game. Validate a physical controller separately.
4. Decide per-reference state and simultaneous-cabinet behavior before adding
   multiple independent stations. Today all references with one texture
   signature share a single browser, with at most four distinct world surfaces.

Starcade's OpenMW/Skyrim/Oblivion entries launch external desktop executables.
They do not render into the embedded browser and are outside this texture path.
The 32 embedded HTML/canvas/WebGL games are the appropriate first target.
