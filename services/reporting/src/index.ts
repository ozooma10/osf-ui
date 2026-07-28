import { adminPage } from './admin';

const MAX_BODY_BYTES = 1024 * 1024;
const MAX_LOG_BYTES = 400 * 1024;
const MAX_TOTAL_LOG_BYTES = 900 * 1024;
const RETENTION_MS = 30 * 24 * 60 * 60 * 1000;
const TICKET_LIFETIME_SECONDS = 90 * 24 * 60 * 60;
const GLOBAL_REPORTS_PER_DAY = 100;
const NETWORK_REPORTS_PER_DAY = 5;
const INSTALL_REPORTS_PER_DAY = 3;
const REPORT_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const CLIENT_ID = /^[A-Za-z0-9_-]{16,80}$/;

export interface Env {
  REPORTS: R2Bucket;
  REPORT_QUEUE: Queue<PublishMessage>;
  REPORT_GATE: DurableObjectNamespace;
  CLIENT_LIMITER: RateLimit;
  NETWORK_LIMITER: RateLimit;
  TICKET_LIMITER: RateLimit;
  GITHUB_OWNER: string;
  GITHUB_REPO: string;
  GITHUB_OSF_ANIMATION_REPO?: string;
  GITHUB_TOKEN: string;
  ADMIN_TOKEN: string;
  TICKET_SIGNING_SECRET: string;
  REPORTING_ENABLED?: string;
  ISSUE_CREATION_ENABLED?: string;
}

interface SubmittedLog { name: string; content: string; truncated?: boolean }
type ReportTarget = 'osf-ui' | 'osf-animation';
interface SubmittedReport {
  schemaVersion: 1;
  clientId: string;
  installationToken: string;
  kind: 'manual' | 'crash';
  target: ReportTarget;
  title: string;
  description: string;
  reproduction?: string;
  pluginVersion: string;
  diagnostics: unknown;
  logs: SubmittedLog[];
}
interface StoredReport extends SubmittedReport {
  id: string;
  receivedAt: string;
  status: 'queued' | 'published' | 'paused' | 'failed';
  issueNumber?: number;
  issueUrl?: string;
  failureCode?: string;
}
interface PublishMessage { id: string }
interface GitHubIssue { html_url?: unknown; number?: unknown }

function json(value: unknown, status = 200): Response {
  return new Response(JSON.stringify(value), { status, headers: {
    'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store',
    'x-content-type-options': 'nosniff',
  } });
}
function text(value: string, status: number): Response {
  return new Response(value, { status, headers: {
    'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store',
    'x-content-type-options': 'nosniff',
  } });
}
function boundedString(value: unknown, max: number, required = false): string | null {
  if (typeof value !== 'string') return required ? null : '';
  const clean = value.replace(/\0/g, '').trim();
  if (required && !clean) return null;
  return clean.slice(0, max);
}
function enabled(value: string | undefined, defaultValue = true): boolean {
  return value === undefined ? defaultValue : value.toLowerCase() === 'true';
}

export function validateReport(value: unknown): SubmittedReport | null {
  if (!value || typeof value !== 'object') return null;
  const input = value as Record<string, unknown>;
  if (input.schemaVersion !== 1 || !CLIENT_ID.test(String(input.clientId || ''))) return null;
  if (input.kind !== 'manual' && input.kind !== 'crash') return null;
  const target = input.target === undefined ? 'osf-ui' : input.target;
  if (target !== 'osf-ui' && target !== 'osf-animation') return null;
  const installationToken = boundedString(input.installationToken, 512, true);
  const title = boundedString(input.title, 120, true);
  const description = boundedString(input.description, 6000, true);
  const reproduction = boundedString(input.reproduction, 4000);
  const pluginVersion = boundedString(input.pluginVersion, 64, true);
  if (installationToken === null || title === null || description === null || pluginVersion === null) return null;
  if (!Array.isArray(input.logs) || input.logs.length > 4) return null;
  let total = 0;
  const logs: SubmittedLog[] = [];
  for (const raw of input.logs) {
    if (!raw || typeof raw !== 'object') return null;
    const source = raw as Record<string, unknown>;
    const name = boundedString(source.name, 80, true);
    const content = typeof source.content === 'string' ? source.content : null;
    if (name === null || content === null) return null;
    const bytes = new TextEncoder().encode(content).byteLength;
    if (bytes > MAX_LOG_BYTES) return null;
    total += bytes;
    if (total > MAX_TOTAL_LOG_BYTES) return null;
    const entry: SubmittedLog = { name, content };
    if (source.truncated === true) entry.truncated = true;
    logs.push(entry);
  }
  const report: SubmittedReport = {
    schemaVersion: 1, clientId: String(input.clientId), installationToken,
    kind: input.kind, target, title, description, pluginVersion,
    diagnostics: input.diagnostics ?? {}, logs,
  };
  if (reproduction) report.reproduction = reproduction;
  return report;
}

function base64Url(bytes: ArrayBuffer): string {
  return btoa(String.fromCharCode(...new Uint8Array(bytes)))
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');
}
async function signingKey(secret: string): Promise<CryptoKey> {
  return crypto.subtle.importKey('raw', new TextEncoder().encode(secret),
    { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
}
async function hmac(secret: string, value: string): Promise<string> {
  return base64Url(await crypto.subtle.sign('HMAC', await signingKey(secret),
    new TextEncoder().encode(value)));
}
function decodeBase64Url(value: string): Uint8Array {
  const base64 = value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - value.length % 4) % 4);
  return Uint8Array.from(atob(base64), (character) => character.charCodeAt(0));
}
async function issueTicket(secret: string, clientId: string): Promise<string> {
  const expires = Math.floor(Date.now() / 1000) + TICKET_LIFETIME_SECONDS;
  const value = `${clientId}.${expires}`;
  return `${value}.${await hmac(secret, value)}`;
}
async function verifyTicket(secret: string, clientId: string, token: string): Promise<boolean> {
  const parts = token.split('.');
  if (parts.length !== 3 || parts[0] !== clientId || !/^\d{10}$/.test(parts[1] || '')) return false;
  const expires = Number(parts[1]);
  if (!Number.isSafeInteger(expires) || expires < Math.floor(Date.now() / 1000)) return false;
  const value = `${parts[0]}.${parts[1]}`;
  try {
    return crypto.subtle.verify('HMAC', await signingKey(secret), decodeBase64Url(parts[2] || ''),
      new TextEncoder().encode(value));
  } catch { return false; }
}

export class ReportGate {
  constructor(private readonly state: DurableObjectState) {}
  async fetch(request: Request): Promise<Response> {
    const input = await request.json() as { day: string; network: string; installation: string };
    const keys = [`global:${input.day}`, `network:${input.day}:${input.network}`,
      `installation:${input.day}:${input.installation}`];
    const limits = [GLOBAL_REPORTS_PER_DAY, NETWORK_REPORTS_PER_DAY, INSTALL_REPORTS_PER_DAY];
    const outcome = await this.state.storage.transaction(async (txn) => {
      const values = await txn.get<number>(keys);
      const counts = keys.map((key) => values.get(key) || 0);
      const blocked = counts.findIndex((count, index) => count >= (limits[index] || 0));
      if (blocked >= 0) return { ok: false, scope: ['global', 'network', 'installation'][blocked] };
      await txn.put(Object.fromEntries(keys.map((key, index) => [key, (counts[index] || 0) + 1])));
      return { ok: true };
    });
    return json(outcome, outcome.ok ? 200 : 429);
  }
}

async function dailyGate(env: Env, network: string, installation: string): Promise<Response> {
  const day = new Date().toISOString().slice(0, 10);
  const networkHash = await hmac(env.TICKET_SIGNING_SECRET, `network:${network}`);
  const installHash = await hmac(env.TICKET_SIGNING_SECRET, `installation:${installation}`);
  const id = env.REPORT_GATE.idFromName('global-report-budget');
  return env.REPORT_GATE.get(id).fetch('https://report-gate/check', {
    method: 'POST', body: JSON.stringify({ day, network: networkHash, installation: installHash }),
  });
}

function publicText(value: string): string {
  return value.replace(/@/g, '＠').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
function issueBody(report: StoredReport): string {
  return [
    'This issue was submitted through OSF UI’s consented diagnostic reporter.', '',
    `**Report ID:** \`${report.id}\``, `**Kind:** ${report.kind}`,
    `**Target:** ${report.target || 'osf-ui'}`,
    `**OSF UI:** ${publicText(report.pluginVersion)}`, '', '### Description',
    publicText(report.description), '', '### Reproduction',
    publicText(report.reproduction || '(not provided)'), '',
    `Diagnostic logs are stored privately for 30 days. Retrieve \`${report.id}\` through the reporting service’s authenticated admin endpoint.`,
  ].join('\n');
}
async function createIssue(env: Env, report: StoredReport): Promise<{ url: string; number: number }> {
  const repository = report.target === 'osf-animation' ?
    (env.GITHUB_OSF_ANIMATION_REPO || 'osf-animation') : env.GITHUB_REPO;
  const response = await fetch(`https://api.github.com/repos/${encodeURIComponent(env.GITHUB_OWNER)}/${encodeURIComponent(repository)}/issues`, {
    method: 'POST', headers: { accept: 'application/vnd.github+json',
      authorization: `Bearer ${env.GITHUB_TOKEN}`, 'content-type': 'application/json',
      'user-agent': 'osfui-reporting-service', 'x-github-api-version': '2022-11-28' },
    body: JSON.stringify({ title: `[Automatic report] ${publicText(report.title)}`,
      body: issueBody(report), labels: ['bug', 'automatic-report'] }),
  });
  if (!response.ok) {
    const detail = (await response.text()).slice(0, 500);
    throw new Error(`GitHub issue creation failed (${response.status}): ${detail}`);
  }
  const issue = await response.json() as GitHubIssue;
  if (typeof issue.html_url !== 'string' || typeof issue.number !== 'number') throw new Error('GitHub returned an invalid issue');
  return { url: issue.html_url, number: issue.number };
}

async function installations(request: Request, env: Env): Promise<Response> {
  const network = request.headers.get('cf-connecting-ip') || 'unknown';
  if (!(await env.TICKET_LIMITER.limit({ key: network })).success) return json({ ok: false, code: 'rate-limited' }, 429);
  let input: unknown;
  try { input = await request.json(); } catch { return json({ ok: false, code: 'invalid-installation' }, 400); }
  const clientId = input && typeof input === 'object' ? String((input as Record<string, unknown>).clientId || '') : '';
  if (!CLIENT_ID.test(clientId)) return json({ ok: false, code: 'invalid-installation' }, 400);
  return json({ ok: true, installationToken: await issueTicket(env.TICKET_SIGNING_SECRET, clientId), expiresIn: TICKET_LIFETIME_SECONDS }, 201);
}

async function submit(request: Request, env: Env): Promise<Response> {
  if (!enabled(env.REPORTING_ENABLED)) return json({ ok: false, code: 'reporting-paused' }, 503);
  const length = Number(request.headers.get('content-length') || '0');
  if (length > MAX_BODY_BYTES) return json({ ok: false, code: 'report-too-large' }, 413);
  const network = request.headers.get('cf-connecting-ip') || 'unknown';
  if (!(await env.NETWORK_LIMITER.limit({ key: network })).success) return json({ ok: false, code: 'rate-limited' }, 429);
  let raw: unknown;
  try {
    const body = await request.arrayBuffer();
    if (body.byteLength > MAX_BODY_BYTES) return json({ ok: false, code: 'report-too-large' }, 413);
    raw = JSON.parse(new TextDecoder().decode(body));
  } catch { return json({ ok: false, code: 'invalid-report' }, 400); }
  const report = validateReport(raw);
  if (!report) return json({ ok: false, code: 'invalid-report' }, 400);
  if (!await verifyTicket(env.TICKET_SIGNING_SECRET, report.clientId, report.installationToken)) {
    return json({ ok: false, code: 'invalid-installation' }, 401);
  }
  if (!(await env.CLIENT_LIMITER.limit({ key: report.clientId })).success) return json({ ok: false, code: 'rate-limited' }, 429);
  const gate = await dailyGate(env, network, report.clientId);
  if (!gate.ok) {
    const denied = await gate.json() as { scope?: string };
    console.warn(JSON.stringify({ event: 'report-rate-limited', scope: denied.scope || 'daily' }));
    return json({ ok: false, code: 'daily-limit-reached' }, 429);
  }
  const id = crypto.randomUUID();
  const stored: StoredReport = { ...report, id, receivedAt: new Date().toISOString(), status: 'queued' };
  delete (stored as Partial<StoredReport>).installationToken;
  const key = `reports/${id}.json`;
  await env.REPORTS.put(key, JSON.stringify(stored), { httpMetadata: { contentType: 'application/json' },
    customMetadata: { kind: report.kind, target: report.target, pluginVersion: report.pluginVersion, status: 'queued' } });
  try { await env.REPORT_QUEUE.send({ id }); }
  catch (error) {
    await env.REPORTS.delete(key);
    console.error(JSON.stringify({ event: 'report-queue-failed', id, error: String(error) }));
    return json({ ok: false, code: 'queue-failed' }, 503);
  }
  console.log(JSON.stringify({ event: 'report-accepted', id, kind: report.kind, target: report.target }));
  return json({ ok: true, reportId: id, publication: 'queued' }, 202);
}

async function adminReport(request: Request, env: Env, id: string): Promise<Response> {
  const authorization = request.headers.get('authorization') || '';
  if (!env.ADMIN_TOKEN || authorization !== `Bearer ${env.ADMIN_TOKEN}`) return json({ ok: false, code: 'unauthorized' }, 401);
  const key = `reports/${id}.json`;
  if (request.method === 'DELETE') { await env.REPORTS.delete(key); return new Response(null, { status: 204 }); }
  const object = await env.REPORTS.get(key);
  if (!object) return json({ ok: false, code: 'not-found' }, 404);
  if (request.method === 'POST') {
    const report = await object.json<StoredReport>();
    if (report.status === 'published') return json({ ok: true, status: 'published', issueNumber: report.issueNumber });
    report.status = 'queued';
    delete report.failureCode;
    await env.REPORTS.put(key, JSON.stringify(report), { httpMetadata: { contentType: 'application/json' } });
    await env.REPORT_QUEUE.send({ id });
    return json({ ok: true, status: 'queued' }, 202);
  }
  return new Response(object.body, { headers: { 'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store', 'x-content-type-options': 'nosniff' } });
}

async function adminReports(request: Request, env: Env, url: URL): Promise<Response> {
  const authorization = request.headers.get('authorization') || '';
  if (!env.ADMIN_TOKEN || authorization !== `Bearer ${env.ADMIN_TOKEN}`) {
    return json({ ok: false, code: 'unauthorized' }, 401);
  }
  const rawCursor = url.searchParams.get('cursor') || '';
  if (rawCursor.length > 1024) return json({ ok: false, code: 'invalid-cursor' }, 400);
  const options: R2ListOptions = { prefix: 'reports/', limit: 50 };
  if (rawCursor) options.cursor = rawCursor;
  const page = await env.REPORTS.list(options);
  const reports = (await Promise.all(page.objects.map(async (listed) => {
    const object = await env.REPORTS.get(listed.key);
    if (!object) return null;
    try {
      const report = await object.json<StoredReport>();
      if (!REPORT_ID.test(report.id)) return null;
      return {
        id: report.id, receivedAt: report.receivedAt, kind: report.kind,
        target: report.target || 'osf-ui',
        title: report.title,
        pluginVersion: report.pluginVersion, status: report.status,
      };
    } catch { return null; }
  }))).filter((report) => report !== null)
    .sort((left, right) => right.receivedAt.localeCompare(left.receivedAt));
  return json({ ok: true, reports, cursor: page.truncated ? page.cursor : null });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === 'GET' && (url.pathname === '/admin' || url.pathname === '/admin/')) return adminPage();
    if (request.method === 'GET' && url.pathname === '/healthz') return json({ ok: true, reporting: enabled(env.REPORTING_ENABLED), publishing: enabled(env.ISSUE_CREATION_ENABLED) });
    if (request.method === 'GET' && url.pathname === '/v1/reports') return adminReports(request, env, url);
    if (request.method === 'POST' && (url.pathname === '/v1/installations' || url.pathname === '/v1/reports')) {
      if (!request.headers.get('content-type')?.toLowerCase().startsWith('application/json')) return json({ ok: false, code: 'unsupported-media-type' }, 415);
      return url.pathname === '/v1/installations' ? installations(request, env) : submit(request, env);
    }
    const match = /^\/v1\/reports\/([0-9a-f-]+)$/i.exec(url.pathname);
    if (match?.[1] && REPORT_ID.test(match[1]) &&
      (request.method === 'GET' || request.method === 'DELETE' || request.method === 'POST')) return adminReport(request, env, match[1]);
    return text('Not found', 404);
  },
  async queue(batch: MessageBatch<unknown>, env: Env): Promise<void> {
    for (const message of batch.messages) {
      const body = message.body;
      if (!body || typeof body !== 'object' || !REPORT_ID.test(String((body as Record<string, unknown>).id || ''))) {
        message.ack(); continue;
      }
      const key = `reports/${String((body as Record<string, unknown>).id)}.json`;
      const object = await env.REPORTS.get(key);
      if (!object) { message.ack(); continue; }
      const report = await object.json<StoredReport>();
      report.target = report.target === 'osf-animation' ? 'osf-animation' : 'osf-ui';
      if (report.status === 'published') { message.ack(); continue; }
      if (!enabled(env.ISSUE_CREATION_ENABLED)) {
        report.status = 'paused';
        await env.REPORTS.put(key, JSON.stringify(report), { httpMetadata: { contentType: 'application/json' } });
        console.warn(JSON.stringify({ event: 'report-publication-paused', id: report.id }));
        message.ack();
        continue;
      }
      try {
        const issue = await createIssue(env, report);
        report.status = 'published'; report.issueNumber = issue.number; report.issueUrl = issue.url;
        await env.REPORTS.put(key, JSON.stringify(report), { httpMetadata: { contentType: 'application/json' },
          customMetadata: { kind: report.kind, target: report.target, pluginVersion: report.pluginVersion, status: 'published' } });
        console.log(JSON.stringify({ event: 'report-published', id: report.id, issueNumber: issue.number }));
        message.ack();
      } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        console.error(JSON.stringify({ event: 'report-publication-failed', id: report.id, error: detail }));
        if (/\((401|403|422)\)/.test(detail)) {
          report.status = 'failed'; report.failureCode = 'github-rejected';
          await env.REPORTS.put(key, JSON.stringify(report), { httpMetadata: { contentType: 'application/json' } });
          message.ack();
        } else { message.retry(); }
      }
    }
  },
  async scheduled(_controller: ScheduledController, env: Env): Promise<void> {
    const cutoff = Date.now() - RETENTION_MS;
    let cursor: string | undefined;
    do {
      const page = await env.REPORTS.list(cursor ? { prefix: 'reports/', cursor } : { prefix: 'reports/' });
      const expired = page.objects.filter((object) => object.uploaded.getTime() < cutoff).map((object) => object.key);
      if (expired.length) await env.REPORTS.delete(expired);
      cursor = page.truncated ? page.cursor : undefined;
    } while (cursor);
  },
} satisfies ExportedHandler<Env>;
