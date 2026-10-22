"use strict";
const isHud = location.pathname.includes('/hud/');
if (isHud) document.body.classList.add('hud');
const evidence = {document: crypto.randomUUID(), complete:false, errors:[], clicks:0, wheel:0, text:'', frame:0, events:[], state:null, rects:{}};
const byId = id => document.getElementById(id);
function report() {
  evidence.performanceAvailable = typeof osfui.performance !== 'undefined';
  evidence.performancePanel = !!document.getElementById('osfui-performance');
  for (const id of ['text','button','drag','scroll','select']) {
    const r=byId(id).getBoundingClientRect();
    evidence.rects[id]={x:(r.x+r.width/2)/innerWidth,y:(r.y+r.height/2)/innerHeight};
  }
  osfui.send('report', evidence);
}
osfui.state.on('value', value => {evidence.state=value;report();});
osfui.on('probe', value => {evidence.events.push(value);report();});
osfui.on('reload', () => location.reload());
osfui.on('papyrus', async value => {
  try { evidence.papyrus=await osfui.request('papyrus', {args:[value.number]}); }
  catch(error){ evidence.errors.push('papyrus: '+error.message); }
  report();
});
byId('text').addEventListener('input', () => {evidence.text=byId('text').value;report();});
byId('select').addEventListener('change', () => {evidence.selection=byId('select').value;report();});
byId('button').addEventListener('click', () => {evidence.clicks++;report();});
byId('scroll').addEventListener('wheel', () => {evidence.wheel++;report();});
byId('drag').addEventListener('mousedown', event => {if(event.button===0)osfui.send('osfui.relativePointer',{active:true});});
byId('close').addEventListener('click', () => osfui.send('close'));
addEventListener('keydown', event => {
  if(event.key==='F11'){evidence.f11Keys=(evidence.f11Keys||0)+1;report();}
  if(event.key==='Escape'){event.preventDefault();osfui.send('close');}
});
function tick(){evidence.frame++;requestAnimationFrame(tick);} requestAnimationFrame(tick);
(async () => {
  try {
    const reply=await osfui.request('echo',{number:42,text:'native round trip'});
    if(reply.number!==42 || reply.text!=='native round trip') throw Error('Native echo mismatch');
    try { await osfui.request('reject',{}); throw Error('Rejection unexpectedly resolved'); }
    catch(error){ if(error.code!=='fixture-rejection') throw error; }
    try { await fetch('https://example.com/osfui-backend-test'); evidence.errors.push('External networking allowed'); }
    catch { evidence.networkBlocked=true; }
    evidence.complete=true;
  } catch(error){ evidence.errors.push(error.message); }
  byId('status').textContent=evidence.errors.length ? evidence.errors.join('; ') : 'Native bridge passed. Interactive test ready.';
  report();setInterval(report,250);
})();
