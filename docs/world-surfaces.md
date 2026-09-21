# Web views on world materials

A `kind: "world"` view displays an opaque browser image through a material on
3D geometry. It starts passive and does not open a fullscreen menu or pause the
game. A native consumer can explicitly acquire keyboard/controller interaction.
Direct pointer interaction with a screen's UV coordinates is not implemented.

Place the page and manifest in the ordinary view discovery directory:
`Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-name>/`.

```json
{
  "manifestVersion": 1,
  "title": "Ship display",
  "kind": "world",
  "entry": "index.html",
  "width": 1600,
  "height": 900,
  "placeholderSize": 1000
}
```

`width` and `height` are the browser resolution, each an integer in 1..4096.
They default to 1600x900. `placeholderSize` is required and identifies the exact
square texture dimensions used by the material. It must be 256..4096 and not a
power of two. Use a distinct placeholder size for each independently rendered
view; the runtime admits at most four world views and rejects duplicate
signatures. References using the same placeholder share the same browser image.

`transparent`, `capturesInput`, `pausesGame`, `openOnStart`, and `order` do not
control world presentation. The runtime forces the view opaque and passive and
starts its dedicated browser only after a matching material texture is seen.
`debugOnly` retains the normal restart-latched developer-mode requirement.

The texture must load as a plain sampled 2D BGRA8 typeless resource, one mip,
one array slice, one sample, and no render-target, depth, or UAV capability.
These checks prevent an engine render target from matching a world surface.
The binding uses this signature rather than a texture filename or reference
FormID. Avoid giving any unrelated texture the same signature.

At SRV creation, OSF UI redirects the matching material to a stable texture it
owns. Completed WebView shared-ring frames are copied there on the engine's
DIRECT queue; producer slots are acknowledged through GPU fence completion.
The browser's sRGB bytes are sampled through an sRGB view. A browser restart
retains the last completed image while a replacement ring is opened.
Ordering is established on that DIRECT queue; material sampling from a separate
asynchronous compute queue has not been validated.

Host/page failure starts at most three automatic restart attempts with bounded
response deadlines. During recovery, native `SendToWeb` events queue until the replacement
document greets the bridge, and old deferred requests are discarded. Exhausted
recovery requires a game restart. There is currently no distance-based suspension
or per-reference lifecycle: a browser that has started remains resident for the
game process.

The physical object, UV mapping, material, and texture paths remain the mod's
responsibility. A useful screen material has readable emission, high roughness,
a flat normal, and full 0..1 UVs on a surface matching the browser aspect ratio.
New Starfield material resource graphs need independent asset-only validation:
historical copied or edited `.mat` files broke ordinary world rendering even
with the plugin disabled.

## Keyboard and controller interaction

The Views service ABI 1.1 appends `BeginInteraction(view, callback, context)` and
`EndInteraction(token)`. Existing 1.0 consumers remain supported. Use
`Client::Has(kInteractionVersion)` to check availability; the Client wrappers
fail safely on older providers.

Begin returns a nonzero queued token, or zero if the request cannot be queued.
The callback receives either `kStarted` followed by `kEnded`, or `kRejected`.
Callbacks run on the game main thread; keep the context alive until the terminal
callback. There is a bounded request queue and a five-second admission deadline.
The page must already be loaded and have completed a GPU frame. An OSF UI menu,
the console, another interaction, unavailable input integration, or a failed
renderer rejects admission. The consumer must also check its gameplay/menu
eligibility before requesting interaction.

Admission waits for keyboard/mouse buttons and XInput buttons to be released and
for two neutral game ticks before moving focus. This lets Starfield observe the
activation key's release, avoiding a stuck held-key state on the next activation.
The world interaction uses the existing focus menu, player control layer, and
Settings hotkey block, without drawing an overlay, showing a pointer, or pausing
simulation. Raw `ui.gamepad` messages target the exact owning world document.

Only the owning token can end a session. Escape and controller Back are native
exit paths. An overlay menu, engine menu, game focus return, browser failure, or
GPU failure also revokes ownership. Automatic browser recovery remains passive;
the consumer must explicitly reactivate. The page receives `ui.interaction` with
`{active:true}` or `{active:false,reason}`. It must clear its held input on release
and blur. Starcade's prototype unloads the game iframe when interaction ends.

The consumer owns activation eligibility (object, distance, line of sight, combat
policy), gameplay state, camera, and presentation. This API does not bind a
particular reference or provide a cabinet asset. The current Starcade prototype
uses an isolated test hotkey and the preserved QASmoke fixture; it is excluded
from Starcade's ordinary deployment.

The repository includes a reproducible
[isolated runtime scenario](../tests/world-surface/README.md), using a preserved
vanilla DisplayScreen5 and material swap in QASmoke. Its test-only Lodge texture
overrides are excluded from the production payload. They establish a controlled
rendering fixture; they are not a standalone screen asset for release.
