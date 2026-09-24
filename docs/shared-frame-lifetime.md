# Shared frame lifetime

`SharedFrameConsumer` is the single owner of incoming ring announcements and
frame lifetimes. The pipe reader publishes into it directly. Runtime reads only
freshness metadata for the reveal gate. The compositor reserves a read before
writing overlay commands into an engine command list.

| State | What keeps the slot reserved |
| --- | --- |
| Pending | The latest accepted frame, waiting for selection at a UI region boundary |
| Current | The frame selected for repeated draws |
| Retiring | Recorded or submitted GPU reads after selection has ended |

The state implementation holds private frame references for selection and for
each recorded read. No references escape the consumer. Dropping the final
reference produces one acknowledgement. Metadata snapshots do not own slots.
An unused frame releases immediately; a displayed frame stays reserved even
after its first draw completes. Hide or presentation invalidation drops both
selection references while preserving GPU reads.

Each actual D3D12 submission queue has a private completion fence. The execute
hook assigns completion points to the reads in that exact submission. A frame
read by several lists or queues stays reserved until all of them complete.
Signal failure and the device-removed fence sentinel never establish completion.
Unsubmitted lists remain reserved. GPU resources are deliberately retained if
shutdown cannot prove their reads complete.

Ring announcements invalidate selection immediately. The newest announcement
waits for all old reads to finish before the compositor replaces imported
textures and SRV descriptors. This requires no whole-queue idle wait. Generations
remain monotonic across helper restarts; old reads cannot acknowledge a new
generation's slots.

The consumer mutex protects short state operations. The compositor draw mutex
protects GPU resource adoption and command recording; its timeline mutex
serializes completion-fence operations. Both may enter the consumer; the
consumer never calls back into them or writes to the pipe. Retirement and ack
draining run on ticks even while hidden. Submission callbacks also collect
completed reads while the tick thread is stalled.

Private host protocol 17 uses `FrameAck` as the only permission to reuse a slot.
Only the produce fence is shared. The host can publish into any acknowledged
slot, keeping the ring at four slots. Exhaustion drops captures as normal
backpressure. An explicit static-view republish uses another free slot when
the old pixels are still held, and retries on acknowledgement if none is free.

Focused checks: `xmake test -j4 'osfui-shared-frame-tests/*' 'osfui-wv2-message-tests/*' 'osfui-view-reveal-tests/*'`.
These cover state transitions and wire encoding; they do not replace in-game
validation of the engine submission hooks and frame-generation path.
