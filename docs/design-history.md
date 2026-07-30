# Design history — retired document names

Source comments across the repository cite several design documents by name
(`mcm-design.md §9`, `api-freeze-plan item 2`, …) that no longer exist in the
tree — some were deleted after their designs shipped, some never entered the
repository at all. The citations are **provenance**, not dangling links: the
decisions they record are live in the code and its tests. This file maps each
name to what it was and where its surviving content lives, so a citation can
be followed without archaeology.

| Cited name | What it was | Status | Surviving content |
|---|---|---|---|
| `docs/mcm-design.md` | The Mod Configuration Menu design: settings schemas, groups/pages, key rebinding and capture, vanilla-key table, hotkey delivery (§-numbered) | Deleted 2026-07-17 in `c20163f` after the design shipped | [authoring-settings.md](authoring-settings.md) (author contract), `docs/schema/settings-schema.schema.json`, `sdk/osfui.d.ts` payload types, `src/runtime/SettingsStore.cpp` + `HotkeyService.cpp` and their `tests/native` suites |
| `docs/api-freeze-plan.md` | The 1.0 API-freeze worklist (numbered items: conflict badges, frozen type set, lenient manifest parsing, …) | Deleted 2026-07-17 in `901107f`; the freeze completed and was verified in-game 2026-07-17 | The frozen surfaces themselves: `sdk/osfui.d.ts`, `sdk/OSFUI_API.h`, `docs/schema/*.schema.json`; advisory `targetVersion` is the only compat mechanism |
| `docs/form-references-design.md` | Design for Papyrus `SetViewForms` / `data.push` `forms` serialization (FormID capture on the VM thread, main-thread serialization) | Never committed — authored during development and cited from code | [authoring-dynamic-data.md](authoring-dynamic-data.md), `src/api/PapyrusApi.cpp` (`QueuedPush`/`SerializeForm` comments), `tests/native/papyrus_form_tests.cpp` |
| `docs/reverse-engineering-notes.md` | Working RE notes for engine hooks and offsets | Deleted 2026-07-17 in `0126172`; RE evidence lives in the workspace's `OSF RE` project (per-workspace convention, findings never live in this repo) | `OSF RE/` context modules; load-bearing conclusions are restated at each hook site |
| `docs/ROADMAP.md` | Early feature roadmap | Deleted 2026-07-17 in `0126172` | Superseded by the issue tracker and `CHANGELOG.md` |

When touching a comment that cites one of these, prefer inlining the actual
fact over keeping the citation; public-facing files (`sdk/`, `examples/`,
`docs/schema/`) must not cite them at all.
