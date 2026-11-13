'use strict';
const $ = s => document.querySelector(s);
const colors = ['#50b9dd', '#e0b851', '#9fa1e1', '#69bb8a'];
const token = $('meta[name="control-token"]').content;
let state, busy = false, oldEpoch = -1, lastSweep = -1, water = [], catalog = [];
let windowInFlight=false,windowPending=false;
let review = null, windowRequest = 0, lastCatalog = 0, lastReceived = 0;
for (let i = 0; i < 4; i++) {
  const card = document.createElement('div'); card.className = 'panel card'; card.style.setProperty('--ch', colors[i]);
  card.innerHTML = `<div class="cardTop"><select id="ch${i}" aria-label="枠${i+1}のチャンネル"></select><span class="freq" id="freq${i}">— MHz</span></div><div class="level"><span id="v${i}">—</span><span class="unit" id="unit${i}">receiver dB</span></div><div class="bar"><i id="bar${i}"></i></div><canvas class="spark" id="spark${i}" aria-label="枠${i+1}の受信履歴"></canvas><div class="detail" id="detail${i}"></div><div class="thresholds" id="thresholds${i}" hidden><label>進入<input id="enter${i}" type="number" min="1" max="254" aria-label="枠${i+1}の進入しきい値"></label><label>退出<input id="exit${i}" type="number" min="1" max="254" aria-label="枠${i+1}の退出しきい値"></label></div>`;
  $('#cards').append(card);
}
function alertText(text) { $('#alert').textContent = text; $('#alert').hidden = false; }
async function http(path, options = {}, timeoutMs=4000) {
  const controller = new AbortController(), timer = setTimeout(() => controller.abort(), timeoutMs);
  try { const r = await fetch(path, {...options, signal: controller.signal, cache:'no-store'}); const d = await r.json(); if (!r.ok) throw Error(d.error || `HTTP ${r.status}`); return d; }
  finally { clearTimeout(timer); }
}
async function control(command) {
  if (busy) return; busy = true;
  try { await http('/api/control', {method:'POST',headers:{'Content-Type':'application/json','X-Control-Token':token},body:JSON.stringify(command)}); $('#alert').hidden = true; }
  catch (e) { alertText(e.message); } finally { busy = false; }
}
function config() {
  return {...state.config, channels:colors.map((_, i) => $(`#ch${i}`).value),
    enter:colors.map((_, i) => Number($(`#enter${i}`).value)), exit:colors.map((_, i) => Number($(`#exit${i}`).value)), minimumMs:Number($('#minimum').value)};
}
for (let i=0;i<4;i++) $(`#ch${i}`).onchange = () => control({action:'configure',config:config()});
document.querySelectorAll('[data-mode]').forEach(b => b.onclick = () => { returnLive(); control({action:'mode',mode:b.dataset.mode}); });
document.querySelectorAll('[data-close]').forEach(b => b.onclick = () => $(`#${b.dataset.close}`).close());
$('#step').onclick = () => control({action:'configure',config:{...config(),step:state.config.step===4?2:4}});
$('#background').onclick = () => $('#bgDialog').showModal();
$('#bgStart').onclick = () => { $('#bgDialog').close(); control({action:'background',allOffConfirmed:true}); };
$('#adopt').onclick = () => $('#adoptDialog').showModal();
$('#adoptConfirm').onclick = () => { $('#adoptDialog').close(); control({action:'adopt_background'}); };
$('#arm').onclick = () => control({action:'arm'});
$('#disarm').onclick = async () => { await control({action:'disarm'}); setTimeout(loadCatalog,700); };
$('#saveThresholds').onclick = () => control({action:'configure',config:config()});
$('#refreshRecordings').onclick = loadCatalog;
$('#reviewOpen').onclick = async () => { await loadCatalog(); $('#recordings').focus(); };
$('#loadRecording').onclick = () => loadRecording($('#recordings').value);
$('#returnLive').onclick = returnLive;
$('#playReplay').onclick = () => { if(!review)return; if(review.end>=review.metadata.duration)review.end=0; review.playing=!review.playing; review.lastTick=performance.now(); loadWindow(); };
$('#replayPosition').oninput = () => { if(!review)return; review.playing=false; review.end=Number($('#replayPosition').value); loadWindow(); };
function returnLive() { review=null; windowRequest++; if(state)render(state); }

async function loadCatalog() {
  try {
    catalog=await http('/api/recordings'); lastCatalog=Date.now(); const chosen=$('#recordings').value;
    $('#recordings').replaceChildren(...catalog.map(m => { const o=document.createElement('option');o.value=m.id;const date=m.createdUnix?new Date(m.createdUnix*1000).toLocaleString('ja-JP',{timeZone:'Asia/Tokyo',hour12:false}):m.id;o.textContent=`${date} / ${m.recordingKind||'native'} / ${m.duration.toFixed(1)}s / ${m.status}`;return o; }));
    if(catalog.length)$('#recordings').value=catalog.some(m=>m.id===chosen)?chosen:catalog[0].id;
    $('#loadRecording').disabled=!catalog.length;
    $('#recordingInfo').textContent=catalog.length?`${catalog.length}件。選択して再生できます。記録中の履歴も読み取り専用で確認できます。`:'保存記録はまだありません。4CH監視または計測・記録タブから開始してください。';
  } catch(e) { alertText('記録一覧：'+e.message); }
}
async function loadRecording(id) {
  if(!id)return;
  try { const metadata=await http('/api/recordings/'+id);review={id,metadata,end:metadata.duration,playing:false,lastTick:performance.now(),model:null};await loadWindow(); }
  catch(e){alertText('記録読み込み：'+e.message);}
}
async function loadWindow() {
  if(!review)return;if(windowInFlight){windowPending=true;return;}if(review.playing&&review.metadata.schema==='c5-scanner-recording/1'&&performance.now()-(review.lastWindow||0)<900)return;review.lastWindow=performance.now();windowInFlight=true;const target=review, ticket=++windowRequest, end=target.end;
  try {
    const w=await http(`/api/recordings/${target.id}/window?end=${end}&seconds=30`,{},12000);
    if(review!==target||ticket!==windowRequest)return;
    const traces=colors.map(()=>[]), latest=[null,null,null,null], events=[];
    for(const r of w.records){const p=r.data;if(r.kind==='sample'&&r.slot>=0&&r.slot<4){const origin=w.metadata.startMonotonic;traces[r.slot].push([r.t,p.rssi,p.extremum,p.extremumFirstHost-origin,p.extremumLastHost-origin,p.extremumIsPeak,p.crossing,p.extremumValid&&!p.late&&!p.gapBefore,p.rtt*1000]);latest[r.slot]=p;}else if(r.kind==='pass')events.push(p);}
    for(let i=0;i<4;i++)if(latest[i]&&w.end-(latest[i].hostMid-w.metadata.startMonotonic)>1)latest[i]=null;
    target.metadata=w.metadata;target.model={traces,latest,events:events.reverse(),series:w.series,frames:w.frames,start:w.start,end:w.end,truncated:w.truncated};
    render(state);
  }catch(e){if(review===target){target.playing=false;alertText('再生：'+e.message);}}finally{windowInFlight=false;if(windowPending&&review){windowPending=false;loadWindow();}}
}
setInterval(()=>{if(!review?.playing)return;const now=performance.now(),dt=(now-review.lastTick)/1000;review.lastTick=now;review.end=Math.min(review.metadata.duration,review.end+dt);if(review.end>=review.metadata.duration)review.playing=false;loadWindow();},300);

function surface(id) {
  const el=$(id),r=el.getBoundingClientRect(),d=Math.min(devicePixelRatio||1,2);
  if(el.width!==Math.round(r.width*d)||el.height!==Math.round(r.height*d)){el.width=Math.round(r.width*d);el.height=Math.round(r.height*d);}
  const ctx=el.getContext('2d');ctx.setTransform(d,0,0,d,0,0);ctx.clearRect(0,0,r.width,r.height);
  return {ctx,w:r.width,h:r.height};
}
function axes(ctx,w,h,lo,hi,start,end) {
  ctx.font='11px ui-monospace,monospace';ctx.lineWidth=1;ctx.setLineDash([]);
  for(let i=0;i<=4;i++){const y=12+(h-35)*i/4;ctx.strokeStyle='#364249';ctx.beginPath();ctx.moveTo(34,y);ctx.lineTo(w-8,y);ctx.stroke();ctx.fillStyle='#9ba8b0';ctx.fillText((hi-(hi-lo)*i/4).toFixed(0),0,y+3);}
  for(let i=0;i<=3;i++){const x=34+(w-42)*i/3;ctx.fillStyle='#8a979f';ctx.fillText((start+(end-start)*i/3).toFixed(0)+'s',Math.min(x,w-31),h-3);}
}
function simpleLine(ctx,values,w,h,color,lo,hi) {
  ctx.strokeStyle=color;ctx.lineWidth=1.4;ctx.setLineDash([]);ctx.beginPath();let active=false;
  values.forEach((v,i)=>{if(v===null||!Number.isFinite(v)){active=false;return}const x=34+(w-42)*i/Math.max(1,values.length-1),y=12+(hi-v)/(hi-lo)*(h-35);if(active)ctx.lineTo(x,y);else ctx.moveTo(x,y);active=true;});ctx.stroke();
}
function drawNative(i,traces,start,end,cfg) {
  const {ctx,w,h}=surface(`#spark${i}`);axes(ctx,w,h,0,254,review?start:-30,review?end:0);
  const x=t=>34+(w-42)*(t-start)/Math.max(.001,end-start),y=v=>12+(254-v)/254*(h-35);
  ctx.save();ctx.beginPath();ctx.rect(34,5,w-42,h-32);ctx.clip();
  [cfg.enter[i],cfg.exit[i]].forEach((v,k)=>{ctx.strokeStyle=k?'#a59f89':'#d2ba84';ctx.setLineDash(k?[2,5]:[6,4]);ctx.beginPath();ctx.moveTo(34,y(v));ctx.lineTo(w-8,y(v));ctx.stroke();});
  ctx.setLineDash([]);ctx.strokeStyle='#737f87';ctx.lineWidth=1;ctx.beginPath();let previous=null;
  for(const p of traces){if(!p[7]){previous=null;continue}if(previous&&p[0]-previous[0]<.5)ctx.lineTo(x(p[0]),y(p[1]));else ctx.moveTo(x(p[0]),y(p[1]));previous=p;}ctx.stroke();
  ctx.strokeStyle=colors[i];ctx.fillStyle=colors[i];ctx.lineWidth=1.4;
  for(const p of traces){if(!p[7])continue;const a=Math.max(start,p[3]),b=Math.min(end,p[4]);if(b<start||a>end)continue;ctx.beginPath();ctx.moveTo(x(a),y(p[2]));ctx.lineTo(x(b),y(p[2]));ctx.stroke();if(p[5])ctx.fillRect(x(b)-1.5,y(p[2])-1.5,3,3);else ctx.strokeRect(x(b)-1.5,y(p[2])-1.5,3,3);if(p[6])ctx.fillRect(Math.max(34,x(p[0]) - 2),h-25,3,3);}
  if(typeof drawHumanMarkers==='function')drawHumanMarkers(ctx,i,x,h,start,end);
  ctx.restore();
}
function heat(v,delta){if(v===null||!Number.isFinite(v))return'#0b0d0f';const t=Math.max(0,Math.min(1,(v-(delta?0:-10))/(delta?35:55))),stops=[[17,23,32],[25,75,105],[50,138,160],[203,189,105],[243,213,176]],z=t*4,i=Math.min(3,Math.floor(z)),f=z-i;return`rgb(${stops[i].map((a,k)=>Math.round(a*(1-f)+stops[i+1][k]*f)).join(',')})`;}
function drawSpectrum(s) {
  const sw=s.sweep;if(!sw){surface('#spectrumPlot');surface('#waterfall');$('#sweepTime').textContent='走査待ち';return;}
  $('#sweepTime').textContent=`1走査 ${sw.duration.toFixed(2)}s / ${sw.values.length}点`;
  {const {ctx,w,h}=surface('#spectrumPlot');axes(ctx,w,h,-10,45,5645,5917);ctx.clearRect(0,h-18,w,18);ctx.fillStyle='#94a4ac';for(let f=5660;f<=5900;f+=40)ctx.fillText(String(f),34+(w-42)*(f-5645)/272-12,h-3);ctx.fillText('MHz',w-30,h-3);if(sw.background)simpleLine(ctx,sw.background,w,h,'#6e7a81',-10,45);simpleLine(ctx,sw.values,w,h,'#d4dce0',-10,45);sw.values.forEach((v,i)=>{if(v===null)return;ctx.fillStyle='#d4dce0';ctx.fillRect(33+(w-42)*i/(sw.values.length-1),11+(45-v)/55*(h-35),2,2);});s.config.channels.forEach((ch,i)=>{const x=34+(w-42)*(s.presets[ch]-5645)/272;ctx.strokeStyle=colors[i];ctx.beginPath();ctx.moveTo(x,6);ctx.lineTo(x,h-24);ctx.stroke();ctx.fillStyle=colors[i];ctx.fillText(ch,Math.min(x+3,w-22),12+(i%2)*12);});}
  {const {ctx,w,h}=surface('#waterfall');const rows=water.slice(-120).reverse(),dy=h/120;rows.forEach((r,row)=>{r.values.forEach((v,i)=>{ctx.fillStyle=heat(v===null?null:v-(r.background?r.background[i]:0),!!r.background);ctx.fillRect(36+i*(w-36)/r.values.length,row*dy,(w-36)/r.values.length+.7,dy+.7)});if(row%20===0){ctx.fillStyle='#9aa9b1';ctx.font='10px ui-monospace,monospace';ctx.fillText('-'+Math.round(rows[0].time-r.time)+'s',0,row*dy+11);}});}
}
function table(events) {
  $('#eventRows').replaceChildren(...(events.length?events.slice(0,40).map(e=>{const tr=document.createElement('tr');for(const text of [e.channel,e.peakSessionSeconds===undefined?'—':e.peakSessionSeconds.toFixed(3)+'s',e.intervalMs===null?'基点':e.intervalMs===undefined?'—':(e.intervalMs/1000).toFixed(3)+'s',e.peakRssi??'—',e.reason==='first_pass'?'基点通過':e.reason==='pass'?'通過':e.reason,e.rttMs===undefined?'—':e.rttMs.toFixed(1)+'ms',e.receivedUtc||'—']){const td=document.createElement('td');td.textContent=text;tr.append(td);}return tr;}):[(()=>{const tr=document.createElement('tr'),td=document.createElement('td');td.colSpan=7;td.className='empty';td.textContent='通過イベントはまだありません。';tr.append(td);return tr;})()]));
}
function render(s) {
  if(!s)return;state=s;const r=review?.model, replay=!!review,scannerReplay=replay&&review.metadata.schema==='c5-scanner-recording/1', timing=replay?!scannerReplay:s.mode==='timing',spectrum=!replay&&s.mode==='spectrum',live=s.connection==='live'&&s.fresh;
  const cfg=replay?review.metadata.context.config:s.config, native=replay?r?.latest:s.nativeStats;
  document.body.classList.toggle('replaying',replay);document.body.classList.toggle('spectrumMode',spectrum);
  $('#connection').textContent=live?'LIVE':s.connection==='live'?'WAIT DATA':s.connection.toUpperCase();$('#connection').className=live?'live':'';
  $('#identity').textContent=`${s.device||'USB —'} / ${s.serial}`;$('#message').textContent=s.message;
  $('#modeNote').textContent=replay?'REPLAY / 記録時の設定':s.mode==='timing'?'RH時刻付き極値 / 4ch計測':spectrum?'広域走査 / 通過計測は停止':'4ch連続監視 / 相対電力';
  $('#reviewBanner').hidden=!replay;$('#timingTools').hidden=replay||!['monitor','timing'].includes(s.mode);$('#eventsPanel').hidden=!timing;$('#replayTools').hidden=!replay;$('#spectrum').hidden=!spectrum;
  document.querySelectorAll('[data-mode]').forEach(b=>{b.classList.toggle('active',!replay&&b.dataset.mode===s.mode);b.disabled=!live||s.timingArmed||s.mode==='background';});
  const editing=document.activeElement?.matches('input,select');
  for(let i=0;i<4;i++){
    const sel=$(`#ch${i}`);if(!sel.options.length){for(const[ch,mhz]of Object.entries(s.presets)){const o=document.createElement('option');o.value=ch;o.textContent=ch+' / '+mhz;sel.append(o);}}
    if(!editing||replay)sel.value=cfg.channels[i];sel.disabled=replay||s.timingArmed||!live;
    let v=replay?(scannerReplay?(r?.series?.[i]?.at(-1)?.last?.[($('#scannerField').value==='raw'?'raw':'db')]??null):(native?.[i]?.rssi??null)):s.levels[i];const ch=cfg.channels[i];
    if(spectrum){const sw=s.sweep;if(sw){let nearest=0;sw.frequencies.forEach((f,j)=>{if(Math.abs(f-s.presets[ch])<Math.abs(sw.frequencies[nearest]-s.presets[ch]))nearest=j;});v=sw.values[nearest];$(`#detail${i}`).textContent=`直近走査点 ${sw.frequencies[nearest]} MHz / 連続監視値ではありません`;}else{v=null;$(`#detail${i}`).textContent='走査待ち';}}
    if(scannerReplay){const last=r?.series?.[i]?.at(-1)?.last;if(!last||last.raw<=-327.68||v<=-327.68||r.end-last.hostT>1)v=null;$(`#v${i}`).classList.toggle('healthBad',!!last&&(last.flags||last.clips||last.gain!==30));}else $(`#v${i}`).classList.remove('healthBad');
    $(`#freq${i}`).textContent=`${s.presets[ch]} MHz`;$(`#v${i}`).textContent=v===null?'—':(spectrum?'≈ ':'')+(timing?Math.round(v):v.toFixed(2));$(`#unit${i}`).textContent=timing?'RH値':'receiver dB';$(`#bar${i}`).style.width=`${v===null?0:Math.max(0,Math.min(100,timing?v/254*100:(v+10)/55*100))}%`;
    $(`#thresholds${i}`).hidden=!timing;for(const key of ['enter','exit']){if(!editing||replay)$(`#${key}${i}`).value=cfg[key][i];$(`#${key}${i}`).disabled=replay||s.timingArmed;}
    if(timing){const p=native?.[i];$(`#detail${i}`).textContent=p?`${p.crossing?'CROSSING通知':'CLEAR通知'} / peak ${p.peak} / nadir ${p.nodeNadir}\nRTT ${(p.rtt*1000).toFixed(1)}ms / C5時刻窓 ${p.clockWindowMs}ms`:'時刻付きデータ待ち';$(`#detail${i}`).style.whiteSpace='pre-line';drawNative(i,replay?(r?.traces[i]||[]):s.timingHistory[i],replay?(r?.start||0):s.nowMonotonic-30,replay?(r?.end||30):s.nowMonotonic,cfg);}
    else if(scannerReplay){const series=r?.series?.[i]||[];drawScanner(i,series,r?.start||0,r?.end||30);const last=series.at(-1);$(`#detail${i}`).textContent=last?`min/max集約 / 表示区間 ${r.frames}フレーム / 最終枠 flags ${last.last.flags}・clip ${last.last.clips}・gain ${last.last.gain}`:'この区間は未取得';}
    else if(!spectrum){$(`#detail${i}`).textContent='100ms電力平均 / 直近30秒';const {ctx,w,h}=surface(`#spark${i}`);axes(ctx,w,h,-10,45,-30,0);simpleLine(ctx,s.history[i],w,h,colors[i],-10,45);if(typeof drawHumanMarkers==='function')drawHumanMarkers(ctx,i,t=>34+(w-42)*(t-s.nowMonotonic+30)/30,h,s.nowMonotonic-30,s.nowMonotonic);}
  }
  $('#modeUnits').textContent=timing?'RH 1..254':'receiver dB';$('#minimumLabel').hidden=!timing;$('#saveThresholds').hidden=!timing;$('#scannerField').disabled=!scannerReplay;$('#recordingTypeTitle').textContent=timing?'NATIVE / RH API 36':'SCANNER / 全受信フレーム';$('#recordingDescription').textContent=timing?'RHスナップショットとC5通過通知':'CRC確認済みペイロードをBLOBで保存';$('#recordHelp').textContent=timing?'RH値1〜254。灰は250ms最大値、色は時刻付き極値。H:の教示マーカーは人のラベルで、C5判定とは別です。':'表示は100ms集計。記録は集計前の全受信フレームで、flags/clipのある値も残します。内部I/Qや未送信・CRC不正データは記録されません。';if(!editing)$('#minimum').value=s.config.minimumMs;$('#minimum').disabled=s.timingArmed;$('#arm').textContent=timing?'RH計測を記録開始':'全フレーム記録開始';$('#arm').disabled=!live||s.timingArmed;$('#disarm').disabled=!s.timingArmed;$('#saveThresholds').disabled=s.timingArmed;
  const rec=s.recording;$('#recordBadge').textContent=s.timingArmed?`REC ${rec?.id} ${rec?.schema?.includes('scanner')?'SCANNER':'NATIVE'} ${rec?.duration.toFixed(1)}s / ${rec?.rows||0}行 / HOST ${rec?.dropped||0}${rec?.schema?.includes('scanner')?' / WIRE '+rec.seqGaps+' / C5 '+rec.c5DropsIncrease:''}${rec?.purpose==='software_test'?' / TEST':''}`:'REC OFF';$('#recordBadge').className=s.timingArmed?'on':'';$('#sessionName').textContent=rec?`${rec.id} / ${rec.duration.toFixed(1)}s`:'記録なし';
  const diag=[['実機モード',s.mode+' / '+(s.mode==='timing'?'RH 1..254':'receiver dB')],['表示単位',timing?'RH 1..254':'receiver dB'],['データ年齢',s.ageSeconds===null?'—':s.ageSeconds.toFixed(2)+'s'],['scannerフレーム',s.records.toLocaleString()],['scanner不正形式',String(s.malformedFrames||0)],['scanner CRC / 欠落',`${s.crcErrors} / ${s.chipDrops}`],['native読出し / エラー',`${s.timingPolls} / ${s.timingErrors}`],['native遅延 / 間隔超過',`${s.timingLate} / ${s.timingGaps}`],['RF詳細フラグ',s.mode==='timing'?'RHでは未提供':String(s.rfErrors)+'件'],['再接続',String(s.reconnects)],['保存キュー / ホスト欠落',rec?`${rec.queued} / ${rec.dropped}`:'—'],['記録seq欠番 / C5 drops',rec?.schema?.includes('scanner')?`${rec.seqGaps} / ${rec.c5DropsIncrease}`:'—'],['記録サイズ / 平均R毎秒',rec?.schema?.includes('scanner')?`${(rec.storageBytes/1024**2).toFixed(1)} MiB / ${(rec.rFrames/Math.max(.001,rec.duration)).toFixed(0)}`:'—'],['空き / 記録下限',`${(s.diskFreeBytes/1024**3).toFixed(2)} GiB / ${(s.diskMinimumBytes/1024**2).toFixed(0)} MiB`]];
  $('#diagnostics').replaceChildren(...diag.flatMap(([k,v])=>{const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=k;dd.textContent=v;return[dt,dd]}));$('#unitBadge').textContent=s.firmware||'—';$('#storage').textContent=rec?.error?'保存エラー：'+rec.error:rec?.closing?'記録の保存を完了しています':rec?.reason?'停止理由: '+rec.reason:'scannerは約100ms、nativeは約500msごとに確定保存';
  $('#bgState').textContent=s.background.active?'BG ACTIVE':s.background.saved?'BG SAVED':'BG NONE';$('#backgroundInfo').textContent=s.background.recordedAt?`記録UTC ${s.background.recordedAt}${s.background.active?' / 採用中':' / 再採用が必要'}`:'全機OFFの背景はまだ保存されていません';$('#adopt').hidden=!s.background.saved||s.background.active;$('#bgProgress').hidden=s.mode!=='background';$('#bgProgress').value=s.backgroundProgress||0;$('#mqtt').textContent=s.mqtt;
  ['background','adopt','step'].forEach(id=>$('#'+id).disabled=replay||!live||s.timingArmed||s.mode==='background');$('#step').textContent=s.config.step===4?'高速 4MHz / 詳細へ':'詳細 2MHz / 高速へ';
  if(s.epoch!==oldEpoch){water=[];lastSweep=-1;oldEpoch=s.epoch;}if(s.sweep&&s.sweep.id!==lastSweep){water.push(s.sweep);if(water.length>120)water.shift();lastSweep=s.sweep.id;}
  $('#legend').replaceChildren(...s.config.channels.map((ch,i)=>{const e=document.createElement('span');e.style.setProperty('--ch',colors[i]);e.textContent=ch+' / '+s.presets[ch]+' MHz';return e}));
  if(spectrum)drawSpectrum(s);table(replay?(r?.events||[]):s.events);$('#eventsTitle').textContent=replay?'再生区間の通過イベント':'通過イベント';
  if(typeof renderAnnotations==='function')renderAnnotations(s);
  if(replay){$('#reviewName').textContent=review.id;$('#downloadRecording').href=`/api/recordings/${review.id}/export`;$('#replayPosition').max=review.metadata.duration;$('#replayPosition').value=review.end;$('#replayTime').textContent=`${review.end.toFixed(1)} / ${review.metadata.duration.toFixed(1)}s`;$('#playReplay').textContent=review.playing?'一時停止':'再生';$('#replayContext').textContent=`${review.metadata.status} / ${review.metadata.rows}行 / 保存欠落 ${review.metadata.dropped}${r?.truncated?' / 表示上限により一部省略':''} / ${review.metadata.context.firmware} / ${cfg.channels.join('・')} / ${scannerReplay?'receiver dB。min/max表示、旗付き値も保持。全原本はJSONLへ。':'RH値。時刻はホスト推定、元のC5時刻窓・ageも保存。'}`;}
}
async function refresh(){try{const s=await http('/api/state');lastReceived=Date.now();render(s);if(Date.now()-lastCatalog>5000)loadCatalog();}catch(e){$('#connection').textContent='OFFLINE';$('#connection').className='';if(!review)for(let i=0;i<4;i++)$(`#v${i}`).textContent='—';alertText('Pyramidとの通信：'+e.message);}finally{setTimeout(refresh,200)}}
window.addEventListener('resize',()=>state&&render(state));
setInterval(()=>{if(lastReceived&&Date.now()-lastReceived>1500){$('#connection').textContent='STALE';$('#connection').className='';document.querySelectorAll('[data-mode],#arm,#saveThresholds,#background,#adopt,#step,.card select,.thresholds input').forEach(el=>el.disabled=true);if(!review)for(let i=0;i<4;i++)$(`#v${i}`).textContent='—';}},300);
loadCatalog();refresh();

$('#scannerField').onchange=()=>state&&render(state);
function drawScanner(i,series,start,end){
  const {ctx,w,h}=surface(`#spark${i}`),field=$('#scannerField').value;
  axes(ctx,w,h,-10,45,start,end);const x=t=>34+(w-42)*(t-start)/Math.max(.001,end-start),y=v=>12+(45-v)/55*(h-35);
  ctx.save();ctx.beginPath();ctx.rect(34,5,w-42,h-32);ctx.clip();
  for(const p of series){const lo=p[field+'Min'],hi=p[field+'Max'];ctx.strokeStyle=p.invalid||p.gap?'#dfa158':colors[i];if(lo!==null&&hi!==null){ctx.beginPath();ctx.moveTo(x(p.t),y(lo));ctx.lineTo(x(p.t),y(hi));ctx.stroke();ctx.fillStyle=ctx.strokeStyle;ctx.fillRect(x(p.t)-1,y(hi)-1,2,2);}if(p.invalid||p.gap){ctx.fillStyle='#dfa158';ctx.fillRect(x(p.t)-1,h-24,2,4);}}
  if(typeof drawHumanMarkers==='function')drawHumanMarkers(ctx,i,x,h,start,end);
  ctx.restore();
}
