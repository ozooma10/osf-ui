# Fullscreen surface check

These are manual fixtures, not extension payload. Use a disposable MO2 profile
and a separate mod that overrides Starcade's `starcade.arcade/screen-play` view:
copy its manifest, add `"fullscreen": true`, retain `"kind": "world"` and its
texture identity, use width 1200 and height 600, and copy `fullscreen.html` as
`index.html`. Compile `OSFFullscreenProbe.psc` against the extension's Papyrus
source and the vanilla sources, then deploy its PEX only to that temporary mod.
Restart Starfield after changing a loaded script.

In loaded gameplay, run:

```text
coc SettleRedMile02
```

Wait for the cell to load, then run:

```text
cgf "OSFFullscreenProbe.OpenRedMile"
```

Close the console and release the keys. The probe waits one second outside menu
mode before requesting fullscreen. Keep Starfield active: focus loss closes the
interaction, and an inactive game rejects admission.

Check black bars and the entire green border. Click the four corners and inspect
the reported target/coordinates, then check the link hand cursor and input text
cursor. Start the quiet tone, note the document ID/timer, and press Esc once.
Audio and animation should continue on the world screen. Reopen using the same
command and compare the ID, timer, click count and tone state.

The existing SFSE native console mailbox can dispatch these commands directly,
with gameplay/cell readback between them. A returned command handler alone is not
proof of fullscreen admission; inspect the game and compositor logs. Do not open
the regular Starcade Play overlay for this check, because it uses a different
view lifecycle.

Record results and remaining gaps in `../../docs/BROWSER_SURFACE.md`.
