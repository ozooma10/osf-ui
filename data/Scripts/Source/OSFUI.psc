ScriptName OSFUI Native Hidden

; OSF UI - Papyrus surface for the shared settings platform (MCM).
;
; Settings are declared in a drop-in schema file:
;   Data/OSFUI/settings/<author>.<modname>.json
; (see docs/authoring-settings.md and examples/settings-only/). This script
; reads them back, writes them, and reacts to changes and hotkey presses.
;
; If OSF UI is absent, every call fails soft: Papyrus logs a missing-native error and the call yields the declared default 
; (GetVersion() yields 0 - the feature-detect gate).
;
; Ids, keys, and enum option values match the schema case-insensitively (Papyrus string interning cannot preserve casing, so OSF UI folds it); 
; write them as authored anyway - mod ids are lowercase "<author>.<modname>" by grammar. 
; The same interning means strings delivered to your callbacks may arrive cased differently than authored; 
; Papyrus == is itself case-insensitive, so plain compares still work.

; Packed host version: major*10000 + minor*100 + patch (1.0.0 -> 10000).
; 0 => OSF UI absent.
int Function GetVersion() Global Native
; Human-readable host version ("1.0.0").
string Function GetVersionString() Global Native

; Reading settings
; Typed getters over the live value store.
; Unknown mod/key, or a type mismatch (e.g. GetInt on a float setting), yields the passed default.
; Cheap and thread-safe; call per use rather than caching.
bool Function GetBool(string asModId, string asKey, bool abDefault = false) Global Native
int Function GetInt(string asModId, string asKey, int aiDefault = 0) Global Native
float Function GetFloat(string asModId, string asKey, float afDefault = 0.0) Global Native

; Covers string-, enum-, and key-typed settings (enum yields the stored option value, key yields the key name, e.g. "F10").
string Function GetString(string asModId, string asKey, string asDefault = "") Global Native

; Writing settings
; Fire-and-forget: the write is queued, then validated/clamped against the schema and persisted on OSF UI's next frame - the same path as the settings menu.
; A refused write (unknown key, wrong type) is logged to OSF UI's log and dropped.
; An open settings menu updates live, and the registered change callback fires once the value commits.

Function SetBool(string asModId, string asKey, bool abValue) Global Native
Function SetInt(string asModId, string asKey, int aiValue) Global Native
Function SetFloat(string asModId, string asKey, float afValue) Global Native
Function SetString(string asModId, string asKey, string asValue) Global Native
; Restore schema defaults: one setting, or the whole mod when asKey is "".
Function Reset(string asModId, string asKey = "") Global Native

; --- change events ------------------------------------------------------------
; Calls akReceiver.asFn(string modId, string key) after any value of asModId commits (any writer: the settings menu, native code, Papyrus). 
; asModId "" subscribes to every mod. Returns a token (0 = failed).
;
; Function OnSettingChanged(string asModId, string asKey)   ; on akReceiver
;
; Registrations are SESSION-scoped: they do not survive a save load. 
; Re-register every time your script handles a game load (e.g. OnPlayerLoadGame on the player alias), like any other event registration.
int Function RegisterForSettingChanges(ScriptObject akReceiver, string asFn, string asModId = "") Global Native
; Instance-free variant for script LIBRARIES: dispatches to the GLOBAL function asScript.asFn(string, string). Same semantics/token as above.
int Function RegisterForSettingChangesStatic(string asScript, string asFn, string asModId = "") Global Native

; --- hotkeys ------------------------------------------------------------------
; A hotkey is a `"type": "key"` setting in your schema - the user sees and rebinds it in the settings menu like everything else, and OSF UI owns the input hook. 
; Registering delivers presses to your script:
;
;   Function OnHotkey(string asModId, string asKey)   ; on akReceiver
;
; asKey "" subscribes to every key-typed setting of asModId. 
; Presses are delivered during gameplay only - never while the user is typing into an overlay view or rebinding a key - and never consume the key. 
; Session-scoped, exactly like RegisterForSettingChanges.
int Function RegisterForHotkey(ScriptObject akReceiver, string asFn, string asModId, string asKey = "") Global Native
; Instance-free variant: GLOBAL function asScript.asFn(string, string).
int Function RegisterForHotkeyStatic(string asScript, string asFn, string asModId, string asKey = "") Global Native

; --- dynamic data <-> views ---------------------------------------------------
; Move DYNAMIC state (live lists, tables, arbitrary strings) between your script and your mod's OSF UI views (see docs/authoring-dynamic-data.md for a worked example).
;
; Your script OWNS game state. Prefer typed SetView* state (cached/replayed),
; ListenForViewActions for one-way mutations, and ListenForViewRequests only
; when JavaScript genuinely needs a returned value. PushToView is the legacy
; transient path and still requires a page-level ready action.

; Push a list of strings to every live view owned by asModId (view ids "<asModId>/..."), delivered to the page as `data.push { mod, key, values }`.
; Fire-and-forget: queued on the calling thread, delivered on OSF UI's next frame; nothing is stored natively. 
; Views ignore keys they don't know, so push freely. An empty asKey or an id that fails the mod-id grammar is logged and dropped.
Function PushToView(string asModId, string asKey, string[] asValues) Global Native

; --- real forms across the bridge (host 1.3+) ---------------------------------
; Serialize REAL game forms into your views: each element of akForms is delivered to the page as an object { formId, formType, name } inside `data.push { mod, key, values: [], forms: [...] }`.
; Same fire-and-forget model as PushToView. A None element keeps its slot as a JS null, so a parallel PushToView (counts, stats, ...) stays index-aligned with it - give forms pushes their own keys.
; A FormList is serialized as ONE form (formType "FLST"); push its members as a Form[] (GetSize/GetAt loop) when the view should see them.
Function PushFormsToView(string asModId, string asKey, Form[] akForms) Global Native

; Preferred state API (protocol 1.6). Each call replaces and caches
; the complete value for (mod, key), sends it to every live owning view as
; `data.state { mod, key, value }`, and automatically replays it when a view
; opens or reloads. The cache is session-scoped; publish again after game load.
; In JS consume it with `osfui.data.on(key, handler)` — no ready action or
; data.push filtering is needed. Use the typed function matching your value.
Function SetViewBool(string asModId, string asKey, bool abValue) Global Native
Function SetViewInt(string asModId, string asKey, int aiValue) Global Native
Function SetViewFloat(string asModId, string asKey, float afValue) Global Native
Function SetViewString(string asModId, string asKey, string asValue) Global Native
Function SetViewBools(string asModId, string asKey, bool[] abValues) Global Native
Function SetViewInts(string asModId, string asKey, int[] aiValues) Global Native
Function SetViewFloats(string asModId, string asKey, float[] afValues) Global Native
Function SetViewStrings(string asModId, string asKey, string[] asValues) Global Native
Function SetViewForms(string asModId, string asKey, Form[] akForms) Global Native

; Resolve a form reference a view echoed back (the `formId` of a pushed form, sent as an args element).
; Accepts decimal ("1370322" - what a JS number arrives as) and hex ("0x0014E8D2"). Unlike Game.GetForm, the full 32-bit range and hex both work.
; Returns None for garbage or a form that no longer exists - CHECK before acting, and cast to the expected type (`GetFormById(asArgs[0]) as Keyword`).
; Runtime FormIDs are SESSION-scoped: resolve promptly, never save one in a script var across saves.
Form Function GetFormById(string asFormId) Global Native
; Bulk variant: element i resolves asFormIds[i]; unresolved entries are None at the same index (length preserved).
Form[] Function GetFormsById(string[] asFormIds) Global Native

; Calls akReceiver.asFn(string asAction, string asArg) when a view owned by asModId fires an action (`osfui.send('ui.action', ...)` on the JS side).
; Returns a token (0 = failed). Actions are fire-and-forget; publish changed state with SetView* (or legacy PushToView).
;
;   Function OnUIAction(string asAction, string asArg)   ; on akReceiver
;
; The action/arg strings may arrive cased differently than the view sent them - compare them with Papyrus == (itself case-insensitive), and keep any case-SENSITIVE comparison out of your JS.
; SESSION-scoped exactly like RegisterForSettingChanges - re-register every time your script handles a game load.
int Function RegisterForViewActions(ScriptObject akReceiver, string asFn, string asModId) Global Native
; Instance-free variant for script LIBRARIES: dispatches to the GLOBAL function asScript.asFn(string, string). Same semantics/token as above.
int Function RegisterForViewActionsStatic(string asScript, string asFn, string asModId) Global Native

; --- multi-argument view actions --------------------------------
; Same as RegisterForViewActions, but delivers the action's argument LIST as a
; Papyrus array instead of one string:
;
;   Function OnUIAction(string asAction, string[] asArgs); on akReceiver
;
; The view sends the list with `osfui.send('ui.action', { action, args: [...] })` (protocol 1.3);
; each element arrives as asArgs[i] (read numbers with `asArgs[i] as int`).
;
; asArgs is never None; it is empty for an action sent with no args, and a view that still sends the scalar `arg` delivers it as a one-element list. 
; A mod may mix both shapes (some scripts on RegisterForViewActions, others here) - each callback is invoked in the form it registered for. 
int Function RegisterForViewActionsArgs(ScriptObject akReceiver, string asFn, string asModId) Global Native
; Instance-free variant for script LIBRARIES: GLOBAL function asScript.asFn(string, string[]).
int Function RegisterForViewActionsArgsStatic(string asScript, string asFn, string asModId) Global Native

; Preferred common-case listener. Dispatches to the fixed callback
; OnOSFUIViewAction(string action, string[] args), so no function-name or
; scalar-vs-list choice is required. The static variant calls the GLOBAL
; function with that name on asScript. Session-scoped; release with Unregister.
int Function ListenForViewActions(ScriptObject akReceiver, string asModId) Global Native
int Function ListenForViewActionsStatic(string asScript, string asModId) Global Native

; Correlated view requests (protocol 1.6). One listener per mod (first wins).
; JS calls `await osfui.papyrus.request(name, ...args)`; the listener receives:
;   Function OnOSFUIViewRequest(string request, string[] args, string replyToken)
; Answer exactly once with the matching typed ReplyView* function, or reject.
; Tokens expire after 10 seconds and are session-scoped; never save them.
int Function ListenForViewRequests(ScriptObject akReceiver, string asModId) Global Native
int Function ListenForViewRequestsStatic(string asScript, string asModId) Global Native
bool Function ReplyViewBool(string asReplyToken, bool abValue) Global Native
bool Function ReplyViewInt(string asReplyToken, int aiValue) Global Native
bool Function ReplyViewFloat(string asReplyToken, float afValue) Global Native
bool Function ReplyViewString(string asReplyToken, string asValue) Global Native
bool Function ReplyViewBools(string asReplyToken, bool[] abValues) Global Native
bool Function ReplyViewInts(string asReplyToken, int[] aiValues) Global Native
bool Function ReplyViewFloats(string asReplyToken, float[] afValues) Global Native
bool Function ReplyViewStrings(string asReplyToken, string[] asValues) Global Native
bool Function ReplyViewForms(string asReplyToken, Form[] akForms) Global Native
bool Function RejectViewRequest(string asReplyToken, string asCode, string asMessage = "") Global Native

; Release a RegisterFor* token. False on a stale/invalid token.
bool Function Unregister(int aiToken) Global Native

; --- menus --------------------------------------------------------------------
; Ask OSF UI to open/close an overlay view; "settings" is the Mods surface (same as F10), where your settings card lives. 
; Honored on OSF UI's next frame through its normal menu policy. OpenMenu returns true when the qualified view id exists, false when no installed view has that id. 
; CloseMenu returns false for an unknown or discovered-but-never-loaded view.
bool Function OpenMenu(string asViewId = "settings") Global Native
bool Function CloseMenu(string asViewId = "settings") Global Native
