ScriptName OSFUITest Hidden

; Console-driven validation of the OSFUI Papyrus API (OSF UI repo,
; tests/papyrus/). Needs osfui.paptest.json deployed to
; SFSE/Plugins/OSFUI/settings/ and this script's pex in Data/Scripts/.
;
;   cgf "OSFUITest.RunAll"      full getter/setter/clamp/reset suite; also
;                               registers the change + hotkey callbacks
;   cgf "OSFUITest.Hook"        register just the callbacks
;   cgf "OSFUITest.Drop" <tok>  exercise Unregister on a token Hook printed
;   press F8 (testHotkey)       in gameplay -> HUD notification from OnHotkey
;
; Results land as HUD notifications (summary + failures) and [OSFUITest]
; lines in the Papyrus user log (enable [Papyrus] bEnableLogging=1). Each
; RunAll/Hook adds ANOTHER callback registration (no state to dedupe in a
; globals-only script) - duplicate change traces after a re-run are expected,
; or save-load to clear (registrations are session-scoped).

string Function ModId() Global
	return "osfui.paptest"
EndFunction

int Function Check(bool abOk, string asWhat, int aiFails) Global
	If abOk
		Debug.Trace("[OSFUITest] PASS " + asWhat)
		return aiFails
	EndIf
	Debug.Trace("[OSFUITest] FAIL " + asWhat, 2)
	Debug.Notification("OSFUITest FAIL: " + asWhat)
	return aiFails + 1
EndFunction

bool Function Near(float afA, float afB) Global
	float d = afA - afB
	return d > -0.001 && d < 0.001
EndFunction

Function Hook() Global
	int t1 = OSFUI.RegisterForSettingChangesStatic("OSFUITest", "OnSettingChanged", ModId())
	int t2 = OSFUI.RegisterForHotkeyStatic("OSFUITest", "OnHotkey", ModId())
	Debug.Trace("[OSFUITest] callback tokens: settings=" + t1 + " hotkey=" + t2)
	If t1 == 0 || t2 == 0
		Debug.Notification("OSFUITest: callback registration FAILED")
	Else
		Debug.Notification("OSFUITest: callbacks registered - press the test hotkey (default F8)")
	EndIf
EndFunction

Function OnSettingChanged(string asModId, string asKey) Global
	Debug.Trace("[OSFUITest] OnSettingChanged " + asModId + "." + asKey)
EndFunction

Function OnHotkey(string asModId, string asKey) Global
	Debug.Trace("[OSFUITest] OnHotkey " + asModId + "." + asKey)
	Debug.Notification("OSFUITest: hotkey fired (" + asKey + ")")
EndFunction

Function Drop(int aiToken) Global
	bool ok = OSFUI.Unregister(aiToken)
	Debug.Trace("[OSFUITest] Unregister(" + aiToken + ") = " + ok)
	Debug.Notification("OSFUITest: Unregister(" + aiToken + ") = " + ok)
EndFunction

Function RunAll() Global
	string m = ModId()
	Debug.Trace("[OSFUITest] === RunAll ===")
	int fails = 0

	; feature detect
	int v = OSFUI.GetVersion()
	fails = Check(v >= 10000, "GetVersion >= 10000 (got " + v + ")", fails)
	If v == 0
		Debug.Notification("OSFUITest: OSF UI absent (GetVersion 0) - aborting")
		return
	EndIf
	fails = Check(OSFUI.GetVersionString() != "", "GetVersionString non-empty (got " + OSFUI.GetVersionString() + ")", fails)

	; callbacks first, so every commit below also produces an OnSettingChanged trace
	Hook()

	; start from schema defaults (writes commit on OSF UI's next frame -> wait)
	OSFUI.Reset(m)
	Utility.Wait(0.5)
	fails = Check(OSFUI.GetBool(m, "enabled", false) == true, "GetBool default true", fails)
	fails = Check(OSFUI.GetInt(m, "count", -1) == 3, "GetInt default 3", fails)
	fails = Check(Near(OSFUI.GetFloat(m, "scale", -1.0), 1.0), "GetFloat default 1.0", fails)
	fails = Check(OSFUI.GetString(m, "mode", "?") == "balanced", "GetString enum default 'balanced'", fails)
	fails = Check(OSFUI.GetString(m, "label", "?") == "hello", "GetString default 'hello'", fails)
	fails = Check(OSFUI.GetString(m, "testHotkey", "?") == "F8", "GetString key default 'F8'", fails)

	; misses yield the caller's default
	fails = Check(OSFUI.GetInt(m, "scale", -1) == -1, "type mismatch yields default", fails)
	fails = Check(OSFUI.GetBool(m, "nope", false) == false, "unknown key yields default", fails)
	fails = Check(OSFUI.GetInt("no.such", "count", 42) == 42, "unknown mod yields default", fails)

	; writes
	OSFUI.SetBool(m, "enabled", false)
	OSFUI.SetInt(m, "count", 7)
	OSFUI.SetFloat(m, "scale", 0.25)
	OSFUI.SetString(m, "mode", "fast")
	OSFUI.SetString(m, "label", "papyrus was here")
	Utility.Wait(0.5)
	fails = Check(OSFUI.GetBool(m, "enabled", true) == false, "SetBool committed", fails)
	fails = Check(OSFUI.GetInt(m, "count", -1) == 7, "SetInt committed", fails)
	fails = Check(Near(OSFUI.GetFloat(m, "scale", -1.0), 0.25), "SetFloat committed", fails)
	fails = Check(OSFUI.GetString(m, "mode", "?") == "fast", "SetString enum committed", fails)
	fails = Check(OSFUI.GetString(m, "label", "?") == "papyrus was here", "SetString committed", fails)

	; clamping + refusal (same validation as the settings menu)
	OSFUI.SetInt(m, "count", 999)
	OSFUI.SetFloat(m, "scale", -5.0)
	OSFUI.SetString(m, "mode", "bogus")
	Utility.Wait(0.5)
	fails = Check(OSFUI.GetInt(m, "count", -1) == 10, "SetInt clamped to max 10", fails)
	fails = Check(Near(OSFUI.GetFloat(m, "scale", -1.0), 0.0), "SetFloat clamped to min 0", fails)
	fails = Check(OSFUI.GetString(m, "mode", "?") == "fast", "invalid enum refused (kept 'fast')", fails)

	; single-key reset, then whole-mod reset
	OSFUI.Reset(m, "count")
	Utility.Wait(0.5)
	fails = Check(OSFUI.GetInt(m, "count", -1) == 3, "Reset(key) restored default", fails)
	OSFUI.Reset(m)
	Utility.Wait(0.5)
	fails = Check(OSFUI.GetBool(m, "enabled", false) == true, "Reset(mod) restored defaults", fails)

	If fails == 0
		Debug.Trace("[OSFUITest] === ALL CHECKS PASSED ===")
		Debug.Notification("OSFUITest: ALL CHECKS PASSED - now press F8, then check the settings menu")
	Else
		Debug.Trace("[OSFUITest] === " + fails + " CHECK(S) FAILED ===", 2)
		Debug.Notification("OSFUITest: " + fails + " CHECK(S) FAILED - see Papyrus log")
	EndIf
EndFunction
