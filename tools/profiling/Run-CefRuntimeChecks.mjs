// Real production-helper checks in private profiles; never launches or closes Starfield.
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn, execFileSync} from 'node:child_process';
import {createHash, randomUUID} from 'node:crypto';
import {fileURLToPath} from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const options = Object.fromEntries(Array.from({length:(process.argv.length-2)/2}, (_,i) => process.argv.slice(i*2+2,i*2+4)));
const output = path.resolve(options['--out'] ?? path.join(repo,'build/cef-runtime',new Date().toISOString().replaceAll(':','-')));
const adapters = (options['--adapter-indices'] ?? '0').split(',').map(Number);
const checks = (options['--checks'] ?? 'gpu,storage,media,isolation').split(',');
if(!adapters.length||adapters.some(index=>!Number.isInteger(index)||index<0))throw new Error('Adapter indices must be nonnegative integers.');
if(!checks.length||checks.some(check=>!['gpu','storage','media','isolation'].includes(check)))throw new Error('Unknown runtime check.');
if (fs.existsSync(output)) throw new Error('Use a new output directory to preserve prior evidence.');
const powershell = command => execFileSync('pwsh',['-NoProfile','-Command',command],{encoding:'utf8',windowsHide:true}).trim();
const gameRunning = () => powershell('@(Get-Process Starfield -ErrorAction SilentlyContinue).Count') !== '0';
if (gameRunning()) throw new Error('Close Starfield normally before these isolated checks.');
const consumer = path.join(repo,'build/backend-bench/Release/osfui_backend_bench.exe');
const host = path.join(repo,'build/cef-host/runtime/osfui_cef_host.exe');
fs.mkdirSync(output,{recursive:true});
const views = path.join(output,'views');
const fixture = path.join(repo,'tools/profiling/backend-bench/runtime.html');
for (const view of ['main','second']) {
  const folder = path.join(views,'bench',view); fs.mkdirSync(folder,{recursive:true});
  for (const file of ['index.html','slow.html','missed.html']) fs.copyFileSync(fixture,path.join(folder,file));
}
// Uncompressed PCM is supported by both shipped browsers and has deterministic sample bytes.
const rate=44100, frames=rate*20, wav=Buffer.alloc(44+frames*2);
wav.write('RIFF');wav.writeUInt32LE(wav.length-8,4);wav.write('WAVEfmt ',8);
wav.writeUInt32LE(16,16);wav.writeUInt16LE(1,20);wav.writeUInt16LE(1,22);
wav.writeUInt32LE(rate,24);wav.writeUInt32LE(rate*2,28);wav.writeUInt16LE(2,32);wav.writeUInt16LE(16,34);
wav.write('data',36);wav.writeUInt32LE(frames*2,40);
for(let i=0;i<frames;i++)wav.writeInt16LE(Math.round(Math.sin(i*2*Math.PI*220/rate)*1000),44+i*2);
fs.writeFileSync(path.join(views,'bench/main/tone.wav'),wav);
const files=[consumer,host,path.join(path.dirname(host),'osfui_cef_host.dll'),path.join(path.dirname(host),'libcef.dll'),fixture,
  fileURLToPath(import.meta.url),path.join(repo,'tools/profiling/backend-bench/main.cpp'),path.join(repo,'tools/profiling/backend-bench/RuntimeChecks.h')];
const metadata={started:new Date().toISOString(),commit:execFileSync('git',['rev-parse','HEAD'],{cwd:repo,encoding:'utf8'}).trim(),
  sourceDirty:!!execFileSync('git',['status','--porcelain','--ignore-submodules'],{cwd:repo,encoding:'utf8'}).trim(),adapters,checks,
  gpu:JSON.parse(powershell('ConvertTo-Json -InputObject @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)')),
  hashes:files.map(file=>({file,sha256:createHash('sha256').update(fs.readFileSync(file)).digest('hex')}))};
fs.writeFileSync(path.join(output,'metadata.json'),JSON.stringify(metadata,null,2));
const sleep = ms => new Promise(resolve=>setTimeout(resolve,ms));
async function freePort() {
  const server=net.createServer();await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const port=server.address().port;await new Promise(resolve=>server.close(resolve));return port;
}
async function crashSecond(port, run) {
  const targets=await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const target=targets.find(item=>item.type==='page'&&item.url==='https://osfui.example/bench/second/index.html');
  if(!target)throw new Error('Owned second view was not found on the private debugging port.');
  fs.writeFileSync(path.join(run,'crash-target.json'),JSON.stringify(target,null,2));
  const socket=new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{socket.close();reject(new Error('CDP crash command timeout'));},5000);
    socket.addEventListener('error',()=>{clearTimeout(timer);reject(new Error('CDP connection error'));},{once:true});
    socket.addEventListener('open',()=>{
      socket.send(JSON.stringify({id:1,method:'Page.crash'}));
      // A crashing target may never acknowledge the command; the consumer requires the host's termination event.
      setTimeout(()=>{clearTimeout(timer);socket.close();resolve();},200);
    },{once:true});
  });
}
const results=[];
async function run(check, adapter, profile, token='') {
  if(gameRunning())throw new Error('Starfield started; stop testing without touching its process.');
  const name=`${check}-adapter${adapter}`, folder=path.join(output,name);fs.mkdirSync(folder);
  const port=check==='isolation'?await freePort():0;
  const args=['--backend','cef','--host',host,'--out',folder,'--views',views,'--profile',profile,
    '--width','1280','--height','720','--adapter-index',String(adapter),'--runtime-check',check,
    '--storage-token',token,'--debug-port',String(port)];
  const stdout=fs.openSync(path.join(folder,'stdout.log'),'w'),stderr=fs.openSync(path.join(folder,'stderr.log'),'w');
  const child=spawn(consumer,args,{cwd:repo,windowsHide:true,stdio:['ignore',stdout,stderr]});
  let ended=false,code,crashed=false,error;child.on('exit',value=>{ended=true;code=value;});child.on('error',e=>{ended=true;error=String(e);});
  const started=Date.now();let nextContention=started+1000;
  try {
    while(!ended){
      if(Date.now()-started>120000)throw new Error('Owned consumer timeout.');
      if(Date.now()>nextContention){nextContention=Date.now()+1000;if(gameRunning())throw new Error('Starfield contention; result invalid.');}
      if(port&&!crashed&&fs.existsSync(path.join(folder,'crash-ready.json'))){crashed=true;await crashSecond(port,folder);}
      await sleep(100);
    }
  }catch(e){error=String(e);child.kill();await new Promise(resolve=>ended?resolve():child.once('exit',resolve));}
  finally{fs.closeSync(stdout);fs.closeSync(stderr);}
  const resultFile=path.join(folder,'result.json');
  const result=fs.existsSync(resultFile)?JSON.parse(fs.readFileSync(resultFile,'utf8')):{};
  const passed=!error&&code===0&&result.status==='passed'&&result.shutdown_clean&&result.shutdown_ms<6000;
  if(!error&&result.shutdown_ms>=6000)error='Shutdown exceeded the production client six-second deadline.';
  results.push({name,passed,code,error,result});
  fs.writeFileSync(path.join(output,'summary.json'),JSON.stringify({passed:results.every(item=>item.passed),results},null,2));
  console.log(`${passed?'PASS':'FAIL'} ${name}: ${error??result.error??result.adapter??''}`);
  return passed;
}
for(const adapter of adapters){
  if(checks.includes('gpu'))await run('gpu',adapter,path.join(output,'profiles',`gpu${adapter}`));
}
const adapter=adapters[0];
if(checks.includes('storage')){
  const profile=path.join(output,'profiles','storage'),token=randomUUID();
  if(await run('storage-write',adapter,profile,token))await run('storage-read',adapter,profile,token);
}
for(const check of ['media','isolation'])if(checks.includes(check))await run(check,adapter,path.join(output,'profiles',check));
process.exitCode=results.every(item=>item.passed)?0:1;
