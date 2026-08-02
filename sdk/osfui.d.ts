/**
 * TypeScript definitions for the OSF UI native <-> web bridge.
 *
 * Bridge protocol version: 2.0. Compatibility is advisory: declare the OSF UI
 * version you authored against as `targetVersion` (view manifest / settings
 * schema). A target NEWER than the running host badges "needs update" on the
 * Mods surface. A declared target older than 2.0 selects the 1.x compatibility
 * facade over the current transport; these declarations intentionally describe
 * the strict 2.0 surface for new and migrated views. `bridgeVersion` is
 * informational, not something to gate on.
 * Keep in lockstep with:
 *   - docs/authoring-views.md          (prose reference)
 *   - docs/mod-api-2.0-migration.md    (what changed, and why)
 *   - docs/schema/*.schema.json        (manifest + settings-schema validation)
 *   - src/core/Version.h               (kBridgeProtocolVersion)
 *   - src/runtime/MessageBridge.cpp    (envelopes + dispatch)
 *   - SFSE/Plugins/OSFUI/views/shared/osfui.js (the shipped JS helper)
 *
 * Usage: this is an ambient declaration file — drop it into your view project
 * (or reference it via tsconfig "types"/"include") and `window.osfui` is
 * typed globally. There is no runtime package to install.
 *
 * THE WHOLE MODEL IN FOUR VERBS. Pick by semantics, not by transport:
 *
 *   send     web -> backend, one-way. No completion, ever.
 *   request  web -> backend, settles exactly once: payload, typed error, timeout.
 *   on       backend -> web, one-shot happenings. NEVER replayed.
 *   state    backend -> web, named values, latest-wins. ALWAYS replayed.
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
 *   "no-response"          the BACKEND missed the host-side deadline (30s)
 *   "wrong-endpoint-kind"  request() naming a send endpoint (or vice versa)
 *   "unknown-endpoint"     no such endpoint
 *   "invalid-request"      malformed envelope (bad kind/name/id/payload)
 *   "request-capacity"     too many of this view's requests are already in flight
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
  /** The running OSF UI version — the reference for every `targetVersion`. */
  version: string;
  bridgeVersion: string; // protocol version — informational
  /** This document's own qualified view id, e.g. "acme.mymod/dashboard". */
  view: string;
  /** Its owning mod id — the prefix to build this view's own state keys with. */
  mod: string;
}

// ---------------------------------------------------------------------------
// Platform endpoints. Mod endpoints are "<author>.<modname>.<name>" and are
// yours; everything here is undotted or single-dot, which is what makes the
// two namespaces collision-proof without a registry.
// ---------------------------------------------------------------------------

/** `osfui.send(name, payload)` targets. */
export type PlatformSend =
  /** Greet the bridge. The helper does this for you on every document. */
  | { name: "osfui.hello"; payload?: Record<string, never> }
  /** Close the calling surface (last menu closing hides the overlay; a live HUD stays up). */
  | { name: "close"; payload?: Record<string, never> }
  /** Open/close the calling surface. */
  | { name: "setVisible"; payload: { visible: boolean } }
  /** Declare meaningful first paint. Only for a manifest with readySignal:true; helper sugar: markReady(). */
  | { name: "view.ready"; payload?: Record<string, never> }
  | { name: "log"; payload: { text: string } }
  /**
   * EXPERIMENTAL. Take over gamepad handling: suppress the default nav/scroll
   * mapping and consume raw `ui.gamepad` events. Cleared when your document
   * reloads — re-assert it from your normal setup code, not from a
   * reload handler; there is no reload handler.
   */
  | { name: "osfui.gamepadRaw"; payload: { raw: boolean } }
  /**
   * Own the back action. While your menu is ACTIVE, Esc / gamepad B arrive as a
   * synthetic Escape keydown/keyup instead of closing the top menu. Same
   * per-document lifetime as osfui.gamepadRaw. The overlay toggle key always
   * closes natively, so this cannot strand the player.
   */
  | { name: "osfui.handleBack"; payload: { handle: boolean } }
  /** Queue an arbitrary GLOBAL Papyrus function. Sugar: osfui.papyrus.call(). */
  | { name: "papyrus.call"; payload: { script: string; function: string; args?: PapyrusCallArgument[] } }
  /** Fire a one-way message at the owning mod's Papyrus listener. Sugar: osfui.papyrus.send(). */
  | { name: "papyrus.send"; payload: { name: string; args?: PapyrusArgument[] } };

/** `osfui.request(name, payload)` targets. Each settles payload-or-error. */
export type PlatformRequest =
  /** Open a discovered surface by id (loading on demand); `view` omitted targets the caller. Rejects "unknown-view". */
  | { name: "menu.open"; payload: { view?: string }; reply: Record<string, never> }
  /** Close a loaded surface. Never loads one. Rejects "unknown-view". */
  | { name: "menu.close"; payload: { view?: string }; reply: Record<string, never> }
  /** Show/hide one loaded view, independent of the overlay toggle; `view` omitted = self. */
  | { name: "setViewHidden"; payload: { view?: string; hidden: boolean }; reply: Record<string, never> }
  | { name: "ping"; payload?: Record<string, never>; reply: Record<string, never> }
  | { name: "game.get"; payload?: Record<string, never>; reply: GameData }
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
  /** Open OSF UI's own Nexus page in the SYSTEM browser. Fixed target: the payload carries nothing, so page content cannot steer the shell. */
  | { name: "osfui.openModPage"; payload?: Record<string, never>; reply: Record<string, never> }
  /** Open the SFSE log folder. Fixed target, derived natively. Rejects "no-log-folder" | "shell-failed". */
  | { name: "osfui.openLogFolder"; payload?: Record<string, never>; reply: Record<string, never> }
  /** (platform-private) Set a HUD's auto-start for the NEXT launch. */
  | { name: "osfui.setViewAutoStart"; payload: { view: string; enabled: boolean }; reply: Record<string, never> }
  /** (platform-private) Open one server-created report issue. */
  | { name: "osfui.openReportIssue"; payload: { issueNumber: number }; reply: Record<string, never> }
  /** (platform-private) Is the consented reporter configured? */
  | { name: "diagnostics.reportStatus"; payload?: Record<string, never>; reply: DiagnosticsReportStatus }
  /** (platform-private) Submit a consented report. Rejects with the failure code. */
  | { name: "diagnostics.submitReport";
      payload: { title: string; description: string; reproduction?: string };
      reply: { reportId?: string; issueNumber?: number } }
  /** Correlated request to the owning mod's Papyrus listener. Sugar: osfui.papyrus.request(). */
  | { name: "papyrus.request"; payload: { name: string; args?: PapyrusArgument[] };
      reply: { value: unknown } };

// ---------------------------------------------------------------------------
// Platform STATE keys. Subscribe with osfui.state.on(key, fn): the handler runs
// immediately with the current value and again on every change, on every
// document, forever. There is nothing to request and nothing to re-request.
// ---------------------------------------------------------------------------

export interface PlatformState {
  /** The whole settings registry. Re-sent when the registry SHAPE changes; individual commits are `settings.changed` events. */
  "osfui/settings": SettingsData;
  /** One entry per discovered surface, with live open/focus/load state. */
  "osfui/views": ViewsData;
  /** The session health snapshot behind the Mods surface. */
  "osfui/diagnostics": DiagnosticsData;
  /** Active-locale overrides for THIS document's owning mod. Consumed by the i18n namespace for you. */
  "osfui/i18n": I18nCatalog;
  /** (platform-private) The first-load handoff surface's current state. */
  "osfui/handoff": HandoffState;
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
   * This view was shown/hidden as the overlay's focused menu. `reason`
   * distinguishes the overlay itself opening/closing from a menu switch while
   * it stays up.
   */
  "ui.visibility": { visible: boolean; reason?: "overlay" | "focus" };
  /** EXPERIMENTAL. Raw gamepad input, sent to the ACTIVE view while the overlay captures input. */
  "ui.gamepad":
    | { kind: "button"; button: { id: number; down: boolean } }
    | { kind: "stick"; axes: { lx: number; ly: number; rx: number; ry: number } };
  /**
   * devMode ONLY: a protocol mistake the page would otherwise never hear about
   * — a send that named a request endpoint, an unknown endpoint, a backend that
   * missed its deadline. The helper prints these to the console for you, so
   * they show up in F12 DevTools with full object inspection. Not emitted in
   * release builds; repeated misuse raises a `view.protocol-misuse` health card
   * there instead.
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
}

export interface I18nCatalog {
  mod: string;
  locale: string;
  strings: Record<string, string>;
}

/** Value of the `osfui/settings` state key. Re-render from it wholesale. */
export interface SettingsData {
  mods: Array<{
    id: string;
    title: string;
    schema: SettingsSchema;
    values: Record<string, SettingValue>;
    /** Drop-in schema files that also claimed this id and lost first-wins — render a conflict badge. */
    shadowed?: string[];
    /** The OSF UI version this schema was authored against. Advisory; feeds the "needs update" badge. */
    targetVersion?: string;
  }>;
  /**
   * The game's own key bindings (the "vanilla hotkeys" table) — the FULL
   * curated table, not just colliding entries. `event` is the engine controlmap
   * event id, `title` reads like "Starfield (Quicksave)". Read-only. Absent
   * when the runtime has no vanilla data.
   */
  vanillaKeys?: Array<{ event: string; title: string; name: string }>;
  /**
   * Settings artifacts that FAILED to load, so a surface can say so instead of
   * a mod silently vanishing. `kind`: "schema-name" | "schema-parse" |
   * "values-parse". `file` is a bare filename.
   */
  loadErrors?: Array<{ kind: string; file: string; mod?: string; message: string }>;
}

/** Value of the `osfui/views` state key. */
export interface ViewsData {
  views: Array<{
    id: string;
    title: string;
    description: string;    // "" when absent
    mod: string;            // owning settings mod id ("" = standalone)
    kind: "menu" | "hud";
    interactive: boolean;   // derived from kind: may hold focus
    hub: boolean;           // false = hidden utility view, omit from catalogs
    targetVersion: string;  // "" if undeclared
    open: boolean;
    focused: boolean;       // the top open menu (receives input)
    /** "unloaded" = discovered on disk but never loaded; opening it loads on demand. */
    loadState: "unloaded" | "loading" | "loaded" | "failed";
    autoStart: boolean;        // effective choice for the NEXT launch
    autoStartMutable: boolean; // catalog-visible HUDs the player may change
    pinned: boolean;           // always-resident core surface
  }>;
}

/** Reply to `game.get`. Each provider nests under its own object; future ones are SIBLINGS of `calendar`. */
export interface GameData {
  calendar: {
    available: boolean;  // false before a save loads
    day?: number;
    month?: number;
    year?: number;
    hour?: number;       // 0..24 (fractional)
    daysPassed?: number;
  };
}

/**
 * One real game form, serialized by OSFUI.SetViewForms. Identity only — richer
 * display data is published by the script under a parallel state key,
 * index-aligned with this array.
 *
 * To reference the form later, echo `formId` back in a papyrus.send/request arg
 * list; the script resolves it with `OSFUI.GetFormById`. Runtime FormIDs are
 * SESSION-scoped — never persist one.
 */
export interface SerializedForm {
  formId: number;    // runtime FormID — also the echo token (send it back verbatim)
  formType: string;  // record signature ("KYWD" | "WEAP" | "FLST" | ...); numeric string for unknown types
  name?: string;     // TESFullName when the form has one
  editorId?: string; // best-effort: usually UNAVAILABLE at runtime in Starfield
}

/**
 * One durable condition in the session health registry. This is a CURATED
 * registry, not a log view: an entry appears only because a subsystem
 * explicitly raised it, and leaves `status:"active"` only because that
 * subsystem explicitly withdrew it.
 *
 * Player-facing copy is derived from `code` by the built-in Mods surface, so it
 * stays localizable and cannot be authored by a mod. `context` is bounded
 * technical detail — never absolute paths, URLs, or shell targets.
 */
export interface DiagnosticIssue {
  /** Stable identity, the dedupe key. Recurrence reuses it and bumps `occurrences`. */
  id: string;
  /**
   * Stable machine code. v2 families:
   * `settings.schema-name` | `settings.schema-parse` | `settings.values-parse`
   * | `settings.hotkey-target`
   * | `view.load-retrying` | `view.load-failed` | `view.protocol-misuse`
   * | `host.ring-truncated`
   * | `compat.needs-newer-osfui` | `compat.legacy-api`
   * A report from another mod carries ITS code, prefixed with its mod id:
   * `<author>.<modname>:<code>`. Treat an unknown code as generic.
   */
  code: string;
  severity: "warning" | "error";
  /** "resolved" = the condition cleared; the record stays for this session only. */
  status: "active" | "resolved";
  /**
   * Producing subsystem: "settings" | "views" | "host" | "render" | "compat" —
   * or, for a report another mod raised through the native ABI, that mod's
   * "<author>.<modname>" id. The host assigns this, never the payload, so the
   * dot is a reliable tell for "came from a mod".
   */
  source: string;
  subject: string;
  context: Record<string, string | number | boolean>;
  occurrences: number;
  /** Session-relative seconds since the runtime started. */
  firstAt: number;
  lastAt: number;
  resolvedAt?: number;
}

/** Value of the `osfui/diagnostics` state key. */
export interface DiagnosticsData {
  /** Informational key/value block (versions, renderer path, host state) — facts live here rather than as noisy "info" issues. */
  system: Record<string, string | number | boolean>;
  issues: DiagnosticIssue[];
}

/** Platform-private reporting availability and disclosure. */
export interface DiagnosticsReportStatus {
  enabled: boolean;
  logs: string[];
  retentionDays: number;
}

/** Value of the `osfui/handoff` state key (platform-private). */
export interface HandoffState {
  target: string;
  mod: string;
  title: string;
  accent: string;
  phase: "linking" | "retrying" | "error";
  retry: boolean;
}



// ---------------------------------------------------------------------------
// Settings schema shapes (mirror docs/schema/settings-schema.schema.json)
// ---------------------------------------------------------------------------

export type SettingValue = boolean | number | string | string[];

/**
 * The FROZEN base type set (part of the 1.0 API freeze). Colour is not a type —
 * use `type:"string"` + `widget:"color"`. A host that predates a type renders
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

export interface InputContext {
  id: string;               // local to this mod; "gameplay" is reserved for the implicit default
  label?: string;           // user-facing/localizable; defaults to id
  blocksGameplay?: boolean; // metadata assertion: omit @game conflicts only; dispatch is unchanged
}

/** Immutable schema-owned GLOBAL Papyrus callback for a key setting. */
export interface PapyrusHotkeyTarget {
  /** Script name without `.pex`; namespace separators (`:`) are allowed. */
  script: string;
  /** GLOBAL `Function name(string asModId, string asKey)` callback. */
  function: string;
}

export interface Setting {
  key: string;
  aliases?: string[]; // former persisted keys; on load the current key's value is adopted from the first still-valid alias, then rewritten under `key` (§11). Native-only; the renderer ignores it.
  label?: string;
  hint?: string;      // optional helper text shown under the control label
  type: SettingType;
  default?: SettingValue;
  min?: number;
  max?: number;
  step?: number;
  maxLength?: number; // string length hint
  allowUnbound?: boolean; // key type only: "" is a legal, deliberate unbound state (no dispatch, no conflicts; the UI renders an unbind ×)
  inputContext?: string; // key type only: local InputContext id; absent/invalid/unknown => implicit gameplay
  onPress?: PapyrusHotkeyTarget; // key type only: read-only schema metadata; never stored in the user's values file
  options?: string[]; // required when type === "enum" or "flags" (a flags value = array drawn from these, canonicalized to this order)
  optionLabels?: string[]; // display labels parallel to options; stored value stays the option
  widget?: WidgetHint;
  format?: NumberFormat;
  requires?: RequiresKind;
  visibleWhen?: Condition;
  enabledWhen?: Condition;
  /**
   * RUNTIME-INJECTED, never authored (protocol 1.0): on a `type:"key"`
   * setting in a `settings.data` document, the OTHER key-typed settings
   * (any mod) currently bound to the same physical key. Informational only —
   * the runtime never rejects a colliding bind; render a warning badge.
   * A context with blocksGameplay omits @game entries because that reuse
   * is expected. Absent when the remaining binding set is unique.
   *
   * `mod` may be the RESERVED id `"@game"`: the game's own bindings
   * participate too (curated defaults + the engine's controlmap override
   * files). `key` is then the engine controlmap event id and `title` reads
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

/** A button that fires a mod-namespaced bridge command (see UiCommandAction). */
export interface ActionItem {
  type: "action";
  key: string;
  label: string;
  hint?: string;
  command: string;      // must start with "<modId>."
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
 * a host that predates them renders the plain group column unchanged.
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
   * The OSF UI version this schema was authored against (e.g. "1.1.0") —
   * same advisory field as a view manifest's. Never gates loading; a newer
   * target than the running host shows the "needs update" badge in the
   * Mods surface. Distinct from `version` (the mod's OWN schema version)
   * and from a SETTING's `requires` (restart/reload/newGame).
   */
  targetVersion?: string;
  accent?: string;       // per-mod accent "#rrggbb"/"#rrggbbaa"
  icon?: string;         // badge image inside the mod's views namespace folder (see the JSON Schema)
  presets?: SettingsPreset[];
  inputContexts?: InputContext[];
  pages?: SettingsPage[];
  groups?: SettingsGroup[];
}

// ---------------------------------------------------------------------------
// The injected bridge object (present only when manifest grants nativeBridge).
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

export type PapyrusArgument = string | number | boolean;
export interface PapyrusFloatArgument { $papyrus: "float"; value: number }
export type PapyrusCallArgument = PapyrusArgument | PapyrusFloatArgument;

/**
 * The surface added by the shipped helper,
 * SFSE/Plugins/OSFUI/views/shared/osfui.js — load it before your own script:
 *   <script src="../../shared/osfui.js"></script>
 * It decorates the same window.osfui object (creating a stub when no native
 * bridge is present, so these members exist even in a plain browser).
 */
export interface OSFUIHelper {
  /** True when a native bridge (or the harness mock) is present. A PROPERTY, not a call. */
  readonly available: boolean;
  /** Resolves with the runtime info. REJECTS with code "no-bridge" in a plain browser rather than hanging. */
  readonly ready: Promise<RuntimeInfo>;

  /**
   * One-way. Returns whether the message could be POSTED LOCALLY — never a
   * remote outcome. Wanting one means it is a request.
   */
  send(name: string, payload?: JsonObject): boolean;

  /**
   * Settles exactly once: the reply PAYLOAD, or a rejection whose `.code` is a
   * stable machine string (see BridgeErrorPayload). Default client timeout
   * 10000 ms; `timeoutMs: 0` disables only the client timer — the host-side
   * 30 s deadline still answers "no-response".
   */
  request<T = unknown>(name: string, payload?: JsonObject, opts?: { timeoutMs?: number }): Promise<T>;

  /**
   * Subscribe to a one-shot happening. Request replies never reach here (1.x
   * fired both). Returns the unsubscribe fn.
   */
  on<T = unknown>(event: string, fn: (payload: T) => void): () => void;

  /**
   * Named backend-owned values. `key` is always "<mod>/<key>" — your own mod's
   * id included. Matched case-insensitively (Papyrus string interning).
   */
  state: {
    get<T = unknown>(key: string): T | undefined;
    /** Replays the current value SYNCHRONOUSLY on subscribe, then fires on every change. */
    on<T = unknown>(key: string, fn: (value: T) => void): () => void;
  };

  /** Declare meaningful first paint; only for a manifest with readySignal:true. */
  markReady(): boolean;

  /** Direct GLOBAL calls plus the owning-mod listener endpoints. */
  papyrus: {
    /** Force a whole-valued JavaScript number to marshal as Papyrus float rather than int. */
    float(value: number): PapyrusFloatArgument;
    /** Fire-and-forget GLOBAL call. Integer/float/string/bool arguments retain their types. */
    call(script: string, fn: string, ...args: PapyrusCallArgument[]): boolean;
    send(name: string, ...args: PapyrusArgument[]): boolean;
    request<T = unknown>(name: string, ...args: PapyrusArgument[]): Promise<T>;
  };

  /** Pure functions over the `osfui/i18n` state key. No bridge semantics. */
  i18n: {
    readonly ready: Promise<I18nCatalog | { locale: string; strings: Record<string, string> }>;
    readonly locale: string;
    /** Active-locale override for a stable structural address, falling back to your inline English. */
    t(address: string, english: string, vars?: Record<string, string | number>): string;
    /** Apply data-i18n / data-i18n-* attributes below a DOM root. */
    localize(root?: ParentNode): void;
  };

  /** Never touches the wire. */
  theme: {
    /** Apply a mod accent hex to a subtree; a missing/invalid hex clears the whole derived set. */
    applyAccent(element: HTMLElement, hex?: string | null): void;
  };
}

declare global {
  interface Window {
    /**
     * Undefined unless the active view's manifest sets
     * permissions.nativeBridge (helper members present once
     * shared/osfui.js runs).
     */
    osfui?: OSFUIBridge & Partial<OSFUIHelper>;
  }
}

export {};
