// fixtures/schemas.ts — the harness's built-in settings schemas. Dev only.
//
// This demo content lives here and only here. The shipped view must never carry
// fictional sample schemas; the "no bridge" preview path is the harness's job.

import type { SettingsSchema } from '@sdk';

/**
 * Seed schemas, used when no real schema source resolves. The mock seeds with
 * these synchronously so an early `settings.get` is answerable before the async
 * sources land.
 */
export const FALLBACK_SCHEMAS: SettingsSchema[] = [
  {
    id: 'osfui',
    title: 'OSF UI',
    // Arbitrary demo text — the shipped data/OSFUI/settings/osfui.json replaces
    // this whenever it resolves.
    description: 'Runtime and overlay behavior for the OSF UI framework itself.',
    groups: [
      {
        label: 'Input',
        settings: [
          {
            key: 'toggleKey',
            label: 'Open / close key',
            type: 'key',
            default: 'F10',
            hint: 'Rebind the overlay key.',
          },
        ],
      },
      {
        label: 'Overlay',
        settings: [
          {
            key: 'allowPanels',
            label: 'Allow mod settings panels',
            type: 'bool',
            default: true,
            hint: "Custom mod panels run in this view's context.",
            requires: 'reload',
          },
        ],
      },
    ],
  },
];
