// Dev harness control bar. Dev only, never shipped in a view.

import { LOCALES, type MockApi } from './mockbridge';

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
  stageOn: boolean;
  onStage: (on: boolean) => void;
  fixturesOn: boolean;
  onFixtures: (on: boolean) => void;
  healthScenario: string;
  onHealth: () => void;
  locale: string;
  onLocale: (loc: string) => void;
}

export function Toolbar(props: ToolbarProps) {
  const { mock, view, views, stageOn, onStage, fixturesOn, onFixtures, healthScenario, onHealth, locale, onLocale } =
    props;

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

      <button type="button" onClick={() => mock.reset()}>
        Reset stored values
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
        title="Cycle the System Health scenario pushed as diagnostics.data: clean → warnings → errors → mixed → resolved-only"
        onClick={onHealth}
      >
        Health: {healthScenario}
      </button>

      <button
        type="button"
        class={stageOn ? 'on' : ''}
        title="Render the view in the game-true 1600×900 reference stage, scaled to fill the window like the game scales it to screen"
        onClick={() => onStage(!stageOn)}
      >
        1600×900
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
            Locale: {l}
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

      <span class="hint">
        Drop a settings/&lt;id&gt;.json or l10n/&lt;id&gt;_&lt;locale&gt;.json here, or add ?schema=&lt;url&gt; —
        traffic logs to the console
      </span>
    </div>
  );
}
