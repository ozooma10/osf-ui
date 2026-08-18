# OSF UI 1.x compatibility contract

OSF UI 2.0 keeps the published 1.x view, native, and Papyrus surfaces working.
They are frozen compatibility contracts: new projects use the current APIs, but
existing projects do not have a scheduled removal deadline.

## Views

A manifest whose `targetVersion` is below 2.0 receives the frozen 1.x helper.
The runtime and CLI select it per view, so old aliases and reply shapes do not
weaken the strict four-verb contract seen by a 2.0 view. New helper features are
not added to the 1.x surface.

## Native plugins

The native bridge remains ABI major 1 and evolves append-only. The original 25
ABI 1.7 virtual slots are unchanged, ABI 1.8 appends `SetViewState`, and ABI 1.9
appends strict `RegisterSend` / `UnregisterSend`. `RegisterCommand` retains its
published request-id injection and automatic-reply behavior. A plugin compiled
against any 1.x minor receives the current bridge and uses only the vtable prefix
it knows; the `Client` wrapper gates newer tail methods by the returned minor.
Only a different ABI major is refused.

## Papyrus

`PushToView`, `PushFormsToView`, and the four `RegisterForViewActions*` natives
remain bound with their published behavior. New scripts should use `SetView*`,
`SendViewEvent`, and `ListenForViewActions{,Static}` so state survives document
recreation and one-shot events do not replay.

## Maintenance rule

Compatibility code stays isolated and regression-tested. Fix correctness and
security defects in it, but do not extend it with new capabilities or make
current code depend on old aliases. A future removal requires a concrete safety
or architectural blocker and a separately documented migration decision; it is
not tied to a release number.
