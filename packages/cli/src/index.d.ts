export type SurfaceKind = "menu" | "hud";

export interface ViewConfig {
  id: string;
  title?: string;
  description?: string;
  source?: string;
  entry?: string;
  kind?: SurfaceKind;
  width?: number;
  height?: number;
  transparent?: boolean;
  capturesInput?: boolean;
  pausesGame?: boolean;
  openOnStart?: boolean;
  order?: number;
  hub?: boolean;
  debugOnly?: boolean;
  readySignal?: boolean;
  targetVersion?: string;
  permissions?: {
    nativeBridge?: boolean;
    filesystem?: boolean;
    network?: boolean;
  };
}

export interface OsfuiConfig {
  modId: string;
  view?: ViewConfig;
  views?: ViewConfig[];
  outDir?: string;
  mock?: string;
}

export type MockResponse =
  | unknown
  | { $type: string; payload: unknown };

export interface MockScenario {
  state?: Record<string, unknown>;
  locale?: string;
  locales?: Record<string, Record<string, string>>;
  requests?: Record<string,
    MockResponse | ((payload: Record<string, unknown>) => MockResponse | Promise<MockResponse>)>;
}

export interface OsfuiMock extends MockScenario {
  scenarios?: Record<string, MockScenario>;
}

/** What the harness knows about the previewed view (mirrors /__osfui/meta.json). */
export interface HarnessViewMeta {
  modId: string;
  viewName: string;
  qualifiedId: string;
  title: string;
  width: number;
  height: number;
  transparent: boolean;
  nativeBridge: boolean;
  viewUrl: string;
  version: string;
  bridgeVersion: string;
}

/** A native->web bridge envelope. */
export interface Envelope {
  type: string;
  payload?: unknown;
  requestId?: string;
}

/**
 * A mock command handler. Return true to stop the chain (the command is
 * handled); anything else falls through to the scenario engine. `reply`
 * echoes the command's requestId.
 */
export type CommandHandler = (
  command: string,
  payload: Record<string, unknown>,
  reply: (type: string, payload?: unknown) => void,
  requestId: string,
) => boolean | void | Promise<boolean | void>;

/**
 * Handed to a mock module's `install(ctx)` export, which runs inside the view
 * page before the view boots. `install` may layer handlers over the scenario
 * from the default export, or take over `window.osfui.postMessage` wholesale
 * — the harness detects the takeover and only drains queued commands into it.
 * Workers are unavailable in mock code (the harness deletes those globals).
 */
export interface MockContext {
  meta: HarnessViewMeta;
  /** The view page's query string (the shell forwards its own params). */
  params: URLSearchParams;
  /** localStorage, or null in private mode. Persists across reloads. */
  storage: Storage | null;
  /** The module's parsed default export, if any. */
  scenario: OsfuiMock | null;
  /** Push a native->web envelope (logged to the shell traffic panel). */
  send(message: Envelope): void;
  /** Register a command handler ahead of the scenario engine. */
  onCommand(handler: CommandHandler): void;
  /** Transient toast for mocked effects that happen outside the browser. */
  notify(text: string): void;
  /** A line in the shell traffic panel. */
  log(message: unknown, level?: 'info' | 'warn'): void;
}

export declare function defineConfig(config: OsfuiConfig): OsfuiConfig;
export declare function defineMock(mock: OsfuiMock): OsfuiMock;
