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
  accent?: string;
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
  /** Data-root files copied into builds before generated OSF UI views. Defaults to "mod". */
  modRoot?: string;
  /** Build output directory. May point to a separate monorepo build tree. Defaults to "dist". */
  outDir?: string;
  /** Optional Papyrus build: loose scripts only, or scripts plus a Spriggit plugin. */
  papyrus?: {
    /** Compile loose PEX files without generating an ESM/ESP/ESL. */
    scriptsOnly: true;
  } | {
    /** Plugin filename written at the mod root, for example "AcmeWidgets.esm". */
    plugin: string;
    /** Spriggit text-representation directory, relative to the project root. */
    source: string;
  };
  mock?: string;
  /**
   * Extra Vite config merged into the `osfui dev` server (vite mergeConfig).
   * Dev only — `osfui build`/`check` never read it, so nothing here can
   * change shipped output. Typical use: resolve.alias, extra dev plugins.
   */
  vite?: Record<string, unknown> | ((env: { command: 'serve' }) => Record<string, unknown> | Promise<Record<string, unknown>>);
}

/** A request payload, or a wrapper that deliberately nests it under `payload`. */
export type MockResponse =
  | undefined | null | boolean | number | string | unknown[] | Record<string, unknown>
  | { $payload: unknown };

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
  targetVersion: string;
  legacyApi: boolean;
  viewUrl: string;
  version: string;
  bridgeVersion: string;
}

/** A native->web bridge protocol 2.0 envelope accepted by `MockContext.send`. */
export type Envelope =
  | { kind: 'ready'; payload: Record<string, unknown> }
  | { kind: 'state'; mod: string; key: string; value: unknown }
  | { kind: 'event'; name: string; payload?: unknown }
  | { kind: 'reply'; id: string; payload?: unknown }
  | { kind: 'error'; id: string; payload: { code: string; message: string } };

export type CommandKind = 'send' | 'request';

/** Settlement and diagnostics for one mocked web->native command. */
export interface CommandIo {
  resolve(payload?: unknown): void;
  reject(code: string, message: string): void;
  surface(code: string, message: string): void;
  report(direction: 'in' | 'out', message: unknown, level?: 'info' | 'warn'): void;
}

/**
 * A mock command handler. Return true to stop the chain (the command is
 * handled); anything else falls through to the scenario engine. Requests
 * settle through `io.resolve()` or `io.reject()`; sends never settle.
 */
export type CommandHandler = (
  kind: CommandKind,
  name: string,
  payload: Record<string, unknown>,
  io: CommandIo,
) => boolean | void | Promise<boolean | void>;

export type ToolKind = 'button' | 'toggle' | 'cycle' | 'select';

export interface ToolOption {
  value: string;
  label?: string;
}

/** A dev control rendered into the harness shell's toolbar. */
export interface ToolSpec {
  /** Unique per mock; lowercase letters, digits, hyphens. */
  id: string;
  kind: ToolKind;
  label: string;
  /** Tooltip. */
  title?: string;
  /** toggle: boolean; cycle/select: an option value. */
  value?: string | boolean;
  /** Required for cycle/select. */
  options?: (ToolOption | string)[];
  /** Highlight the control. */
  active?: boolean;
}

export type ToolPatch = Partial<Pick<ToolSpec, 'label' | 'title' | 'value' | 'options' | 'active'>>;

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
  /**
   * Register dev controls in the shell toolbar (replaces any previous
   * registration). `onInvoke` receives the tool id and, for toggles,
   * cycles and selects, the new value.
   */
  registerTools(tools: ToolSpec[], onInvoke?: (id: string, value?: string | boolean) => void): void;
  /** Update one registered tool in place (label, value, active, ...). */
  updateTool(id: string, patch: ToolPatch): void;
  /**
   * Receive files dropped onto the harness (already read as text).
   * `<modId>_<locale>.json` catalogs are merged into the scenario before
   * this fires; handlers only see everything else.
   */
  onDrop(handler: (files: { name: string; text: string }[]) => void): void;
  /**
   * Wrap the shared kit's `osfui.i18n.t` (applied lazily once the kit exists;
   * wraps compose in registration order). The pseudo locale is itself a wrap.
   */
  wrapT(wrap: (t: (address: string, english: string, vars?: unknown) => string)
           => (address: string, english: string, vars?: unknown) => string): void;
  /** Transient toast for mocked effects that happen outside the browser. */
  notify(text: string): void;
  /** A line in the shell traffic panel. */
  log(message: unknown, level?: 'info' | 'warn'): void;
}

export declare function defineConfig(config: OsfuiConfig): OsfuiConfig;
export declare function defineMock(mock: OsfuiMock): OsfuiMock;
