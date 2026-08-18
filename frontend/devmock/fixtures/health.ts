// Mock health-registry snapshots (System Health); dev only. Mirrors the OSF UI
// runtime's `osfui/diagnostics` state.
//
// One named scenario per state the destination has to render, so the browser
// harness can walk clean -> warnings -> errors -> mixed -> resolved-only without
// needing a broken game. Cycled with the toolbar "Health" button or selected with
// ?health=<name>.
//
// Times are session-relative seconds, exactly as native emits them.

import type { DiagnosticsData } from '@sdk';

export type MockHealth = DiagnosticsData;

const SYSTEM: MockHealth['system'] = {
  version: '2.0.0-mock',
  bridgeVersion: '2.0',
  renderer: 'webview2',
  compositor: 'd3d12',
  drawPath: 'ui-pass',
  frameGeneration: false,
  nativeFocus: true,
  locale: 'en',
  devMode: false,
};

/**
 * The scenarios, in cycle order. `clean` is first so the harness opens on the
 * calm state — the one a player should normally see.
 */
export const MOCK_HEALTH: Record<string, MockHealth> = {
  clean: { system: SYSTEM, issues: [] },

  warnings: {
    system: SYSTEM,
    issues: [
      {
        id: 'settings.values-parse:acme.kit',
        code: 'settings.values-parse',
        severity: 'warning',
        status: 'active',
        source: 'settings',
        subject: 'acme.kit',
        context: { file: 'acme.kit.json', message: 'parse error at line 4, column 12' },
        occurrences: 1,
        firstAt: 2.4,
        lastAt: 2.4,
      },
      {
        id: 'compat.needs-newer-osfui:view:future.mod/panel',
        code: 'compat.needs-newer-osfui',
        severity: 'warning',
        status: 'active',
        source: 'compat',
        subject: 'future.mod/panel',
        context: { kind: 'view', targetVersion: '9.9.0', installedVersion: '2.0.0-mock' },
        occurrences: 1,
        firstAt: 0.6,
        lastAt: 0.6,
      },
    ],
  },

  errors: {
    system: { ...SYSTEM, drawPath: 'unavailable', frameGeneration: true },
    issues: [
      {
        id: 'settings.schema-parse:rogue.mod',
        code: 'settings.schema-parse',
        severity: 'error',
        status: 'active',
        source: 'settings',
        subject: 'rogue.mod',
        context: { file: 'rogue.mod.json', message: 'unexpected token at line 1' },
        occurrences: 1,
        firstAt: 0.3,
        lastAt: 0.3,
      },
      {
        id: 'view.load-failed:broken.mod/panel',
        code: 'view.load-failed',
        severity: 'error',
        status: 'active',
        source: 'views',
        subject: 'broken.mod/panel',
        context: {
          errorCode: -6,
          description: 'ERR_FILE_NOT_FOUND',
          attemptsLeft: 0,
        },
        occurrences: 4,
        firstAt: 3.1,
        lastAt: 25.8,
      },
    ],
  },

  /** Every severity at once, plus history — the densest layout the pane gets. */
  mixed: {
    system: SYSTEM,
    issues: [
      {
        id: 'settings.schema-parse:rogue.mod',
        code: 'settings.schema-parse',
        severity: 'error',
        status: 'active',
        source: 'settings',
        subject: 'rogue.mod',
        context: { file: 'rogue.mod.json', message: 'unexpected token at line 1' },
        occurrences: 1,
        firstAt: 0.3,
        lastAt: 0.3,
      },
      {
        id: 'host.ring-truncated',
        code: 'host.ring-truncated',
        severity: 'warning',
        status: 'active',
		source: 'host',
        subject: 'webview2',
		context: { detail: 'browser host announced 12 slots, capacity 8', renderer: 'webview2' },
        occurrences: 7,
        firstAt: 12.0,
        lastAt: 96.5,
      },
      {
        id: 'view.load-retrying:slow.mod/panel',
        code: 'view.load-retrying',
        severity: 'warning',
        status: 'active',
        source: 'views',
        subject: 'slow.mod/panel',
        context: { errorCode: -105, description: 'ERR_NAME_NOT_RESOLVED', attemptsLeft: 2 },
        occurrences: 1,
        firstAt: 88.0,
        lastAt: 88.0,
      },
      {
        id: 'view.load-failed:fixed.mod/panel',
        code: 'view.load-failed',
        severity: 'error',
        status: 'resolved',
        source: 'views',
        subject: 'fixed.mod/panel',
        context: { errorCode: -6, description: 'ERR_FILE_NOT_FOUND', attemptsLeft: 0 },
        occurrences: 3,
        firstAt: 5.0,
        lastAt: 18.0,
        resolvedAt: 41.2,
      },
    ],
  },

  /** Everything cleared: the summary is nominal but the history is not empty. */
  resolved: {
    system: SYSTEM,
    issues: [
      {
        id: 'host.ring-truncated',
        code: 'host.ring-truncated',
        severity: 'warning',
        status: 'resolved',
		source: 'host',
        subject: 'webview2',
		context: { detail: 'browser host announced 12 slots, capacity 8', renderer: 'webview2' },
        occurrences: 1,
        firstAt: 1.2,
        lastAt: 1.2,
        resolvedAt: 9.9,
      },
      {
        id: 'settings.values-parse:acme.kit',
        code: 'settings.values-parse',
        severity: 'warning',
        status: 'resolved',
        source: 'settings',
        subject: 'acme.kit',
        context: { file: 'acme.kit.json', message: 'parse error at line 4, column 12' },
        occurrences: 2,
        firstAt: 2.4,
        lastAt: 6.0,
        resolvedAt: 14.5,
      },
    ],
  },

  /**
   * Every code the copy table knows, one card each, plus one it does not — the
   * proof-reading scenario. Nothing here is a realistic session; it exists so
   * the whole catalog of titles, impact/next copy and action rows can be read
   * side by side (and run through `?locale=pseudo`) without provoking nine
   * distinct failures in a live game. Keep it in sync with COPY in
   * `src/lib/settings/health.ts` — a code missing here is an issue nobody
   * has ever looked at.
   *
   * Severities and context keys match what native actually emits; see
   * HealthReconciler.cpp (settings/compat/document loading), Runtime.cpp
   * (input/view protocol), and WebView2HostWebRenderer.cpp (`host.*` compatibility issue codes).
   */
  catalog: {
    system: { ...SYSTEM, frameGeneration: true, drawPath: 'unavailable', devMode: true },
    issues: [
      {
        id: 'settings.schema-name:Bad Name.json',
        code: 'settings.schema-name',
        severity: 'error',
        status: 'active',
        source: 'settings',
        subject: 'Bad Name.json',
        context: { file: 'Bad Name.json', message: 'not a valid mod id' },
        occurrences: 1,
        firstAt: 0.2,
        lastAt: 0.2,
      },
      {
        id: 'settings.schema-parse:rogue.mod',
        code: 'settings.schema-parse',
        severity: 'error',
        status: 'active',
        source: 'settings',
        subject: 'rogue.mod',
        context: { file: 'rogue.mod.json', message: 'unexpected token at line 1' },
        occurrences: 1,
        firstAt: 0.3,
        lastAt: 0.3,
      },
      {
        id: 'input.control-map-unavailable',
        code: 'input.control-map-unavailable',
        severity: 'warning',
        status: 'active',
        source: 'input',
        subject: 'Starfield ControlMap',
        context: { gameVersion: '1.15.222.0', reason: 'unsupported ControlMap layout' },
        occurrences: 1,
        firstAt: 0.4,
        lastAt: 0.4,
      },
      {
        id: 'view.load-failed:broken.mod/panel',
        code: 'view.load-failed',
        severity: 'error',
        status: 'active',
        source: 'views',
        subject: 'broken.mod/panel',
        context: { errorCode: -6, description: 'ERR_FILE_NOT_FOUND', attemptsLeft: 0 },
        occurrences: 4,
        firstAt: 3.1,
        lastAt: 25.8,
      },
      {
        id: 'settings.values-parse:acme.kit',
        code: 'settings.values-parse',
        severity: 'warning',
        status: 'active',
        source: 'settings',
        subject: 'acme.kit',
        context: { file: 'acme.kit.json', message: 'parse error at line 4, column 12' },
        occurrences: 1,
        firstAt: 2.4,
        lastAt: 2.4,
      },
      {
        id: 'settings.hotkey-target:acme.kit.startScene',
        code: 'settings.hotkey-target',
        severity: 'error',
        status: 'active',
        source: 'settings',
        subject: 'acme.kit.startScene',
        context: {
          script: 'Acme_Hotkeys',
          function: 'StartScene',
          message: 'Papyrus rejected the call',
        },
        occurrences: 1,
        firstAt: 3.0,
        lastAt: 3.0,
      },
      {
        id: 'view.load-retrying:slow.mod/panel',
        code: 'view.load-retrying',
        severity: 'warning',
        status: 'active',
        source: 'views',
        subject: 'slow.mod/panel',
        context: { errorCode: -105, description: 'ERR_NAME_NOT_RESOLVED', attemptsLeft: 2 },
        occurrences: 1,
        firstAt: 88.0,
        lastAt: 88.0,
      },
      {
        id: 'view.protocol-misuse:buggy.mod/panel',
        code: 'view.protocol-misuse',
        severity: 'warning',
        status: 'active',
        source: 'views',
        subject: 'buggy.mod/panel',
        context: { code: 'unknown-endpoint', count: 10 },
        occurrences: 1,
        firstAt: 42.0,
        lastAt: 42.0,
      },
      {
        id: 'host.ring-truncated',
        code: 'host.ring-truncated',
        severity: 'warning',
        status: 'active',
		source: 'host',
        subject: 'webview2',
		context: { detail: 'browser host announced 12 slots, capacity 8', renderer: 'webview2' },
        occurrences: 1,
        firstAt: 1.2,
        lastAt: 1.2,
      },
      {
        id: 'compat.needs-newer-osfui:view:future.mod/panel',
        code: 'compat.needs-newer-osfui',
        severity: 'warning',
        status: 'active',
        source: 'compat',
        subject: 'future.mod/panel',
        context: { kind: 'view', targetVersion: '9.9.0', installedVersion: '2.0.0-mock' },
        occurrences: 1,
        firstAt: 0.6,
        lastAt: 0.6,
      },
      {
        id: 'compat.pre-2-view:view:legacy.mod/panel',
        code: 'compat.pre-2-view',
        severity: 'warning',
        status: 'active',
        source: 'compat',
        subject: 'legacy.mod/panel',
        context: {
          kind: 'view',
          consumer: 'legacy.mod/panel',
          targetVersion: '1.9.0',
          installedVersion: '2.0.0-mock',
          removalVersion: '2.1.0',
        },
        occurrences: 1,
        firstAt: 0.7,
        lastAt: 0.7,
      },
      {
        id: 'compat.legacy-api:plugin:SuitProtocol.dll',
        code: 'compat.legacy-api',
        severity: 'warning',
        status: 'active',
        source: 'compat',
        subject: 'SuitProtocol.dll',
        context: {
          kind: 'plugin',
          consumer: 'SuitProtocol.dll',
          abi: '1.7',
          installedVersion: '2.0.0-mock',
          removalVersion: '2.1.0',
        },
        occurrences: 1,
        firstAt: 0.8,
        lastAt: 0.8,
      },
      {
        id: 'compat.legacy-papyrus:Papyrus mod:ak.autosort',
        code: 'compat.legacy-papyrus',
        severity: 'warning',
        status: 'active',
        source: 'compat',
        subject: 'ak.autosort',
        context: {
          kind: 'Papyrus mod',
          consumer: 'ak.autosort',
          api: '1.x natives',
          installedVersion: '2.0.0-mock',
          removalVersion: '2.1.0',
        },
        occurrences: 1,
        firstAt: 0.9,
        lastAt: 0.9,
      },
      {
        id: 'compat.unsupported-api:plugin:FuturePlugin.dll',
        code: 'compat.unsupported-api',
        severity: 'error',
        status: 'active',
        source: 'compat',
        subject: 'FuturePlugin.dll',
        context: {
          kind: 'plugin',
          consumer: 'FuturePlugin.dll',
          abi: '3.0',
          installedVersion: '2.0.0-mock',
        },
        occurrences: 1,
        firstAt: 1.0,
        lastAt: 1.0,
      },
      // A code this build predates: must render through GENERIC_COPY with its
      // context visible, never as a blank card.
      {
        id: 'future.unheard-of:something',
        code: 'future.unheard-of',
        severity: 'error',
        status: 'active',
        source: 'future',
        subject: 'something',
        context: { detail: 'emitted by a newer OSF UI runtime than this frontend' },
        occurrences: 1,
        firstAt: 50.0,
        lastAt: 50.0,
      },
      // A report another mod raised through the native ABI (1.7). The OSF UI
      // runtime namespaces its code and assigns the source, and MOD_COPY names that mod
      // rather than telling the player to update OSF UI — which would change
      // nothing here.
      {
        id: 'osf.animation:pack-parse:highlights',
        code: 'osf.animation:catalog.pack-parse',
        severity: 'error',
        status: 'active',
        source: 'osf.animation',
        subject: 'highlights',
        context: { file: 'highlights.json', line: 12, message: 'unexpected token' },
        occurrences: 2,
        firstAt: 4.0,
        lastAt: 61.0,
      },
      // One resolved card so the history section is populated too.
      {
        id: 'view.load-failed:fixed.mod/panel',
        code: 'view.load-failed',
        severity: 'error',
        status: 'resolved',
        source: 'views',
        subject: 'fixed.mod/panel',
        context: { errorCode: -6, description: 'ERR_FILE_NOT_FOUND', attemptsLeft: 0 },
        occurrences: 3,
        firstAt: 5.0,
        lastAt: 18.0,
        resolvedAt: 41.2,
      },
    ],
  },
};

/** Cycle order for the toolbar button. */
export const HEALTH_SCENARIOS = Object.keys(MOCK_HEALTH);
