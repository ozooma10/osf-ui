/**
 * TypeScript definitions for the OSF UI native <-> web bridge.
 *
 * Bridge protocol version: 0.4 (UNSTABLE — minor bumps may break views until
 * 1.0). Negotiate against the `bridgeVersion` field of the `runtime.ready`
 * message. Keep in lockstep with:
 *   - docs/authoring-views.md          (prose reference)
 *   - docs/schema/*.schema.json        (manifest + settings-schema validation)
 *   - src/core/Version.h               (kBridgeProtocolVersion)
 *   - src/runtime/MessageBridge.cpp    (envelope + dispatch)
 *
 * Usage: this is an ambient declaration file — drop it into your view project
 * (or reference it via tsconfig "types"/"include") and `window.osfui` is
 * typed globally. There is no runtime package to install.
 */

/** Every message in both directions is JSON text of this shape. */
export interface BridgeEnvelope<TType extends string = string, TPayload = unknown> {
  type: TType;
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
  /** Read the settings registry; replies `settings.data` and SUBSCRIBES the caller: committed values arrive as `settings.changed`, registry shape changes re-send `settings.data`. */
  | { command: "settings.get" }
  | { command: "settings.set"; mod: string; key: string; value: SettingValue }
  | { command: "settings.reset"; mod: string; key?: string }
  /** Arm native key-rebind capture; the next key press returns as settings.captured. Any type:"key" setting may be captured. */
  | { command: "settings.captureKey"; mod: string; key: string };

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
  version: string;     // plugin version (kPluginVersion)
  bridgeVersion: string; // protocol version (kBridgeProtocolVersion) — negotiate on this
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
  }>;
  /**
   * The game's own key bindings (protocol 0.4, mcm-design §9 "vanilla
   * hotkeys") — the FULL curated table, not just colliding entries (those
   * also appear per-setting as `conflicts` with mod `"@game"`). `event` is
   * the engine controlmap event id, `title` reads like "Starfield
   * (Quicksave)", `name` is the bound OSF UI key name. Read-only — there is
   * no settings.set for the game's bindings. Absent when the runtime has no
   * vanilla data (feature off, or a pre-0.4 runtime).
   */
  vanillaKeys?: Array<{ event: string; title: string; name: string }>;
}

export interface SettingsAckPayload {
  mod: string;
  key: string;
  ok: boolean; // false => rejected or clamped server-side
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
 * A hotkey fired (protocol 0.4, mcm-design.md §9): the physical key currently
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
   * Live-warn during capture (protocol 0.4, mcm-design.md §9): the OTHER
   * key-typed settings (any mod) already bound to the captured key — the
   * collisions this bind WOULD create, delivered before the view commits it.
   * Informational only — the bind is never rejected; warn, don't block.
   * Absent when the captured key is unique (and always on cancelled:true).
   */
  conflicts?: Array<{ mod: string; key: string; title: string }>;
}

export interface UiErrorPayload {
  reason: string;    // "malformed message" | "unknown message type" | "unknown command"
  type?: string;     // present for "unknown message type"
  command?: string;  // present for "unknown command"
}

/** In-game date/time from RE::Calendar. `available` is false before a save loads. */
export interface GameDataPayload {
  available: boolean;
  day?: number;
  month?: number;
  year?: number;
  hour?: number;        // 0..24 (fractional)
  daysPassed?: number;
}

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
    kind: "menu" | "hud";
    interactive: boolean;
    hub: boolean;           // manifest `hub` — false = hidden utility view, omit from catalogs
    open: boolean;          // menu: on the stack; hud: shown
    focused: boolean;       // the top open menu (receives input)
    loadState: "loading" | "loaded" | "failed";
  }>;
}

export type NativeToWebMessage =
  | BridgeEnvelope<"runtime.ready", RuntimeReadyPayload>
  | BridgeEnvelope<"runtime.pong", Record<string, never>>
  | BridgeEnvelope<"game.data", GameDataPayload>
  | BridgeEnvelope<"views.data", ViewsDataPayload>
  | BridgeEnvelope<"settings.data", SettingsDataPayload>
  | BridgeEnvelope<"settings.ack", SettingsAckPayload>
  | BridgeEnvelope<"settings.changed", SettingsChangedPayload>
  | BridgeEnvelope<"settings.persisted", SettingsPersistedPayload>
  | BridgeEnvelope<"settings.captured", SettingsCapturedPayload>
  | BridgeEnvelope<"ui.hotkey", UiHotkeyPayload>
  | BridgeEnvelope<"ui.error", UiErrorPayload>;

// ---------------------------------------------------------------------------
// Settings schema shapes (mirror docs/schema/settings-schema.schema.json)
// ---------------------------------------------------------------------------

export type SettingValue = boolean | number | string;

export type SettingType = "bool" | "int" | "float" | "enum" | "string" | "key";

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
  options?: string[]; // required when type === "enum"
  optionLabels?: string[]; // display labels parallel to options; stored value stays the option
  widget?: WidgetHint;
  format?: NumberFormat;
  requires?: RequiresKind;
  visibleWhen?: Condition;
  enabledWhen?: Condition;
  /**
   * RUNTIME-INJECTED, never authored (protocol 0.4): on a `type:"key"`
   * setting in a `settings.data` document, the OTHER key-typed settings
   * (any mod) currently bound to the same physical key. Informational only —
   * the runtime never rejects a colliding bind; render a warning badge.
   * Absent when the binding is unique.
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
  text: string;
  style?: "info" | "warn" | "danger";
  visibleWhen?: Condition;
}

/** Static image, resolved relative to the mod's own views/<id>/ folder. */
export interface ImageItem {
  type: "image";
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
  label?: string;
  collapsed?: boolean;
  visibleWhen?: Condition;
  settings: SettingsItem[];
}

/** Author-shipped value set, applied as a batch of validated settings.set. */
export interface SettingsPreset {
  label: string;
  description?: string;
  values: Record<string, SettingValue>;
}

export interface SettingsSchema {
  id?: string;
  title?: string;
  description?: string;  // one-line blurb shown under the title in the detail pane
  version?: number;      // schema version (default 0); native stamps it as $schemaVersion + logs a version move (§11). Renderer ignores it.
  accent?: string;       // per-mod accent "#rrggbb"/"#rrggbbaa"
  presets?: SettingsPreset[];
  groups?: SettingsGroup[];
}

// ---------------------------------------------------------------------------
// The injected bridge object (present only when manifest grants nativeBridge).
// ---------------------------------------------------------------------------

export interface OSFUIBridge {
  /** web -> native. Pass a JSON string; the typed helper below is recommended. */
  postMessage(json: string): void;
  /** native -> web. Assign this; the runtime calls it with a JSON string. */
  onMessage?: (json: string) => void;
}

declare global {
  interface Window {
    /** Undefined unless the active view's manifest sets permissions.nativeBridge. */
    osfui?: OSFUIBridge;
  }
}

export {};
