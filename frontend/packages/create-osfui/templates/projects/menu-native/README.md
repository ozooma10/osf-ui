# __OSFUI_PROJECT_NAME__

A plain web view plus a native SFSE bridge example. View files are already at
`mod/Data/SFSE/Plugins/OSF/UI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`.

```powershell
git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf
xmake f -P . -m releasedbg
xmake build -P .
xmake install -P .
```

The plugin uses only `OSFUI.h`: it registers send/request handlers, publishes retained state, emits an event, and registers the qualified view ID. Register at `kPostPostLoad` without waiting for `Client::IsReady()`; the browser starts lazily.

For settings, use the OSF Settings SDK and forward only the values the page needs. To open the view from a hotkey, declare a callback hotkey in OSF Settings and queue `RequestMenu` from it, as in the OSF UI repository's `examples/settings-view`.
