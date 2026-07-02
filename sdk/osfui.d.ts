/**
 * TypeScript definitions for the OSF UI native <-> web bridge.
 *
 * Bridge protocol version: 0.2 (UNSTABLE — minor bumps may break views until
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
  | { command: "settings.get" }
  | { command: "settings.set"; mod: string; key: string; value: SettingValue }
  | { command: "settings.reset"; mod: string; key?: string };

export type WebToNativeMessage = BridgeEnvelope<"ui.command", UiCommand>;

// ---------------------------------------------------------------------------
// native -> web messages (assign window.osfui.onMessage and switch on type)
// ---------------------------------------------------------------------------

export interface RuntimeReadyPayload {
  game: string;        // "Starfield"
  plugin: string;      // plugin metadata name
  version: string;     // plugin version (kPluginVersion)
  bridgeVersion: string; // protocol version (kBridgeProtocolVersion) — negotiate on this
}

export interface SettingsDataPayload {
  mods: Array<{
    id: string;
    title: string;
    schema: SettingsSchema;
    values: Record<string, SettingValue>;
  }>;
}

export interface SettingsAckPayload {
  mod: string;
  key: string;
  ok: boolean; // false => rejected or clamped server-side
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
  | BridgeEnvelope<"ui.error", UiErrorPayload>;

// ---------------------------------------------------------------------------
// Settings schema shapes (mirror docs/schema/settings-schema.schema.json)
// ---------------------------------------------------------------------------

export type SettingValue = boolean | number | string;

export type SettingType = "bool" | "int" | "float" | "enum" | "string";

export interface Setting {
  key: string;
  label?: string;
  type: SettingType;
  default?: SettingValue;
  min?: number;
  max?: number;
  step?: number;
  options?: string[]; // required when type === "enum"
}

export interface SettingsGroup {
  label?: string;
  settings: Setting[];
}

export interface SettingsSchema {
  id?: string;
  title?: string;
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
