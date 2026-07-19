// fixtures/schemas.ts — the harness's built-in settings schemas. DEV ONLY.
//
// TWO copies of this data used to exist:
//
//  1. devtools/harness/mockbridge.js:717-725 `FALLBACK` — the mock's seed so a
//     file:// page still renders something before the real schemas resolve.
//  2. src/views/osfui/settings/main.legacy.js:1930-1948 `sampleMods()` — the
//     SHIPPED view's standalone-preview data, compiled into the production
//     bundle for every user who never opens a browser preview.
//
// (2) is the one that must never come back: fictional demo content has no place
// in a shipped view, and the "no bridge" path it fed is the harness's job now.
// Its schema was the richer of the two (it carries `hint` text), so THAT is
// what survives here, given the id/title the mock's copy supplied — the merge
// is a strict superset of both, and nothing observable is lost.

import type { SettingsSchema } from '@sdk';

/**
 * Seed schemas, used when no real schema source resolves (the mock also seeds
 * with these synchronously so an early `settings.get` is answerable before the
 * async sources land — mockbridge.js:862).
 */
export const FALLBACK_SCHEMAS: SettingsSchema[] = [
  {
    id: 'osfui',
    title: 'OSF UI',
    // main.legacy.js:1935 wording; the mock's copy said "Framework runtime +
    // overlay behavior." Either is arbitrary demo text — the shipped
    // data/OSFUI/settings/osfui.json replaces this whenever it resolves.
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
