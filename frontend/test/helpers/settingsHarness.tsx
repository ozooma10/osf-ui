// Fake bridge + mount helpers shared by the settings-view test suites.
// Test only.
//
// Bridge protocol 2.0. The fake keeps the four verbs apart exactly as the real
// bridge does, because that split is most of what these suites now pin:
//
//   send(name, payload)    one-way. Recorded in `sent`; never settles.
//   request(name, payload) recorded in `requests`; settled BY INDEX through
//                          settle() / reject().
//   on(event, fn)          one-shot happenings, fired with deliver(). NEVER
//                          replayed to a later subscriber.
//   state(key, fn)         named values: the handler runs IMMEDIATELY with the
//                          current value and again on every publish().
//
// Seeding `makeBridge({ state: { ... } })` models the OSF UI runtime replaying state to a
// fresh document — the reason a 2.0 view issues no reads and needs no
// lifecycle code. Publishing afterwards models a later change push.

import { render } from 'preact';
import { act } from 'preact/test-utils';
import { nullBridge, type Bridge } from '@lib/bridge';
import { App } from '@views/osfui/settings/App';
import type { RuntimeInfo } from '@sdk';

type Handler = (value: unknown) => void;

/** One outbound envelope, minus the verb that carried it. */
export interface OutboundMessage {
  name: string;
  payload?: Record<string, unknown>;
}

export interface FakeBridge extends Bridge {
  /** Fire a one-shot EVENT at whatever subscribed through on(). Not replayed. */
  deliver(event: string, payload: unknown): void;
  /** Publish a STATE key. Replayed to every later subscriber, and to peek(). */
  publish(key: string, value: unknown): void;
  sent: OutboundMessage[];
  requests: Array<OutboundMessage & { opts?: unknown }>;
  /**
   * Every outbound message in issue order, sends and requests alike.
   *
   * Mod Settings drives both send and request endpoints. These suites
   * assert which endpoint was addressed and with what payload, while dedicated
   * request assertions below also verify settlement behavior.
   */
  outbound: OutboundMessage[];
  settle(index: number, value: unknown): void;
  reject(index: number, err: unknown): void;
  countRequests(name: string): number;
  /** Index of the Nth (0-based) request matching `name`, or -1. */
  indexOf(name: string, nth?: number): number;
}

export interface MakeBridgeOptions {
  version?: string;
  available?: boolean;
  /**
   * State the OSF UI runtime has already replayed to this document, present before the
   * first paint. Keys are absolute ("osfui/settings", "osfui/views", ...).
   */
  state?: Record<string, unknown>;
  /**
   * Never settle `ready()`. Nothing but the OSF UI release-version badge may depend on the
   * handshake — the data arrives as replayed state either way.
   */
  readyNeverResolves?: boolean;
  /** Reject `ready()`, as the 2.0 helper does with no bridge underneath it. */
  readyRejects?: boolean;
}

export function makeBridge(opts: MakeBridgeOptions = {}): FakeBridge {
  const eventListeners = new Map<string, Set<Handler>>();
  const stateListeners = new Map<string, Set<Handler>>();
  const values = new Map<string, unknown>(Object.entries(opts.state ?? {}));
  const pending: Array<{ resolve: (v: unknown) => void; reject: (e: unknown) => void }> = [];
  const available = opts.available ?? true;

  const info: RuntimeInfo = {
    game: 'Starfield',
    plugin: 'OSF UI',
    version: opts.version ?? '1.0.0',
    bridgeVersion: '2.0',
    view: 'osfui/settings',
    mod: 'osfui',
  };

  const subscribe = (map: Map<string, Set<Handler>>, key: string, fn: Handler) => {
    let set = map.get(key);
    if (!set) {
      set = new Set();
      map.set(key, set);
    }
    set.add(fn);
    return () => {
      map.get(key)?.delete(fn);
    };
  };

  const bridge: FakeBridge = {
    ...nullBridge,
    available: () => available,
    sent: [],
    requests: [],
    outbound: [],

    send(name: string, payload?: Record<string, unknown>) {
      const message: OutboundMessage = payload === undefined ? { name } : { name, payload };
      bridge.sent.push(message);
      bridge.outbound.push(message);
      return available;
    },

    request(name: string, payload?: Record<string, unknown>, o?: unknown) {
      const message: OutboundMessage = payload === undefined ? { name } : { name, payload };
      bridge.requests.push(o === undefined ? { ...message } : { ...message, opts: o });
      bridge.outbound.push(message);
      return new Promise((resolve, reject) => {
        pending.push({ resolve: resolve as (v: unknown) => void, reject });
      }) as never;
    },

    on(event: string, fn: unknown) {
      return subscribe(eventListeners, event, fn as Handler);
    },
    onAny(event: string, fn: unknown) {
      return subscribe(eventListeners, event, fn as Handler);
    },

    state(key: string, fn: unknown) {
      const off = subscribe(stateListeners, key, fn as Handler);
      // The defining property: subscribing IS the read.
      if (values.has(key)) (fn as Handler)(values.get(key));
      return off;
    },
    peek(key: string) {
      return values.get(key) as never;
    },

    ready() {
      if (opts.readyNeverResolves) return new Promise(() => {}) as never;
      if (opts.readyRejects) {
        return Promise.reject(
          Object.assign(new Error('no bridge (standalone preview)'), { code: 'no-bridge' }),
        ) as never;
      }
      return Promise.resolve(info) as never;
    },

    applyAccent() {
      // DOM side-effect is not under test.
    },

    deliver(event, payload) {
      const set = eventListeners.get(event);
      if (set) for (const fn of [...set]) fn(payload);
    },
    publish(key, value) {
      values.set(key, value);
      const set = stateListeners.get(key);
      if (set) for (const fn of [...set]) fn(value);
    },

    settle(index, value) {
      const p = pending[index];
      if (p) p.resolve(value);
    },
    reject(index, err) {
      const p = pending[index];
      if (p) p.reject(err);
    },
    countRequests(name) {
      return bridge.requests.filter((r) => r.name === name).length;
    },
    indexOf(name, nth = 0) {
      let seen = 0;
      for (let i = 0; i < bridge.requests.length; i++) {
        if (bridge.requests[i]!.name === name) {
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
