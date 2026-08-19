
import type { GameplayMode, HotkeyContext, Setting, SettingsSchema } from '@sdk';

export const HOTKEY_CONTEXT_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

/** Reserved id of the implicit default context. */
export const GAMEPLAY_ID = 'gameplay';

/** Always fully populated — no optional fields to defend. */
export interface ResolvedHotkeyContext {
  id: string;
  label: string;
  blocksGameplay: boolean;
  gameplayModes?: GameplayMode[];
}

export function gameplayContext(label = 'Gameplay'): ResolvedHotkeyContext {
  return { id: GAMEPLAY_ID, label, blocksGameplay: false };
}

const GAMEPLAY_MODES = new Set<GameplayMode>(['onFoot', 'ship', 'vehicle', 'zeroG']);

function validModes(value: unknown): GameplayMode[] | null {
  if (!Array.isArray(value) || value.length === 0) return null;
  const out: GameplayMode[] = [];
  for (const mode of value) {
    if (typeof mode !== 'string' || !GAMEPLAY_MODES.has(mode as GameplayMode)) return null;
    if (!out.includes(mode as GameplayMode)) out.push(mode as GameplayMode);
  }
  return out.length ? out : null;
}

export function dedupeHotkeyContexts(contexts: unknown): ResolvedHotkeyContext[] {
  if (!Array.isArray(contexts)) return [];
  const seen = new Set<string>();
  const out: ResolvedHotkeyContext[] = [];
  for (const raw of contexts) {
    if (!raw || typeof raw !== 'object') continue;
    const c = raw as Partial<HotkeyContext>;
    const id = typeof c.id === 'string' ? c.id : '';
    if (id === GAMEPLAY_ID || !HOTKEY_CONTEXT_ID_RE.test(id) || seen.has(id)) continue;
    seen.add(id);
    const resolved: ResolvedHotkeyContext = {
      id,
      // An empty label falls back to the id, so a badge is never blank.
      label: typeof c.label === 'string' && c.label ? c.label : id,
      // Strict `=== true`: any other truthy value is not an assertion.
      blocksGameplay: c.blocksGameplay === true,
    };
    const modes = validModes(c.gameplayModes);
    if (modes) resolved.gameplayModes = modes;
    out.push(resolved);
  }
  return out;
}

export function resolveHotkeyContext(
  schema: SettingsSchema | undefined,
  setting: Pick<Setting, 'inputContext'> | undefined,
  gameplayLabel = 'Gameplay',
): ResolvedHotkeyContext {
  const fallback = gameplayContext(gameplayLabel);
  const ref = setting && typeof setting.inputContext === 'string' ? setting.inputContext : '';
  // Cases 1-3.
  if (!ref || ref === GAMEPLAY_ID || !HOTKEY_CONTEXT_ID_RE.test(ref)) return fallback;
  const declared = dedupeHotkeyContexts(schema && schema.inputContexts);
  // Case 4 when nothing matches.
  return declared.find((c) => c.id === ref) || fallback;
}
