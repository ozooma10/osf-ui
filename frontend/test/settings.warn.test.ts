// The schema-diagnostic channel's dedupe. Every caller sits in a render path,
// so the property under test is "once per distinct message per page load", not
// "once per call".

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { devWarn, resetDevWarnDedupe } from '../src/views/osfui/settings/warn';

let warn: ReturnType<typeof vi.spyOn>;

beforeEach(() => {
  resetDevWarnDedupe();
  warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
  warn.mockRestore();
});

describe('devWarn', () => {
  it('logs with the stable grep prefix', () => {
    devWarn('condition references unknown key "enabled"');
    expect(warn).toHaveBeenCalledTimes(1);
    expect(warn).toHaveBeenCalledWith('[osfui settings] condition references unknown key "enabled"');
  });

  it('logs a repeated message only once — renders repeat, the fault does not', () => {
    for (let i = 0; i < 25; i++) devWarn('"speed" has invalid step 0');
    expect(warn).toHaveBeenCalledTimes(1);
  });

  it('still logs distinct messages, so a second bad setting is not swallowed', () => {
    devWarn('"speed" has invalid step 0');
    devWarn('"volume" has invalid step 0');
    expect(warn).toHaveBeenCalledTimes(2);
  });
});
