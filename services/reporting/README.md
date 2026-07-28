# OSF UI reporting service

This Cloudflare Worker receives bug reports only after the player confirms the
in-game disclosure. It stores diagnostic logs in a **private** R2 bucket and
queues a public GitHub issue containing the player-authored title, description,
reproduction steps, version information, and an opaque report ID. Log contents
are never copied to the public issue. Public mentions and raw HTML are
neutralized before publication.

## Deploy

1. Create an R2 bucket named `osfui-bug-reports`.
2. Configure an R2 lifecycle rule that deletes `reports/` objects after 30 days.
   The Worker's daily scheduled cleanup enforces the same limit as defense in
   depth; the bucket lifecycle remains the independent backstop.
3. Create queues named `osfui-report-issues` and
   `osfui-report-issues-dlq`.
4. Create the GitHub label `automatic-report` (and ensure `bug` exists) in both
   `ozooma10/osf-ui` and `ozooma10/osf-animation`.
5. Create a fine-grained GitHub token limited to those two repositories with
   **Issues: write**. Client payloads contain only the closed target ids
   `osf-ui` and `osf-animation`; the Worker maps them to these configured
   repositories and rejects arbitrary repository names.
6. From this directory, install dependencies and set all three secrets:

   ```powershell
   npm install
   npx wrangler secret put GITHUB_TOKEN
   npx wrangler secret put ADMIN_TOKEN
   npx wrangler secret put TICKET_SIGNING_SECRET
   npm run deploy
   ```

7. Set `bugReportEndpoint` in the shipped `data/OSFUI/config.json` to the
   resulting HTTPS endpoint plus `/v1/reports`.

Use a long random `ADMIN_TOKEN`. A maintainer retrieves a private report with:

```powershell
Invoke-RestMethod `
  -Headers @{ Authorization = "Bearer $env:OSFUI_REPORT_ADMIN_TOKEN" } `
  https://YOUR-WORKER/v1/reports/REPORT-ID
```

The endpoint also accepts authenticated `DELETE` for early removal. Do not make
the R2 bucket public.

For day-to-day inspection, open `/admin` on the deployed Worker. Enter the same
`ADMIN_TOKEN` to browse report summaries and inspect each report's private
diagnostics and log artifacts. The token is kept only in the browser tab's
session storage. The dashboard fetches report contents through the same
authenticated endpoints; it does not make the R2 bucket public.

## Abuse controls

The client first obtains a 90-day HMAC-signed installation ticket. Fast
Cloudflare bindings allow two submissions per installation and three per source
network per minute (ticket issuance has its own five-per-minute limiter). A
single globally addressed SQLite Durable Object then enforces hard UTC-day
budgets: three reports per signed installation, five per source network, and 100
for the entire service. These global limits are the cost/issue-spam circuit
breaker; the per-location bindings are only burst mitigation.

Accepted reports are written privately and sent to `osfui-report-issues`.
GitHub publication runs asynchronously at concurrency one, retries transient
failures, and moves exhausted messages to the DLQ. The client receives its
private report reference immediately and does not depend on GitHub availability.

Set `REPORTING_ENABLED` to `false` to reject all new reports. Set
`ISSUE_CREATION_ENABLED` to `false` to keep accepted reports private without
creating GitHub issues. Both are Worker variables and require no client update.
Structured `report-accepted`, `report-published`, `report-rate-limited`, and
failure events appear in Workers Logs.

After fixing credentials or reviewing a held report, set issue creation back to
`true`, deploy, and requeue that report with an authenticated `POST` to its
`/v1/reports/REPORT-ID` admin URL. The same URL uses `GET` for inspection and
`DELETE` for early removal. Production configuration intentionally begins with
public issue creation paused until the GitHub credential has been verified.
