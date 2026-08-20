ScriptName OSFUI_View Native Hidden

; OSF UI JavaScript view API.
;
; This script mirrors the four communication semantics:
;
;   JavaScript send()     -> RegisterSend callback
;   JavaScript request()  -> RegisterRequest callback, then Reply/Reject
;   JavaScript on()       <- EmitEvent
;   JavaScript state      <- SetState (retained and replayed)
;
; Registrations and reply tokens are SESSION-scoped. Register again after every game load. Never save a registration or reply token.
;
; Scalar values support None, bool, int, float, string, and Form through Papyrus Var. Papyrus cannot implicitly convert typed arrays to Var, so state and reply arrays use explicit typed functions.
; Send/request and event argument lists use Var[] explicitly. Unsupported objects, structs, and nested arrays are rejected instead of being silently converted.
;
; The shared JavaScript helper wraps direct send arguments as { args: [...] }.
; Native-only endpoints may instead define a richer JSON object payload.
;

; =============================================================================
; JavaScript -> Papyrus: send
; =============================================================================

; Use RegisterSend for a one-way command when JavaScript does not need a result.
;
; A view owned by asModId uses the short local name; other views use the fully-qualified "<asModId>.<asName>" address.
;
; JavaScript
;   osfui.send("equip", 2)
;   osfui.send("equip", 2, 3, 4)
;   osfui.send("acme.inventory.equip", 2) ; cross-mod caller
;
; Receiver callback example:
;   Function OnOSFUISend(string asName, Var[] akArgs, string asSourceViewId)
;       If asName == "equip" && akArgs.Length > 0
;           int slot = akArgs[0] as int
;           Debug.Trace("equip slot " + slot + " from " + asSourceViewId)
;       EndIf
;   EndFunction
;
; Returns a session-scoped registration token, or 0 on failure.
int Function RegisterSend(ScriptObject akReceiver, string asModId, string asName) Global Native
; GLOBAL-function variant. It still must be registered again after game load.
int Function RegisterSendStatic(string asScript, string asModId, string asName) Global Native


; =============================================================================
; JavaScript -> Papyrus: request
; =============================================================================

; Use RegisterRequest when JavaScript must await a value or a structured error.
;
; A name cannot simultaneously be a send and request endpoint. The first registration wins.
;
; JavaScript
;   const count = await osfui.request("getCount")
;   const total = await osfui.request("sum", 2, 3, 4)
;   const count = await osfui.request("acme.inventory.getCount") ; cross-mod caller
;
; Receiver callback example:
;   Function OnOSFUIRequest(string asName, Var[] akArgs, string asSourceViewId, string asReplyToken)
;       If asName == "getCount"
;           OSFUI_View.Reply(asReplyToken, 3)
;       Else
;           OSFUI_View.Reject(asReplyToken, "unknown-request", asName)
;       EndIf
;   EndFunction
;
; Reply resolves the JavaScript promise to the raw bridge value (3 above).
; Reject rejects it with asCode/asMessage. Settle every request exactly once on every path; otherwise JavaScript waits until timeout.
; The token expires and must never be saved or reused.
int Function RegisterRequest(ScriptObject akReceiver, string asModId, string asName) Global Native

; GLOBAL-function variant. It still must be registered again after game load.
int Function RegisterRequestStatic(string asScript, string asModId, string asName) Global Native

; Resolve a request with one scalar bridge value.
bool Function Reply(string asReplyToken, Var aValue = None) Global Native

; Array replies are the same operation; separate names are required because Papyrus cannot pass a typed array as Var.
bool Function ReplyBools(string asReplyToken, bool[] abValues) Global Native
bool Function ReplyInts(string asReplyToken, int[] aiValues) Global Native
bool Function ReplyFloats(string asReplyToken, float[] afValues) Global Native
bool Function ReplyStrings(string asReplyToken, string[] asValues) Global Native
bool Function ReplyForms(string asReplyToken, Form[] akForms) Global Native

; Reject with a stable machine-readable code and optional human-readable text.
bool Function Reject(string asReplyToken, string asCode, string asMessage = "") Global Native


; =============================================================================
; Papyrus -> JavaScript: retained state
; =============================================================================

; Replace the complete retained value for (asModId, asKey).
;
; JavaScript:
;   osfui.state.on(asKey, handler)
;   osfui.state.get(asKey)
;
; State is replayed to every fresh document belonging to asModId. The cache is session-scoped; publish again after game load.
bool Function SetState(string asModId, string asKey, Var aValue) Global Native

bool Function SetStateBools(string asModId, string asKey, bool[] abValues) Global Native
bool Function SetStateInts(string asModId, string asKey, int[] aiValues) Global Native
bool Function SetStateFloats(string asModId, string asKey, float[] afValues) Global Native
bool Function SetStateStrings(string asModId, string asKey, string[] asValues) Global Native
bool Function SetStateForms(string asModId, string asKey, Form[] akForms) Global Native


; =============================================================================
; Papyrus -> JavaScript: transient events
; =============================================================================

; Emit to the currently live views belonging to asModId.
;
; JavaScript:
;   osfui.on(asName, handler)
;
; The handler receives { args: [...] }. Events are delivered at most once and never retained or replayed. Use SetState for reload-safe data.
bool Function EmitEvent(string asModId, string asName, Var[] akArgs = None) Global Native


; Remove a send/request registration. Returns false for 0 or a stale or session-expired token.
bool Function Unregister(int aiRegistrationToken) Global Native

; =============================================================================
; View presentation
; =============================================================================

; View ids are always qualified "<modId>/<viewName>". Menu and HUD views are both supported.
bool Function Open(string asViewId = "osfui/settings") Global Native
bool Function Close(string asViewId = "osfui/settings") Global Native
