# Named world material feeds

World-view definitions remain the authoring entry point. Declare `kind: "world"`,
a view name, and output `width`/`height` (1..4096, default 1600x900). `osfui build`
generates a normal one-mip BGRA8 DDS and a `texture` field in the packaged
manifest. Reference that path in the object's material, including its emissive
slot when needed. The material and mesh belong to the consumer mod.

For qualified feed ID `market/prices`, the binding path is
`textures/osfui/feeds/<first 32 SHA-256 hex digits>/<last 32 digits>.dds`.
The hash input is the exact qualified ID in UTF-8. Dimensions, registration
order, browser instances, and physical object identities are absent from it.
Every generated placeholder is the same opaque-black 64x64 image. Multiple
objects using one path share one output. Different feeds have independent paths.

Old `placeholderSize` definitions are rejected. Rebuild them and update their
material references. Native configuration validates the generated path against
the qualified feed ID. Duplicate feed IDs (including case-only aliases), asset paths, or engine asset keys are
errors. There is no four-feed limit in authoring or the texture registry.

The verified integration currently supports Starfield 1.16.244.0. The engine's
TextureDB creation record carries the asset key through the synchronous renderer
resource/SRV creation call. Only that exact asset can select a feed. The call
site and original target are checked before installation; an unsupported or
already modified site disables world binding. There is no size/pixel fallback.

At SRV creation, the engine resource receives an ownership lease for the output.
The lease neither retains the original resource nor rewrites a live descriptor.
When the engine destroys its original texture resource, it releases that lease.
Every OSF GPU submission separately retains its source ring and output until
its completion fence passes. After the last engine owner disappears, a separate
fence is queued after prior DIRECT submissions and must complete before eviction.
Resource addresses and descriptor slot indices
can therefore be recycled without transferring feed identity.

Outputs allocate on material discovery, within a default **256 MiB** budget
charged using `GetResourceAllocationInfo`. Pressure evicts only outputs with no
engine owners and no in-flight initialization/copy commands. If none are safe,
allocation is deferred with diagnostics and that material keeps its placeholder.
At this milestone a deferred binding retries on asset reload. Initialization
and browser transport have separate, temporary resource costs; the output
budget is not a cap on total game or browser memory.

The first completed implementation stage is exact identity and resource
lifetime. The current world runtime still starts a dedicated browser for a bound
feed and keeps it resident; it is not yet the shared snapshot scheduler. The
remaining cached-feed work is recorded in [the implementation plan](named-world-feeds-plan.md): immutable
feed state, render-ready/job/session acknowledgement, shared snapshot worker,
per-feed InPrivate profiles, idle browser release, and cached/live transitions.
No session-only image-cache or 64-feed/one-worker acceptance is claimed yet.

World displays are opaque and passive. They do not open a menu or pause the
game. Fullscreen overlay compositing remains independent. GPU writes are ordered
on the engine DIRECT queue, as validated with ordinary screen materials;
material sampling from an independent asynchronous compute queue is not proven.

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

The [isolated runtime fixture](../tests/world-surface/README.md) uses private
material graphs and generated textures on vanilla DisplayScreen5 geometry.
It does not override vanilla textures. See [identity/lifetime evidence](world-texture-identity.md)
for the traced engine sequence and runtime acceptance results.
