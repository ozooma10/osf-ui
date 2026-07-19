// SettingRow.tsx — one `.row` of a settings group: label line, hint, and the
// typed control.
//
// Ports `makeSettingRow`, `unknownRow` and `buildSettingControl`
// (settings/main.legacy.js:525-622).
//
// ---------------------------------------------------------------------------
// THE THREE STRUCTURAL RULES
// ---------------------------------------------------------------------------
//
// 1. THE ROW ID IS MOD-PREFIXED: `ctl-<mod.id>-<setting.key>`. Two mods that
//    both declare a key called "enabled" would otherwise mint the same DOM id,
//    and `label[for]` would bind the second mod's label to the FIRST mod's
//    control — clicking one label would toggle a different mod's setting.
//
// 2. A SETTING WITH NO USABLE KEY IS SKIPPED, not rendered. The store keeps
//    schemas verbatim, so a keyless setting reaches us intact; it can neither be
//    committed nor even labelled, and rendering it would put a dead control in
//    the pane. It is dropped with a devWarn instead (main.legacy.js:545-550).
//
// 3. THE CONTROL CELL ORDER IS `[reset ↺][optional readout][control]`. The
//    reset LEADS so the control itself stays flush with the row's right edge,
//    on the same line as surface and action rows — which have no reset slot at
//    all (main.legacy.js:598-606). Only the slider contributes a readout; the
//    stepper carries its own inside itself (see Stepper.tsx).
//
// ---------------------------------------------------------------------------
// DISABLING IS SCOPED TO THE CONTROL, NOT THE ROW
// ---------------------------------------------------------------------------
// `enabledWhen` greys the row (class `disabled`) but only actually disables the
// CONTROL subtree — the per-setting reset stays live, so a setting you have
// gated yourself out of is still resettable (main.legacy.js:1442-1446). Legacy
// achieved that by walking `control.querySelectorAll("input,select,button,
// textarea")` and skipping `.pending` nodes; here the `disabled` flag is simply
// threaded into the widget and withheld from the reset, which is the same set
// declaratively. The `.pending` exclusion lives in ActionButton.

import { Row } from '@ui/Row';
import { Badge } from '@ui/Badge';
import { Switch } from '@ui/Switch';
import { Slider } from '@ui/Slider';
import { Stepper } from '@ui/Stepper';
import { Segmented } from '@ui/Segmented';
import { Flags } from '@ui/Flags';
import { ColorField } from '@ui/ColorField';
import { TextField } from '@ui/TextField';
import { KeyField } from '@ui/KeyField';
import { requiresLabel } from '@lib/settings/format';
import { hasInvalidStep, stepperFor } from '@lib/settings/stepper';
import { resolveInputContext } from '@lib/settings/inputContext';
import { isModified } from '@lib/settings/modified';
import type { ModRecord } from '@lib/settings/rail';
import type { Translator } from '@lib/i18n';
import type { Setting, SettingValue } from '@sdk';
import { devWarn } from './warn';

/** main.legacy.js:553. */
export function rowId(modId: string, key: string): string {
  return `ctl-${modId}-${key}`;
}

export interface SettingRowProps {
  mod: ModRecord;
  setting: Setting;
  value: SettingValue | undefined;
  /** `visibleWhen` result — false adds `hidden-cond` (CSS display:none). */
  visible: boolean;
  /** `enabledWhen` result — false adds `disabled` AND disables the control. */
  enabled: boolean;
  /** True while THIS setting's key capture is armed. */
  listening: boolean;
  /** Search-jump highlight (`.flash`, 1.2s). */
  flashing: boolean;
  tr: Translator;
  onCommit: (key: string, value: SettingValue) => void;
  onReset: (key: string) => void;
  onBeginCapture: (key: string) => void;
  /** Invalid-colour warning, raised to the App so it owns the toast stack. */
  onInvalidColor: () => void;
}

export function SettingRow(props: SettingRowProps) {
  const { mod, setting, value, visible, enabled, listening, flashing, tr } = props;

  if (typeof setting.key !== 'string' || !setting.key) {
    devWarn(`skipping a "${setting.type}" setting with no key in "${mod.id}"`);
    return null;
  }

  const id = rowId(mod.id, setting.key);
  const control = renderControl(props, id);

  // An unknown/newer type renders read-only so a stale runtime degrades
  // cleanly. NOTE what this row does NOT do: it takes no part in
  // visibleWhen/enabledWhen and carries no modified dot (legacy returns before
  // pushing it into `liveRows`, main.legacy.js:557), and it has no `data-key`,
  // so a search result can never jump to it.
  if (control === null) {
    return (
      <div class="row row--unknown" data-label={labelText(setting)}>
        <div class="row-text">
          <div class="row-label">{setting.label || setting.key || '(setting)'}</div>
          <div class="row-hint">
            {tr('typeNeedsUpdate', 'Type "{type}" needs a newer OSF UI.', {
              type: String(setting.type),
            })}
          </div>
        </div>
      </div>
    );
  }

  const modified = isModified(setting, value);
  const classes = [
    visible ? '' : 'hidden-cond',
    enabled ? '' : 'disabled',
    modified ? 'is-modified' : '',
    flashing ? 'flash' : '',
  ]
    .filter(Boolean)
    .join(' ');

  const context =
    setting.type === 'key' ? resolveInputContext(mod.schema, setting, tr('gameplay', 'Gameplay')) : null;
  const conflicts = setting.type === 'key' && Array.isArray(setting.conflicts) ? setting.conflicts : [];

  return (
    <Row class={classes} dataLabel={labelText(setting)} dataKey={setting.key}>
      <div class="row-text">
        <div class="row-label-line">
          <label class="row-label" for={id}>
            {setting.label || setting.key}
          </label>
          <span
            class={modified ? 'osf-dot osf-on' : 'osf-dot'}
            title={tr('changedDefault', 'Changed from default')}
          />
          {setting.requires ? (
            <Badge modifier="osf-badge--warn" title="">
              {requiresLabel(setting.requires, tr)}
            </Badge>
          ) : null}
          {/* The gameplay context is the implicit default, so it earns no
              badge — an unresolvable reference falls back to it and therefore
              reads as "no special context" rather than as a broken badge. */}
          {context && context.id !== 'gameplay' ? (
            <Badge
              modifier=""
              title={
                context.blocksGameplay
                  ? tr(
                      'contextBlocksGameplay',
                      'Active in this context; Starfield gameplay bindings are unavailable.',
                    )
                  : tr('activeInputContext', 'Active in this input context.')
              }
            >
              {context.label}
            </Badge>
          ) : null}
          {/* Key collisions are INFORMATIONAL (mcm-design §9): the runtime
              never refuses a colliding bind, it just badges both sides. */}
          {conflicts.length ? (
            <Badge
              modifier="osf-badge--stop"
              title={tr('alsoBoundBy', 'Also bound by: {others}', {
                others: [...new Set(conflicts.map((c) => c.title || c.mod))].join(', '),
              })}
            >
              {tr('keyConflict', 'Key conflict')}
            </Badge>
          ) : null}
        </div>
        {setting.hint ? <div class="row-hint">{setting.hint}</div> : null}
      </div>

      <div class="control">
        <button
          type="button"
          class="row-reset"
          title={tr('resetDefault', 'Reset to default')}
          // Deliberately NOT disabled by enabledWhen — see the header.
          onClick={() => props.onReset(setting.key)}
        >
          ↺
        </button>
        {control}
      </div>
    </Row>
  );
}

/** `row.dataset.label`, lowercased for the (removed) DOM filter. */
function labelText(setting: Setting): string {
  return (setting.label || setting.key || '').toLowerCase();
}

/**
 * The typed control, or null when this host predates the type.
 *
 * Mirrors `buildSettingControl`'s switch (main.legacy.js:525-536) — including
 * `int` and `float` sharing one builder, which is why the int/float split lives
 * inside `stepperFor` rather than here.
 */
function renderControl(props: SettingRowProps, id: string) {
  const { mod, setting, value, enabled, tr } = props;
  const disabled = !enabled;
  const commit = (v: SettingValue) => props.onCommit(setting.key, v);

  switch (setting.type) {
    case 'bool':
      return (
        <Switch
          id={id}
          // STRICTLY `=== true`: undefined, or a truthy non-boolean that
          // slipped past the store, renders OFF.
          on={value === true}
          disabled={disabled}
          onToggle={commit}
        />
      );

    case 'int':
    case 'float': {
      // A step of 0, a negative, or a NaN would divide-by-zero inside the
      // stepper's snap and commit NaN over the bridge. hasInvalidStep reports
      // exactly the cases legacy warned about (a `step: null` is nullish and
      // silently takes the type default, with no warning — see the lib).
      if (hasInvalidStep(setting)) {
        devWarn(`"${setting.key}" has invalid step ${String(setting.step)}`);
      }
      const spec = stepperFor(setting);
      const current = typeof value === 'number' ? value : undefined;
      return setting.widget === 'stepper' ? (
        <Stepper id={id} spec={spec} setting={setting} value={current} disabled={disabled} onCommit={commit} />
      ) : (
        <Slider id={id} spec={spec} setting={setting} value={current} disabled={disabled} onCommit={commit} />
      );
    }

    case 'enum':
      return (
        <Segmented
          id={id}
          setting={setting}
          value={typeof value === 'string' ? value : undefined}
          disabled={disabled}
          onCommit={commit}
        />
      );

    case 'flags':
      return (
        <Flags
          id={id}
          setting={setting}
          value={Array.isArray(value) ? value : undefined}
          disabled={disabled}
          onCommit={commit}
        />
      );

    case 'string': {
      const current = typeof value === 'string' ? value : undefined;
      return setting.widget === 'color' ? (
        <ColorField
          id={id}
          value={current}
          disabled={disabled}
          onCommit={commit}
          onInvalid={props.onInvalidColor}
        />
      ) : (
        <TextField id={id} setting={setting} value={current} disabled={disabled} onCommit={commit} />
      );
    }

    case 'key':
      return (
        <KeyField
          id={id}
          value={typeof value === 'string' ? value : undefined}
          allowUnbound={setting.allowUnbound === true}
          listening={props.listening}
          disabled={disabled}
          onRebind={() => props.onBeginCapture(setting.key)}
          // The ✕ commits the deliberate unbound state; the store accepts ""
          // only because `allowUnbound` gates the button's existence.
          onUnbind={() => commit('')}
          listeningLabel={tr('pressKey', 'Press a key…')}
          unbindTitle={tr('unbind', 'Unbind')}
          unbindLabel={tr('unbindSetting', 'Unbind {setting}', {
            setting: setting.label || setting.key,
          })}
        />
      );

    default:
      // Unknown to this host. `mod` is unused on this path but the parameter
      // keeps the signature uniform.
      void mod;
      return null;
  }
}
