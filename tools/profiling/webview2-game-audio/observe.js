// Private trusted-chrome observer. Remote content receives no bridge or script.
const documentId = crypto.randomUUID();
let browserState = null, interaction = null, result = null, frames = 0;
osfui.on("browser.state", value => { browserState = value; });
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
    text:document.body.innerText.slice(0, 1800), at:performance.now()
  });
}, 500);
