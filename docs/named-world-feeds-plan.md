# Named, cached world feeds

Mods publish named images rendered from web content. OSF UI refreshes and caches
them; any number of game objects may share each image. Feed identity is qualified
by mod, independent of browser instances, output dimensions, and load order.
There is no fixed four-feed limit; resource and work budgets remain bounded.

## Implementation order and status

1. **Texture identity and lifetime integration:** implemented for .244's queued
   single-mip DDS route. Generated SHA-256 asset paths, verified TextureDB-to-SRV
   association, engine-resource leases, explicit GPU retirement fences, dynamic
   surface registry, lazy allocation and 256 MiB output budget. See
   [evidence](world-texture-identity.md).
2. **Authoring migration:** generated bindings replace `placeholderSize` in the
   CLI, manifest/schema, stock-board example, private fixtures and Starcade cabinet.
3. **Feed state and jobs:** pending. Add feed-scoped publication and refresh APIs,
   immutable per-job data, page render-ready acknowledgement, exact capture/job/
   game-session correlation, and old-image preservation on refresh failure.
4. **Shared workers:** pending. One snapshot worker and a separate interactive
   browser; coalesced fair scheduling across mods; no resident browser/rendering
   for idle cached feeds. Use per-feed InPrivate profiles via Environment10
   composition-controller options. Browser metadata may remain on disk; rendered
   image caching is session-only.
5. **Interaction and session reset:** pending. Admit Pong input only after its
   first valid live frame; retain its last completed image and release its browser
   on exit. Clear cached images and pending jobs on save/session transitions;
   publishers resend current data. No per-object content overrides or disk image
   cache in this version.

## Acceptance gates

- Two mods with identical placeholder pixels and dimensions: passed in game.
- Safe unload/reload and bounded output allocation: passed in game for the
  verified DIRECT-queue screen-material route.
- Sixty-four feeds through one snapshot worker: pending (64 authoring/identity
  definitions are tested; browser scalability is not yet implemented).
- Cross-mod fairness, stale captures, browser failure and save changes: pending
  for the new job protocol. Existing continuous-world browser recovery remains.
- Idle cached displays do no browser rendering: pending.
- Pong while cached feeds refresh independently: pending shared workers.

The first deliverable deliberately leaves the existing continuous browser runtime
in place while establishing the texture identity/lifetime foundation. Do not
interpret the registry's lack of a fixed cap as a claim about browser scalability.
