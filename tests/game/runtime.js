'use strict';
const second=location.pathname.includes('/runtime-second/');
if(second)document.body.classList.add('second');
const status=document.getElementById('status'),video=document.querySelector('video');
const evidence={document:crypto.randomUUID(),frame:0,complete:true,errors:[],results:{},path:location.pathname};
const bits=document.getElementById('bits');
for(let i=0;i<12;i++)bits.append(document.createElement('i'));
function report(){osfui.send('report',evidence);}
function require(ok,message){if(!ok)throw Error(message);}
function event(target,name){return new Promise((resolve,reject)=>{
  const cleanup=()=>{clearTimeout(timer);target.removeEventListener(name,success);target.removeEventListener('error',failure);};
  const success=()=>{cleanup();resolve();},failure=()=>{cleanup();reject(Error(`media ${name}: ${target.error?.message}`));};
  const timer=setTimeout(()=>{cleanup();reject(Error(`timeout waiting for ${name}`));},15000);
  target.addEventListener(name,success,{once:true});target.addEventListener('error',failure,{once:true});
});}
function tick(){evidence.frame++;[...bits.children].forEach((bit,i)=>bit.style.background=evidence.frame&(1<<i)?'white':'black');requestAnimationFrame(tick);}
async function storage(writing,token){
  const db=await new Promise((resolve,reject)=>{const r=indexedDB.open('osfui-game-runtime',1);
    r.onupgradeneeded=()=>r.result.createObjectStore('values');r.onsuccess=()=>resolve(r.result);r.onerror=()=>reject(r.error);});
  const expected=token+':'+ 'stored-at-game-exit'.repeat(32768);let persisted;
  await new Promise((resolve,reject)=>{const tx=db.transaction('values',writing?'readwrite':'readonly'),s=tx.objectStore('values');
    if(writing)s.put(expected,'value');else{const r=s.get('value');r.onsuccess=()=>persisted=r.result;}
    tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error);tx.onabort=()=>reject(tx.error);});db.close();
  if(writing){localStorage.setItem('runtime',expected);document.cookie=`runtimeToken=${token}; Max-Age=86400; Path=/; Secure; SameSite=Strict`;}
  else{require(localStorage.getItem('runtime')===expected,'localStorage lost at game exit');require(persisted===expected,'IndexedDB lost at game exit');}
  require(document.cookie.split('; ').includes(`runtimeToken=${token}`),'persistent cookie mismatch');
  return {bytes:expected.length,localStorage:true,indexedDB:true,cookie:true,token};
}
async function media(){
  const head=await fetch('seek.webm',{method:'HEAD'}),size=Number(head.headers.get('content-length'));
  require(head.status===200&&size>1000000,'video HEAD length');
  const ranges=[];
  for(const header of ['bytes=120-239','bytes=-128','bytes=1000000-1000127']){
    const r=await fetch('seek.webm',{headers:{Range:header}}),bytes=(await r.arrayBuffer()).byteLength;
    require(r.status===206&&bytes===(header==='bytes=120-239'?120:128),'video range response');
    ranges.push({header,status:r.status,bytes,contentRange:r.headers.get('content-range')});
  }
  const metadata=event(video,'loadedmetadata');video.src='seek.webm';await metadata;
  require(Math.abs(video.duration-20)<.1,'video duration');
  const canvas=document.createElement('canvas');canvas.width=1280;canvas.height=720;
  const context=canvas.getContext('2d',{willReadFrequently:true}),seeks=[];
  for(const [target,channel] of [[14.25,2],[2.5,0],[9.25,1],[18.25,2]]){
    const complete=event(video,'seeked');video.currentTime=target;await complete;
    require(Math.abs(video.currentTime-target)<.04&&video.readyState>=2,'video seek position');
    context.drawImage(video,0,0);const pixel=[...context.getImageData(32,32,1,1).data];
    require(pixel[channel]>180&&pixel.filter((_,i)=>i<3&&i!==channel).every(v=>v<70),`wrong decoded frame at ${target}: ${pixel}`);
    seeks.push({target,actual:video.currentTime,readyState:video.readyState,pixel});
  }
  const framesBefore=video.getVideoPlaybackQuality().totalVideoFrames,start=video.currentTime;
  await video.play();await new Promise(r=>setTimeout(r,500));video.pause();
  const framesAfter=video.getVideoPlaybackQuality().totalVideoFrames;
  require(video.currentTime>start+.2&&framesAfter>framesBefore,'video playback did not resume after seeking');
  return {size,duration:video.duration,width:video.videoWidth,height:video.videoHeight,ranges,seeks,framesBefore,framesAfter};
}
osfui.on('runtime',async command=>{
  const {test,id,token}=command;
  try{
    let result={};
    if(test==='gpu'){const gl=document.createElement('canvas').getContext('webgl');require(gl,'WebGL unavailable');
      const ext=gl.getExtension('WEBGL_debug_renderer_info');require(ext,'ANGLE renderer unavailable');
      result={renderer:gl.getParameter(ext.UNMASKED_RENDERER_WEBGL)};}
    else if(test==='media')result=await media();
    else if(test==='storage-write'||test==='storage-read')result=await storage(test==='storage-write',token);
    else if(test==='navigate'){location.href=command.entry;return;}
    else throw Error(`unknown runtime test ${test}`);
    evidence.results[id]={test,passed:true,...result};status.textContent=`${test} passed`;
  }catch(error){evidence.results[id]={test,passed:false,error:String(error)};status.textContent=String(error);}
  report();
});
// Stall only this renderer's document parser before OnLoadEnd; keep another HUD live.
if(location.pathname.endsWith('/slow.html')){
  evidence.phase='slow-start';report();const end=performance.now()+10000;while(performance.now()<end){}
}
if(location.pathname.endsWith('/missed.html'))window.requestAnimationFrame=()=>0;
status.textContent=second?'Independent sibling HUD':'Healthy animated HUD';
requestAnimationFrame(tick);report();setInterval(report,100);
