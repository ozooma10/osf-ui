# __OSFUI_PROJECT_NAME__

This starter contains a plain web view and a native SFSE bridge example. View files
are already in their final location:

`mod/Data/SFSE/Plugins/OSF/UI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`

Add CommonLibSF and build the plugin:

```powershell
git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf
xmake f -P . -m releasedbg
xmake build -P .
xmake install -P .
```

The C++ example uses only `OSFUI_Views.h`. It registers send/request handlers,
publishes retained state, emits an event, and registers the qualified view ID.
If this mod has player settings, use the separate OSF Settings SDK and explicitly
forward the values that the page needs.
