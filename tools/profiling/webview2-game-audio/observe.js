// Private trusted-chrome observer. Remote content receives no bridge or script.
const documentId = crypto.randomUUID();
let browserState = null, interaction = null, result = null, frames = 0;
let stateEvents = 0, audioOnlyEvents = 0, presentationKey = null, domMutations = 0;
osfui.on("browser.state", value => {
  browserState = value;
  stateEvents++;
  const presentation = { ...value };
  delete presentation.audio;
  const key = JSON.stringify(presentation);
  if (key === presentationKey) audioOnlyEvents++;
  presentationKey = key;
});
// Count mutations without retaining nodes or records. Kept identical in both
// variants so diagnostic traffic and UI work can be distinguished from CPU noise.
new MutationObserver(records => { domMutations += records.length; })
  .observe(document.body, { attributes: true, childList: true, characterData: true, subtree: true });
osfui.on("ui.interaction", value => { interaction = value; });
osfui.on("osfui-browser-test.command", async value => {
  try {
    const reply = await osfui.request("browser." + value.action, value.payload || {});
    result = {id:value.id, action:value.action, ok:true, reply};
  } catch (error) {
    result = {id:value.id, action:value.action, ok:false, error:String(error)};
  }
});
function tick() { frames++; requestAnimationFrame(tick); }
requestAnimationFrame(tick);
setInterval(async () => {
  try { browserState = await osfui.request("browser.getState", {}); } catch {}
  osfui.send("osfui-backend-test.report", {
    documentId, frames, browserState, interaction, result, audioProbe: window.osfAudioProbeSnapshot?.(),
    text:document.body.innerText.slice(0, 1800), at:performance.now(),
    diagnostics: { stateEvents, audioOnlyEvents, domMutations,
      jsHeapUsed: performance.memory?.usedJSHeapSize ?? null,
      jsHeapTotal: performance.memory?.totalJSHeapSize ?? null }
  });
}, 500);
