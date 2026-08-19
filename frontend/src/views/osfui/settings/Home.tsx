
import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { modIconSrc, type AssetRoots } from '@lib/settings/assets';
import { titleOf, type ModRecord, type ViewRecord } from '@lib/settings/rail';
import { issueForSubject, type HealthModel } from '@lib/settings/health';
import type { Translator } from '@lib/i18n';
import { homeAccentFor, initials, Mark } from './marks';
import { useOpenCooldown } from './openCooldown';

export interface HomeProps {
  views: ViewRecord[];
  mods: ModRecord[];
  health: HealthModel;
  tr: Translator;
  assetRoots: AssetRoots | undefined;
  /** Resolved open state for a HUD, including any optimistic override. */
  hudOn: (view: ViewRecord) => boolean;
  onOpen: (viewId: string) => void;
  onHud: (viewId: string, next: boolean) => void;
  /** Jump to System Health with this issue expanded. */
  onOpenIssue: (issueId: string) => void;
}

export function Home({
  views,
  mods,
  health,
  tr,
  assetRoots,
  hudOn,
  onOpen,
  onHud,
  onOpenIssue,
}: HomeProps) {
  const menus = views.filter((v) => v.kind === 'menu');
  const huds = views.filter((v) => v.kind !== 'menu');

  const ownerIcon = (modId: string | undefined): string | null => {
    if (!modId) return null;
    const owner = (mods.find((m) => m.id === modId) || null) as Parameters<typeof modIconSrc>[0];
    return modIconSrc(owner, assetRoots);
  };
  const caption = (v: ViewRecord): string => homeModCaption(v, mods);

  return (
    <>
      <div class="detail-head">
        <div>
          {/* Compatibility catalog address; fallback copy uses the canonical view noun. */}
          <h2>{tr('allSystems', 'All views')}</h2>
        </div>
      </div>

      <div class="detail-body detail-body--home">
        {!views.length ? (
          <div class="home-empty">
            {/* Compatibility catalog addresses retain the former system noun. */}
            <div class="osf-eyebrow">{tr('noSystems', 'No views available')}</div>
            <p>
              {tr(
                'noSystemsHint',
                'Mods that provide menu or HUD views appear here as launch cards. Mods that only provide settings are listed on the left.',
              )}
            </p>
          </div>
        ) : (
          <>
            {menus.length ? (
              <>
                {/* Compatibility catalog address; fallback copy uses the canonical menu kind. */}
                <SectionHead title={tr('terminals', 'Menus')} count={menus.length} note="" />
                <div class="home-grid">
                  {menus.map((v) => (
                    <MenuCard
                      key={v.id}
                      view={v}
                      tr={tr}
                      iconSrc={ownerIcon(v.mod)}
                      caption={caption(v)}
                      issueId={(issueForSubject(health.issues, v.id) || {}).id ?? null}
                      onOpen={onOpen}
                      onOpenIssue={onOpenIssue}
                    />
                  ))}
                </div>
              </>
            ) : null}

            {huds.length ? (
              <>
                {/* Compatibility catalog address; fallback copy uses the canonical HUD kind. */}
                <SectionHead
                  title={tr('overlays', 'HUDs')}
                  count={huds.length}
                  note={tr('toggleStays', 'TOGGLE · STAYS ON SCREEN')}
                />
                <div class="home-hud-list">
                  {huds.map((v) => (
                    <HudCard
                      key={v.id}
                      view={v}
                      on={hudOn(v)}
                      iconSrc={ownerIcon(v.mod)}
                      caption={caption(v)}
                      onToggle={onHud}
                    />
                  ))}
                </div>
              </>
            ) : null}
          </>
        )}
      </div>
    </>
  );
}

export function homeModCaption(v: ViewRecord, mods: ModRecord[]): string {
  if (!v.mod) return '';
  const owner = mods.find((m) => m.id === v.mod);
  return owner ? titleOf(owner) : v.mod;
}

function SectionHead({ title, count, note }: { title: string; count: number; note: string }) {
  return (
    <div class="home-head">
      <span class="home-head-title">{title}</span>
      {/* Zero-padded to two digits — instrument-panel reading. */}
      <span class="home-head-count">{String(count).padStart(2, '0')}</span>
      <span class="home-head-rule" />
      {note ? <span class="home-head-note">{note}</span> : null}
    </div>
  );
}

function Patch({
  accent,
  failed,
  children,
}: {
  accent: string;
  failed: boolean;
  children: ComponentChildren;
}) {
  return (
    <span class="home-patch" style={{ color: failed ? 'var(--osf-signal-stop)' : accent }}>
      <svg width="64" height="64" viewBox="0 0 200 200" aria-hidden="true">
        <circle
          cx="100"
          cy="100"
          r="93"
          fill="rgba(11,14,18,0.55)"
          stroke="currentColor"
          stroke-width="2"
          opacity="0.9"
        />
        <circle cx="100" cy="100" r="83" fill="none" stroke="currentColor" stroke-width="1" opacity="0.32" />
        <polygon points="22,100 27,94 32,100 27,106" fill="currentColor" opacity="0.8" />
        <polygon points="178,100 173,94 168,100 173,106" fill="currentColor" opacity="0.8" />
        {failed ? (
          <g stroke="currentColor" stroke-width="6" stroke-linecap="round" fill="none">
            <path d="M100 78 v26" />
            <path d="M100 118 v.5" />
          </g>
        ) : null}
      </svg>
      {children}
    </span>
  );
}

interface MenuCardProps {
  view: ViewRecord;
  tr: Translator;
  iconSrc: string | null;
  caption: string;
  issueId: string | null;
  onOpen: (viewId: string) => void;
  onOpenIssue: (issueId: string) => void;
}

function MenuCard({ view: v, tr, iconSrc, caption, issueId, onOpen, onOpenIssue }: MenuCardProps) {
  const failed = v.loadState === 'failed';
  const accent = homeAccentFor(v.id);
  const cooldown = useOpenCooldown();
  const [iconFailed, setIconFailed] = useState(false);
  const showIcon = !!iconSrc && !iconFailed;
  const reviewable = failed && !!issueId;

  return (
    <button
      type="button"
      class={failed ? 'home-tile failed' : 'home-tile'}
      disabled={(failed && !reviewable) || cooldown.active}
      onClick={() => {
        if (reviewable) {
          onOpenIssue(issueId as string);
          return;
        }
        if (!cooldown.begin()) return;
        onOpen(v.id);
      }}
    >
      <Patch accent={accent} failed={failed}>
        {/* A failed view shows the alert stroke instead of an identity: its
            monogram would read as a working app. */}
        {failed ? null : showIcon ? (
          <img class="home-patch-icon" src={iconSrc as string} alt="" onError={() => setIconFailed(true)} />
        ) : (
          <span class="home-monogram" style={{ color: accent }}>
            {initials(v.title || v.id)}
          </span>
        )}
      </Patch>

      <span class="home-tile-body">
        <span class="home-tile-title">{v.title || v.id}</span>
        {v.description ? <span class="home-tile-desc">{v.description}</span> : null}
        {caption ? <span class="home-tile-mod">{caption}</span> : null}
      </span>

      <span class="home-tile-foot">
        {reviewable
          ? tr('failedReviewIssue', 'FAILED — REVIEW ISSUE ▸')
          : failed
            ? tr('failedSeeLog', 'FAILED — SEE OSF UI.LOG')
            : tr('openArrow', 'OPEN ▸')}
      </span>
    </button>
  );
}

interface HudCardProps {
  view: ViewRecord;
  on: boolean;
  iconSrc: string | null;
  caption: string;
  onToggle: (viewId: string, next: boolean) => void;
}

function HudCard({ view: v, on, iconSrc, caption, onToggle }: HudCardProps) {
  return (
    <button
      type="button"
      class="home-hud"
      role="switch"
      aria-checked={on ? 'true' : 'false'}
      aria-pressed={on ? 'true' : 'false'}
      onClick={() => onToggle(v.id, !on)}
    >
      <Mark
        class="home-hud-chip"
        iconClass="home-hud-chip--icon"
        src={iconSrc}
        color={homeAccentFor(v.id)}
        fallback={initials(v.title || v.id)}
      />
      <span class="home-hud-main">
        <span class="home-hud-name">{v.title || v.id}</span>
        {/* description -> owning mod -> empty, so the line keeps its height
            even when a view describes itself with nothing. */}
        <span class="home-hud-desc">{v.description || caption || ''}</span>
      </span>
      <span class="osf-switch home-hud-switch" />
    </button>
  );
}
