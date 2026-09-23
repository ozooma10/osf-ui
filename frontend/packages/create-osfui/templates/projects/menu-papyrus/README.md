# __OSFUI_PROJECT_NAME__

A plain web view plus a recordless Papyrus backend. View files are already at
`mod/Data/SFSE/Plugins/OSF/UI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`.

Install the Creation Kit, then compile and optionally deploy:

```powershell
./build-papyrus.ps1 -Mo2Mods "C:\path\to\MO2\mods"
```

The script uses `OSFUI` to publish retained state and events. For settings, use the OSF Settings Papyrus API and forward only the values the page needs.
