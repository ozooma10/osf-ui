// The right-hand pane, and the dispatcher for its five mutually exclusive
// modes. Dispatch order is the behaviour:
//   1. search results   a non-empty filter replaces the pane, whatever is selected
//   2. Home             the launcher
//   3. not found        a selection that names no entry
//   4. view-only        an entry with views but no settings schema
//   5. settings page    the normal case
// Search wins over Home, so typing while on the launcher shows results rather
// than a filtered card grid. "Not found" precedes the view-only test because
// `entry.mod` cannot be read off an entry that does not exist.
//
// Accent: a mod's schema `accent` drives the kit's whole linked accent set on
// this subtree. Modes 2 and 4 clear it, so one mod's colour cannot leak onto the
// launcher or onto a mod that ships none. Modes 1 and 3 leave it untouched, so
// searching from a red mod keeps a red-tinted result list.

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
import { findEntry, titleOf, FRAMEWORK_ID, HOME_ID, type ModRecord, type ViewRecord } from '@lib/settings/rail';
import { versionLess } from '@lib/version';
import type { SearchResult } from '@lib/settings/search';
import type { Translator } from '@lib/i18n';
import type { Setting, SettingsGroup, SettingsItem, SettingsSchema, SettingValue } from '@sdk';
import { SettingRow } from './SettingRow';
import { SearchResults } from './SearchResults';
import { Home } from './Home';
import { Presets, type PresetRecord } from './Presets';
import type { CaptureTarget } from './useCapture';
import { devWarn } from './warn';

/** How long a launched panel's button stays "Opening…". */
const OPEN_COOLDOWN_MS = 1600;

/** The anchor a section-index button jumps to. */
export function groupSlug(label: string): string {
  return 'grp-' + label.toLowerCase().replace(/\s+/g, '-');
}

/** Stable identity for a group's collapse state. */
export function groupKey(ownerId: string, index: number): string {
  return `${ownerId}::g${index}`;
}
export function surfacesKey(ownerId: string): string {
  return `${ownerId}::surfaces`;
}

export interface DetailProps {
  mods: ModRecord[];
  views: ViewRecord[];
  /** Unfiltered views.data, including hub:false platform/utility surfaces. */
  diagnosticViews: ViewRecord[];
  /** Pre-trimmed, pre-lowercased. Non-empty selects mode 1. */
  query: string;
  selectedId: string | null;
  hostVersion: string;
  tr: Translator;
  assetRoots: AssetRoots | undefined;

  /** User overrides on top of each group's schema `collapsed` default. */
  collapsed: Record<string, boolean>;
  onToggleGroup: (key: string, next: boolean) => void;

  capturing: CaptureTarget | null;
  /** The search-jump target to highlight, or null. */
  flash: { modId: string; key: string } | null;

  hudOn: (view: ViewRecord) => boolean;
  onOpenView: (viewId: string) => void;
  onHudToggle: (viewId: string, next: boolean) => void;
  onRenderStatsToggle: (viewId: string, next: boolean) => void;

  onCommit: (modId: string, key: string, value: SettingValue) => void;
  onResetSetting: (modId: string, key: string) => void;
  onResetMod: (modId: string) => void;
  onBeginCapture: (modId: string, key: string) => void;
  onApplyPreset: (mod: ModRecord, preset: PresetRecord) => void;
  onJump: (result: SearchResult) => void;
  onToast: (message: string, kind?: 'warn' | 'danger') => void;
  runAction: (command: string, modId: string, key: string | undefined) => Promise<string | null>;
  /** bridge.applyAccent, injected so this file never touches the bridge. */
  applyAccent: (el: HTMLElement, hex: string | null) => void;
}

export function Detail(props: DetailProps) {
  const { mods, views, query, selectedId, tr } = props;
  const paneRef = useRef<HTMLElement | null>(null);

  const entry = query || selectedId === HOME_ID ? undefined : findEntry(mods, views, selectedId);
  const schema: SettingsSchema = (entry && entry.mod && entry.mod.schema) || {};

  // `undefined` means "do not touch the accent at all".
  let accentIntent: string | null | undefined;
  if (query) accentIntent = undefined;
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
      {query ? (
        <SearchResults mods={mods} query={query} tr={tr} onJump={props.onJump} />
      ) : selectedId === HOME_ID ? (
        <Home
          views={views}
          mods={mods}
          tr={tr}
          assetRoots={props.assetRoots}
          hudOn={props.hudOn}
          onOpen={props.onOpenView}
          onHud={props.onHudToggle}
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

// Mode 4: a mod that registered views but no settings schema.
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
        <Surfaces {...props} views={entry.views} ownerId={entry.id} />
        <p class="detail-quiet">{tr('noModSettings', 'This mod registers no settings.')}</p>
      </div>
    </>
  );
}

// Mode 5: the settings page.
interface SettingsPageProps extends DetailProps {
  mod: ModRecord;
  entryViews: ViewRecord[];
  schema: SettingsSchema;
}

function SettingsPage(props: SettingsPageProps) {
  const { mod, schema, tr, hostVersion } = props;
  const values = mod.values || {};
  const isFramework = mod.id === FRAMEWORK_ID;
  const groups = schema.groups || [];

  // The section index appears only above 4 labelled groups; below that it is
  // longer than the content it indexes. Unlabelled groups do not count (no
  // anchor to jump to) but still render.
  const labelled = groups.filter((g) => g.label);
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
        <Surfaces {...props} views={props.entryViews} ownerId={mod.id} />

        {isFramework ? <RenderDiagnostics {...props} /> : null}

        {/* Advisory only, not a gate: everything below still renders
            best-effort, and a setting of a type this host predates comes up
            read-only with its own per-row hint. */}
        {mod.targetVersion && versionLess(hostVersion, mod.targetVersion) ? (
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

        {autoIndex ? <SectionIndex {...props} groups={groups} /> : null}

        {groups.map((group, i) => (
          <Group key={groupKey(mod.id, i)} {...props} group={group} index={i} values={values} />
        ))}
      </div>
    </>
  );
}

function RenderDiagnostics({ diagnosticViews, onRenderStatsToggle, tr }: DetailProps) {
  const views = [...diagnosticViews].sort((a, b) =>
    (a.title || a.id).localeCompare(b.title || b.id, undefined, { sensitivity: 'base' }),
  );
  if (!views.length) return null;

  return (
    <div class="group render-diagnostics">
      <div class="group-label-static">{tr('renderDiagnostics', 'Render diagnostics')}</div>
      <p class="group-hint">
        {tr(
          'renderDiagnosticsHint',
          'Overlay live browser, capture and transfer timing on a view. Diagnostic sampling adds a small amount of work.',
        )}
      </p>
      <div class="group-rows">
        {views.map((view) => {
          const failed = view.loadState === 'failed';
          return (
            <Row key={view.id} class={failed ? 'disabled' : ''} dataLabel={(view.title || view.id).toLowerCase()} dataKey="">
              <div class="row-text">
                <div class="row-label">{view.title || view.id}</div>
                <div class="row-hint">{view.id}</div>
              </div>
              <div class="control render-stats-control">
                <span class="render-stats-state">
                  {view.renderStats ? tr('statsOn', 'Stats on') : tr('statsOff', 'Stats off')}
                </span>
                <Switch
                  id={`render-stats-${view.id}`}
                  on={view.renderStats === true}
                  disabled={failed}
                  onToggle={(next) => onRenderStatsToggle(view.id, next)}
                />
              </div>
            </Row>
          );
        })}
      </div>
    </div>
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

interface SectionIndexProps extends SettingsPageProps {
  groups: SettingsGroup[];
}

function SectionIndex({ groups, mod, collapsed, onToggleGroup }: SectionIndexProps) {
  return (
    <div class="section-index">
      {groups.map((g, i) =>
        g.label ? (
          <button
            key={groupKey(mod.id, i)}
            type="button"
            class="section-index-item"
            onClick={() => {
              // The target may be collapsed — expand it first, or the scroll
              // lands on a heading with nothing under it.
              onToggleGroup(groupKey(mod.id, i), false);
              const target = document.getElementById(groupSlug(g.label as string));
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
  const key = groupKey(mod.id, index);
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
    <div class={classes} {...(group.label ? { id: groupSlug(group.label) } : {})}>
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
      <Row class={classes} dataLabel={(action.label || action.key || '').toLowerCase()} dataKey="">
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
      onCommit={(key, value) => props.onCommit(mod.id, key, value)}
      onReset={(key) => props.onResetSetting(mod.id, key)}
      onBeginCapture={(key) => props.onBeginCapture(mod.id, key)}
      onInvalidColor={() =>
        props.onToast(tr('invalidColor', 'Enter a hex colour like #5aa9b8'), 'warn')
      }
    />
  );
}

// Surfaces: the catalog views attached to this entry, rendered above the
// settings groups. A menu gets an Open button (menu.open — single-menu policy,
// so the opened panel replaces this surface); a HUD gets a hud.show/hud.hide
// switch.

interface SurfacesProps extends DetailProps {
  views: ViewRecord[];
  ownerId: string;
}

function Surfaces(props: SurfacesProps) {
  const { views, ownerId, tr, collapsed, onToggleGroup } = props;
  if (!views.length) return null;

  const menus = views.filter((v) => v.kind === 'menu');
  const huds = views.filter((v) => v.kind !== 'menu');
  const label =
    menus.length && huds.length
      ? tr('terminalsOverlays', 'Terminals & overlays')
      : menus.length
        ? tr('terminals', 'Terminals')
        : tr('overlays', 'Overlays');

  const key = surfacesKey(ownerId);
  // No schema default here — the surfaces section starts expanded.
  const isCollapsed = collapsed[key] ?? false;

  return (
    <div class={isCollapsed ? 'group collapsed' : 'group'}>
      <button type="button" class="group-label" onClick={() => onToggleGroup(key, !isCollapsed)}>
        {label}
      </button>
      <div class="group-rows">
        {menus.map((v) => (
          <PanelRow key={v.id} view={v} tr={tr} onOpen={props.onOpenView} />
        ))}
        {huds.map((v) => (
          <HudRow key={v.id} view={v} on={props.hudOn(v)} onToggle={props.onHudToggle} />
        ))}
      </div>
    </div>
  );
}

function PanelRow({
  view: v,
  tr,
  onOpen,
}: {
  view: ViewRecord;
  tr: Translator;
  onOpen: (id: string) => void;
}) {
  const failed = v.loadState === 'failed';
  const [opening, setOpening] = useState(false);

  return (
    <Row class="" dataLabel={(v.title || '').toLowerCase()} dataKey="">
      <div class="row-text">
        <div class="row-label">{v.title || v.id}</div>
        {v.description ? <div class="row-hint">{v.description}</div> : null}
      </div>
      <div class="control">
        <button
          type="button"
          class={`osf-btn osf-btn--sm ${failed ? 'osf-btn--danger' : 'osf-btn--osf-accent'}`}
          disabled={failed || opening}
          {...(failed ? { title: tr('viewFailed', 'The view failed to load; see OSF UI.log.') } : {})}
          onClick={() => {
            onOpen(v.id);
            // The opened panel replaces this surface, so this state is normally
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
    <Row class="" dataLabel={(v.title || '').toLowerCase()} dataKey="">
      <div class="row-text">
        <div class="row-label">{v.title || v.id}</div>
        {v.description ? <div class="row-hint">{v.description}</div> : null}
      </div>
      <div class="control">
        <Switch id="" on={on} disabled={false} onToggle={(next) => onToggle(v.id, next)} />
      </div>
    </Row>
  );
}
