export type ViewKind = 'menu' | 'hud';

export interface ViewConfig {
  id: string;
  title?: string;
  description?: string;
  source?: string;
  entry?: string;
  kind?: ViewKind;
  width?: number;
  height?: number;
  transparent?: boolean;
  capturesInput?: boolean;
  pausesGame?: boolean;
  openOnStart?: boolean;
  order?: number;
  hub?: boolean;
  debugOnly?: boolean;
  targetVersion?: string;
  permissions?: { nativeBridge?: boolean };
}

export interface OsfuiConfig {
  modId: string;
  view?: ViewConfig;
  views?: ViewConfig[];
  outDir?: string;
  mock?: string;
  vite?: Record<string, unknown> | ((env: { command: 'serve' }) =>
    Record<string, unknown> | Promise<Record<string, unknown>>);
}

export type JsonPrimitive = null | boolean | number | string;
export type JsonValue = JsonPrimitive | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };
export type MockReply = JsonValue | ((payload: JsonObject) => JsonValue | Promise<JsonValue>);

export interface MockScenario {
  state?: Record<string, unknown>;
  locale?: string;
  locales?: Record<string, Record<string, string>>;
  /** Raw reply values keyed by local owner endpoint (qualified names also work). */
  requests?: Record<string, MockReply>;
}

export interface OsfuiMock extends MockScenario {
  scenarios?: Record<string, MockScenario>;
}

export interface HarnessViewMeta {
  modId: string;
  viewName: string;
  qualifiedId: string;
  title: string;
  width: number;
  height: number;
  transparent: boolean;
  nativeBridge: boolean;
  targetVersion: string;
  viewUrl: string;
  version: string;
  bridgeVersion: string;
}

export type Envelope =
  | { kind: 'ready'; payload: JsonObject }
  | { kind: 'state'; mod: string; key: string; value: unknown }
  | { kind: 'event'; name: string; payload?: unknown }
  | { kind: 'reply'; id: string; payload?: unknown }
  | { kind: 'error'; id: string; payload: { code: string; message: string } };

export type EndpointKind = 'send' | 'request';

export interface EndpointIo {
  /** Resolve with the raw bridge reply value; false, 0, empty string, and null are preserved. */
  resolve(payload?: unknown): void;
  reject(code: string, message: string): void;
  surface(code: string, message: string): void;
  report(direction: 'in' | 'out', message: unknown, level?: 'info' | 'warn'): void;
}

export type EndpointHandler = (
  kind: EndpointKind,
  name: string,
  payload: JsonObject,
  io: EndpointIo,
) => boolean | void | Promise<boolean | void>;

export type ToolKind = 'button' | 'toggle' | 'cycle' | 'select';

export interface ToolOption {
  value: string;
  label?: string;
}

export interface ToolSpec {
  id: string;
  kind: ToolKind;
  label: string;
  title?: string;
  value?: string | boolean;
  options?: (ToolOption | string)[];
  active?: boolean;
}

export type ToolPatch = Partial<Pick<ToolSpec,
  'label' | 'title' | 'value' | 'options' | 'active'>>;

export interface MockContext {
  meta: HarnessViewMeta;
  params: URLSearchParams;
  storage: Storage | null;
  scenario: OsfuiMock;
  /** Push a native-to-web ready/state/event/reply/error envelope. */
  send(message: Envelope): void;
  /** Handle local owner endpoints or explicitly qualified cross-mod endpoints. */
  onEndpoint(handler: EndpointHandler): void;
  registerTools(
    tools: ToolSpec[],
    onInvoke?: (id: string, value?: string | boolean) => void,
  ): void;
  updateTool(id: string, patch: ToolPatch): void;
  notify(text: string): void;
}

export declare function defineConfig(config: OsfuiConfig): OsfuiConfig;
export declare function defineMock(mock: OsfuiMock): OsfuiMock;
