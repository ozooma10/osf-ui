const KINDS = new Set(['button', 'toggle', 'cycle', 'select']);
const ID_PATTERN = /^[a-z0-9][a-z0-9-]{0,63}$/;

function normalizeOptions(raw) {
  if (!Array.isArray(raw)) return [];
  const options = [];
  for (const entry of raw) {
    if (typeof entry === 'string') options.push({ value: entry, label: entry });
    else if (entry && typeof entry.value === 'string') {
      options.push({
        value: entry.value,
        label: typeof entry.label === 'string' ? entry.label : entry.value,
      });
    }
  }
  return options;
}

export function normalizeTools(raw) {
  const byId = new Map();
  const dropped = [];
  for (const entry of Array.isArray(raw) ? raw : []) {
    if (!entry || typeof entry !== 'object' || !ID_PATTERN.test(entry.id || '') ||
        !KINDS.has(entry.kind) || typeof entry.label !== 'string' || !entry.label) {
      dropped.push(entry && typeof entry === 'object' ? String(entry.id ?? '?') : String(entry));
      continue;
    }
    const options = normalizeOptions(entry.options);
    if ((entry.kind === 'cycle' || entry.kind === 'select') && options.length === 0) {
      dropped.push(entry.id);
      continue;
    }
    const tool = {
      id: entry.id,
      kind: entry.kind,
      label: entry.label,
      title: typeof entry.title === 'string' ? entry.title : '',
      active: entry.active === true,
      options,
    };
    if (entry.kind === 'toggle') tool.value = entry.value === true;
    else if (entry.kind === 'cycle' || entry.kind === 'select') {
      tool.value = options.some((option) => option.value === entry.value)
        ? entry.value
        : options[0].value;
    }
    byId.delete(tool.id);
    byId.set(tool.id, tool);
  }
  return { tools: [...byId.values()], dropped };
}

export function applyPatch(tools, id, patch) {
  if (!patch || typeof patch !== 'object') return tools;
  return tools.map((tool) => {
    if (tool.id !== id) return tool;
    const next = { ...tool };
    if (typeof patch.label === 'string' && patch.label) next.label = patch.label;
    if (typeof patch.title === 'string') next.title = patch.title;
    if (typeof patch.active === 'boolean') next.active = patch.active;
    if (patch.options !== undefined) {
      const options = normalizeOptions(patch.options);
      if (options.length || tool.kind === 'button' || tool.kind === 'toggle') next.options = options;
    }
    if (patch.value !== undefined) {
      if (tool.kind === 'toggle') next.value = patch.value === true;
      else if ((tool.kind === 'cycle' || tool.kind === 'select') &&
               next.options.some((option) => option.value === patch.value)) {
        next.value = patch.value;
      }
    }
    return next;
  });
}

export function nextCycleValue(tool) {
  const index = tool.options.findIndex((option) => option.value === tool.value);
  return tool.options[(index + 1) % tool.options.length]?.value ?? tool.value;
}
