/**
 * TypeScript definitions for the OSF UI native <-> web bridge.
 *
 * Bridge protocol version: 2.0. Compatibility is advisory: declare the OSF UI
 * version you authored against as `targetVersion` (view manifest / settings
 * schema). A target NEWER than the running OSF UI runtime badges "needs update"
 * in Mod Settings. A declared target older than 2.0 selects the 1.x compatibility
 * facade over the current transport; these declarations intentionally describe
 * the strict 2.0 surface for new and migrated views. `bridgeVersion` is
 * informational, not something to gate on.
 * Keep in lockstep with:
 *   - docs/schema/*.schema.json        (manifest + settings-schema validation)
 *   - src/Core/Version.h               (kBridgeProtocolVersion)
 *   - src/Bridge/MessageBridge.cpp    (envelopes + dispatch)
 *   - SFSE/Plugins/OSFUI/views/shared/osfui.js (the shipped JS helper)
 *
 * Usage: this is an ambient declaration file — drop it into your view project
 * (or reference it via tsconfig "types"/"include") and `window.osfui` is
 * typed globally. There is no runtime package to install.
 *
 * THE WHOLE MODEL IN FOUR VERBS. Pick by semantics, not by transport:
 *
 *   send     web -> native endpoint handler, one-way. No completion, ever.
 *   request  web -> native endpoint handler, settles once: payload, typed error, timeout.
 *   on       OSF UI runtime or mod backend -> web, one-shot. NEVER replayed.
 *   state    OSF UI runtime or mod backend -> web, latest-wins. ALWAYS replayed.
 *
 * The events/state split is the load-bearing one. Replaying an event on reload
 * re-fires its effect; not replaying state on reload is the blank HUD. A
 * correctly written view therefore has ZERO lifecycle code — if you find
 * yourself writing "on ready, re-request my data", the value you want is state.
 */

// ---------------------------------------------------------------------------
// Envelopes. Routing metadata (kind/name/id/mod/key) lives BESIDE an opaque
// payload, never inside it, so a payload field can never override routing.
// ---------------------------------------------------------------------------

/** web -> native. `id` is required on "request" and forbidden on "send". */
export type WebToNativeMessage =
  | { kind: "send"; name: string; payload: JsonObject }
  | { kind: "request"; name: string; id: string; payload: JsonObject };

/** native -> web. */
export type NativeToWebMessage =
  /** The handshake answer to `osfui.hello`, before any state for that document. */
  | { kind: "ready"; payload: RuntimeInfo }
  /** A named value. Complete per key, never a delta. */
  | { kind: "state"; mod: string; key: string; value: unknown }
  /** A one-shot happening. Delivered at most once; never replayed. */
  | { kind: "event"; name: string; payload: unknown }
  /** Settlement of the request carrying `id`. */
  | { kind: "reply"; id: string; payload: unknown }
  | { kind: "error"; id: string; payload: BridgeErrorPayload };

export type JsonObject = Record<string, unknown>;

/**
 * Why a request failed. `code` is a stable machine string; the layers stay
 * distinguishable on purpose:
 *
 *   "no-bridge"            local, immediate — a plain browser
 *   "timeout"              the CLIENT timer gave up (default 10s; timeoutMs:0 disables it)
 *   "no-response"          the endpoint handler missed the OSF UI runtime deadline (30s)
 *   "wrong-endpoint-kind"  request() naming a send endpoint (or vice versa)
 *   "unknown-endpoint"     no such endpoint
 *   "invalid-request"      malformed envelope (bad kind/name/id/payload)
 *   "request-capacity"     too many of this view's requests are already in flight
 *   "papyrus-timeout"      an OSFUI_View request token was not settled within 10s
 *   anything else          the handler's own rejection code
 */
export interface BridgeErrorPayload {
  code: string;
  message: string;
}

/** The `ready` payload: who is running, and who this document is. */
export interface RuntimeInfo {
  game: string;          // "Starfield"
  plugin: string;        // plugin metadata name
  /** The running OSF UI release version — the reference for every `targetVersion`. */
  version: string;
  bridgeVersion: string; // protocol version — informational
  /** This document's own qualified view id, e.g. "acme.mymod/dashboard". */
  view: string;
  /** Its owning mod id — the helper uses this to expose own state/events by local name. */
  mod: string;
}

// ---------------------------------------------------------------------------
// Platform endpoints. A mod-defined endpoint owned by this document's mod uses
// its local name; use "<modId>.<name>" only to address another mod. The native
// bridge explicitly reserves this surface and the case-insensitive osfui.*
// namespace. A portable Papyrus registration is also rejected unless its full
// "<modId>.<name>" address fits the bridge's 128-byte name limit.
// ---------------------------------------------------------------------------

/** `osfui.send(name, payload)` targets. */
export type PlatformSend =
  /** Greet the bridge. The helper does this for you on every document. */
  | { name: "osfui.hello"; payload?: Record<string, never> }
  /** Close the calling view (closing the active menu hides the menu layer; an open HUD stays rendered). */
  | { name: "close"; payload?: Record<string, never> }
  /** Open/close the calling view. */
  | { name: "setVisible"; payload: { visible: boolean } }
  /**
   * EXPERIMENTAL. Take over gamepad handling: suppress the default nav/scroll
   * mapping and consume raw `ui.gamepad` events. Cleared when your document
   * reloads — re-assert it from your normal setup code, not from a
   * reload handler; there is no reload handler.
   */
  | { name: "osfui.gamepadRaw"; payload: { raw: boolean } }
  /**
   * Own the back action. While your menu is ACTIVE, Esc / gamepad B either
   * arrive as a synthetic Escape keydown/keyup or open the optional discovered
   * menu `view` directly. Same per-document lifetime as osfui.gamepadRaw. The
   * overlay toggle key always closes natively, so this cannot strand the player.
   */
  | { name: "osfui.handleBack"; payload: { handle: boolean; view?: string } }
  /** Queue an arbitrary GLOBAL Papyrus function. */
  | { name: "papyrus.call"; payload: { script: string; function: string; args?: PapyrusCallArgument[] } };

/** `osfui.request(name, payload)` targets. Each settles payload-or-error. */
export type PlatformRequest =
  /** Open a discovered view by id (instantiating on demand); `view` omitted targets the caller. Rejects "unknown-view". */
  | { name: "menu.open"; payload: { view?: string }; reply: Record<string, never> }
  /** Close an instantiated view. Never instantiates one. Rejects "unknown-view". */
  | { name: "menu.close"; payload: { view?: string }; reply: Record<string, never> }
  /** Show/hide one instantiated view, independent of the overlay toggle; `view` omitted = self. */
  | { name: "setViewHidden"; payload: { view?: string; hidden: boolean }; reply: Record<string, never> }
  | { name: "ping"; payload?: Record<string, never>; reply: Record<string, never> }
  /**
   * Write one setting. Resolves with the post-clamp COMMITTED value, so you can
   * tell clamped from accepted without a re-fetch. REJECTS on failure
   * ("forbidden" | "unknown-setting" | "read-only" | "invalid-value") — 1.x
   * resolved an { ok:false } document you had to remember to inspect.
   */
  | { name: "settings.set"; payload: { mod: string; key: string; value: SettingValue };
      reply: { mod: string; key: string; value?: SettingValue } }
  /** Reset one key, or the whole mod when `key` is omitted. The refreshed registry arrives as `osfui/settings` state. */
  | { name: "settings.reset"; payload: { mod: string; key?: string }; reply: Record<string, never> }
  /**
   * ARM native key-rebind capture. Settles in MACHINE time — resolves
   * `{ armed:true }` or rejects "capture-busy" / "forbidden" /
   * "not-rebindable" — and the captured key arrives later as the
   * `settings.captured` EVENT, however long the player takes.
   * Requests settle in machine time; human-time outcomes are events.
   */
  | { name: "settings.captureKey"; payload: { mod: string; key: string };
      reply: { armed: true; mod: string; key: string } }
  /** (platform-private) Set a HUD's auto-start for the NEXT launch. */
  | { name: "osfui.setViewAutoStart"; payload: { view: string; enabled: boolean }; reply: Record<string, never> };

// ---------------------------------------------------------------------------
// Platform STATE keys. Subscribe with osfui.state.on(key, fn): the handler runs
// immediately with the current value and again on every change, on every
// document, forever. There is nothing to request and nothing to re-request.
// ---------------------------------------------------------------------------

export interface PlatformState {
  /** The whole settings registry. Re-sent when the registry SHAPE changes; individual commits are `settings.changed` events. */
  "osfui/settings": SettingsData;
  /** One entry per discovered view, with current open/active-menu/main-frame-load state. */
  "osfui/views": ViewsData;
  /** Session-local conditions shown by Mod Settings. No report submission or upload surface is attached. */
  "osfui/diagnostics": DiagnosticsData;
  /** Starfield's complete read-only keyboard map, copied from the live engine ControlMap. */
  "osfui/keybindings": KeybindingsData;
  /** The exact active engine input-context stack and OSF UI's derived semantic gameplay mode. */
  "osfui/input-context": EngineInputContextState;
  /** Active-locale overrides for this document's owning mod. */
  "osfui/i18n": I18nCatalog;
}

// ---------------------------------------------------------------------------
// Platform EVENTS. osfui.on(name, fn). Never replayed: a document that was not
// open when one fired never learns about it, by design.
// ---------------------------------------------------------------------------

export interface PlatformEvents {
  /**
   * One committed setting value, post-validation (clamped) — authoritative, not
   * the caller's raw input. This is how a mod's HUD reacts live to its settings
   * with zero polling and zero native code.
   */
  "settings.changed": {
    mod: string;
    key: string;
    value: SettingValue;
    /** `type:"key"` settings only: the recomputed conflict list, [] when unique. */
    conflicts?: SettingConflict[];
  };
  /** A mod's values FILE write landed (write-behind, ~500ms). Drives a "Saved" indicator. */
  "settings.persisted": { mod: string };
  /** The outcome of a `settings.captureKey`: the captured key, or cancelled. */
  "settings.captured": {
    mod: string;
    key: string;
    name: string;       // OSF UI key name (e.g. "F9"); "" when cancelled
    cancelled: boolean; // Escape or an unbindable key — keep the old binding
    /**
     * The current keyboard layout's keycap for `name` ("Ö" for the key that
     * stores "Semicolon" on German layouts). DISPLAY ONLY — commit `name`,
     * never a label. Absent on cancels and on older OSF UI runtimes: fall back to `name`.
     */
    label?: string;
    /**
     * Why `cancelled` is true: "escape" (the player backed out), "reserved"
     * (Esc/Win keys are never bindable), "unnameable" (the press carried no
     * usable key identity). Absent on success and on older OSF UI runtimes.
     */
    reason?: string;
    /** Collisions this bind WOULD create, delivered before you commit it. Warn, never block. */
    conflicts?: SettingConflict[];
  };
  /**
   * The physical key bound to a `type:"key"` setting was pressed during
   * gameplay. Filter on `mod` (and `key`) and ignore the rest. Suppressed while
   * the overlay captures input or a rebind is armed.
   */
  "ui.hotkey": { mod: string; key: string };
  /**
   * This view was shown/hidden as the overlay's active menu. `reason`
   * distinguishes the overlay itself opening/closing from a menu switch while
   * it stays up.
   */
  "ui.visibility": { visible: boolean; reason?: "overlay" | "focus" };
  /** EXPERIMENTAL. Raw gamepad input, sent to the active menu while the overlay captures input. */
  "ui.gamepad":
    | { kind: "button"; button: { id: number; down: boolean } }
    | { kind: "stick"; axes: { lx: number; ly: number; rx: number; ry: number } };
  /**
   * Developer mode only: a protocol mistake the page would otherwise never hear about
   * — a send that named a request endpoint, an unknown endpoint, an endpoint handler that
   * missed its deadline. The helper prints these to the console for you, so
   * they show up in F12 DevTools with full object inspection. Not emitted in
   * release builds.
   */
  "osfui.debug.error": { code: string; message: string; detail?: unknown };
}

/** Convenience aliases for the two event payloads consumers name directly. */
export type UiVisibilityPayload = PlatformEvents["ui.visibility"];
export type UiGamepadPayload = PlatformEvents["ui.gamepad"];

export interface SettingConflict {
  /** May be the RESERVED id "@game": the game's own bindings participate too. Display `title`; do not resolve "@game" against the mod registry. */
  mod: string;
  key: string;
  title: string;
  /** Hard collision by default; "possible" is used for special game input contexts whose overlap is conservative. */
  severity?: "conflict" | "possible";
  /** Game-binding conflict only: exact engine input-context name. */
  vanillaContext?: string;
  /** Game-binding conflict only: main or alternate binding slot. */
  slot?: "main" | "alternate";
}

export type GameplayMode = "onFoot" | "ship" | "vehicle" | "zeroG";
export type GameInputContextClassification = "core" | "special" | "menu" | "unknown";

/** @deprecated Compatibility name. Use GameInputContextClassification. */
export type VanillaContextClassification = GameInputContextClassification;

export interface GameBindingSlot {
  slot: "main" | "alternate";
  /** OSF UI physical key name, or null when this slot is unbound. */
  key: string | null;
  /** Ordered display chord. A one-element array is an ordinary single-key binding. */
  chord: string[];
  unbound: boolean;
}

/** @deprecated Compatibility name. Use GameBindingSlot. */
export interface VanillaBindingSlot extends GameBindingSlot {}

/** One read-only Starfield ControlMap action and its physical-key bindings. */
export interface GameInputAction {
  event: string;
  label: string;
  category: string;
  /** Frozen field spelling: the owning engine input context. */
  context: { id: number; name: string; order: number };
  classification: GameInputContextClassification;
  modes: { definite: GameplayMode[]; possible: GameplayMode[] };
  sortIndex: number;
  required: boolean;
  bindings: GameBindingSlot[];
}

/** @deprecated Compatibility name. Use GameInputAction. */
export interface VanillaKeyAction extends GameInputAction {}

export interface KeybindingsData {
  available: boolean;
  revision: number;
  gameVersion: string;
  error?: string;
  /** Starfield ControlMap order. Includes unbound, alternate, and chorded rows. Read-only. */
  actions: GameInputAction[];
}

/** The live engine input-context stack, not a settings-schema hotkey context. */
export interface EngineInputContextState {
  available: boolean;
  revision: number;
  /** null when the live stack does not prove one of the stable semantic modes. */
  mode: GameplayMode | null;
  contexts: Array<{ id: number; name: string }>;
}

/** @deprecated Compatibility name. Use EngineInputContextState. */
export interface InputContextState extends EngineInputContextState {}

export interface I18nCatalog {
  mod: string;
  locale: string;
  strings: Record<string, string>;
}

/** Value of the `osfui/settings` state key. Re-render from it wholesale. */
export interface SettingsData {
  mods: Array<{
    /** Opaque filesystem-safe mod id; dots have no special meaning. */
    id: string;
    title: string;
    schema: SettingsSchema;
    values: Record<string, SettingValue>;
    /** Drop-in schema files that also claimed this id and lost first-wins — render a conflict badge. */
    shadowed?: string[];
	/** The OSF UI release version this schema was authored against. Advisory; feeds the "needs update" badge. */
    targetVersion?: string;
  }>;
  /**
   * Localized keycap labels for the CURRENT OS keyboard layout. Keys are OSF UI
   * key names — the same layout-independent vocabulary `type:"key"` values use;
   * values are what the layout prints on that physical key ("Ö", "^", "Shift").
   * Covers the whole bindable set, not just bound keys. DISPLAY ONLY — never
   * store or send a label where a key name is expected. Absent on older OSF UI runtimes
   * and when undetermined: fall back to showing the name. Republished when the
   * player switches layouts.
   */
  keyboard?: {
    /** Layout tag, e.g. "de-DE"; "" when unknown. */
    layout: string;
    labels: Record<string, string>;
  };
  /** Settings artifacts that failed to load; filenames only, never absolute paths. */
  loadErrors?: Array<{ kind: string; file: string; mod?: string; message: string }>;
}

/** Value of the `osfui/views` state key. */
export interface ViewsData {
  views: Array<{
    /** Qualified view id: "<modId>/<viewName>". */
    id: string;
    title: string;
    description: string;    // "" when absent
    mod: string;            // owning settings mod id ("" = standalone)
    kind: "menu" | "hud";
    interactive: boolean;   // compatibility field: menu-kind focus eligibility, not current focus/input capture
    hub: boolean;           // compatibility field: false = not catalog-visible
    targetVersion: string;  // "" if undeclared
    open: boolean;
    focused: boolean;       // occupies the single active-menu slot
    /** "unloaded" = discovered but not instantiated; opening it instantiates on demand. */
    loadState: "unloaded" | "loading" | "loaded" | "failed";
    autoStart: boolean;        // effective choice for the NEXT launch
    autoStartMutable: boolean; // catalog-visible HUDs the player may change
  }>;
}

/**
 * One real game form, serialized by OSFUI_View.SetStateForms or ReplyForms.
 * Identity only — richer display data is published by the script under a
 * parallel state key, index-aligned with this array.
 *
 * To pass it back as a Papyrus Form, echo this object (or `{ formId }`) in the
 * generic endpoint's `{ args: [...] }` list. A bare number remains a Papyrus
 * int. Runtime FormIDs are SESSION-scoped — never persist one.
 */
export interface SerializedForm {
  formId: number;    // unsigned 32-bit runtime FormID — also the echo token (send it back verbatim)
  formType: string;  // record signature ("KYWD" | "WEAP" | "FLST" | ...); numeric string for unknown types
  name?: string;     // TESFullName when the form has one
  editorId?: string; // best-effort: usually UNAVAILABLE at runtime in Starfield
}

export interface DiagnosticIssue {
  id: string;
  code: string;
  severity: "warning" | "error";
  status: "active" | "resolved";
  source: string;
  sourceKind?: "platform" | "mod";
  subject: string;
  context: Record<string, string | number | boolean>;
  occurrences: number;
  firstAt: number;
  lastAt: number;
  resolvedAt?: number;
}

export interface DiagnosticsData {
  system: Record<string, string | number | boolean>;
  issues: DiagnosticIssue[];
}

// ---------------------------------------------------------------------------
// Settings schema shapes (mirror docs/schema/settings-schema.schema.json)
// ---------------------------------------------------------------------------

export type SettingValue = boolean | number | string | string[];

/**
 * The FROZEN base type set (part of the 1.0 API freeze). Colour is not a type —
 * use `type:"string"` + `widget:"color"`. An OSF UI runtime that predates a type renders
 * the setting read-only and serves the schema default; the user's saved value
 * is preserved on disk untouched. A genuinely new base type ships behind a
 * schema-level `requires: ["type:<t>"]` gate.
 */
export type SettingType = "bool" | "int" | "float" | "enum" | "string" | "key" | "flags";

/** Display hint; older runtimes ignore it and use the type default. */
export type WidgetHint =
  | "slider" | "stepper"       // int/float
  | "dropdown" | "segmented"   // enum
  | "text" | "textarea" | "color"; // string

/** int/float display formatting — store the raw value, show a friendly string. */
export interface NumberFormat {
  prefix?: string;
  suffix?: string;
  scale?: number;    // multiply stored value by this for display (default 1)
  decimals?: number; // fixed decimal places (0-20; clamped by the renderer)
}

export type RequiresKind = "restart" | "reload" | "newGame";

/**
 * Display-only predicate over sibling setting values in the same mod. A leaf
 * references one `key` with exactly one operator; combinators nest. A reference
 * to an unknown key evaluates false. Never affects native validation.
 */
export type Condition =
  | { all: Condition[] }
  | { any: Condition[] }
  | { not: Condition }
  | {
      key: string;
      eq?: SettingValue;
      ne?: SettingValue;
      in?: SettingValue[];
      gt?: number;
      gte?: number;
      lt?: number;
      lte?: number;
      truthy?: boolean;
    };

/** A settings-schema context that scopes a mod hotkey. */
export interface HotkeyContext {
  id: string;               // local to this mod; "gameplay" is reserved for the implicit default
  label?: string;           // user-facing/localizable; defaults to id
  blocksGameplay?: boolean; // assertion that Starfield gameplay input is blocked; game-binding collisions are expected shares
  /** Stable semantic modes in which keys using this context dispatch. Invalid/missing/empty lists keep legacy unscoped behavior. */
  gameplayModes?: GameplayMode[];
}

/** @deprecated Compatibility name. Use HotkeyContext. */
export interface InputContext extends HotkeyContext {}

/** Immutable schema-owned GLOBAL Papyrus callback for a key setting. */
export interface PapyrusHotkeyTarget {
  /** Script name without `.pex`; namespace separators (`:`) are allowed. */
  script: string;
  /** GLOBAL `Function name(string asModId, string asKey)` callback. */
  function: string;
}

export interface Setting {
  key: string;
  label?: string;
  hint?: string;      // optional helper text shown under the control label
  type: SettingType;
  default?: SettingValue;
  min?: number;
  max?: number;
  step?: number;
  maxLength?: number; // string length hint
  allowUnbound?: boolean; // key type only: "" is a legal, deliberate unbound state (no dispatch, no conflicts; the UI renders an unbind ×)
  inputContext?: string; // key type only: local HotkeyContext id; absent/invalid/unknown => implicit gameplay
  onPress?: PapyrusHotkeyTarget; // key type only: read-only schema metadata; never stored in the user's values file
  options?: string[]; // required when type === "enum" or "flags" (a flags value = array drawn from these, canonicalized to this order)
  optionLabels?: string[]; // display labels parallel to options; stored value stays the option
  widget?: WidgetHint;
  format?: NumberFormat;
  requires?: RequiresKind;
  visibleWhen?: Condition;
  enabledWhen?: Condition;
  /**
   * RUNTIME-INJECTED, never authored (introduced in web bridge protocol 1.0): on a `type:"key"`
   * setting in the `osfui/settings` state value, the OTHER key-typed settings
   * (any mod) currently bound to the same physical key. Informational only —
   * the runtime never rejects a colliding bind; render a warning badge.
   * A context with blocksGameplay omits @game entries because that reuse
   * is expected. Absent when the remaining binding set is unique.
   *
   * `mod` may be the RESERVED id `"@game"`: the game's own input bindings
   * projected from the live ControlMap participate too. `key` is then the
   * engine ControlMap event id and `title` reads
   * like "Starfield (Quicksave)" — display `title`, don't resolve `"@game"`
   * against the mod registry.
   */
  conflicts?: Array<{ mod: string; key: string; title: string }>;
}

/** Static rich-text callout. Micro-markdown only: **bold**, *italic*, `code`, \n. */
export interface NoteItem {
  type: "note";
	id?: string;
  text: string;
  style?: "info" | "warn" | "danger";
  visibleWhen?: Condition;
}

/** Static image, resolved relative to the mod's own views/<modId>/ namespace folder. */
export interface ImageItem {
  type: "image";
	id?: string;
  src: string;
  caption?: string;
  height?: number;
  visibleWhen?: Condition;
}

/** A button that calls a mod-namespaced request endpoint. */
export interface ActionItem {
  type: "action";
  key: string;
  label: string;
  hint?: string;
  command: string;      // compatibility field name: request endpoint, must start with "<modId>."
  style?: "default" | "accent" | "danger";
  confirm?: string;     // inline confirmation prompt before firing
  enabledWhen?: Condition;
  visibleWhen?: Condition;
}

/** A row in a group: a value-bearing setting, or a static/action item. */
export type SettingsItem = Setting | NoteItem | ImageItem | ActionItem;

/**
 * A tab in the mod's settings pane. Groups opt in via their `page` field;
 * groups naming no (or an unknown) page collect on an implicit "General" tab
 * painted first. Pages are display-only annotations on the flat group list, so
 * an OSF UI runtime that predates them renders the plain group column unchanged.
 */
export interface SettingsPage {
  id: string;     // referenced by groups' `page`; localized at pages.<id>.label
  label?: string; // English tab label; defaults to id
}

export interface SettingsGroup {
	id?: string;
  label?: string;
  collapsed?: boolean;
  /** Id of a schema-level pages[] entry this group renders under. */
  page?: string;
  visibleWhen?: Condition;
  settings: SettingsItem[];
}

/** Author-shipped value set, applied as a batch of validated settings.set. */
export interface SettingsPreset {
	id?: string;
  label: string;
  description?: string;
  values: Record<string, SettingValue>;
}

export interface SettingsSchema {
  id?: string;
  title?: string;
  description?: string;  // one-line blurb shown under the title in the detail pane
  version?: number;      // schema version (default 0); native stamps it as $schemaVersion + logs a version move (§11). Renderer ignores it.
  /**
	* The OSF UI release version this schema was authored against (e.g. "1.1.0") —
   * same advisory field as a view manifest's. Never gates loading; a newer
   * target than the running OSF UI runtime shows the "needs update" badge in
   * Mod Settings. Distinct from `version` (the mod's OWN schema version)
   * and from a SETTING's `requires` (restart/reload/newGame).
   */
  targetVersion?: string;
  accent?: string;       // per-mod accent "#rrggbb"/"#rrggbbaa"
  icon?: string;         // badge image inside the mod's views namespace folder (see the JSON Schema)
  presets?: SettingsPreset[];
  inputContexts?: HotkeyContext[];
  pages?: SettingsPage[];
  groups?: SettingsGroup[];
}

// ---------------------------------------------------------------------------
// The bridge object injected into every instantiated OSF UI view.
// ---------------------------------------------------------------------------

export interface OSFUIBridge {
  /** web -> native. Pass a JSON string; use the helper's send()/request() instead. */
  postMessage(json: string): void;
  /**
   * native -> web, called with a JSON string. With the shared helper loaded the
   * helper OWNS this slot — never assign it yourself; use osfui.on() and
   * osfui.state.on().
   */
  onMessage?: (json: string) => void;
}

/**
 * One portable value in a mod-defined `{ args: [...] }` endpoint payload.
 * JSON whole numbers marshal as Papyrus int; fractional numbers marshal as
 * float. JSON cannot preserve a JavaScript spelling such as `4.0` separately
 * from `4`, so whole-valued endpoint numbers cannot be forced to float. Ints
 * must fit signed 32-bit; floats must be finite Papyrus float values. A Form
 * object requires an unsigned 32-bit `formId`; extra SerializedForm fields are
 * ignored. Arrays as individual arguments and other/nested objects are rejected.
 */
export type PapyrusArgument = null | string | number | boolean | Pick<SerializedForm, "formId"> | SerializedForm;
export type PapyrusEndpointPayload = { args?: PapyrusArgument[] };
export interface PapyrusFloatArgument { $papyrus: "float"; value: number }
export type PapyrusCallArgument = string | number | boolean | PapyrusFloatArgument;

/**
 * The API added by the shipped helper,
 * SFSE/Plugins/OSFUI/views/shared/osfui.js — load it before your own script:
 *   <script src="/shared/osfui.js"></script>
 * The shared stylesheet and optional directional-navigation helper are:
 *   /shared/osfui.css
 *   /shared/gamepadnav.js
 * It decorates the same window.osfui object (creating a stub when no native
 * bridge is present, so these members exist even in a plain browser).
 */
export interface OSFUIHelper {
  /**
   * One-way. Returns whether the message could be POSTED LOCALLY — never a
   * remote outcome. Wanting one means it is a request. Pass one JSON object for
   * a generic/native endpoint payload. For a portable Papyrus endpoint, scalar
   * or multiple arguments after `name` are wrapped as `{ args: [...] }`.
   * Because one object remains a generic payload, pass a lone Form explicitly
   * as `{ args: [form] }`.
   */
  send(name: string, firstArg: null | string | number | boolean, ...args: PapyrusArgument[]): boolean;
  send(name: string, firstArg: PapyrusArgument, secondArg: PapyrusArgument, ...args: PapyrusArgument[]): boolean;
  send(name: string, payload?: JsonObject): boolean;

  /**
   * Settles exactly once: the reply PAYLOAD, or a rejection whose `.code` is a
   * stable machine string (see BridgeErrorPayload). Default client timeout
   * 10000 ms; `timeoutMs: 0` disables only the client timer — the OSF UI
   * runtime-side 30 s deadline still answers "no-response". An OSFUI_View
   * request token has its own 10 s deadline and answers "papyrus-timeout".
   * OSFUI_View.Reply resolves this promise directly with its scalar value; no
   * `{ value }` wrapper is added. As with send(), scalar or multiple arguments
   * after `name` are wrapped as `{ args: [...] }` for portable Papyrus
   * endpoints. One object remains a generic payload, so pass a lone Form as
   * `{ args: [form] }`. A trailing `{ timeoutMs }` remains request options.
   */
  request<T = unknown>(
    name: string,
    firstArg: null | string | number | boolean,
    ...args: [...PapyrusArgument[], { timeoutMs?: number }]
  ): Promise<T>;
  request<T = unknown>(name: string, firstArg: null | string | number | boolean, ...args: PapyrusArgument[]): Promise<T>;
  request<T = unknown>(name: string, firstArg: PapyrusArgument, secondArg: PapyrusArgument, ...args: PapyrusArgument[]): Promise<T>;
  request<T = unknown>(name: string, payload?: JsonObject, opts?: { timeoutMs?: number }): Promise<T>;

  /**
   * Subscribe to a one-shot happening. Use the local name for an event emitted
   * by this document's mod; qualified "<modId>.<name>" names still work.
   * Request replies never reach here (1.x fired both). Returns unsubscribe.
   */
  on<T = unknown>(event: string, fn: (payload: T) => void): () => void;

  /**
   * Named state values. This document's own values use their local key. Fully
   * qualified "<mod>/<key>" keys remain available, and platform keys use the
   * `osfui` namespace. Matched case-insensitively (Papyrus string interning).
   */
  state: {
    get<T = unknown>(key: string): T | undefined;
    /** Replays the current value SYNCHRONOUSLY on subscribe, then fires on every change. */
    on<T = unknown>(key: string, fn: (value: T) => void): () => void;
  };
}

declare global {
  interface Window {
    /**
     * The native-injected bridge is present in every instantiated OSF UI view.
     * It may be undefined in standalone browser/tooling environments; loading
     * shared/osfui.js can still create a no-bridge helper stub there.
     */
    osfui?: OSFUIBridge & Partial<OSFUIHelper>;
  }
}

export {};
