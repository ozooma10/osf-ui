# Example consumer

[`ExampleConsumer.cpp`](ExampleConsumer.cpp) is a **reference sketch** showing how
another SFSE plugin drives PrismaUI SF: request the API after `kPostLoad`,
`CreateView`, then `Invoke` / `InteropCall` / `RegisterJSListener` /
`RegisterConsoleCallback` / `Focus`.

It is intentionally **not** wired into this repo's build — a real consumer is its
own CommonLibSF plugin. To use it:

1. Copy [`sdk/PrismaUI_API.h`](../../sdk/PrismaUI_API.h) into your plugin project
   (it's also installed to `SFSE/Plugins/PrismaUI/api/PrismaUI_API.h`).
2. Adapt the pattern in `ExampleConsumer.cpp` to your plugin's load flow.
3. Ship your view under `Data/SFSE/Plugins/PrismaUI/views/<YourMod>/index.html`.

Full reference: [docs/consumer-api.md](../../docs/consumer-api.md).
