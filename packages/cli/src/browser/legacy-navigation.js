// Frozen CLI preview URL handling for projects authored against the 1.x helper.

export function appendLegacyApi(urlLike, base = 'http://osfui.local') {
  const url = new URL(urlLike, base);
  url.searchParams.set('osfui-api', '1');
  return url.pathname + url.search + url.hash;
}

export function mergeHarnessViewUrl(targetViewUrl, shellHref) {
  const shell = new URL(shellHref);
  const forwarded = new URLSearchParams(shell.search);
  for (const own of ['view', 'res', 'checker', 'osfui-api']) forwarded.delete(own);
  const target = new URL(targetViewUrl, shell.origin);
  for (const [name, value] of forwarded) target.searchParams.set(name, value);
  return target.pathname + target.search + (shell.hash || target.hash);
}
