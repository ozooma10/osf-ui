# Texture identity and lifetime evidence

Implemented in OSF UI's `osf/world-multiple-textures` worktree, based on `f48c3bd`.
Starcade's material migration is isolated on `osf/named-world-feeds`, based on
`9bb1590`. Runtime observations below use Starfield **1.16.244.0**. Main checkouts
and ordinary MO2 deployment were not changed.

## Exact asset binding

`osfui build` hashes the exact qualified feed ID as UTF-8 with SHA-256. The first
32 hexadecimal digits form a directory and the remaining 32 form the DDS name:
`textures/osfui/feeds/<32 hex>/<32 hex>.dds`. Native code independently verifies
this binding with Windows CNG. Every placeholder is the same opaque-black 64x64,
single-mip BGRA8 DDS. Browser output dimensions are independent.

The engine resource key is 12 bytes: filename CRC, packed extension, directory
CRC. The CRC uses polynomial `0xEDB88320`, starts at zero, has no final XOR, folds
ASCII to lowercase, and normalizes slashes to backslashes. `.dds` is `0x00736464`.
The initial runtime probe verified `textures/OSFUIWorldTest/board0.dds` as
`ADF9FC9A:00736464:07E6C45A`. Duplicate feed IDs, case-only aliases, and engine-key
collisions are explicit errors. The SHA-256 asset path does not eliminate the
need to check collisions in the engine's narrower key representation.

The trace uses the matching unpacked executable, Address Library, read-only
Ghidra decompilation, checked call-site bytes, and a private runtime probe.
Locations below are RVAs relative to the executable image base.

| Stage | Verified location and relationship |
| --- | --- |
| TextureDB queued create | `0x28F87B0`, Address Library ID 139982 |
| Scoped create call | `0x28F8826` (`+0x76`), bytes `E8 D5 47 00 00` |
| Original creation callee | `0x28FD000`, ID 140026 |
| Job asset identity | queued record `+0xE0` points to DB entry; entry `+8` is the 12-byte resource key |
| Renderer creation | `0x2A105B0`; synchronous resource and initial SRV creation within the scoped call |
| Texture publication | `0x28F91E0`, called at `0x28F9BC4`; publishes the AAL texture from job `+0xC0` under the same resource key |
| DB removal | `0x28F8500`, reached from eviction at `0x28FE1CC` and entry destruction at `0x28FE313` |
| Renderer disposal | `0x28FF3E0` -> `0x28FEF30` -> queued disposal through `0x28FF7A0` / `0x28FED90` -> `0x28FD820` |
| AAL texture destruction | `0x2A11DF0`; frees descriptor allocations and releases the resource at AAL `+0x38` |

`EngineTextureIdentity` intercepts only the verified creation call. A scoped
thread-local asset key exists only while the original function runs, with nested
call restoration. `CreateShaderResourceView` associates it with the actual
resource. Later descriptors can use the resource's attached identity lease.
Dimensions, pixels, global last-loaded state, address lookup, and remembered
SRV slots never select a feed. Unrelated resources use original descriptor
creation. Unsupported or modified call sites fail closed. Format checks only
establish resource compatibility after identity lookup.

The initial probe (`20260922-010151-873-WorldSurface-Live`) observed all four
asset keys across creation threads, DB publication, removal, original COM
resource destruction, and new resources/descriptor slots after reload. DB
retirement at 21:05:15.079 preceded COM resource destruction at 21:05:15.091.
Returning to QASmoke published the same keys into fresh resources. The local
ignored `build/texture-identity-proof` folder preserves decompilations and the
initial probe source; it is not compiled into the final runtime.

## Ownership and GPU retirement

Each original engine resource stores an `IUnknown` lease using
`SetPrivateDataInterface`. It owns the OSF output but never references the
original resource, so it cannot pin streaming or create a cycle. D3D12 owns the
interface until replacement/destruction; `GetPrivateData` takes a temporary
reference which the hook releases. See Microsoft's
[attachment contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12object-setprivatedatainterface)
and [retrieval contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12object-getprivatedata).

Initialization and GPU copies separately retain their output and source
resources until OSF's completion fence passes. After the final engine owner
releases its lease, the serialized DIRECT submission path queues another fence
behind preceding engine work. Eviction requires its completion and absence of
initialization/copy leases. A new engine owner resets that retirement fence.
No live descriptor is rewritten during eviction. Device/signal failure retains
resources and disables further copies rather than guessing completion.

Outputs allocate lazily and are charged by `GetResourceAllocationInfo`, with a
256 MiB default budget. Safe unowned outputs are LRU candidates. If all candidates
remain engine/GPU-owned, allocation is deferred explicitly and that asset keeps
its own placeholder. A deferred asset retries on reload at this milestone.
Temporary initialization uploads and browser transport memory are outside this
**output-texture** budget.

## Fresh game acceptance

Artifacts are under `C:/Modding/Starfield/OSF Test Harness/artifacts/`.

| Run | Result |
| --- | --- |
| `20260922-012335-716-WorldSurface-Live` | Four independent feeds from two namespaces, identical DDS files; three complete unload/reload cycles; captures inspected |
| `20260922-013449-549-WorldSurface-Live` | Forced 8 MiB output budget: two admitted feeds and two explicit deferrals; screenshot shows two correct feeds and two black faces |
| `20260922-013836-073-WorldSurface-Live` | Final explicit-retirement-fence build: four feeds and three complete eviction/unload/reload cycles passed |
| `20260922-014752-472-WorldSurface-Live` | Final retirement-fence build with 8 MiB budget: two correctly bound outputs, two explicit deferrals, 8,257,536 resident bytes; screenshot inspected |
| `20260922-014150-558-Starcade-Cabinet` | Migrated cabinet: Pong, player animation, trusted keyboard input, release, reactivation, browser recovery and portable takeover passed; playing capture inspected |

All four placeholder DDS files hash to
`DD81733D39CAAB9D6DD141756048DA05F60F4E42E1A424B9CB7ABC0B0ADEACB3`.
Each 1000x1000 output consumes 4,128,768 allocation bytes on this device; total
four-feed residency is 16,515,072 bytes. Every final-build lifetime cycle first
proved requested eviction cannot reclaim a visible material, then reached
**zero** output residency after real cell unload, then returned to 16,515,072
bytes. Retired allocation counts progressed 4, 8, 12; reloaded generations were
5-8, 9-12, 13-16 with unchanged feed IDs and no GPU/shared-ring failures.

The log records retirement fences 287-290 queued after engine-owner release
at 21:40:32.250-.255; eviction occurs at .257-.258. The
`texture-lifetime-cycles.json` artifact preserves before/held/empty/reloaded
observations. Camera posture can change after `coc`, so the fresh-game
four-board capture is the primary visual check.

Final instrumented DLL SHA-256:
`92DA28397F65C4A5EC2067BBD93CD95D8F4A3DB61606392D9D4F283E887BDCBD`.
Helper SHA-256:
`26431140CA5A4B54F8A5C151B2FFBD74EBE32A2DB25A4A6BCA7837CDF1884986`.

Host validation: 30 native suites, 21 CLI tests, three focused manifest schema
tests, frontend type checking, releasedbg build, and Starcade native/Papyrus
build plus asset validation passed. The 64-feed test covers stable unique
identity/authoring definitions and ordering, not 64 browsers through one worker.

Two harness defects were corrected: a snapshot filter omitted the second mod
namespace, and a newly created post-launch control file was missing from MO2's
VFS. One cycle sequence stopped on lost foreground focus; per-command refocusing
then allowed three cycles. Failed/interrupted evidence was not claimed as a
pass. Owned games were stopped and the private profile restored after testing.

## Limits

This proves the queued single-mip DDS route and ordinary DIRECT-queue screen
materials on .244. Streaming creation paths and independent asynchronous-compute
sampling are not covered. Future versions require verification. The integration
does not promise support for arbitrary shader/resource types.

The [remaining implementation](named-world-feeds-plan.md) includes feed state,
immutable jobs, readiness/capture/session correlation, fair shared workers,
per-feed InPrivate profiles, idle browser release and cached/live transitions.
World views still own continuous browsers. Save changes do not yet clear cached
images, and 64-feed/one-worker acceptance is not claimed. No rendered-image
files are written to disk by this integration.

## Packaged build

A separate normal build with `test_harness=false` passed and is staged at
`build/release-mods/OSF UI`; the proven instrumented payload remains at
`build/isolated-mods/OSF UI`. The normal DLL contains no world-test snapshot,
private-browser profile, or fixture budget-control markers. Its SHA-256 is
`49B8F11D32154F55408F3C1B6148AF2F3899E7698C5B832A27A3864224A90C8D`.
This is build/staging proof; fresh-game evidence above uses the instrumented
build of the same texture integration. Neither payload replaces the main
checkout's ordinary mod deployment. Current worktree configuration is normal;
re-enable `--test_harness=y` when rebuilding for the private acceptance runner.
