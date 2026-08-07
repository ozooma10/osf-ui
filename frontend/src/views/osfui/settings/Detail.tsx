// The right-hand pane, and the dispatcher for its six mutually exclusive
// modes. Dispatch order is the behaviour:
//   1. System Health    the fixed destination, which outranks even search —
//                       selecting it clears the filter, so the two never fight
//   2. search results   a non-empty filter replaces the pane, whatever is selected
//   3. Home             the launcher
//   4. not found        a selection that names no entry
//   5. view-only        an entry with views but no settings schema
//   6. settings page    the normal case
// Search wins over Home, so typing while on the launcher shows results rather
// than a filtered card grid. "Not found" precedes the view-only test because
// `entry.mod` cannot be read off an entry that does not exist.
//
// Accent: a mod's schema `accent` drives the kit's whole linked accent set on
// this subtree. Modes 1, 3 and 5 clear it, so one mod's colour cannot leak onto
// the launcher, onto System Health, or onto a mod that ships none. Modes 2 and 4
// leave it untouched, so searching from a red mod keeps a red-tinted result list.

import { Fragment } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';
import { Row } from '@ui/Row';
import { Note } from '@ui/Note';
import { ImageRow } from '@ui/ImageRow';
import { ActionButton } from '@ui/ActionButton';
import { Switch } from '@ui/Switch';
import { evalGate } from '@lib/settings/conditions';
import { isModified } from '@lib/settings/modified';
import { isSetting } from '@lib/settings/normalize';
import { safeAssetSrc, type AssetRoots } from '@lib/settings/assets';
import { findEntry, titleOf, FRAMEWORK_ID, HEALTH_ID, HOME_ID, type ModRecord, type ViewRecord } from '@lib/settings/rail';
import { pageBuckets, GENERAL_PAGE_ID, type PageBucket } from '@lib/settings/pages';
import { issueForSubject, type HealthModel } from '@lib/settings/health';
import { versionLess } from '@lib/version';
import type { SearchResult } from '@lib/settings/search';
import type { Translator } from '@lib/i18n';
import type { Setting, SettingsGroup, SettingsItem, SettingsSchema, SettingValue } from '@sdk';
import { SettingRow } from './SettingRow';
import type { KeyLabeler } from '@lib/keybinds/labels';
import { SearchResults } from './SearchResults';
import { Health } from './Health';
import { Home } from './Home';
import { Presets, type PresetRecord } from './Presets';
import type { CaptureTarget } from './useCapture';
import { devWarn } from './warn';
import { OPEN_COOLDOWN_MS } from './openCooldown';

// `groupSlug`/`groupKey` are exported because App.tsx needs the same strings
// when a search jump has to expand (and page-select) the group it lands in —
// it works from the schema rather than from a rendered Group. Both prefer the
// group's stable `id`: the index-keyed collapse fallback goes stale when a
// schema update reorders groups, and a label-derived anchor collides when two
// pages reuse a heading.

/** The anchor a section-index button jumps to. Only called on labelled groups. */
export function groupSlug(group: SettingsGroup): string {
  const base = (typeof group.id === 'string' && group.id) || group.label || '';
  return 'grp-' + base.toLowerCase().replace(/\s+/g, '-');
}

/** Stable identity for a group's collapse state. The `gi:`/`g` prefixes keep
 * an id that looks like a number from aliasing another group's index key. */
export function groupKey(ownerId: string, group: SettingsGroup, index: number): string {
  return typeof group.id === 'string' && group.id
    ? `${ownerId}::gi:${group.id}`
    : `${ownerId}::g${index}`;
}

/** The views section's collapse identity — never collides with a group's. */
function viewsSectionKey(ownerId: string): string {
  return `${ownerId}::views`;
}

export interface DetailProps {
  mods: ModRecord[];
  /** Localized keycap lookup for key rows; undefined per name = show the name. */
  labeler?: KeyLabeler;
  /** Complete, unfiltered `osfui/views` catalog for view status and health. */
  discoveredViews: ViewRecord[];
  /** Catalog-visible views used by normal navigation and per-mod view sections. */
  views: ViewRecord[];
  health: HealthModel;
  /** Pre-trimmed, pre-lowercased. Non-empty selects mode 2. */
  query: string;
  selectedId: string | null;
  osfuiReleaseVersion: string;
  tr: Translator;
  assetRoots: AssetRoots | undefined;

  /** Issue to expand in System Health, set by a deep link and cleared after. */
  focusIssueId: string | null;
  /** Jump to System Health with the failed-view `issueId` expanded. */
  onOpenIssue: (issueId: string) => void;
  /** Call a payload-free shell request endpoint from a health issue. */
  onShellRequest: (requestEndpoint: string) => void;

  /** User overrides on top of each group's schema `collapsed` default. */
  collapsed: Record<string, boolean>;
  onToggleGroup: (key: string, next: boolean) => void;

  /** Selected page tab per mod id, for mods whose schema declares `pages`. */
  activePages: Record<string, string>;
  onSelectPage: (modId: string, pageId: string) => void;

  capturing: CaptureTarget | null;
  /** The search-jump target to highlight, or null. */
  flash: { modId: string; key: string } | null;

  hudOn: (view: ViewRecord) => boolean;
  onOpenView: (viewId: string) => void;
  onHudToggle: (viewId: string, next: boolean) => void;
  /** Effective next-launch auto-start, optimistic while a save is in flight. */
  autoStartOf: (view: ViewRecord) => boolean;
  /** True while an `osfui.setViewAutoStart` for this view awaits its ack. */
  autoStartBusy: (view: ViewRecord) => boolean;
  onAutoStartToggle: (viewId: string, next: boolean) => void;

  onCommit: (modId: string, key: string, value: SettingValue) => void;
  onResetSetting: (modId: string, key: string) => void;
  onResetMod: (modId: string) => void;
  onBeginCapture: (modId: string, key: string) => void;
  onApplyPreset: (mod: ModRecord, preset: PresetRecord) => void;
  onJump: (result: SearchResult) => void;
  onToast: (message: string, kind?: 'warn' | 'danger') => void;
  runAction: (requestEndpoint: string, modId: string, key: string | undefined) => Promise<string | null>;
  /** bridge.applyAccent, injected so this file never touches the bridge. */
  applyAccent: (el: HTMLElement, hex: string | null) => void;
}

export function Detail(props: DetailProps) {
  const { mods, views, query, selectedId, tr } = props;
  const paneRef = useRef<HTMLElement | null>(null);

  const health = selectedId === HEALTH_ID;
  const entry =
    health || query || selectedId === HOME_ID ? undefined : findEntry(mods, views, selectedId);
  const schema: SettingsSchema = (entry && entry.mod && entry.mod.schema) || {};

  // `undefined` means "do not touch the accent at all".
  let accentIntent: string | null | undefined;
  if (health) accentIntent = null;
  else if (query) accentIntent = undefined;
  else if (selectedId === HOME_ID) accentIntent = null;
  else if (!entry) accentIntent = undefined;
  else if (!entry.mod) accentIntent = null;
  else accentIntent = schema.accent ?? null;

  useEffect(() => {
    const el = paneRef.current;
    if (!el || accentIntent === undefined) return;
    props.applyAccent(el, accentIntent);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- re-running on
    // anything but a change of intent fights the kit's own transitions.
  }, [accentIntent]);

  return (
    <section id="detail" class="detail" aria-live="polite" ref={paneRef}>
      {health ? (
        <Health
          health={props.health}
          tr={tr}
          focusIssueId={props.focusIssueId}
          onRetryView={props.onOpenView}
          onShellRequest={props.onShellRequest}
          onToast={props.onToast}
        />
      ) : query ? (
        <SearchResults mods={mods} query={query} tr={tr} onJump={props.onJump} />
      ) : selectedId === HOME_ID ? (
        <Home
          views={views}
          mods={mods}
          health={props.health}
          tr={tr}
          assetRoots={props.assetRoots}
          hudOn={props.hudOn}
          onOpen={props.onOpenView}
          onHud={props.onHudToggle}
          onOpenIssue={props.onOpenIssue}
        />
      ) : !entry ? (
        <div class="detail-empty">
          <div class="osf-eyebrow">{tr('nothingSelected', 'Nothing selected')}</div>
        </div>
      ) : !entry.mod ? (
        <ViewOnly {...props} entry={entry} />
      ) : (
        <SettingsPage {...props} mod={entry.mod} entryViews={entry.views} schema={schema} />
      )}
    </section>
  );
}

// Mode 5: a mod with discovered views but no settings schema.
function ViewOnly(props: DetailProps & { entry: NonNullable<ReturnType<typeof findEntry>> }) {
  const { entry, tr } = props;
  // Same lead-view rule as the rail title: prefer a menu, which reads like a
  // product name where a HUD often does not.
  const lead = entry.views.find((v) => v.kind === 'menu') || entry.views[0];

  return (
    <>
      <div class="detail-head">
        <div>
          <div class="osf-eyebrow kicker">
            {tr('mod', 'Mod') + (lead && lead.mod ? ' · ' + lead.mod : '')}
          </div>
          <h2>{entry.title}</h2>
          {lead && lead.description ? <div class="detail-desc">{lead.description}</div> : null}
        </div>
        {/* No "Reset all": there is nothing to reset. */}
      </div>
      <div class="detail-body">
        <ViewsSection {...props} views={entry.views} ownerId={entry.id} />
        <p class="detail-quiet">{tr('noModSettings', 'This mod registers no settings.')}</p>
      </div>
    </>
  );
}

// Mode 6: the settings page.
interface SettingsPageProps extends DetailProps {
  mod: ModRecord;
  entryViews: ViewRecord[];
  schema: SettingsSchema;
}

function SettingsPage(props: SettingsPageProps) {
  const { mod, schema, tr, osfuiReleaseVersion } = props;
  const values = mod.values || {};
  const isFramework = mod.id === FRAMEWORK_ID;

  // A schema with usable `pages` renders a tab row and only the active tab's
  // groups; without one, every group renders in one column as always. Either
  // way `pageGroups` carries the original schema index — collapse identity
  // and the search jump are keyed on it.
  const buckets = pageBuckets(schema);
  const wanted = buckets ? props.activePages[mod.id] : undefined;
  const activePageId = buckets
    ? (wanted && buckets.some((b) => b.id === wanted) ? wanted : buckets[0]!.id)
    : null;
  const pageGroups: Array<{ group: SettingsGroup; index: number }> = buckets
    ? buckets.find((b) => b.id === activePageId)!.groups
    : (schema.groups || []).map((group, index) => ({ group, index }));

  // The section index appears only above 4 labelled groups (of the page in
  // view, when paged); below that it is longer than the content it indexes.
  // Unlabelled groups do not count (no anchor to jump to) but still render.
  const labelled = pageGroups.filter(({ group }) => group.label);
  const autoIndex = labelled.length > 4;

  const restartCount = countRestartChanges(mod);

  return (
    <>
      <div class="detail-head">
        <div>
          <div class="osf-eyebrow kicker">
            {isFramework ? tr('framework', 'Framework') : tr('modWithId', 'Mod · {id}', { id: mod.id })}
          </div>
          <h2>{titleOf(mod)}</h2>
          {schema.description ? <div class="detail-desc">{schema.description}</div> : null}
        </div>
        <button
          type="button"
          class="osf-btn osf-btn--danger osf-btn--sm"
          onClick={() => props.onResetMod(mod.id)}
        >
          {tr('resetAll', 'Reset all')}
        </button>
      </div>

      <div class="detail-body">
        <ViewsSection {...props} views={props.entryViews} ownerId={mod.id} />

        {/* Advisory only, not a gate: everything below still renders
            best-effort, and a setting of a type this OSF UI release predates comes up
            read-only with its own per-row hint. */}
        {mod.targetVersion && versionLess(osfuiReleaseVersion, mod.targetVersion) ? (
          <div class="osf-note osf-note--warn">
            <div>
              {tr(
                'modNeedsUpdate',
                '{mod} was made for OSF UI {version} — some settings may be unavailable until you update OSF UI.',
                { mod: titleOf(mod), version: mod.targetVersion },
              )}
            </div>
          </div>
        ) : null}

        <Presets
          presets={schema.presets}
          tr={tr}
          onApply={(preset) => props.onApplyPreset(mod, preset)}
        />

        {/* The empty slot is kept: it is a styled node in the stylesheet's flow. */}
        <div class="banner-slot">
          {restartCount ? (
            <div class="banner banner--warn">
              <span class="banner-text">
                {tr.plural(
                  'restartChange',
                  restartCount,
                  '{count} change takes effect after a game restart.',
                  '{count} changes take effect after a game restart.',
                )}
              </span>
            </div>
          ) : null}
        </div>

        {buckets ? (
          <PageTabs
            buckets={buckets}
            activeId={activePageId as string}
            tr={tr}
            onSelect={(pageId) => props.onSelectPage(mod.id, pageId)}
          />
        ) : null}

        {autoIndex ? <SectionIndex {...props} groups={pageGroups} /> : null}

        {pageGroups.map(({ group, index }) => (
          <Group
            key={groupKey(mod.id, group, index)}
            {...props}
            group={group}
            index={index}
            values={values}
          />
        ))}

        {isFramework ? (
          <div class="group discovered-views-group">
            <div class="group-rows">
              <DiscoveredViews
                views={props.discoveredViews}
                tr={props.tr}
                onOpen={props.onOpenView}
              />
            </div>
          </div>
        ) : null}
      </div>
    </>
  );
}

/**
 * How many changed-from-default settings are flagged `requires:"restart"`.
 *
 * Only settings with a usable key count: keyless and unknown-type rows are not
 * rendered, and a row you cannot see the value of must not claim a restart.
 */
function countRestartChanges(mod: ModRecord): number {
  const values = mod.values || {};
  let n = 0;
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const item of g.settings || []) {
      if (!isSetting(item)) continue;
      const s = item as Setting;
      if (typeof s.key !== 'string' || !s.key) continue;
      if (s.requires !== 'restart') continue;
      if (isModified(s, values[s.key])) n++;
    }
  }
  return n;
}

/**
 * The tab row a paged schema renders in place of piling every group into one
 * column. The implicit General bucket has no authored label; it translates
 * here, at the last moment, like every other chrome string.
 */
function PageTabs({
  buckets,
  activeId,
  tr,
  onSelect,
}: {
  buckets: PageBucket[];
  activeId: string;
  tr: Translator;
  onSelect: (pageId: string) => void;
}) {
  return (
    <div class="page-tabs" role="tablist">
      {buckets.map((b) => (
        <button
          key={b.id}
          type="button"
          role="tab"
          aria-selected={b.id === activeId ? 'true' : 'false'}
          class={b.id === activeId ? 'page-tab active' : 'page-tab'}
          onClick={() => onSelect(b.id)}
        >
          {b.id === GENERAL_PAGE_ID ? tr('generalPage', 'General') : b.label}
        </button>
      ))}
    </div>
  );
}

interface SectionIndexProps extends SettingsPageProps {
  groups: Array<{ group: SettingsGroup; index: number }>;
}

function SectionIndex({ groups, mod, onToggleGroup }: SectionIndexProps) {
  return (
    <div class="section-index">
      {groups.map(({ group: g, index }) =>
        g.label ? (
          <button
            key={groupKey(mod.id, g, index)}
            type="button"
            class="section-index-item"
            onClick={() => {
              // The target may be collapsed — expand it first, or the scroll
              // lands on a heading with nothing under it.
              onToggleGroup(groupKey(mod.id, g, index), false);
              const target = document.getElementById(groupSlug(g));
              if (target) target.scrollIntoView({ block: 'start' });
            }}
          >
            {g.label}
          </button>
        ) : null,
      )}
    </div>
  );
}

interface GroupProps extends SettingsPageProps {
  group: SettingsGroup;
  index: number;
  values: Record<string, SettingValue>;
}

function Group(props: GroupProps) {
  const { group, index, values, mod, collapsed, onToggleGroup } = props;
  const key = groupKey(mod.id, group, index);
  // The schema's `collapsed` is only the default; a user toggle overrides it
  // and persists across re-renders, so applying a preset no longer snaps every
  // group back to its schema default.
  const isCollapsed = collapsed[key] ?? group.collapsed === true;
  const visible = evalGate(group.visibleWhen, values, (k) =>
    devWarn(`condition references unknown key "${k}"`),
  );

  const classes = ['group', isCollapsed ? 'collapsed' : '', visible ? '' : 'hidden-cond']
    .filter(Boolean)
    .join(' ');

  return (
    <div class={classes} {...(group.label ? { id: groupSlug(group) } : {})}>
      {group.label ? (
        <button type="button" class="group-label" onClick={() => onToggleGroup(key, !isCollapsed)}>
          {group.label}
        </button>
      ) : null}
      <div class="group-rows">
        {(group.settings || []).map((item, i) => (
          <Item key={itemKey(item, i)} {...props} item={item} values={values} />
        ))}
      </div>
    </div>
  );
}

/**
 * Reconciliation identity for a group item. Must be stable across re-renders
 * (else a control remounts mid-edit, losing an in-flight text edit or an open
 * action confirmation) and unique within the group (else Preact reuses the
 * wrong instance). Notes and images have no key, so they fall back to the index.
 */
function itemKey(item: SettingsItem, index: number): string {
  const it = item as { type?: unknown; key?: unknown; id?: unknown };
  if (typeof it.key === 'string' && it.key) return `k:${it.key}`;
  if (typeof it.id === 'string' && it.id) return `i:${it.id}`;
  return `n:${index}`;
}

interface ItemProps extends GroupProps {
  item: SettingsItem;
}

/** Dispatch order: notes, images, actions, then settings as the fallthrough. */
function Item(props: ItemProps) {
  const { item, values, mod, tr } = props;
  const it = item as { type?: unknown } | null;
  const warnUnknownKey = (k: string) => devWarn(`condition references unknown key "${k}"`);

  if (it && it.type === 'note') {
    const note = item as { text?: unknown; style?: unknown; visibleWhen?: unknown };
    return (
      <Note
        style={note.style}
        text={note.text}
        hiddenCond={!evalGate(note.visibleWhen as never, values, warnUnknownKey)}
      />
    );
  }

  if (it && it.type === 'image') {
    const img = item as { src?: unknown; caption?: unknown; height?: unknown; visibleWhen?: unknown };
    return (
      <ImageRow
        // Resolved with the harness roots the App was given, not from a global.
        // In production `assetRoots` is undefined and the path can only resolve
        // inside ../../<modId>/.
        src={safeAssetSrc(mod.id, img.src, props.assetRoots)}
        caption={typeof img.caption === 'string' ? img.caption : ''}
        height={typeof img.height === 'number' ? img.height : undefined}
        rejectedText={tr(
          'imageRejected',
          "Image path rejected (must be inside the mod's view folder).",
        )}
        hiddenCond={!evalGate(img.visibleWhen as never, values, warnUnknownKey)}
      />
    );
  }

  if (it && it.type === 'action') {
    const action = item as {
      key?: string;
      label?: string;
      hint?: string;
      command?: unknown;
      style?: string;
      confirm?: string;
      visibleWhen?: unknown;
      enabledWhen?: unknown;
    };
    const visible = evalGate(action.visibleWhen as never, values, warnUnknownKey);
    const enabled = evalGate(action.enabledWhen as never, values, warnUnknownKey);
    const classes = ['row--action', visible ? '' : 'hidden-cond', enabled ? '' : 'disabled']
      .filter(Boolean)
      .join(' ');

    return (
      <Row class={classes} dataKey="">
        <div class="row-text">
          <div class="row-label">{action.label || action.key || '(action)'}</div>
          {action.hint ? <div class="row-hint">{action.hint}</div> : null}
        </div>
        <div class="control">
          <ActionButton
            modId={mod.id}
            item={action}
            enabled={enabled}
            tr={tr}
            onToast={props.onToast}
            onRun={() => props.runAction(String(action.command), mod.id, action.key)}
          />
        </div>
      </Row>
    );
  }

  const setting = item as Setting;
  const capturing = props.capturing;
  const flash = props.flash;
  return (
    <SettingRow
      mod={mod}
      setting={setting}
      value={values[setting.key]}
      visible={evalGate(setting.visibleWhen, values, warnUnknownKey)}
      enabled={evalGate(setting.enabledWhen, values, warnUnknownKey)}
      listening={!!capturing && capturing.modId === mod.id && capturing.key === setting.key}
      flashing={!!flash && flash.modId === mod.id && flash.key === setting.key}
      tr={tr}
      labeler={props.labeler}
      onCommit={(key, value) => props.onCommit(mod.id, key, value)}
      onReset={(key) => props.onResetSetting(mod.id, key)}
      onBeginCapture={(key) => props.onBeginCapture(mod.id, key)}
      onInvalidColor={() =>
        props.onToast(tr('invalidColor', 'Enter a hex colour like #5aa9b8'), 'warn')
      }
    />
  );
}

/**
 * Complete OSF UI runtime discovery inventory for mod-provided views. Unlike the launcher
 * and per-mod view sections this intentionally includes `hub:false`,
 * debug-only and uninstantiated entries: its purpose is to let a user prove that a
 * mod view was discovered and drive the exact same open path without first making
 * it visible in normal menus. Framework-owned views are implementation detail,
 * so they do not crowd this list.
 */
function DiscoveredViews({
  views,
  tr,
  onOpen,
}: {
  views: ViewRecord[];
  tr: Translator;
  onOpen: (id: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const ordered = views
    .filter((view) => view.mod !== FRAMEWORK_ID)
    .sort((a, b) => a.id.localeCompare(b.id, undefined, { sensitivity: 'base' }));

  return (
    <div class="discovered-views">
      <button
        type="button"
        class="discovered-views-head"
        aria-expanded={open ? 'true' : 'false'}
        onClick={() => setOpen(!open)}
      >
        <div>
          {/* Compatibility catalog addresses retain the former registration noun. */}
          <div class="row-label">{tr('registeredViews', 'Discovered views')}</div>
          <div class="row-hint">
            {tr(
              'registeredViewsHint',
              'Mod-provided views discovered this session, including catalog-hidden and uninstantiated views.',
            )}
          </div>
        </div>
      </button>
      {open ? (
        <div class="discovered-views-body">
          {ordered.length ? (
            ordered.map((view) => (
              <Row key={view.id} class="discovered-view" dataKey="">
                <div class="row-text">
                  <div class="row-label">{view.title || view.id}</div>
                  <div class="row-hint discovered-view-meta">
                    <span class="discovered-view-id">{view.id}</span>
                    <span aria-hidden="true"> · </span>
                    <span>{view.kind || 'view'}</span>
                    <span aria-hidden="true"> · </span>
                    <span>{view.loadState || 'unloaded'}</span>
                  </div>
                </div>
                <div class="control">
                  <button
                    type="button"
                    class="osf-btn osf-btn--sm osf-btn--osf-accent"
                    onClick={() => onOpen(view.id)}
                  >
                    {/* Compatibility catalog address; this action opens the discovered view. */}
                    {tr('trigger', 'Open')}
                  </button>
                </div>
              </Row>
            ))
          ) : (
            <div class="discovered-views-empty">
              {tr('noRegisteredViews', 'No mod-provided views were discovered.')}
            </div>
          )}
        </div>
      ) : null}
    </div>
  );
}
// Views: the catalog entries attached to this mod, rendered above its settings
// groups. A Menu gets an Open button (single-menu policy means it replaces this
// view); a HUD gets a visibility toggle.

interface ViewsSectionProps extends DetailProps {
  views: ViewRecord[];
  ownerId: string;
}

function ViewsSection(props: ViewsSectionProps) {
  const { views, ownerId, tr, collapsed, onToggleGroup } = props;
  if (!views.length) return null;

  const menus = views.filter((v) => v.kind === 'menu');
  const huds = views.filter((v) => v.kind !== 'menu');
  // Compatibility catalog addresses retain terminal/overlay spellings.
  const label =
    menus.length && huds.length
      ? tr('terminalsOverlays', 'Menus & HUDs')
      : menus.length
        ? tr('terminals', 'Menus')
        : tr('overlays', 'HUDs');

  const key = viewsSectionKey(ownerId);
  // No schema default here — the views section starts expanded.
  const isCollapsed = collapsed[key] ?? false;

  return (
    <div class={isCollapsed ? 'group collapsed' : 'group'}>
      <button type="button" class="group-label" onClick={() => onToggleGroup(key, !isCollapsed)}>
        {label}
      </button>
      <div class="group-rows">
        {menus.map((v) => (
          <MenuRow
            key={v.id}
            view={v}
            tr={tr}
            issueId={(issueForSubject(props.health.issues, v.id) || {}).id ?? null}
            onOpen={props.onOpenView}
            onOpenIssue={props.onOpenIssue}
          />
        ))}
        {huds.map((v) => (
          <Fragment key={v.id}>
            <HudRow view={v} on={props.hudOn(v)} onToggle={props.onHudToggle} />
            {v.autoStartMutable === true ? (
              <AutoStartRow
                view={v}
                tr={tr}
                on={props.autoStartOf(v)}
                busy={props.autoStartBusy(v)}
                onToggle={props.onAutoStartToggle}
              />
            ) : null}
          </Fragment>
        ))}
      </div>
    </div>
  );
}

/**
 * The label/hint pair every view row renders. Action rows keep their own copy —
 * they label from `label`/`key` and hint from `hint`, a different shape.
 */
function ViewRowText({ view }: { view: ViewRecord }) {
  return (
    <div class="row-text">
      <div class="row-label">{view.title || view.id}</div>
      {view.description ? <div class="row-hint">{view.description}</div> : null}
    </div>
  );
}

function MenuRow({
  view: v,
  tr,
  issueId,
  onOpen,
  onOpenIssue,
}: {
  view: ViewRecord;
  tr: Translator;
  /** Health issue naming this view, when one is active. */
  issueId: string | null;
  onOpen: (id: string) => void;
  onOpenIssue: (issueId: string) => void;
}) {
  const failed = v.loadState === 'failed';
  const [opening, setOpening] = useState(false);

  // A failed row sends the player to the issue rather than to the log: the
  // issue says what happened, and carries the retry. A failure with no issue
  // (an older OSF UI runtime) keeps the old dead-end button.
  if (failed && issueId) {
    return (
      <Row class="" dataKey="">
        <ViewRowText view={v} />
        <div class="control">
          <button
            type="button"
            class="osf-btn osf-btn--sm osf-btn--danger"
            onClick={() => onOpenIssue(issueId)}
          >
            {tr('reviewIssue', 'Review issue')}
          </button>
        </div>
      </Row>
    );
  }

  return (
    <Row class="" dataKey="">
      <ViewRowText view={v} />
      <div class="control">
        <button
          type="button"
          class={`osf-btn osf-btn--sm ${failed ? 'osf-btn--danger' : 'osf-btn--osf-accent'}`}
          disabled={failed || opening}
          {...(failed ? { title: tr('viewFailed', 'The view failed to load; see OSF UI.log.') } : {})}
          onClick={() => {
            onOpen(v.id);
            // The opened menu replaces this view, so this state is normally
            // discarded with the page. The timer covers the open never
            // happening (failed registration), which would strand the button.
            setOpening(true);
            setTimeout(() => setOpening(false), OPEN_COOLDOWN_MS);
          }}
        >
          {failed ? tr('failed', 'Failed') : opening ? tr('opening', 'Opening…') : tr('open', 'Open')}
        </button>
      </div>
    </Row>
  );
}

function HudRow({
  view: v,
  on,
  onToggle,
}: {
  view: ViewRecord;
  on: boolean;
  onToggle: (id: string, next: boolean) => void;
}) {
  return (
    <Row class="" dataKey="">
      <ViewRowText view={v} />
      <div class="control">
        <Switch id="" on={on} disabled={false} onToggle={(next) => onToggle(v.id, next)} />
      </div>
    </Row>
  );
}

/**
 * The per-HUD startup-policy switch (protocol 1.6), rendered as its own row
 * under the HUD's visibility row so the two switches stay unambiguous. Only
 * views the OSF UI runtime marks `autoStartMutable` get one; the choice applies at the
 * next game launch, so the row deliberately does not touch `hud.show`/`hide`.
 */
function AutoStartRow({
  view: v,
  tr,
  on,
  busy,
  onToggle,
}: {
  view: ViewRecord;
  tr: Translator;
  on: boolean;
  busy: boolean;
  onToggle: (id: string, next: boolean) => void;
}) {
  return (
    <Row class="autostart-row" dataKey="">
      <div class="row-text">
        <div class="row-label">{tr('startAutomatically', 'Start automatically')}</div>
        <div class="row-hint">{tr('startAutomaticallyHint', 'Applies at the next game launch')}</div>
      </div>
      <div class="control">
        <Switch id="" on={on} disabled={busy} onToggle={(next) => onToggle(v.id, next)} />
      </div>
    </Row>
  );
}
