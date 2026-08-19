
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
import { canonicalName } from '@lib/keybinds/canonical';
import type { KeyLabeler } from '@lib/keybinds/labels';
import { requiresLabel } from '@lib/settings/format';
import { hasInvalidStep, stepperFor } from '@lib/settings/stepper';
import { resolveHotkeyContext } from '@lib/settings/hotkeyContext';
import { isModified } from '@lib/settings/modified';
import type { ModRecord } from '@lib/settings/rail';
import type { Translator } from '@lib/i18n';
import type { Setting, SettingValue } from '@sdk';
import { devWarn } from './warn';

export function rowId(modId: string, key: string): string {
  return `ctl-${modId}-${key}`;
}

export interface SettingRowProps {
  mod: ModRecord;
  setting: Setting;
  value: SettingValue | undefined;
  /** `visibleWhen` result — false adds `hidden-cond` (CSS display:none). */
  visible: boolean;
  /** `enabledWhen` result — false adds `disabled` and disables the control. */
  enabled: boolean;
  /** True while this setting's key capture is armed. */
  listening: boolean;
  /** Search-jump highlight (`.flash`, 1.2s). */
  flashing: boolean;
  tr: Translator;
  /** Localized keycap lookup for the key control; undefined = show the name. */
  labeler?: KeyLabeler | undefined;
  onCommit: (key: string, value: SettingValue) => void;
  onReset: (key: string) => void;
  onBeginCapture: (key: string) => void;
  /** Invalid-colour warning, raised to the App so it owns the toast stack. */
  onInvalidColor: () => void;
}

export function SettingRow(props: SettingRowProps) {
  const { mod, setting, value, visible, enabled, flashing, tr } = props;

  if (typeof setting.key !== 'string' || !setting.key) {
    devWarn(`skipping a "${setting.type}" setting with no key in "${mod.id}"`);
    return null;
  }

  const id = rowId(mod.id, setting.key);
  const control = renderControl(props, id);

  if (control === null) {
    return (
      <div class="row row--unknown">
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
    setting.type === 'key' ? resolveHotkeyContext(mod.schema, setting, tr('gameplay', 'Gameplay')) : null;
  const conflicts = setting.type === 'key' && Array.isArray(setting.conflicts) ? setting.conflicts : [];

  return (
    <Row class={classes} dataKey={setting.key}>
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
          {/* Gameplay is the implicit default, so it gets no badge — an
              unresolvable reference falls back to it and reads as "no special
              context" rather than as a broken badge. */}
          {context && context.id !== 'gameplay' ? (
            /* `activeInputContext` is a compatibility catalog address. */
            <Badge
              modifier=""
              title={
                context.blocksGameplay
                  ? tr(
                      'contextBlocksGameplay',
                      'Active in this context; Starfield gameplay bindings are unavailable.',
                    )
                  : tr('activeInputContext', 'Active in this hotkey context.')
              }
            >
              {context.label}
            </Badge>
          ) : null}
          {/* Key collisions are informational: the OSF UI runtime does
              not refuse a colliding bind, it badges both sides. */}
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
          // Not disabled by enabledWhen — see the header.
          onClick={() => props.onReset(setting.key)}
        >
          ↺
        </button>
        {control}
      </div>
    </Row>
  );
}

function renderControl(props: SettingRowProps, id: string) {
  const { mod, setting, value, enabled, tr } = props;
  const disabled = !enabled;
  const commit = (v: SettingValue) => props.onCommit(setting.key, v);

  switch (setting.type) {
    case 'bool':
      return (
        <Switch
          id={id}
          on={value === true}
          disabled={disabled}
          onToggle={commit}
        />
      );

    case 'int':
    case 'float': {
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
          label={
            typeof value === 'string' && value
              ? props.labeler?.(canonicalName(value))
              : undefined
          }
          allowUnbound={setting.allowUnbound === true}
          listening={props.listening}
          disabled={disabled}
          onRebind={() => props.onBeginCapture(setting.key)}
          onUnbind={() => commit('')}
          listeningLabel={tr('pressKey', 'Press a key…')}
          unbindTitle={tr('unbind', 'Unbind')}
          unbindLabel={tr('unbindSetting', 'Unbind {setting}', {
            setting: setting.label || setting.key,
          })}
        />
      );

    default:
      void mod;
      return null;
  }
}
