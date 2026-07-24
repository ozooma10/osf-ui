// Mock session-health snapshots (System Health); dev only. Mirrors the
// runtime's `diagnostics.data` push.
//
// One named scenario per state the pane has to render, so the browser harness
// can walk clean -> warnings -> errors -> mixed -> resolved-only without
// needing a broken game. Cycled with the toolbar "Health" button or pinned with
// ?health=<name>.
//
// Times are session-relative seconds, exactly as native emits them.

import type { DiagnosticsDataPayload } from '@sdk';

export type MockHealth = DiagnosticsDataPayload;

const SYSTEM: MockHealth['system'] = {
  version: '1.4.0-mock',
  bridgeVersion: '1.4',
  renderer: 'webview2',
  compositor: 'd3d12',
  drawPath: 'ui-seam',
  frameGeneration: false,
  nativeFocus: true,
  locale: 'en',
  debugMode: false,
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
        context: { kind: 'view', targetVersion: '9.9.0', installedVersion: '1.4.0-mock' },
        occurrences: 1,
        firstAt: 0.6,
        lastAt: 0.6,
      },
    ],
  },

  errors: {
    system: { ...SYSTEM, drawPath: 'present', frameGeneration: true },
    issues: [
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
      {
        id: 'render.framegen-fallback',
        code: 'render.framegen-fallback',
        severity: 'warning',
        status: 'active',
        source: 'render',
        subject: 'd3d12',
        context: { compositor: 'd3d12', seamMode: false, frameGeneration: true },
        occurrences: 2,
        firstAt: 30.0,
        lastAt: 44.0,
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
        id: 'host.focus-stranded',
        code: 'host.focus-stranded',
        severity: 'warning',
        status: 'active',
        source: 'host',
        subject: 'webview2',
        context: { detail: 'focus watchdog re-asserting', renderer: 'webview2' },
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
        context: { detail: 'host announced 12 slots, capacity 8', renderer: 'webview2' },
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
};

/** Cycle order for the toolbar button. */
export const HEALTH_SCENARIOS = Object.keys(MOCK_HEALTH);
