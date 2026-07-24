// Vite dev harness entry, dev only. One page, one mock, one stage;
// `?view=<modId>/<viewName>` selects which view's App is mounted.
//
// The import block below is order-sensitive: the mock must install
// `window.osfui.postMessage` before the shared kit's module body runs, or the
// kit concludes there is no bridge and every view boots standalone.

import './install-mock';
import '../src/shared-kit/osfui.js';
import '../src/legacy/padnav.js';

// Stylesheets in shipped order: kit tokens first (the harness chrome consumes
// its --osf-* custom properties), harness chrome last. Each view's own style.css
// loads with the view, below.
import '../src/shared-kit/osfui.css';
import './harness.css';

import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import type { FunctionComponent } from 'preact';

import { Stage } from './Stage';
import { Toolbar } from './Toolbar';
import { mock } from './install-mock';
import { LOCALE_EVENT } from './mockbridge';

// View registry. `import.meta.glob` rather than static imports: it builds even
// while a view's App.tsx does not exist yet, and each view's App and style.css
// load only when selected, so a broken view cannot take the others down.

const VIEW_APPS = import.meta.glob<{ App: FunctionComponent }>('../src/views/*/*/App.tsx');
const VIEW_STYLES = import.meta.glob('../src/views/*/*/style.css');

/** Toolbar labels for the views the harness knows about. */
const VIEW_TITLES: Record<string, string> = {
  'osfui/settings': 'Mods',
  'osfui/keybinds': 'Keybinds',
};

const DEFAULT_VIEW = 'osfui/settings';

function viewKey(view: string, file: string): string {
  return `../src/views/${view}/${file}`;
}

/** "<modId>/<viewName>" ids for every App.tsx the glob found, in stable order. */
function knownViews(): string[] {
  return Object.keys(VIEW_APPS)
    .map((path) => path.replace('../src/views/', '').replace('/App.tsx', ''))
    .sort((a, b) => {
      // Documented order (Mods, Keybinds, then anything new) rather than
      // alphabetical, which would put Keybinds first.
      const ia = Object.keys(VIEW_TITLES).indexOf(a);
      const ib = Object.keys(VIEW_TITLES).indexOf(b);
      if (ia !== ib) return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib);
      return a.localeCompare(b);
    });
}

const params = new URLSearchParams(location.search);

// `?view=` picks the mounted view.
const requested = params.get('view') || DEFAULT_VIEW;
// Unrecognised falls back to the Mods surface, and past that to whatever App
// does exist, so the harness never renders an empty stage.
const activeView = VIEW_APPS[viewKey(requested, 'App.tsx')]
  ? requested
  : VIEW_APPS[viewKey(DEFAULT_VIEW, 'App.tsx')]
    ? DEFAULT_VIEW
    : knownViews()[0] || DEFAULT_VIEW;
if (activeView !== requested) {
  console.warn(`[harness] no App at src/views/${requested}/App.tsx — showing ${activeView}.`);
}

// The fixed reference stage is the default: a view that only looks right when it
// can reflow to the browser window will be wrong in game. `?res=off` opts into
// the fluid fill-the-window mode.
const STAGE_DEFAULT = params.get('res') !== 'off';

// `?schema=`, `?fixtures=1` and `?locale=` are read (and persisted to
// localStorage) by the mock itself, as is the drag-drop loader for schemas and
// `<modId>_<locale>.json` catalogs. The mock owns what the bridge serves; this
// file owns only how the page is framed.

function Harness() {
  const [App, setApp] = useState<FunctionComponent | null>(null);
  const [failed, setFailed] = useState<string>('');
  const [stageOn, setStageOn] = useState(STAGE_DEFAULT);
  const [fixturesOn, setFixturesOn] = useState(mock.fixturesOn());
  const [healthScenario, setHealthScenario] = useState(mock.healthScenario());
  const [locale, setLocale] = useState(mock.locale());

  // Commands that omit `view` target the calling surface; tell the mock which
  // one that is.
  useEffect(() => {
    mock.setSelfView(activeView);
  }, []);

  useEffect(() => {
    let live = true;
    const style = VIEW_STYLES[viewKey(activeView, 'style.css')];
    if (style) void style();
    const loader = VIEW_APPS[viewKey(activeView, 'App.tsx')];
    if (!loader) {
      setFailed(`No App.tsx for "${activeView}".`);
      return;
    }
    loader().then(
      (mod) => {
        if (!live) return;
        if (typeof mod.App === 'function') setApp(() => mod.App);
        else setFailed(`src/views/${activeView}/App.tsx does not export "App".`);
      },
      (err: unknown) => {
        if (live) setFailed(String(err));
      },
    );
    return () => {
      live = false;
    };
  }, []);

  // Stage mode drives a body class: the fluid-mode margins that clear the
  // toolbar are a body-level rule in harness.css, and the view's own root must
  // stay untouched in stage mode.
  useEffect(() => {
    document.body.classList.toggle('res900', stageOn);
  }, [stageOn]);

  // A dropped catalog can auto-activate its locale (mockbridge applyLocale), so
  // the picker follows the mock, not the other way round.
  useEffect(() => {
    const onLocale = (e: Event) => {
      const detail = (e as CustomEvent<{ locale: string }>).detail;
      if (detail && detail.locale) setLocale(detail.locale);
    };
    window.addEventListener(LOCALE_EVENT, onLocale);
    return () => window.removeEventListener(LOCALE_EVENT, onLocale);
  }, []);

  return (
    <>
      <Toolbar
        mock={mock}
        view={activeView}
        views={[
          ...knownViews().map((id) => ({ id, title: VIEW_TITLES[id] || id })),
          // The OSF Animation browser is the sibling repo's view in an iframe on
          // its own page: it self-mocks and must not get this page's bridge, so
          // it is a plain link, not a ?view= target.
          { id: 'osf.animation/browser', title: 'OSF Animation', href: 'osf.html' },
        ]}
        stageOn={stageOn}
        onStage={setStageOn}
        fixturesOn={fixturesOn}
        onFixtures={(on) => setFixturesOn(mock.fixtures(on))}
        healthScenario={healthScenario}
        onHealth={() => setHealthScenario(mock.health())}
        locale={locale}
        onLocale={(loc) => {
          setLocale(loc);
          void mock.locale(loc);
        }}
      />
      <Stage enabled={stageOn}>
        {App ? renderView(App, activeView) : <p class="status osf-eyebrow">{failed || 'Loading view…'}</p>}
      </Stage>
    </>
  );
}

/**
 * Mount a view's App, injecting the harness-only props it accepts.
 *
 * The Mods (settings) view resolves schema `icon`/`image` paths against a
 * mod-id -> root map. In game every view sits under one `views/` root, so the
 * shipped default "../../<modId>" is correct and the view passes nothing; from
 * the harness that lands nowhere, so the mock's map is handed in explicitly.
 * Reading the mock's global here and passing it as a prop keeps
 * `window.OSFUI_MOD_ASSET_ROOTS` out of src/, so nothing that sets that global
 * can redirect the shipped path.
 */
function renderView(App: FunctionComponent, view: string) {
  if (view === 'osfui/settings') {
    const roots = (window as { OSFUI_MOD_ASSET_ROOTS?: Record<string, string> }).OSFUI_MOD_ASSET_ROOTS;
    const Settings = App as FunctionComponent<{ assetRoots?: Record<string, string> }>;
    return <Settings {...(roots ? { assetRoots: roots } : {})} />;
  }
  return <App />;
}

const mount = document.getElementById('harness-mount');
if (mount) render(<Harness />, mount);
