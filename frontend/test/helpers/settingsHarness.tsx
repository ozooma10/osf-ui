// Fake bridge + mount helpers shared by the settings-view test suites.
// Test only.

import { render } from 'preact';
import { act } from 'preact/test-utils';
import { nullBridge, type Bridge } from '@lib/bridge';
import { App } from '@views/osfui/settings/App';

type Listener = (payload: unknown, message: unknown) => void;

export interface FakeBridge extends Bridge {
  emit(type: string, payload: unknown, message?: unknown): void;
  sent: Array<{ command: string; fields?: Record<string, unknown> }>;
  requests: Array<{ command: string; fields?: Record<string, unknown>; opts?: unknown }>;
  settle(index: number, value: unknown): void;
  reject(index: number, err: unknown): void;
  countRequests(command: string): number;
  /** Index of the Nth (0-based) request matching `command`, or -1. */
  indexOf(command: string, nth?: number): number;
}

export interface MakeBridgeOptions {
  version?: string;
  available?: boolean;
  /**
   * Never resolve `ready()`. Models a transport that missed the one-shot
   * `runtime.ready` greeting (the WebView2 host process starts long after the
   * runtime emits it). The view must still work: nothing but the version badge
   * may depend on that handshake.
   */
  readyNeverResolves?: boolean;
}

export function makeBridge(opts: MakeBridgeOptions = {}): FakeBridge {
  const listeners = new Map<string, Set<Listener>>();
  const pending: Array<{ resolve: (v: unknown) => void; reject: (e: unknown) => void }> = [];
  const version = opts.version ?? '1.0.0';
  const available = opts.available ?? true;

  const bridge: FakeBridge = {
    ...nullBridge,
    available: () => available,
    sent: [],
    requests: [],
    ready() {
      return opts.readyNeverResolves
        ? (new Promise(() => {}) as never)
        : (Promise.resolve({ version }) as never);
    },
    send(command, fields) {
      bridge.sent.push(fields === undefined ? { command } : { command, fields });
      return available;
    },
    request(command: string, fields?: Record<string, unknown>, o?: unknown) {
      bridge.requests.push(
        fields === undefined ? { command, opts: o } : { command, fields, opts: o },
      );
      return new Promise((resolve, reject) => {
        pending.push({ resolve: resolve as (v: unknown) => void, reject });
      }) as never;
    },
    on(type: string, fn: unknown) {
      let set = listeners.get(type);
      if (!set) {
        set = new Set();
        listeners.set(type, set);
      }
      set.add(fn as Listener);
      return () => {
        const s = listeners.get(type);
        if (s) s.delete(fn as Listener);
      };
    },
    applyAccent() {
      // DOM side-effect is not under test.
    },
    emit(type, payload, message) {
      const set = listeners.get(type);
      if (set) for (const fn of [...set]) fn(payload, message);
    },
    settle(index, value) {
      const p = pending[index];
      if (p) p.resolve(value);
    },
    reject(index, err) {
      const p = pending[index];
      if (p) p.reject(err);
    },
    countRequests(command) {
      return bridge.requests.filter((r) => r.command === command).length;
    },
    indexOf(command, nth = 0) {
      let seen = 0;
      for (let i = 0; i < bridge.requests.length; i++) {
        if (bridge.requests[i]!.command === command) {
          if (seen === nth) return i;
          seen++;
        }
      }
      return -1;
    },
  } as FakeBridge;
  return bridge;
}

/** Drain Preact's effect queue and any pending promise callbacks. */
export const flush = async () => {
  await act(async () => {
    await new Promise((r) => setTimeout(r, 0));
  });
};

/** Advance past the 120ms filter debounce. */
export const flushDebounce = async () => {
  await act(async () => {
    await new Promise((r) => setTimeout(r, 140));
  });
};

/**
 * Type into the filter box and settle both the input state and the 120ms
 * debounce. The input dispatch must land in its own `act` before the debounce
 * timer is waited on, or Preact has not processed the value change when the
 * debounce window opens and `query` never updates.
 */
export async function typeFilter(el: HTMLElement, value: string): Promise<void> {
  const input = el.querySelector('#filter') as HTMLInputElement;
  await act(async () => {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  });
  await flushDebounce();
  await flush();
}

let host: HTMLElement | null = null;

export async function mount(bridge: Bridge): Promise<HTMLElement> {
  host = document.createElement('div');
  document.body.appendChild(host);
  await act(async () => {
    render(<App bridge={bridge} />, host as HTMLElement);
  });
  await flush();
  return host;
}

export function unmount(): void {
  if (host) {
    render(null, host);
    host.remove();
    host = null;
  }
  document.body.innerHTML = '';
}
