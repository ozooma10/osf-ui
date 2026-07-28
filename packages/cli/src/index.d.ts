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

export declare function defineConfig(config: OsfuiConfig): OsfuiConfig;
export declare function defineMock(mock: OsfuiMock): OsfuiMock;
