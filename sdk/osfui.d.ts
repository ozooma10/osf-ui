/**
 * TypeScript definitions for the OSF UI native <-> web bridge.
 *
 * Bridge protocol version: 1.1 (STABLE — additive changes bump the minor;
 * breaking changes bump the major). Compatibility is advisory: declare the
 * OSF UI version you authored against as `targetVersion` (view manifest /
 * settings schema) and the Mods surface badges "needs update" when the
 * running host (`runtime.ready`'s `version`) is older. `bridgeVersion` is
 * informational, not something to gate on. Keep in lockstep with:
 *   - docs/authoring-views.md          (prose reference)
 *   - docs/schema/*.schema.json        (manifest + settings-schema validation)
 *   - src/core/Version.h               (kBridgeProtocolVersion)
 *   - src/runtime/MessageBridge.cpp    (envelope + dispatch)
 *   - data/OSFUI/views/shared/osfui.js (the shipped JS helper)
 *
 * Usage: this is an ambient declaration file — drop it into your view project
 * (or reference it via tsconfig "types"/"include") and `window.osfui` is
 * typed globally. There is no runtime package to install.
 */

/**
 * Every message in both directions is JSON text of this shape.
 *
 * `requestId` is the correlation envelope (protocol 1.0): any ui.command may carry a
 * caller-chosen id (string, 1-64 chars); every reply echoes it top-level, and
 * a command with no reply type of its own answers `ui.result` when (and only
 * when) an id was supplied. Omit it for fire-and-forget. The shared helper's
 * `osfui.request()` does all of this for you.
 */
export interface BridgeEnvelope<TType extends string = string, TPayload = unknown> {
  type: TType;
  requestId?: string;
  payload: TPayload;
}

// ---------------------------------------------------------------------------
// web -> native: exactly one envelope type, "ui.command".
// The payload carries `command` plus that command's arguments.
// ---------------------------------------------------------------------------

export type UiCommand =
  /** Close the calling surface (last menu closing hides the overlay; a live HUD stays up). */
  | { command: "close" }
  /** Open/close the calling surface. */
  | { command: "setVisible"; visible: boolean }
  /** Open/close a registered surface by id; `view` omitted targets the calling view. */
  | { command: "menu.open"; view?: string }
  | { command: "menu.close"; view?: string }
  /** Aliases of menu.open/menu.close — a surface's kind is fixed by its manifest. */
  | { command: "hud.show"; view?: string }
  | { command: "hud.hide"; view?: string }
  /** Show/hide one loaded view, independent of the overlay toggle; `view` omitted = self. */
  | { command: "setViewHidden"; view?: string; hidden: boolean }
  | { command: "log"; text: string }
  | { command: "ping" }
  | { command: "game.get" }
  /** Catalog of loaded surfaces; replies `views.data` and subscribes the caller to change pushes. */
  | { command: "views.get" }
	/** Active-locale overrides for a mod (omitted mod = calling view's owner); replies and subscribes with i18n.data. */
	| { command: "i18n.get"; mod?: string }
  /** Read the settings registry; replies `settings.data` and SUBSCRIBES the caller: committed values arrive as `settings.changed`, registry shape changes re-send `settings.data`. */
  | { command: "settings.get" }
  | { command: "settings.set"; mod: string; key: string; value: SettingValue }
  | { command: "settings.reset"; mod: string; key?: string }
  /** Arm native key-rebind capture; the next key press returns as settings.captured (echoing the arming requestId, however much later). One capture at a time — a second arm answers ui.result code "capture-busy". Any type:"key" setting may be captured. */
  | { command: "settings.captureKey"; mod: string; key: string }
  /** EXPERIMENTAL (gamepad navigation is being refined; exempt from the 1.0 stability guarantee until stabilized). Take over gamepad handling: suppress the default nav/scroll mapping and consume raw ui.gamepad events. STICKY PER VIEW — survives overlay hide/show; cleared when your page (re)loads or the view is destroyed. */
  | { command: "osfui.gamepadRaw"; raw: boolean }
  /** Own the back action. While your menu is ACTIVE, Esc / gamepad B arrive as a synthetic Escape keydown/keyup instead of closing the top menu — your page decides: navigate (`menu.open`), dismiss an inner panel, or send `close`. STICKY PER VIEW — survives overlay hide/show; cleared when your page (re)loads or the view is destroyed. The overlay toggle key always closes natively regardless. */
  | { command: "osfui.handleBack"; handle: boolean }
  /** (protocol 1.1) Open OSF UI's own Nexus Mods page in the user's SYSTEM browser — the overlay itself never navigates. The URL is hardcoded in the host and the payload carries nothing, so page content cannot steer the shell. For "update OSF UI" affordances. Failures answer ui.result { ok:false, code:"shell-failed" }. */
  | { command: "osfui.openModPage" }
  /**
   * Fire an action at the OWNING mod's Papyrus scripts
   * (OSFUI.RegisterForViewActions). The mod id is derived from the calling
   * view's id — it cannot be spoofed via the payload. Fire-and-forget: there
   * is no reply payload; the script answers by pushing state back as
   * `data.push`. Convention: send `{ action: "ready" }` on load (and on
   * `runtime.ready` re-handshakes) so the script knows to (re)push current
   * state — OSF UI caches nothing (docs/authoring-dynamic-data.md).
   */
  | { command: "ui.action"; action: string; arg?: string };

/**
 * A mod-defined action command fired by a schema `action` item. The command
 * string MUST be namespaced with the mod id ("<id>.something"); the settings
 * view refuses to send anything else. Handled by the mod's own SFSE plugin
 * (IOSFUIBridge.RegisterCommand). Not part of the fixed UiCommand union — it is
 * an open extension point owned by the mod.
 */
export interface UiCommandAction {
  command: string; // "<modId>.<action>"
  [field: string]: unknown;
}

export type WebToNativeMessage = BridgeEnvelope<"ui.command", UiCommand | UiCommandAction>;

// ---------------------------------------------------------------------------
// native -> web messages (assign window.osfui.onMessage and switch on type)
// ---------------------------------------------------------------------------

export interface RuntimeReadyPayload {
  game: string;        // "Starfield"
  plugin: string;      // plugin metadata name
  /**
   * The running OSF UI version (kPluginVersion) — the reference point for
   * every advisory `targetVersion` declaration.
   */
  version: string;
  bridgeVersion: string; // protocol version (kBridgeProtocolVersion) — informational
}

export interface I18nDataPayload {
	mod: string;
	locale: string;
	strings: Record<string, string>;
}

/**
 * Reply to `settings.get` (and to a successful `settings.reset`). Also pushed
 * unsolicited to every subscriber (any view that has sent `settings.get`)
 * whenever the registry SHAPE changes — a mod registering or unregistering a
 * schema at runtime over the native API. Re-render from it wholesale.
 */
export interface SettingsDataPayload {
  mods: Array<{
    id: string;
    title: string;
    schema: SettingsSchema;
    values: Record<string, SettingValue>;
    /**
     * Additive (api-freeze-plan item 1): drop-in schema files that also
     * claimed this id and lost first-wins — render a conflict badge.
     * Omitted in the (normal) no-conflict case.
     */
    shadowed?: string[];
    /**
     * The OSF UI version this schema was authored against (same advisory
     * contract as a view manifest's `targetVersion`). Never gates — the
     * schema loads best-effort — but feeds the Mods surface "needs update"
     * badge when newer than the running host. Omitted when undeclared.
     */
    targetVersion?: string;
  }>;
  /**
   * The game's own key bindings (protocol 1.0, mcm-design §9 "vanilla
   * hotkeys") — the FULL curated table, not just colliding entries (those
   * also appear per-setting as `conflicts` with mod `"@game"`). `event` is
   * the engine controlmap event id, `title` reads like "Starfield
   * (Quicksave)", `name` is the bound OSF UI key name. Read-only — there is
   * no settings.set for the game's bindings. Absent when the runtime has no
   * vanilla data (feature off).
   */
  vanillaKeys?: Array<{ event: string; title: string; name: string }>;
  /**
   * Additive: settings
   * artifacts that FAILED to load, so a surface can tell the user instead of
   * a mod silently vanishing. `kind` is a stable enum string:
   * "schema-name" (file name fails the mod-id grammar; file skipped),
   * "schema-parse" (schema file unreadable / not an object; file skipped),
   * "values-parse" (a mod's values file was corrupt — quarantined on disk as
   * `<file>.bad`, defaults served; `mod` is set and the mod still loads).
   * `file` is a bare filename, `message` a human-readable reason (parse
   * position etc.). Omitted in the (normal) clean case.
   */
  loadErrors?: Array<{ kind: string; file: string; mod?: string; message: string }>;
}

export interface SettingsAckPayload {
  mod: string;
  key: string;
  ok: boolean;
  /**
   * ok:true — the authoritative committed value, post-clamp (protocol 1.0).
   * Compare against what you sent to detect clamping without a re-fetch.
   */
  value?: SettingValue;
  /**
   * ok:false — stable machine code (protocol 1.0): "unknown-setting" (mod or
   * key not in any loaded schema), "read-only" (requires-gated stub, or a
   * setting type this host doesn't know), "invalid-value" (validation
   * refused). `message` is the human sentence when the host adds one.
   */
  code?: string;
  message?: string;
}

/**
 * One committed value, pushed to every subscriber (any view that has sent
 * `settings.get`) on each store commit — a settings.set from any view, a
 * reset, a preset application, or a native-side write. This is how a mod's
 * own HUD reacts live to its settings with zero polling and zero native code
 * (subscribe with a single `settings.get` at startup). The value is
 * post-validation (clamped) — authoritative, not the caller's raw input.
 */
export interface SettingsChangedPayload {
  mod: string;
  key: string;
  value: SettingValue;
  /**
   * On `type:"key"` settings only (protocol 1.0): the setting's recomputed
   * conflict list — same shape and @game filtering as the `settings.data`
   * annotation, [] when the new binding is unique. Update badges in place
   * (collisions are symmetric — mirror the delta onto the named partners)
   * instead of re-fetching the registry.
   */
  conflicts?: Array<{ mod: string; key: string; title: string }>;
}

/**
 * The mod's values FILE write landed. Persistence is write-behind: a commit
 * notifies immediately via `settings.changed`, while the disk write coalesces
 * (~500ms per mod, guaranteed on menu close and shutdown) and confirms here.
 * Pushed to `settings.get` subscribers; the settings view drives its "Saved"
 * indicator off this. Purely informational — values are already authoritative
 * from `settings.changed`.
 */
export interface SettingsPersistedPayload {
  mod: string;
}

/**
 * A hotkey fired (protocol 1.0, mcm-design.md §9): the physical key currently
 * bound to the identified `type:"key"` setting was pressed during gameplay.
 * Pushed to every subscriber (any view that has sent `settings.get`) — filter
 * on `mod` (and `key`) and ignore the rest. This is how a mod's own HUD
 * implements "toggle myself on my hotkey" with zero native code. Suppressed
 * while the overlay captures input (typing in a settings field) or a rebind
 * capture is armed; rebinds re-route automatically.
 */
export interface UiHotkeyPayload {
  mod: string;
  key: string;
}

/** Result of a settings.captureKey: the captured key name, or cancelled (Esc / unbindable). */
export interface SettingsCapturedPayload {
  mod: string;
  key: string;
  name: string;       // OSF UI key name (e.g. "F9"); "" when cancelled
  cancelled: boolean; // true on Escape or an unbindable key — keep the old binding
  /**
   * Live-warn during capture (protocol 1.0, mcm-design.md §9): the OTHER
   * key-typed settings (any mod) already bound to the captured key — the
   * collisions this bind WOULD create, delivered before the view commits it.
   * Informational only — the bind is never rejected; warn, don't block.
   * Absent when the captured key is unique (and always on cancelled:true).
   */
  conflicts?: Array<{ mod: string; key: string; title: string }>;
}

/**
 * The runtime rejected an inbound message (protocol 1.0 shape). When the
 * offending ui.command carried a requestId it is echoed top-level, so the
 * shared helper rejects the matching request() promise with `code`.
 */
export interface UiErrorPayload {
  /** Stable machine code: "malformed-message" | "unknown-message-type" | "unknown-command". */
  code: string;
  /** Human sentence. */
  message: string;
  type?: string;     // present for "unknown-message-type"
  command?: string;  // present for "unknown-command"
}

/**
 * The uniform command outcome (protocol 1.0, api-freeze item 5). Sent ONLY
 * when the ui.command carried a requestId: verb commands with no reply type
 * of their own (close, menu.open, hud.show, ...) answer ok:true on success;
 * failures carry a stable `code` ("unknown-view", "capture-busy",
 * "unknown-setting", ...). A plugin-registered command acks ok:true =
 * delivered to the plugin's handler (richer replies are the plugin's own
 * message types). The shared helper resolves/rejects request() with this.
 */
export interface UiResultPayload {
  ok: boolean;
  command?: string;  // echo of the ui.command
  code?: string;     // ok:false — stable machine code
  message?: string;  // human sentence
}

/**
 * Reply to `game.get`. Each data provider nests under its own object
 * (protocol 1.0) — future providers appear as SIBLINGS of `calendar`.
 */
export interface GameDataPayload {
  /** In-game date/time from RE::Calendar. `available` is false before a save loads. */
  calendar: {
    available: boolean;
    day?: number;
    month?: number;
    year?: number;
    hour?: number;        // 0..24 (fractional)
    daysPassed?: number;
  };
}

/**
 * The receiving view was shown/hidden as the overlay's focused menu (pushed on
 * edges). Fires on overlay open/close AND on a `menu.open` view switch while
 * the overlay stays up (the outgoing view gets `visible:false`, the incoming
 * one `visible:true`). The reference views scope their "session undo" to a
 * visit off this.
 */
export interface UiVisibilityPayload {
  visible: boolean;
  /**
   * Why the edge fired: "overlay" = the overlay itself opened/closed;
   * "focus" = the overlay stayed up and only the focused menu changed
   * (hub -> panel navigation). Absent on runtimes older than this field —
   * treat absent as "overlay". Scope per-visit resets to "overlay" shows;
   * treat any `visible:false` as a real hide.
   */
  reason?: 'overlay' | 'focus';
}

/**
 * EXPERIMENTAL — gamepad navigation is explicitly "basic and being refined",
 * so this shape is exempt from the 1.0 stability guarantee until stabilized.
 *
 * Raw gamepad events, sent to the ACTIVE (focused) view while the overlay
 * captures input. Per-kind nesting (protocol 1.0): buttons and axes extend
 * inside their objects (triggers will join as axes.lt/rt; a second controller
 * as a `pad` index). Unless `osfui.gamepadRaw` was asserted, the runtime ALSO
 * applies its default mapping (D-pad and left stick -> arrows, A -> Enter,
 * B -> close, right stick -> scroll); raw mode makes these events the page's
 * alone.
 */
export type UiGamepadPayload =
  | { kind: "button"; button: { id: number; down: boolean } }
  | { kind: "stick"; axes: { lx: number; ly: number; rx: number; ry: number } };

/**
 * One entry per loaded (registered) surface. Reply to `views.get`; also pushed
 * unsolicited to every view that has sent `views.get` whenever any entry's
 * open/focused/loadState changes (a view torn down by crash-recovery drops out
 * of the list entirely).
 */
export interface ViewsDataPayload {
  views: Array<{
    id: string;
    title: string;
    description: string;    // manifest `description`, "" when absent
    mod: string;            // manifest `mod` — owning settings mod id ("" = standalone); groups the view onto that mod's page on the Mods surface
    kind: "menu" | "hud";
    interactive: boolean;   // derived from kind (menu=true, hud=false): may hold focus
    hub: boolean;           // manifest `hub` — false = hidden utility view, omit from catalogs
    targetVersion: string;  // manifest `targetVersion` — OSF UI version the view was authored against; "" if undeclared
    open: boolean;          // menu: on the stack; hud: shown
    focused: boolean;       // the top open menu (receives input)
    loadState: "loading" | "loaded" | "failed";
  }>;
}

/**
 * Dynamic data pushed from the owning mod's Papyrus script
 * (OSFUI.PushToView), delivered to every live view of that mod. `key` names
 * which piece of the mod's state this is — IGNORE keys you don't know
 * (additive contract), and compare `key` case-INSENSITIVELY: Papyrus string
 * interning means it can arrive cased differently than the script authored
 * it. The payload is the WHOLE current value for that key (state replacement,
 * not a delta); nothing is cached natively, so re-request state by firing a
 * `ready` ui.action whenever your page (re)loads.
 */
export interface DataPushPayload {
  mod: string;      // canonical lowercase pushing-mod id
  key: string;      // which state slice; mod-defined
  values: string[]; // the current value, as a list of strings
}

export type NativeToWebMessage =
  | BridgeEnvelope<"runtime.ready", RuntimeReadyPayload>
  | BridgeEnvelope<"data.push", DataPushPayload>
  | BridgeEnvelope<"runtime.pong", Record<string, never>>
  | BridgeEnvelope<"game.data", GameDataPayload>
  | BridgeEnvelope<"views.data", ViewsDataPayload>
  | BridgeEnvelope<"i18n.data", I18nDataPayload>
  | BridgeEnvelope<"settings.data", SettingsDataPayload>
  | BridgeEnvelope<"settings.ack", SettingsAckPayload>
  | BridgeEnvelope<"settings.changed", SettingsChangedPayload>
  | BridgeEnvelope<"settings.persisted", SettingsPersistedPayload>
  | BridgeEnvelope<"settings.captured", SettingsCapturedPayload>
  | BridgeEnvelope<"ui.hotkey", UiHotkeyPayload>
  | BridgeEnvelope<"ui.visibility", UiVisibilityPayload>
  | BridgeEnvelope<"ui.gamepad", UiGamepadPayload>
  | BridgeEnvelope<"ui.result", UiResultPayload>
  | BridgeEnvelope<"ui.error", UiErrorPayload>;

// ---------------------------------------------------------------------------
// Settings schema shapes (mirror docs/schema/settings-schema.schema.json)
// ---------------------------------------------------------------------------

export type SettingValue = boolean | number | string | string[];

/**
 * The FROZEN base type set (api-freeze-plan item 2). Colour is not a type —
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

export interface SettingsGroup {
	id?: string;
  label?: string;
  collapsed?: boolean;
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
  presets?: SettingsPreset[];
  inputContexts?: InputContext[];
  groups?: SettingsGroup[];
}

// ---------------------------------------------------------------------------
// The injected bridge object (present only when manifest grants nativeBridge).
// ---------------------------------------------------------------------------

export interface OSFUIBridge {
  /** web -> native. Pass a JSON string; prefer the helper's send()/request(). */
  postMessage(json: string): void;
  /**
   * native -> web, called with a JSON string. With the shared helper loaded
   * the helper OWNS this slot — never assign it yourself; use osfui.on().
   */
  onMessage?: (json: string) => void;
}

/**
 * The surface added by the shipped helper, data/OSFUI/views/shared/osfui.js
 * (protocol 1.0) — load it before your own script:
 *   <script src="../../shared/osfui.js"></script>
 * It decorates the same window.osfui object (creating a stub when no native
 * bridge is present, so these members exist even in a plain browser).
 */
export interface OSFUIHelper {
  /** True when a native bridge (or the harness mock) is present. */
  available(): boolean;
  /** Resolves with the runtime.ready payload. Never resolves standalone. */
  ready: Promise<RuntimeReadyPayload>;
  /** Fire-and-forget ui.command. Returns false when no bridge is present. */
  send(command: string, fields?: object): boolean;
  /**
   * ui.command with a generated requestId; resolves with the reply MESSAGE
   * ({ type, requestId, payload }). Rejects (Error with .code) on ui.error,
   * ui.result ok:false, timeout (default 10000 ms; 0 disables), or no bridge.
   */
  request(command: string, fields?: object, opts?: { timeoutMs?: number }): Promise<NativeToWebMessage & { requestId?: string }>;
  /** Subscribe to a native->web message type; returns the unsubscribe fn. */
  on(type: string, fn: (payload: unknown, message: NativeToWebMessage) => void): () => void;
	/** Current normalized locale ("en", "de", "pt-BR", ...). */
	locale(): string;
	/** Resolves after the first active-locale override catalog arrives. */
	i18nReady: Promise<I18nDataPayload | { locale: string; strings: Record<string, string> }>;
	/** Translate a stable structural address, falling back to inline English. */
	t(address: string, english: string, variables?: Record<string, string | number>): string;
	/** Apply data-i18n/data-i18n-* attributes below a DOM root. */
	localize(root?: ParentNode): void;
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
