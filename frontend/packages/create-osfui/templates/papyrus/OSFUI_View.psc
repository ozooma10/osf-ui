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
; Portable Papyrus endpoints accept the JavaScript payload shape
; { args: [...] }. Native-only endpoints may define richer JSON payloads.

; =============================================================================
; JavaScript -> Papyrus: send
; =============================================================================

; Register one send endpoint. Endpoint ownership is (asModId, asName), and the
; first registration wins. A view owned by asModId uses the short local name;
; other views use the fully-qualified "<asModId>.<asName>" address.
; The rendered qualified address must fit within 128 UTF-8 bytes. Platform
; endpoint names (including close, ping, papyrus.call, and osfui.*) are
; reserved; registration returns 0 instead of creating an unreachable alias.
; Dots are legal in both parts, so two pairs rendering the same qualified
; address collide and the first registration wins.
;
; JavaScript:
;   osfui.send(asName, { args: [...] })
;   osfui.send(asModId + "." + asName, { args: [...] }) ; cross-view
;
; Receiver callback:
;   Function OnOSFUISend(string asName, Var[] akArgs, string asSourceViewId)
;
; asSourceViewId is the authoritative "<modId>/<viewName>" identity supplied
; by OSF UI. Returns a session-scoped registration token, or 0 on failure.
int Function RegisterSend(ScriptObject akReceiver, string asModId, string asName) Global Native

; GLOBAL-function variant. It still must be registered again after game load.
int Function RegisterSendStatic(string asScript, string asModId, string asName) Global Native


; =============================================================================
; JavaScript -> Papyrus: request
; =============================================================================

; Register one request endpoint. It follows the same address length, reserved
; name, and collision rules as RegisterSend. A name cannot simultaneously be a
; send and request endpoint. The first registration wins. A view owned by
; asModId uses the short local name; other views use "<asModId>.<asName>".
;
; JavaScript:
;   const count = await osfui.request("getCount")
;   const price = await osfui.request("getPrice", 2)
;   const total = await osfui.request("sum", 2, 3, 4)
;   const count = await osfui.request("acme.inventory.getCount") ; cross-mod caller
;
; Receiver callback:
;   Function OnOSFUIRequest(
;       string asName,
;       Var[] akArgs,
;       string asSourceViewId,
;       string asReplyToken
;   )
;
; Settle asReplyToken exactly once. It expires and must never be saved.
int Function RegisterRequest(ScriptObject akReceiver, string asModId, string asName) Global Native

; GLOBAL-function variant. It still must be registered again after game load.
int Function RegisterRequestStatic(string asScript, string asModId, string asName) Global Native

; Resolve a request with one scalar bridge value.
bool Function Reply(string asReplyToken, Var aValue = None) Global Native

; Array replies are the same operation; separate names are required because
; Papyrus cannot pass a typed array as Var.
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
; State is replayed to every fresh document belonging to asModId. The cache is
; session-scoped; publish again after game load. True means validated/queued,
; not that a browser consumed the value.
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
; The handler receives { args: [...] }. Events are delivered at most once and
; never retained or replayed. Use SetState for reload-safe data.
bool Function EmitEvent(string asModId, string asName, Var[] akArgs = None) Global Native


; Remove a send/request registration. Returns false for 0 or a stale or
; session-expired token.
bool Function Unregister(int aiRegistrationToken) Global Native


; =============================================================================
; View presentation
; =============================================================================

; View ids are always qualified "<modId>/<viewName>". Menu and HUD views are
; both supported.
bool Function Open(string asViewId = "osfui/settings") Global Native
bool Function Close(string asViewId = "osfui/settings") Global Native
