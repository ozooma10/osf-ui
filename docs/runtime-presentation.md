# Runtime presentation ordering

The game main thread owns desired presentation through `ViewPresentationController`.
Native API calls, browser endpoints, hotkeys, and relative-pointer ownership edges
share `BridgeApi::ViewRequests()`. Its mutex defines FIFO enqueue order, including
requests from different producers. `TakePendingBatch()` takes a finite snapshot;
ready and presentation callbacks cannot extend the batch currently executing.

`Runtime::Update` processes work in this order:

1. Observe game lifecycle and drain browser notifications. Native endpoints are
   installed before incoming pages can call them.
2. Snapshot requests and backend state. Apply captured state before ready
   callbacks can publish newer values. Process registrations before
   transient Papyrus messages so autostart HUDs receive their initial events.
3. Process the request batch in order. `PrepareViewOpen` validates,
   creates a hidden view if needed, and selects desired presentation
   or records a pending load. A warm open is visible to a following Back request;
   a cold open leaves the existing menu active until ready.
4. Run recovery after notification callbacks have returned. `CommitPresentation`
   drains callback-published native state, resolves eligible pending opens, and
   applies presentation and engine input policy. Newly queued requests stay in
   the inbox. Only actual presentation changes rebuild layers and view metadata;
   engine focus/control reconciliation still runs each update.
5. Route gamepad input, maintain geometry and the renderer, check reveal and reply
   deadlines, then dispatch relative-pointer motion and the frame callback.

Relative-pointer operations and browser-owned Back establish additional commit
boundaries: they must observe the presentation selected by preceding requests.
Renderer failure and failed-view teardown apply the release policy immediately,
without draining state or promoting pending opens.

An accepted open/close request acknowledges enqueueing, not browser presentation.
Closing a discovered but uninstantiated view is valid; it can cancel an earlier
queued open. Explicit close removes pending intent, so a late load cannot reopen it.

Pending native state is retained and sent before presentation effects and `kShown`
callbacks. This is a native publication guarantee, not a browser-paint guarantee.
The existing presentation epochs, frame ownership, and reveal handshake continue
to govern which asynchronous browser frames can be displayed.
