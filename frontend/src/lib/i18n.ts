
import type { Bridge } from '@lib/bridge';

/** Interpolation variables. Mirrors the frozen helper's accepted value types. */
export type TranslationVars = Record<string, string | number>;

export type TranslatorHost = Pick<Bridge, 't'>;

export interface Translator {
  (address: string, english: string, vars?: TranslationVars): string;

  plural(
    base: string,
    count: number,
    one: string,
    other: string,
    vars?: TranslationVars,
  ): string;
}

export function isAbsoluteAddress(address: string): boolean {
  return address.includes('.');
}

export function makeTranslator(bridge: TranslatorHost, prefix: string): Translator {
  // Normalise once: a missing trailing dot would corrupt every address.
  const ns = prefix && !prefix.endsWith('.') ? `${prefix}.` : prefix;

  const tr = ((address: string, english: string, vars?: TranslationVars): string =>
    bridge.t(isAbsoluteAddress(address) ? address : ns + address, english, vars)) as Translator;

  tr.plural = (base, count, one, other, vars) =>
    tr(base + (count === 1 ? 'One' : 'Other'), count === 1 ? one : other, {
      count,
      ...vars,
    });

  return tr;
}
