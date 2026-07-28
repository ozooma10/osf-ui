import { afterEach, describe, expect, it, vi } from 'vitest';
import worker, { validateReport, type Env } from '../src/index';

afterEach(() => vi.unstubAllGlobals());

function valid(token = 'client_0123456789abcdef.9999999999.signature') {
  return {
    schemaVersion: 1, clientId: 'client_0123456789abcdef', installationToken: token,
    kind: 'manual', target: 'osf-ui', title: 'Overlay <b>@admin</b> is blank',
    description: 'The Mods menu opens but no content appears.', reproduction: 'Press F10.',
    pluginVersion: '1.4.0', diagnostics: { system: { renderer: 'webview2' }, issues: [] },
    logs: [{ name: 'OSF UI.log', content: 'one line' }],
  };
}

function bindings(overrides: Partial<Env> = {}): Env {
  return {
    REPORTS: {
      put: vi.fn().mockResolvedValue({}), delete: vi.fn().mockResolvedValue(undefined),
      get: vi.fn().mockResolvedValue(null), list: vi.fn().mockResolvedValue({ objects: [], truncated: false }),
    } as unknown as R2Bucket,
    REPORT_QUEUE: { send: vi.fn().mockResolvedValue(undefined) } as unknown as Queue,
    REPORT_GATE: {
      idFromName: vi.fn().mockReturnValue('gate-id'),
      get: vi.fn().mockReturnValue({ fetch: vi.fn().mockResolvedValue(new Response('{"ok":true}')) }),
    } as unknown as DurableObjectNamespace,
    CLIENT_LIMITER: { limit: vi.fn().mockResolvedValue({ success: true }) } as unknown as RateLimit,
    NETWORK_LIMITER: { limit: vi.fn().mockResolvedValue({ success: true }) } as unknown as RateLimit,
    TICKET_LIMITER: { limit: vi.fn().mockResolvedValue({ success: true }) } as unknown as RateLimit,
    GITHUB_OWNER: 'ozooma10', GITHUB_REPO: 'osf-ui',
    GITHUB_OSF_ANIMATION_REPO: 'osf-animation', GITHUB_TOKEN: 'secret',
    ADMIN_TOKEN: 'admin', TICKET_SIGNING_SECRET: 'test-signing-secret-with-enough-entropy',
    REPORTING_ENABLED: 'true', ISSUE_CREATION_ENABLED: 'true', ...overrides,
  };
}

async function ticket(env: Env): Promise<string> {
  const response = await worker.fetch(new Request('https://reports.example/v1/installations', {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ clientId: 'client_0123456789abcdef' }),
  }), env);
  return String((await response.json() as { installationToken: string }).installationToken);
}

describe('validation and tickets', () => {
  it('accepts a bounded report and rejects missing tickets', () => {
    expect(validateReport(valid())).toMatchObject({ kind: 'manual' });
    expect(validateReport({ ...valid(), installationToken: '' })).toBeNull();
  });
  it('rejects oversized logs and spoof-shaped client ids', () => {
    expect(validateReport({ ...valid(), clientId: '../../someone' })).toBeNull();
    expect(validateReport({ ...valid(), logs: [{ name: 'x', content: 'x'.repeat(400 * 1024 + 1) }] })).toBeNull();
  });
  it('accepts only server-allowlisted repository targets', () => {
    expect(validateReport({ ...valid(), target: 'osf-animation' })).toMatchObject({ target: 'osf-animation' });
    expect(validateReport({ ...valid(), target: 'someone/private-repo' })).toBeNull();
  });
  it('issues a signed installation ticket and accepts it', async () => {
    const env = bindings();
    const installationToken = await ticket(env);
    const response = await worker.fetch(new Request('https://reports.example/v1/reports', {
      method: 'POST', headers: { 'content-type': 'application/json', 'cf-connecting-ip': '192.0.2.1' },
      body: JSON.stringify(valid(installationToken)),
    }), env);
    expect(response.status).toBe(202);
    expect(await response.json()).toMatchObject({ ok: true, publication: 'queued' });
    expect(env.REPORTS.put).toHaveBeenCalledOnce();
    expect(env.REPORT_QUEUE.send).toHaveBeenCalledOnce();
  });
  it('rejects invented tickets', async () => {
    const response = await worker.fetch(new Request('https://reports.example/v1/reports', {
      method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(valid()),
    }), bindings());
    expect(response.status).toBe(401);
    expect(await response.json()).toMatchObject({ code: 'invalid-installation' });
  });
});

describe('abuse controls and publication', () => {
  it('enforces the globally consistent daily gate', async () => {
    const env = bindings({ REPORT_GATE: {
      idFromName: vi.fn().mockReturnValue('gate-id'),
      get: vi.fn().mockReturnValue({ fetch: vi.fn().mockResolvedValue(new Response('{"ok":false,"scope":"global"}', { status: 429 })) }),
    } as unknown as DurableObjectNamespace });
    const response = await worker.fetch(new Request('https://reports.example/v1/reports', {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify(valid(await ticket(env))),
    }), env);
    expect(response.status).toBe(429);
    expect(await response.json()).toMatchObject({ code: 'daily-limit-reached' });
  });
  it('queues without calling GitHub synchronously', async () => {
    const github = vi.fn(); vi.stubGlobal('fetch', github);
    const env = bindings();
    await worker.fetch(new Request('https://reports.example/v1/reports', {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify(valid(await ticket(env))),
    }), env);
    expect(github).not.toHaveBeenCalled();
  });
  it('neutralizes mentions and HTML when the queue publishes', async () => {
    const id = '12345678-1234-4123-8123-123456789abc';
    const report = { ...valid(), id, receivedAt: new Date().toISOString(), status: 'queued' };
    const env = bindings({ REPORTS: {
      get: vi.fn().mockResolvedValue({ json: vi.fn().mockResolvedValue(report) }),
      put: vi.fn().mockResolvedValue({}),
    } as unknown as R2Bucket });
    const github = vi.fn().mockResolvedValue(new Response(JSON.stringify({
      html_url: 'https://github.com/ozooma10/osf-ui/issues/42', number: 42,
    }), { status: 201 }));
    vi.stubGlobal('fetch', github);
    const message = { body: { id }, ack: vi.fn(), retry: vi.fn() };
    await worker.queue({ messages: [message] } as unknown as MessageBatch<unknown>, env);
    const requestBody = JSON.parse(String(github.mock.calls[0]![1]!.body));
    expect(requestBody.title).toContain('＠admin');
    expect(requestBody.title).not.toContain('<b>');
    expect(requestBody.body).not.toContain('one line');
    expect(message.ack).toHaveBeenCalledOnce();
  });
  it('maps the osf-animation target to its fixed repository', async () => {
    const id = '12345678-1234-4123-8123-123456789abc';
    const report = { ...valid(), target: 'osf-animation', id,
      receivedAt: new Date().toISOString(), status: 'queued' };
    const env = bindings({ REPORTS: {
      get: vi.fn().mockResolvedValue({ json: vi.fn().mockResolvedValue(report) }),
      put: vi.fn().mockResolvedValue({}),
    } as unknown as R2Bucket });
    const github = vi.fn().mockResolvedValue(new Response(JSON.stringify({
      html_url: 'https://github.com/ozooma10/osf-animation/issues/7', number: 7,
    }), { status: 201 }));
    vi.stubGlobal('fetch', github);
    const message = { body: { id }, ack: vi.fn(), retry: vi.fn() };
    await worker.queue({ messages: [message] } as unknown as MessageBatch<unknown>, env);
    expect(String(github.mock.calls[0]![0])).toBe(
      'https://api.github.com/repos/ozooma10/osf-animation/issues');
    expect(message.ack).toHaveBeenCalledOnce();
  });
  it('holds reports privately when publishing is paused', async () => {
    const id = '12345678-1234-4123-8123-123456789abc';
    const report = { ...valid(), id, receivedAt: new Date().toISOString(), status: 'queued' };
    const r2 = { get: vi.fn().mockResolvedValue({ json: vi.fn().mockResolvedValue(report) }), put: vi.fn() };
    const message = { body: { id }, ack: vi.fn(), retry: vi.fn() };
    await worker.queue({ messages: [message] } as unknown as MessageBatch<unknown>,
      bindings({ REPORTS: r2 as unknown as R2Bucket, ISSUE_CREATION_ENABLED: 'false' }));
    expect(r2.put).toHaveBeenCalledOnce();
    expect(message.ack).toHaveBeenCalledOnce();
  });
});

describe('admin artifact browser', () => {
  it('serves a locked-down dashboard without embedding the admin secret', async () => {
    const response = await worker.fetch(new Request('https://reports.example/admin'), bindings());
    expect(response.status).toBe(200);
    expect(response.headers.get('content-security-policy')).toContain("frame-ancestors 'none'");
    const body = await response.text();
    expect(body).toContain('Report artifacts');
    expect(body).not.toContain('Bearer admin');
  });
  it('requires authentication to list reports', async () => {
    const response = await worker.fetch(new Request('https://reports.example/v1/reports'), bindings());
    expect(response.status).toBe(401);
    expect(await response.json()).toMatchObject({ code: 'unauthorized' });
  });
  it('lists report summaries without exposing log contents', async () => {
    const id = '12345678-1234-4123-8123-123456789abc';
    const report = { ...valid(), id, receivedAt: '2026-07-27T12:00:00.000Z', status: 'queued' };
    const r2 = {
      list: vi.fn().mockResolvedValue({ objects: [{ key: `reports/${id}.json` }], truncated: false }),
      get: vi.fn().mockResolvedValue({ json: vi.fn().mockResolvedValue(report) }),
    };
    const response = await worker.fetch(new Request('https://reports.example/v1/reports', {
      headers: { authorization: 'Bearer admin' },
    }), bindings({ REPORTS: r2 as unknown as R2Bucket }));
    expect(response.status).toBe(200);
    const body = await response.json() as { reports: Array<Record<string, unknown>> };
    expect(body.reports[0]).toMatchObject({ id, title: report.title, target: 'osf-ui', status: 'queued' });
    expect(body.reports[0]).not.toHaveProperty('logs');
    expect(body.reports[0]).not.toHaveProperty('diagnostics');
  });
});

describe('retention', () => {
  it('deletes only reports older than thirty days', async () => {
    const r2 = { list: vi.fn().mockResolvedValue({ objects: [
      { key: 'reports/old.json', uploaded: new Date(Date.now() - 31 * 86400000) },
      { key: 'reports/new.json', uploaded: new Date(Date.now() - 2 * 86400000) },
    ], truncated: false }), delete: vi.fn() };
    await worker.scheduled({} as ScheduledController, bindings({ REPORTS: r2 as unknown as R2Bucket }));
    expect(r2.delete).toHaveBeenCalledWith(['reports/old.json']);
  });
});
