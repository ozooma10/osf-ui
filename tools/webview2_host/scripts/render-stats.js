(() => {
						let enabled=false, host=null, raf=0, lastFrame=0;
						let windowStart=0, frames=0, gaps=[], longCount=0, longMs=0;
						let observer=null, viewId='';
						const isTop=window===window.top;
						const safe=(n,d=1)=>Number.isFinite(Number(n))?Number(n).toFixed(d):'--';
						const ensurePanel=()=>{
							if(host&&host.isConnected)return host.shadowRoot;
							host=document.createElement('div');
							host.id='__osfui-render-stats';
							host.style.cssText='all:initial;position:fixed;z-index:2147483647;top:12px;right:12px;pointer-events:none;contain:layout style paint;';
							const root=host.attachShadow({mode:'open'});
							root.innerHTML=`<style>
								.panel{min-width:340px;padding:10px 12px;color:#e7f6ff;background:rgba(4,8,12,.92);border:1px solid rgba(118,199,239,.72);box-shadow:0 4px 20px rgba(0,0,0,.45);font:11px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace;letter-spacing:.03em}
								.head{color:#7ed2ff;font-weight:700;border-bottom:1px solid rgba(126,210,255,.3);padding-bottom:5px;margin-bottom:5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:360px}
								.row{display:grid;grid-template-columns:88px 1fr;gap:8px}.key{color:#8da0ad}.primary{color:#9ee6a8;font-weight:700}.bad{color:#ffb36b}
							</style><div class="panel"><div class="head"></div><div class="rows"></div></div>`;
							(document.documentElement||document).appendChild(host);
							return root;
						};
						const percentile=(values,p)=>{
							if(!values.length)return 0;
							const sorted=values.slice().sort((a,b)=>a-b);
							return sorted[Math.min(sorted.length-1,Math.floor(sorted.length*p))];
						};
						const frame=(now)=>{
							if(!enabled)return;
							if(!windowStart)windowStart=now;
							if(lastFrame)gaps.push(now-lastFrame);
							lastFrame=now;frames++;
							raf=requestAnimationFrame(frame);
						};
						const localSample=()=>{
							const now=performance.now(),elapsed=Math.max(1,now-windowStart);
							const result={
								pageFps:frames*1000/elapsed,p95:percentile(gaps,.95),
								max:gaps.length?Math.max(...gaps):0,longCount,longMs,
								nodes:document.getElementsByTagName('*').length,
								heapMb:performance.memory&&performance.memory.usedJSHeapSize?
									performance.memory.usedJSHeapSize/1048576:0,
								frame:location.pathname||document.title||'document'
							};
							windowStart=now;frames=0;gaps=[];longCount=0;longMs=0;
							return result;
						};
						const setChildStats=(next)=>{
							for(const iframe of document.querySelectorAll('iframe')){
								try{iframe.contentWindow?.__osfuiSetRenderStats?.(next,viewId);}catch(_){}
							}
						};
						const takeSample=()=>{
							let selected=localSample(),selectedArea=0;
							for(const iframe of document.querySelectorAll('iframe')){
								try{
									const rect=iframe.getBoundingClientRect();
									const area=Math.max(0,rect.width)*Math.max(0,rect.height);
									if(area<1)continue;
									const child=iframe.contentWindow;
									child?.__osfuiSetRenderStats?.(true,viewId);
									const sample=child?.__osfuiTakeRenderStatsSample?.();
									if(sample&&area>selectedArea){selected=sample;selectedArea=area;}
								}catch(_){}
							}
							return selected;
						};
						window.__osfuiTakeRenderStatsSample=takeSample;
						window.__osfuiRenderStatsNative=(native)=>{
							if(!enabled||!isTop)return;
							const page=takeSample();
							const {pageFps,p95,max,longCount,longMs}=page;
							const fresh=Number.isFinite(Number(native.freshFps))?native.freshFps:native.transferFps;
							const memory=page.heapMb?`${safe(page.heapMb,0)} MB`:'n/a';
							const root=ensurePanel(),bad=p95>25||max>50;
							root.querySelector('.head').textContent=`RENDER STATS · ${viewId}`;
							root.querySelector('.rows').innerHTML=
								`<div class="row primary"><span class="key">FRESH VIEW</span><span>${safe(fresh)} fps · new textures drawn in game</span></div>`+
								`<div class="row"><span class="key">OVERLAY PASSES</span><span>${safe(native.drawFps)} /s · ${native.reusedDraws||0} reused</span></div>`+
								`<div class="row"><span class="key">CAPTURE</span><span>${safe(native.captureFps)} fps WGC · publish ${safe(native.transferFps)}</span></div>`+
								`<div class="row"><span class="key">TRANSPORT</span><span>submit ${safe(native.submitFps)} fps · copy ${safe(native.copyMs,2)} ms</span></div>`+
								`<div class="row"><span class="key">LATENCY</span><span>capture→draw ${safe(native.sourceToDrawMs,2)} ms · CPU ${safe(native.recordCpuMs,3)} ms</span></div>`+
								`<div class="row ${bad?'bad':''}"><span class="key">PAGE RAF</span><span>${safe(pageFps)} fps · p95 ${safe(p95)} ms · max ${safe(max)} ms</span></div>`+
								`<div class="row ${longCount?'bad':''}"><span class="key">LONG TASKS</span><span>${longCount} · ${safe(longMs,0)} ms total</span></div>`+
								`<div class="row"><span class="key">PAGE</span><span>${page.nodes} nodes · heap ${memory}</span></div>`+
								`<div class="row ${native.backpressure?'bad':'}"><span class="key">BACKPRESSURE</span><span>ring ${native.backpressure||0}</span></div>`;
							try{
								chrome.webview.postMessage('__osfuiRenderStatsPage:'+JSON.stringify({
									pageFps,p95,max,longCount,longMs,
									nodes:page.nodes,heapMb:page.heapMb,frame:page.frame
								}));
							}catch(_){}
						};
						window.__osfuiSetRenderStats=(next,id)=>{
							viewId=String(id||'');
							setChildStats(!!next);
							if(!!next===enabled)return;
							enabled=!!next;
							if(!enabled){
								if(raf)cancelAnimationFrame(raf);
								raf=0;lastFrame=0;windowStart=0;frames=0;gaps=[];
								if(observer)observer.disconnect();
								observer=null;if(host)host.remove();host=null;return;
							}
							if(isTop)ensurePanel();
							windowStart=performance.now();raf=requestAnimationFrame(frame);
							try{
								observer=new PerformanceObserver((list)=>{
									for(const entry of list.getEntries()){longCount++;longMs+=entry.duration;}
								});
								observer.observe({type:'longtask',buffered:true});
							}catch(_){}
						};
					})();
