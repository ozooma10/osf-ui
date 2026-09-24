# Named stock feed example

Use this page at `views/market/prices/`, or declare a world view named `prices`
in an `osfui` project with `modId: 'market'`. `osfui build` generates the named
DDS asset and writes its `texture` path into the packaged manifest. Reference
that exact path in each screen material. Rebuild the asset path when changing
feed identity; output dimensions do not affect it.

`market/news` has a different generated path, even with identical dimensions,
HTML, and placeholder pixels. Multiple screens referencing `market/prices`
share its output. No placed-reference content overrides are provided.

The current page receives `stock.update` messages (`symbol`, `price`) and draws
a chart. This is the identity/lifetime prototype; the snapshot worker and
feed-scoped retained-state API are the next stage of the cached-feeds plan.
