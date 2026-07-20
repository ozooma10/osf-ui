// The launcher, and the page every fresh visit lands on.
//
// A card grid rather than settings rows: a registered panel is a surface a mod
// ships, not a preference. Settings stay per-mod on the rail; anything you
// launch lives here, across every mod.
//
// Cards derive a monogram and an accent from the view id (marks.tsx), so an
// unbranded third-party view still looks intentional. All untrusted text
// (titles, descriptions) renders as a text child; the only markup is the static
// patch SVG below.

import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';
import { modIconSrc, type AssetRoots } from '@lib/settings/assets';
import { titleOf, type ModRecord, type ViewRecord } from '@lib/settings/rail';
import type { Translator } from '@lib/i18n';
import { homeAccentFor, initials } from './marks';

/** How long a launched card stays inert. */
const OPEN_COOLDOWN_MS = 1600;

export interface HomeProps {
  views: ViewRecord[];
  mods: ModRecord[];
  tr: Translator;
  assetRoots: AssetRoots | undefined;
  /** Resolved open state for a HUD, including any optimistic override. */
  hudOn: (view: ViewRecord) => boolean;
  onOpen: (viewId: string) => void;
  onHud: (viewId: string, next: boolean) => void;
}

export function Home({ views, mods, tr, assetRoots, hudOn, onOpen, onHud }: HomeProps) {
  const menus = views.filter((v) => v.kind === 'menu');
  // Anything that is not a menu is treated as a HUD, so an unknown future
  // `kind` lands here rather than vanishing.
  const huds = views.filter((v) => v.kind !== 'menu');

  const ownerIcon = (modId: string | undefined): string | null => {
    if (!modId) return null;
    // Cast: the SDK `SettingsSchema` omits the advisory `icon` field that
    // modIconSrc reads as `unknown`; bridges it without loosening the lib.
    const owner = (mods.find((m) => m.id === modId) || null) as Parameters<typeof modIconSrc>[0];
    return modIconSrc(owner, assetRoots);
  };
  const caption = (v: ViewRecord): string => homeModCaption(v, mods);

  return (
    <>
      <div class="detail-head">
        <div>
          <h2>{tr('allSystems', 'All systems')}</h2>
        </div>
      </div>

      <div class="detail-body detail-body--home">
        {!views.length ? (
          <div class="home-empty">
            <div class="osf-eyebrow">{tr('noSystems', 'No systems online')}</div>
            <p>
              {tr(
                'noSystemsHint',
                'Mods that put terminals or overlays on screen appear here as launch cards. Mods that only carry settings are listed on the left.',
              )}
            </p>
          </div>
        ) : (
          <>
            {menus.length ? (
              <>
                <SectionHead title={tr('terminals', 'Terminals')} count={menus.length} note="" />
                <div class="home-grid">
                  {menus.map((v) => (
                    <MenuCard
                      key={v.id}
                      view={v}
                      tr={tr}
                      iconSrc={ownerIcon(v.mod)}
                      caption={caption(v)}
                      onOpen={onOpen}
                    />
                  ))}
                </div>
              </>
            ) : null}

            {huds.length ? (
              <>
                <SectionHead
                  title={tr('overlays', 'Overlays')}
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

/**
 * Owning-mod caption for a card: the settings mod's title when one is loaded,
 * else the manifest `mod` string verbatim (a view-only mod has no schema title
 * to borrow).
 */
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

/**
 * The card's ring. Static SVG nodes only; no `dangerouslySetInnerHTML` here or
 * anywhere in this view.
 *
 * Tinted through `currentColor`, so the whole ring recolours from one inline
 * `color` on the wrapper — a failed view overrides that to the kit's stop
 * signal instead of needing a second copy of the markup.
 */
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
          // Exclamation mark as two strokes: a glyph would need a font the
          // shipped bundle cannot rely on.
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
  onOpen: (viewId: string) => void;
}

function MenuCard({ view: v, tr, iconSrc, caption, onOpen }: MenuCardProps) {
  const failed = v.loadState === 'failed';
  const accent = homeAccentFor(v.id);
  // Single-menu policy: the opened panel replaces this surface, so there is no
  // local state to reconcile afterwards. The cooldown only swallows a dead
  // double-click when the open never happens.
  const [cooling, setCooling] = useState(false);
  const [iconFailed, setIconFailed] = useState(false);
  const showIcon = !!iconSrc && !iconFailed;

  return (
    <button
      type="button"
      class={failed ? 'home-tile failed' : 'home-tile'}
      data-label={(v.title || '').toLowerCase()}
      disabled={failed || cooling}
      onClick={() => {
        onOpen(v.id);
        setCooling(true);
        setTimeout(() => setCooling(false), OPEN_COOLDOWN_MS);
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
        {failed ? tr('failedSeeLog', 'FAILED — SEE OSF UI.LOG') : tr('openArrow', 'OPEN ▸')}
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

/**
 * The card itself is the switch: `role="switch"` on the button, with the
 * `.osf-switch` span as decoration keyed off the card's `aria-pressed` in CSS.
 * The span carries no role of its own — two nested switches would be two tab
 * stops for one control.
 */
function HudCard({ view: v, on, iconSrc, caption, onToggle }: HudCardProps) {
  const accent = homeAccentFor(v.id);
  const [iconFailed, setIconFailed] = useState(false);
  const showIcon = !!iconSrc && !iconFailed;

  return (
    <button
      type="button"
      class="home-hud"
      role="switch"
      aria-pressed={on ? 'true' : 'false'}
      onClick={() => onToggle(v.id, !on)}
    >
      <span
        class={showIcon ? 'home-hud-chip home-hud-chip--icon' : 'home-hud-chip'}
        style={{ color: accent }}
      >
        {showIcon ? (
          <img src={iconSrc as string} alt="" onError={() => setIconFailed(true)} />
        ) : (
          initials(v.title || v.id)
        )}
      </span>
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
