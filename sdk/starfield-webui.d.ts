/**
 * TypeScript definitions for the StarfieldWebUI native <-> web bridge.
 *
 * Bridge protocol version: 0.1 (UNSTABLE — minor bumps may break views until
 * 1.0). Negotiate against the `bridgeVersion` field of the `runtime.ready`
 * message. Keep in lockstep with:
 *   - docs/authoring-views.md          (prose reference)
 *   - docs/schema/*.schema.json        (manifest + settings-schema validation)
 *   - src/core/Version.h               (kBridgeProtocolVersion)
 *   - src/runtime/MessageBridge.cpp    (envelope + dispatch)
 *
 * Usage: this is an ambient declaration file — drop it into your view project
 * (or reference it via tsconfig "types"/"include") and `window.starfield` is
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
  | { command: "close" }
  | { command: "setVisible"; visible: boolean }
  | { command: "log"; text: string }
  | { command: "ping" }
  | { command: "game.get" }
  | { command: "settings.get" }
  | { command: "settings.set"; mod: string; key: string; value: SettingValue }
  | { command: "settings.reset"; mod: string; key?: string };

export type WebToNativeMessage = BridgeEnvelope<"ui.command", UiCommand>;

// ---------------------------------------------------------------------------
// native -> web messages (assign window.starfield.onMessage and switch on type)
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

export type NativeToWebMessage =
  | BridgeEnvelope<"runtime.ready", RuntimeReadyPayload>
  | BridgeEnvelope<"runtime.pong", Record<string, never>>
  | BridgeEnvelope<"game.data", GameDataPayload>
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

export interface StarfieldBridge {
  /** web -> native. Pass a JSON string; the typed helper below is recommended. */
  postMessage(json: string): void;
  /** native -> web. Assign this; the runtime calls it with a JSON string. */
  onMessage?: (json: string) => void;
}

declare global {
  interface Window {
    /** Undefined unless the active view's manifest sets permissions.nativeBridge. */
    starfield?: StarfieldBridge;
  }
}

export {};
