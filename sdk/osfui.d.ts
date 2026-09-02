/** OSF UI 2.0 web-view contract. Settings are intentionally not exposed. */

export type JsonPrimitive = null | boolean | number | string;
export type JsonValue = JsonPrimitive | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };

export interface BridgeError extends Error {
  code: string;
  payload?: JsonValue;
}

export interface RequestOptions {
  /** Client timeout in milliseconds. Defaults to 10 seconds; 0 disables it. */
  timeoutMs?: number;
}

export interface PapyrusEndpointPayload extends JsonObject {
  args: JsonValue[];
}

export interface OSFUIState {
  /** Return the latest retained value, or undefined when none has arrived. */
  get<T extends JsonValue = JsonValue>(key: string): T | undefined;
  /** Subscribe and synchronously replay the current value when available. */
  on<T extends JsonValue = JsonValue>(key: string, handler: (value: T) => void): () => void;
}

export interface OSFUIBridge {
  /** Low-level injected transport used by the shared helper. */
  postMessage(json: string): void;
  /** Low-level inbound transport callback owned by the shared helper. */
  onMessage?: (json: string) => void;

  /** Post a one-way named JSON payload. */
  send(name: string, payload?: JsonObject): boolean;
  /** Papyrus-friendly shorthand; arguments are wrapped as { args: [...] }. */
  send(name: string, ...args: JsonValue[]): boolean;

  /** Request a reply payload from a registered endpoint. */
  request<T extends JsonValue = JsonValue>(
    name: string,
    payload?: JsonObject,
    options?: RequestOptions,
  ): Promise<T>;
  /** Papyrus-friendly shorthand; arguments are wrapped as { args: [...] }. */
  request<T extends JsonValue = JsonValue>(name: string, ...args: JsonValue[]): Promise<T>;

  /** Subscribe to a transient event. Events are never retained or replayed. */
  on<T extends JsonValue = JsonValue>(name: string, handler: (payload: T) => void): () => void;
  /** Retained state scoped to this view's owning mod. */
  state: OSFUIState;
}

export type OSFUIHelper = OSFUIBridge;

export interface ReadyEnvelope {
  kind: "ready";
  payload: { mod: string; view: string; protocolVersion: 2 };
}
export interface StateEnvelope {
  kind: "state";
  mod: string;
  key: string;
  value: JsonValue;
}
export interface EventEnvelope {
  kind: "event";
  name: string;
  payload: JsonValue;
}
export interface ReplyEnvelope {
  kind: "reply";
  id: string;
  payload: JsonValue;
}
export interface ErrorEnvelope {
  kind: "error";
  id: string;
  payload: { code: string; message?: string; [key: string]: JsonValue | undefined };
}
export type InboundEnvelope = ReadyEnvelope | StateEnvelope | EventEnvelope | ReplyEnvelope | ErrorEnvelope;

export interface SendEnvelope {
  kind: "send";
  name: string;
  payload: JsonObject;
}
export interface RequestEnvelope {
  kind: "request";
  name: string;
  id: string;
  payload: JsonObject;
}
export type OutboundEnvelope = SendEnvelope | RequestEnvelope;

/** Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-name>/manifest.json */
export interface ViewManifestV1 {
  $schema?: string;
  manifestVersion: 1;
  mod?: string;
  title?: string;
  description?: string;
  entry?: string;
  width?: number;
  height?: number;
  transparent?: boolean;
  kind?: "menu" | "hud";
  capturesInput?: boolean;
  pausesGame?: boolean;
  openOnStart?: boolean;
  order?: number;
  debugOnly?: boolean;
  [forwardCompatibleProperty: string]: unknown;
}

declare global {
  interface Window {
    osfui?: OSFUIBridge;
  }
}

export {};
