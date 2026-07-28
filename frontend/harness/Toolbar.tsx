// Dev harness control bar. Dev only, never shipped in a view.

import { nextStageMode, type StageMode } from './Stage';
import { LOCALES, type MockApi } from '../devmock/mockbridge';

/** Button face and tooltip per stage mode, in cycle order. */
const STAGE_LABELS: Record<StageMode, { label: string; title: string }> = {
  fixed: {
    label: '1600×900',
    title: 'Stage: the game-true 1600×900 reference frame, letterboxed and scaled to the window. Click to fill the window instead.',
  },
  fill: {
    label: 'Fill window',
    title:
      'Stage: 900 reference rows tall, widened to the window aspect — how the game resizes the view to the output. Click for fluid (unscaled) mode.',
  },
  off: {
    label: 'Fluid',
    title: 'No stage: the view reflows to the raw browser window, unscaled. Click to return to the 1600×900 frame.',
  },
};

/**
 * One entry in the view switcher. `href` overrides the default `?view=<id>`
 * link — OSF Animation's browser is a separate page (it loads the sibling
 * repo's real view in an iframe and self-mocks), not a view this page can
 * mount.
 */
export interface ToolbarView {
  id: string;
  title: string;
  href?: string;
}

export interface ToolbarProps {
  mock: MockApi;
  view: string;
  views: ToolbarView[];
  stageMode: StageMode;
  onStage: (mode: StageMode) => void;
  fixturesOn: boolean;
  onFixtures: (on: boolean) => void;
  healthScenario: string;
  onHealth: () => void;
  locale: string;
  onLocale: (loc: string) => void;
}

export function Toolbar(props: ToolbarProps) {
  const { mock, view, views, stageMode, onStage, fixturesOn, onFixtures, healthScenario, onHealth, locale, onLocale } =
    props;
  const stage = STAGE_LABELS[stageMode];

  return (
    <div class="harness-bar">
      <b>OSF UI · MOCK BRIDGE</b>

      {views.map((v) => (
        <a
          key={v.id}
          class={v.id === view ? 'here' : ''}
          href={v.href || `?view=${encodeURIComponent(v.id)}`}
        >
          {v.title}
        </a>
      ))}

      <button type="button" title="Clear stored setting values" onClick={() => mock.reset()}>
        Reset values
      </button>

      <button
        type="button"
        class={fixturesOn ? 'on' : ''}
        title="Show fictional sample panels/HUDs that exercise every catalog state (failed load, HUD live/hidden, …)"
        onClick={() => onFixtures(!fixturesOn)}
      >
        Sample views: {fixturesOn ? 'on' : 'off'}
      </button>

      <button
        type="button"
        class={healthScenario !== 'clean' ? 'on' : ''}
        title="Cycle the System Health scenario pushed as diagnostics.data: clean → warnings → errors → mixed → resolved-only → catalog (every known code + one unknown)"
        onClick={onHealth}
      >
        Health: {healthScenario}
      </button>

      <button
        type="button"
        class={stageMode !== 'off' ? 'on' : ''}
        title={stage.title}
        onClick={() => onStage(nextStageMode(stageMode))}
      >
        {stage.label}
      </button>

      <select
        class={locale !== 'en' ? 'on' : ''}
        value={locale}
        title="Preview localization. 'pseudo' pseudo-localizes every localized string ([åççéñŧš] + padding) so hardcoded text and tight layouts stand out; a real locale applies l10n catalogs — drop a <modId>_<locale>.json on the page, same file the game loads."
        onChange={(e) => onLocale((e.currentTarget as HTMLSelectElement).value)}
      >
        {/* A dropped catalog can introduce a locale not in this list; include the
            active one so the select never shows a blank value. */}
        {(LOCALES.includes(locale) ? LOCALES : [...LOCALES, locale]).map((l) => (
          <option key={l} value={l}>
            {l}
            {l === 'en' ? ' (authored)' : ''}
          </option>
        ))}
      </select>

      {/* Injectors for the messages the runtime pushes in game. */}
      <button type="button" title="Inject a ui.hotkey message" onClick={() => mock.hotkey()}>
        Hotkey
      </button>
      <button
        type="button"
        title="Inject a ui.gamepad LB down-edge (cycles the rail)"
        onClick={() => mock.gamepad('LB')}
      >
        LB
      </button>
      <button
        type="button"
        title="Inject a ui.gamepad RB down-edge (cycles the rail)"
        onClick={() => mock.gamepad('RB')}
      >
        RB
      </button>

      {/* Kept short so it does not crowd the controls; the full instruction is
          in the tooltip, and this truncates before any button does. */}
      <span
        class="hint"
        title="Drop a settings/<id>.json or l10n/<id>_<locale>.json onto the page, or add ?schema=<url>. Bridge traffic logs to the console."
      >
        Drop a schema or l10n file here
      </span>
    </div>
  );
}
