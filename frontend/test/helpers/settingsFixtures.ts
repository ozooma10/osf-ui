// Hand-written settings.data / views.data payloads for the settings-view suites.
// Cast through `unknown`: wire fixtures, not SDK-constructed values, so optional
// fields are omitted.

import type { SettingsDataPayload, ViewsDataPayload } from '@sdk';

/** A mod with one of every widget the view renders, for the widget-quirk suite. */
export const WIDGETS: SettingsDataPayload = {
  mods: [
    {
      id: 'osfui',
      title: 'OSF UI',
      values: { toggleKey: 'F10', allowPanels: true },
      schema: {
        accent: '#5aa9b8',
        groups: [
          {
            label: 'Input',
            settings: [
              { key: 'toggleKey', label: 'Open / close key', type: 'key', default: 'F10' },
            ],
          },
        ],
      },
    },
    {
      id: 'acme.kit',
      title: 'Acme Kit',
      values: {
        boolOn: true,
        boolTruthy: 1,
        slide: 40,
        step: 3,
        segMode: 'b',
        pickMode: 'y',
        flagSet: ['read', 'write'],
        colorHex: '#5AA9B8',
        note: '',
        text: 'hello',
        bindKey: 'K',
      },
      schema: {
        accent: '#c8503a',
        presets: [
          { label: 'Aggressive', values: { slide: 90, boolOn: false } },
          { label: 'Empty', values: null },
        ],
        groups: [
          {
            label: 'Toggles',
            settings: [
              { key: 'boolOn', label: 'Bool On', type: 'bool', default: false },
              // A truthy-but-non-boolean value must render OFF, strictly.
              { key: 'boolTruthy', label: 'Bool Truthy', type: 'bool', default: false },
            ],
          },
          {
            label: 'Numbers',
            settings: [
              { key: 'slide', label: 'Slider', type: 'int', min: 0, max: 100, step: 1, default: 50 },
              {
                key: 'step',
                label: 'Stepper',
                type: 'int',
                min: 0,
                max: 10,
                step: 3,
                widget: 'stepper',
                default: 0,
              },
            ],
          },
          {
            label: 'Choices',
            settings: [
              {
                key: 'segMode',
                label: 'Segmented',
                type: 'enum',
                widget: 'segmented',
                options: ['a', 'b', 'c'],
                default: 'a',
              },
              {
                key: 'pickMode',
                label: 'Dropdown',
                type: 'enum',
                options: ['x', 'y', 'z', 'w', 'v', 'u'],
                default: 'x',
              },
              {
                key: 'flagSet',
                label: 'Flags',
                type: 'flags',
                options: ['read', 'write', 'exec'],
                default: [],
              },
            ],
          },
          {
            label: 'Text',
            settings: [
              { key: 'colorHex', label: 'Colour', type: 'string', widget: 'color', default: '#000000' },
              { key: 'text', label: 'Text', type: 'string', default: '' },
              { key: 'bindKey', label: 'Bind', type: 'key', allowUnbound: true, default: 'K' },
              { type: 'note', style: 'warn', text: 'A **bold** note' },
              { type: 'note', style: 'evil', text: 'sneaky' },
              { type: 'action', key: 'go', label: 'Run it', command: 'acme.kit.run' },
              { type: 'action', key: 'bad', label: 'Reserved', command: 'ui.doThing' },
              // A keyless setting: must be skipped, not rendered.
              { label: 'No key here', type: 'bool', default: false },
              // A type this host predates: read-only row--unknown.
              { key: 'future', label: 'Future', type: 'quantum', default: 0 },
            ],
          },
        ],
      },
    },
  ],
} as unknown as SettingsDataPayload;

/** Views for the Home launcher, including a mod with no settings behind it. */
export const VIEWS: ViewsDataPayload = {
  views: [
    {
      id: 'acme.kit/panel',
      title: 'Kit Panel',
      description: 'A terminal',
      mod: 'acme.kit',
      kind: 'menu',
      hub: true,
      open: false,
      loadState: 'loaded',
      targetVersion: '',
    },
    {
      id: 'acme.kit/hud',
      title: 'Kit HUD',
      description: 'An overlay',
      mod: 'acme.kit',
      kind: 'hud',
      hub: true,
      open: true,
      loadState: 'loaded',
      targetVersion: '',
    },
    {
      id: 'acme.viewonly/browser',
      title: 'Standalone Browser',
      description: 'No settings mod behind me',
      mod: 'acme.viewonly',
      kind: 'menu',
      hub: true,
      open: false,
      loadState: 'loaded',
      targetVersion: '',
    },
  ],
} as unknown as ViewsDataPayload;

/** A mod with more than four labelled groups, for the section-index test. */
export const MANY_GROUPS: SettingsDataPayload = {
  mods: [
    {
      id: 'acme.big',
      title: 'Big Mod',
      values: {},
      schema: {
        groups: [
          { label: 'Alpha', settings: [{ key: 'a', label: 'A', type: 'bool', default: false }] },
          { label: 'Bravo', settings: [{ key: 'b', label: 'B', type: 'bool', default: false }] },
          { label: 'Charlie', settings: [{ key: 'c', label: 'C', type: 'bool', default: false }] },
          { label: 'Delta', settings: [{ key: 'd', label: 'D', type: 'bool', default: false }] },
          { label: 'Echo', settings: [{ key: 'e', label: 'E', type: 'bool', default: false }] },
        ],
      },
    },
  ],
} as unknown as SettingsDataPayload;

/** Four labelled groups — one under the section-index threshold. */
export const FOUR_GROUPS: SettingsDataPayload = {
  mods: [
    {
      id: 'acme.four',
      title: 'Four Mod',
      values: {},
      schema: {
        groups: [
          { label: 'Alpha', settings: [{ key: 'a', label: 'A', type: 'bool', default: false }] },
          { label: 'Bravo', settings: [{ key: 'b', label: 'B', type: 'bool', default: false }] },
          { label: 'Charlie', settings: [{ key: 'c', label: 'C', type: 'bool', default: false }] },
          { label: 'Delta', settings: [{ key: 'd', label: 'D', type: 'bool', default: false }] },
        ],
      },
    },
  ],
} as unknown as SettingsDataPayload;

/** A load-failure record for the rail alert. */
export const WITH_LOAD_ERRORS: SettingsDataPayload = {
  mods: [],
  loadErrors: [{ kind: 'values-parse', file: 'acme.broken.json', mod: 'acme.broken', message: 'bad json' }],
} as unknown as SettingsDataPayload;
