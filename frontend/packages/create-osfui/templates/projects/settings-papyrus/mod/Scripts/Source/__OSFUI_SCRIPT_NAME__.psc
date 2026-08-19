ScriptName __OSFUI_SCRIPT_NAME__ Hidden
; The mods settings .json file's onPress target names this script and function, so OSF UI dispatches straight here

; The signature must be exactly (string, string) and the function must be GLOBAL. 
Function OnHotkey(string asModId, string asKey) Global
    If !OSFUI.GetBool(asModId, "enabled", true)
        Return
    EndIf

    int strength = OSFUI.GetInt(asModId, "strength", 50)
    string mode = OSFUI.GetString(asModId, "mode", "normal")
    Debug.Notification("__OSFUI_DISPLAY_NAME__: " + mode + " at " + strength + "%")
EndFunction

; Next steps:
;   - Add rows to the schema and read them here with the same typed getters.
;     OSFUI.SetBool/SetInt/SetFloat/SetString write back through the same validation the menu uses.
;   - Add a second "type": "key" row with its own onPress to hand a different key to a different function.
;   - To drive quest or actor state, resolve your quest lazily and start it:
;       Quest target = Game.GetFormFromFile(0x000800, "YourMod.esm") as Quest
;     0x000800 is the PLUGIN-LOCAL FormID. 