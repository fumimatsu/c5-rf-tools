'use strict';
let annotationId=null, annotationMeta=null, humanRows=[], humanActive=[], annotationBusy=false;
let annotationFetch=0, annotationFetching=false, annotationContext='';
function humanName(value){return {pass:'通過',no_pass:'非通過',unsure:'不明'}[value]||value;}
function newHumanId(){return [...crypto.getRandomValues(new Uint32Array(4))].map(x=>x.toString(16).padStart(8,'0')).join('');}
function renderAnnotations(s){
  const id=review?.id||s.recording?.id||null;
  const meta=review?.metadata||null;
  const key=id+':'+!!review;
  if(annotationContext!==key){annotationContext=key;annotationId=id;annotationMeta=meta;humanRows=[];humanActive=[];annotationFetch=0;$('#labelStart').value='';$('#labelEnd').value='';$('#labelMessage').textContent='';}
  annotationMeta=meta||annotationMeta;
  const cfg=review?meta.context.config:s.config;
  const chosen=$('#labelChannel').value;
  if($('#labelChannel').dataset.profile!==cfg.channels.join(',')){
    $('#labelChannel').replaceChildren(...cfg.channels.map((ch,i)=>{const o=document.createElement('option');o.value=i;o.textContent=ch+' / '+s.presets[ch]+'MHz';return o;}));
    $('#labelChannel').dataset.profile=cfg.channels.join(',');if(chosen)$('#labelChannel').value=chosen;
  }
  const live=!review&&s.timingArmed&&id===s.session;
  $('#labelRecording').textContent=id?`${review?'REPLAY':'LIVE'} / ${id}${live?' / 人が見た時刻を教示':''}${(review?meta.context.purpose==='software_test':s.recording?.purpose==='software_test')?' / SOFTWARE TEST・評価対象外':''}`:'記録開始または再生で利用';
  $('#labelStart').disabled=live;$('#labelEnd').disabled=live;$('#labelSource').disabled=live;$('#labelCursor').disabled=!review?.model;
  if(review?.model&&$('#labelStart').value===''){$('#labelStart').value=review.model.end.toFixed(3);$('#labelEnd').value=review.model.end.toFixed(3);}
  document.querySelectorAll('[data-human]').forEach(b=>b.disabled=annotationBusy||!id||(!review&&!live));
  $('#labelRetract').disabled=annotationBusy||!id||!$('#labelTarget').value;
  if(id&&(review?meta.status!=='recording':!s.timingArmed)&&!s.recording?.closing){$('#trainingDownload').href=`/api/recordings/${id}/training`;}else $('#trainingDownload').removeAttribute('href');
  document.body.classList.toggle('blindLabel',$('#blindLabel').checked);
  if(id&&Date.now()-annotationFetch>2000&&!annotationFetching)fetchAnnotations(id);
}
async function fetchAnnotations(id){
  annotationFetching=true;annotationFetch=Date.now();
  try{
    const ledger=await http(`/api/recordings/${id}/labels`);
    if(id!==annotationId)return;
    humanRows=ledger.rows;humanActive=ledger.active;renderHumanTable();
    if(!annotationMeta){annotationMeta=await http('/api/recordings/'+id);}
  }catch(e){$('#labelMessage').textContent='ラベル読出し: '+e.message;}
  finally{annotationFetching=false;}
}
function renderHumanTable(){
  const slot=Number($('#labelChannel').value),selected=$('#labelTarget').value;
  const placeholder=document.createElement('option');placeholder.value='';placeholder.textContent='選択してください';
  $('#labelTarget').replaceChildren(placeholder,...humanActive.filter(r=>r.slot===slot).map(r=>{const o=document.createElement('option');o.value=r.labelId;o.textContent=`${r.channel} / ${r.tStart.toFixed(3)}s / ${humanName(r.label)}`;return o;}));
  if(humanActive.some(r=>r.slot===slot&&r.labelId===selected))$('#labelTarget').value=selected;
  $('#humanRows').replaceChildren(...humanActive.slice(-30).reverse().map(r=>{
    const tr=document.createElement('tr');
    for(const text of [r.channel,humanName(r.label),`${r.tStart.toFixed(3)}–${r.tEnd.toFixed(3)}`,`±${r.uncertaintySec}s`,r.viewedRfWhileLabeling?'あり':'なし（申告）',r.method,r.note]){const td=document.createElement('td');td.textContent=text;tr.append(td);}return tr;
  }));
  $('#labelRetract').disabled=!$('#labelTarget').value;
}
async function addHuman(value,retract=false){
  if(!annotationId||annotationBusy)return;
  const id=annotationId,live=!review&&state?.timingArmed;
  const operation=retract?'retract':$('#labelOperation').value;
  const source=live?'live_button':$('#labelSource').value;
  const end=$('#labelEnd').value;
  const body={labelId:newHumanId(),operation,slot:Number($('#labelChannel').value),label:value,
    timeSource:source,method:{live_button:'human_live',replay_marker:'human_replay',video_review:'human_video_review'}[source],
    viewedRfWhileLabeling:!$('#blindLabel').checked,uncertaintySec:Number($('#labelUncertainty').value),
    operator:$('#labelOperator').value,note:$('#labelNote').value};
  if(operation!=='add')body.supersedes=$('#labelTarget').value;
  if(!live){body.tStart=Number($('#labelStart').value);body.tEnd=end===''?body.tStart:Number(end);}
  annotationBusy=true;
  try{
    const saved=await http(`/api/recordings/${id}/labels`,{method:'POST',headers:{'Content-Type':'application/json','X-Control-Token':token},body:JSON.stringify(body)});
    $('#labelMessage').textContent=`保存: ${saved.channel} / ${humanName(saved.label)} / ${saved.tStart.toFixed(3)}s / ${operation}\nRF参照${saved.viewedRfWhileLabeling?'あり':'なし（申告）'}。確認方法と時刻の不確かさも保存しました。`;await fetchAnnotations(id);
  }catch(e){$('#labelMessage').textContent='ラベル保存失敗: '+e.message;}
  finally{annotationBusy=false;if(state)render(state);}
}
document.querySelectorAll('[data-human]').forEach(b=>b.onclick=()=>addHuman(b.dataset.human));
$('#labelRetract').onclick=()=>addHuman('unsure',true);
$('#labelCursor').onclick=()=>{if(review?.model){$('#labelStart').value=review.model.end.toFixed(3);$('#labelEnd').value=review.model.end.toFixed(3);}};
$('#labelChannel').onchange=renderHumanTable;
$('#labelTarget').onchange=()=>{$('#labelRetract').disabled=!$('#labelTarget').value;};
$('#blindLabel').onchange=()=>{document.body.classList.toggle('blindLabel',$('#blindLabel').checked);if(state)render(state);};
function drawHumanMarkers(ctx,slot,x,h,start,end){
  if(!annotationId||annotationId!==(review?.id||state?.recording?.id))return;
  const origin=review?0:annotationMeta?.startMonotonic;
  if(origin===undefined)return;
  ctx.save();ctx.font='10px ui-monospace,monospace';ctx.strokeStyle='#eef0f0';ctx.fillStyle='#eef0f0';ctx.setLineDash([2,3]);
  for(const r of humanActive){
    if(r.slot!==slot)continue;
    const a=r.tStart+origin,b=r.tEnd+origin;if(b<start||a>end)continue;
    const px=x(Math.max(a,start)),right=x(Math.min(b,end));
    if(b>a){ctx.globalAlpha=.1;ctx.fillRect(px,12,Math.max(1,right-px),h-42);ctx.globalAlpha=1;}
    ctx.beginPath();ctx.moveTo(px,12);ctx.lineTo(px,h-30);ctx.stroke();
    ctx.fillText('H:'+({pass:'P',no_pass:'N',unsure:'?'}[r.label]),px+2,22);
  }
  ctx.restore();
}
