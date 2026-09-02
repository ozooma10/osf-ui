import { describe, expect, it, vi } from 'vitest';
import { nullBridge } from '@lib/bridge';

describe('null bridge', () => {
  it('fails closed without requiring a DOM', async () => {
    expect(nullBridge.available()).toBe(false);
    expect(nullBridge.send('acme.notice', { value: 1 })).toBe(false);
    await expect(nullBridge.request('acme.query')).rejects.toMatchObject({
      code: 'no-bridge',
      message: 'no bridge (standalone preview)',
    });
  });

  it('returns safe inert subscriptions', () => {
    const event = vi.fn();
    const state = vi.fn();
    const offEvent = nullBridge.on('acme.notice', event);
    const offState = nullBridge.state('acme/status', state);
    expect(() => { offEvent(); offState(); }).not.toThrow();
    expect(event).not.toHaveBeenCalled();
    expect(state).not.toHaveBeenCalled();
  });
});
