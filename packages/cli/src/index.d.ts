export type ViewKind = 'menu' | 'hud';

export interface ViewConfig {
  id: string;
  title?: string;
  description?: string;
  kind?: ViewKind;
  width?: number;
  height?: number;
  transparent?: boolean;
  pausesGame?: boolean;
  targetVersion?: string;
  permissions?: { nativeBridge?: boolean };
}

export interface OsfuiConfig {
  modId: string;
  views: ViewConfig[];
  outDir?: string;
}

export type OsfuiMock = Record<string, never>;

export interface ToolOption {
  value: string;
  label?: string;
}

export interface SelectTool {
  id: string;
  kind: 'select';
  label: string;
  title?: string;
  value?: string;
  options: (ToolOption | string)[];
}

export interface MockContext {
  registerTools(
    tools: SelectTool[],
    onInvoke?: (id: string, value?: string) => void,
  ): void;
}

export declare function defineConfig(config: OsfuiConfig): OsfuiConfig;
export declare function defineMock(mock: OsfuiMock): OsfuiMock;
