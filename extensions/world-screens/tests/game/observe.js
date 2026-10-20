// Added only to a private test copy. Observe the real addon document without replacing its data.
// Launcher pages already load the helper as a classic script. Loading it again
// as a module would replace their state/event subscriptions and break the game.
if (typeof window.osfui?.send !== 'function') await import('/shared/osfui.js');
const documentId = crypto.randomUUID();
let frames = 0;
const input = {interaction: null, keys: [], gamepad: [], childKeys: []};
function retain(list, value) { list.push(value); if (list.length > 32) list.shift(); }
for (const type of ['keydown', 'keyup']) window.addEventListener(type, event =>
  retain(input.keys, {type, key:event.key, trusted:event.isTrusted, at:performance.now()}), true);
window.osfui.on('ui.interaction', value => { input.interaction = value; });
window.osfui.on('ui.gamepad', value => retain(input.gamepad, {value, at:performance.now()}));
const observed = new WeakSet();
function readGame() {
  const child = document.querySelector('iframe')?.contentWindow;
  if (!child) return null;
  try {
    if (!observed.has(child.document)) {
      observed.add(child.document);
      for (const type of ['keydown', 'keyup']) child.addEventListener(type, event =>
        retain(input.childKeys, {type, key:event.key, trusted:event.isTrusted, at:performance.now()}), true);
    }
    const canvas = child.document.querySelector('canvas');
    if (!canvas || !child.location.pathname.includes('orbital-paddle')) return null;
    const pixels = canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data;
    let sumY=0, count=0, ballX=0, ballY=0, ballCount=0;
    for (let y=0;y<canvas.height;y++) {
      const i=(y*canvas.width+30)*4;
      if (pixels[i]===197 && pixels[i+1]===140 && pixels[i+2]===255) {sumY+=y;count++;}
      if(y<45 || y>520 || y%2) continue;
      for(let x=45;x<855;x+=2){
        const p=(y*canvas.width+x)*4;
        if(pixels[p]>235 && pixels[p+1]>235 && pixels[p+2]>235){ballX+=x;ballY+=y;ballCount++;}
      }
    }
    return {paddleY:count?sumY/count:null,ballX:ballCount?ballX/ballCount:null,ballY:ballCount?ballY/ballCount:null};
  } catch(error) { return {error:String(error)}; }
}
function frame() { ++frames; requestAnimationFrame(frame); }
requestAnimationFrame(frame);
setInterval(() => window.osfui.send('osfui-backend-test.report', {
  documentId, frames, title:document.title, text:document.body.innerText.slice(0,2000),
  company:document.querySelector('main')?.dataset.companyId ?? null,
  rows:[...document.querySelectorAll('.company')].map(row => row.innerText),
  phase:document.getElementById('phase')?.innerText,
  status:document.getElementById('status')?.innerText,
  iframe:document.querySelector('iframe')?.getAttribute('src'), input, game:readGame()
}), 500);
