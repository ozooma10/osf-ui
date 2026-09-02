# __OSFUI_PROJECT_NAME__

This starter contains a plain web view and a recordless Papyrus backend. View files
are already in their final location:

`mod/Data/SFSE/Plugins/OSF/UI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`

Install the Creation Kit, then compile and optionally deploy:

```powershell
./build-papyrus.ps1 -Mo2Mods "C:\path\to\MO2\mods"
```

The generated script uses `OSFUI_View` to publish retained state and events. If
this mod has player settings, use the separate OSF Settings Papyrus API and
explicitly forward the values that the page needs.
