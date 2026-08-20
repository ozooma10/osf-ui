const state = { clicks: 0 };

export default { state };

export function install(ctx) {
  const publishClicks = () => ctx.send({
    kind: 'state',
    mod: '__OSFUI_MOD_ID_SQ__',
    key: 'clicks',
    value: state.clicks,
  });

  const handleEndpoint = (kind, name, payload) => {
    if (name !== 'papyrus.call' || payload.script !== '__OSFUI_SCRIPT_NAME__') return false;

    if (payload.function === 'Refresh') {
      state.clicks = 0;
      publishClicks();
    } else if (payload.function === 'Bump') {
      state.clicks = Number(payload.args?.[0]) || 0;
      publishClicks();
      ctx.send({
        kind: 'event',
        name: '__OSFUI_MOD_ID_SQ__.notice',
        payload: { args: ['JavaScript called a GLOBAL Papyrus function'] },
      });
    }
    return true;
  };

  ctx.onEndpoint(handleEndpoint);
}
