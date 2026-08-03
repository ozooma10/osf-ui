# Simplification notes

The 2026 cleanup audit has been implemented. The repository now keeps only a
short list of deliberate follow-ups that need a product or compatibility
decision rather than more mechanical cleanup:

- Decide whether the unused public shared-kit tokens and classes may be removed.
- Revisit the two intentionally different key-conflict models only with a shared
  product definition for their behavior.
- Keep the 1.x native compatibility-command registry until ABI major 2.
- Consider unifying the browser mock layers only if their ownership model changes;
  the current runtime mock and built-in-view mock serve different harnesses.
- Treat marketing SVG/PNG generation as a separate asset-pipeline project.

One in-game-only experiment remains intentionally untouched: the pause-menu list
debounce in `src/input/PauseMenuEntry.cpp`. Its original crash was fixed by moving
reconciliation to the game main thread, but removing the now-unproven debounce
still requires a fresh FSR3 Frame Generation acceptance run. The adjacent
`LivePauseMenu` liveness checks are independent and remain load-bearing.
