ScriptName OSFUI Native Hidden

; OSF UI - Papyrus API for the shared Mod Settings platform.
;
; Settings are declared in a drop-in schema file:
;   Data/SFSE/Plugins/OSFUI/settings/<modId>.json
; (see docs/authoring-settings.md, or scaffold one with `npm create osfui@latest -- --surface settings`).
; This script reads them back, writes them, and reacts to changes and hotkey presses.
;
; If OSF UI is absent, every call fails soft: Papyrus logs a missing-native error and the call yields the declared default 
; (GetVersion() yields 0 - the feature-detect gate).
;
; Ids, keys, and enum option values match the schema case-insensitively;
; write them as authored - mod ids are opaque path-safe names, and dots have no special meaning.
; The same interning means strings delivered to your callbacks may arrive cased differently than authored; 

; Packed OSF UI release version: major*10000 + minor*100 + patch (1.0.0 -> 10000).
; 0 => OSF UI absent.
int Function GetVersion() Global Native
; Human-readable OSF UI release version ("1.0.0").
string Function GetVersionString() Global Native

; ============== Reading settings ==============
; Unknown mod/key, or a type mismatch (e.g. GetInt on a float setting), yields the passed default.
bool Function GetBool(string asModId, string asKey, bool abDefault = false) Global Native
int Function GetInt(string asModId, string asKey, int aiDefault = 0) Global Native
float Function GetFloat(string asModId, string asKey, float afDefault = 0.0) Global Native

; Covers string-, enum-, and key-typed settings (enum yields the stored option value, key yields the key name, e.g. "F10").
string Function GetString(string asModId, string asKey, string asDefault = "") Global Native

; Writing settings
; Fire-and-forget: the write is queued, then validated/clamped against the schema and persisted on OSF UI's next frame.
; A refused write (unknown key, wrong type) is logged to OSF UI's log and dropped.
; Open Mod Settings updates live, and the registered change callback fires once the value commits.

Function SetBool(string asModId, string asKey, bool abValue) Global Native
Function SetInt(string asModId, string asKey, int aiValue) Global Native
Function SetFloat(string asModId, string asKey, float afValue) Global Native
Function SetString(string asModId, string asKey, string asValue) Global Native
; Restore schema defaults: one setting, or the whole mod when asKey is "".
Function Reset(string asModId, string asKey = "") Global Native

; --- change events ------------------------------------------------------------
; Calls akReceiver.asFn(string asModId, string asKey) after any value of asModId commits (any writer: Mod Settings, native code, Papyrus).
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
; A hotkey is a `"type": "key"` setting in your schema - the user sees and rebinds it in Mod Settings like everything else, and OSF UI owns the input hook.
; Registering delivers presses to your script:
;
;   Function OnHotkey(string asModId, string asKey)   ; on akReceiver
;
; asKey "" subscribes to every key-typed setting of asModId.
; Presses are delivered during gameplay only - never while the user is typing into an OSF UI view or rebinding a key - and never consume the key.
; Session-scoped, exactly like RegisterForSettingChanges.
int Function RegisterForHotkey(ScriptObject akReceiver, string asFn, string asModId, string asKey = "") Global Native
; Instance-free variant: GLOBAL function asScript.asFn(string, string).
int Function RegisterForHotkeyStatic(string asScript, string asFn, string asModId, string asKey = "") Global Native

; --- dynamic data <-> views ---------------------------------------------------
; Move DYNAMIC state (live lists, tables, arbitrary strings) between your script and your mod's OSF UI views (see docs/authoring-dynamic-data.md for a worked example).
;
; Your script OWNS game state. It reaches the view through exactly two channels
;
;   SetView*        - STATE. What is true now. Cached and REPLAYED to every fresh document, so a view survives F5 with no handshake.
;   SendViewEvent   - EVENT. Something that just happened. Delivered at most once and NEVER replayed.
;
; Encoding an event as state re-fires it on every reload;

; --- form references ----------------------------------------------------------
; Resolve a form reference a view echoed back (the `formId` of a form published with SetViewForms, sent as an args element).
; Accepts decimal ("1370322") and hex ("0x0014E8D2"). Returns None for garbage or a form that no longer exists
Form Function GetFormById(string asFormId) Global Native
; Bulk variant: element i resolves asFormIds[i]; unresolved entries are None at the same index (length preserved).
Form[] Function GetFormsById(string[] asFormIds) Global Native

; --- state --------------------------------------------------------------------
; DEPRECATED 1.x compatibility (removed in OSF UI 2.1.0): transient pushes.
; Prefer SetViewStrings/SetViewForms, which are retained and replayed.
Function PushToView(string asModId, string asKey, string[] asValues) Global Native
Function PushFormsToView(string asModId, string asKey, Form[] akForms) Global Native

; replaces the complete value for (asModId, asKey), sends it to view as `{ kind:"state", mod, key, value }`, replays it whenever a view opens or reloads.
; In JS: `osfui.state.on(asKey, handler)` - the handler fires immediately with the current value when it subscribes.
; The cache is session-scoped (values may hold form identities): publish again after a game load.
; At most 64 keys per mod; an empty asKey or an id that fails the mod-id grammar is logged and dropped.
; SetViewForms serializes REAL game forms: each element arrives as an object { formId, formType, name }, and a None element keeps its slot as a JS null so a parallel values key stays index-aligned.
Function SetViewBool(string asModId, string asKey, bool abValue) Global Native
Function SetViewInt(string asModId, string asKey, int aiValue) Global Native
Function SetViewFloat(string asModId, string asKey, float afValue) Global Native
Function SetViewString(string asModId, string asKey, string asValue) Global Native
Function SetViewBools(string asModId, string asKey, bool[] abValues) Global Native
Function SetViewInts(string asModId, string asKey, int[] aiValues) Global Native
Function SetViewFloats(string asModId, string asKey, float[] afValues) Global Native
Function SetViewStrings(string asModId, string asKey, string[] asValues) Global Native
Function SetViewForms(string asModId, string asKey, Form[] akForms) Global Native

; --- events -------------------------------------------------------------------
; emit to views registered with `osfui.on("<asModId>.<asName>", handler)` with `payload.args` = asArgs
; Fire-and-forget: queued on the calling thread, delivered on OSF UI's next frame.
Function SendViewEvent(string asModId, string asName, string[] asArgs) Global Native

; --- one-way messages FROM a view ---------------------------------------------
; DEPRECATED 1.x compatibility (removed in OSF UI 2.1.0). Prefer the fixed
; ListenForViewActions / ListenForViewActionsStatic callback below.
int Function RegisterForViewActions(ScriptObject akReceiver, string asFn, string asModId) Global Native
int Function RegisterForViewActionsStatic(string asScript, string asFn, string asModId) Global Native
int Function RegisterForViewActionsArgs(ScriptObject akReceiver, string asFn, string asModId) Global Native
int Function RegisterForViewActionsArgsStatic(string asScript, string asFn, string asModId) Global Native

; Dispatches to the fixed callback
; OnOSFUIViewAction(string actionName, string[] args) - the parameter must not be named "action"
;
; view sends with `osfui.papyrus.send(name, ...args)`. Fire-and-forget, no return value use ListenForViewRequests when JavaScript needs value back.
;
; args never None; empty for message sent with no args, numbers arrive as strings (read with `args[i] as int`).
; The strings may arrive cased differently than the view sent them - compare with Papyrus == (case-insensitive), and keep any case-SENSITIVE comparison out of your JS.
int Function ListenForViewActions(ScriptObject akReceiver, string asModId) Global Native
int Function ListenForViewActionsStatic(string asScript, string asModId) Global Native

; Correlated view requests (introduced with OSF UI 1.5). One listener per mod (first wins).
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
; Ask OSF UI to open/close a view; "osfui/settings" is Mod Settings (same as F10), where your settings card lives.
; OpenMenu/CloseMenu are compatibility names and accept both menu and HUD views.
; View ids are always qualified "<modId>/<viewName>" — a bare name never resolves. Returns true when the qualified view id exists, false when no installed view has that id.
; CloseMenu returns false for an unknown or discovered-but-never-instantiated view.
bool Function OpenMenu(string asViewId = "osfui/settings") Global Native
bool Function CloseMenu(string asViewId = "osfui/settings") Global Native
