#pragma once

static const char INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>EDA Robot v6-1</title>
<style>
:root{--bg:#121418;--card:#1c2128;--fg:#e6edf3;--muted:#8b949e;--acc:#3fb950;--warn:#d29922;--bad:#f85149;--line:#30363d}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 system-ui,Segoe UI,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:14px 16px;border-bottom:1px solid var(--line);display:flex;gap:12px;flex-wrap:wrap;align-items:center}
header h1{font-size:16px;margin:0;font-weight:600}
.action-note{font-size:12px;color:var(--muted)}
.badge{padding:2px 8px;border-radius:999px;background:#238636;font-size:12px}
.badge.off{background:#6e7681}
main{padding:12px;display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
h2{margin:0 0 10px;font-size:13px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
button,input{font:inherit}
button{background:#21262d;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:8px 12px;cursor:pointer}
button:hover{border-color:#8b949e}
button.primary{background:#238636;border-color:#2ea043}
button.danger{background:#da3633;border-color:#f85149}
button:disabled{opacity:.45;cursor:not-allowed}
input[type=range]{width:140px;accent-color:var(--acc)}
input[type=number],input[type=text]{width:72px;background:#0d1117;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px}
.led-row{display:grid;grid-template-columns:72px 1fr 40px;gap:8px;align-items:center;margin:8px 0}
.led-row input[type=range]{width:100%;min-width:0}
.led-pct{font-variant-numeric:tabular-nums;color:var(--muted);text-align:right}
input[type=file]{max-width:100%;color:var(--muted)}
.progress{height:8px;background:#0d1117;border-radius:4px;overflow:hidden;margin-top:8px}
.progress>i{display:block;height:100%;width:0;background:var(--acc);transition:width .15s}
pre{margin:0;white-space:pre-wrap;word-break:break-all;font:12px/1.4 ui-monospace,Consolas,monospace;color:#c9d1d9;max-height:180px;overflow:auto}
.log-card{grid-column:1/-1}.log-console{height:280px;max-height:50vh;background:#0d1117;border:1px solid var(--line);border-radius:6px;padding:8px;white-space:pre;overflow:auto}.log-meta{font-size:12px;color:var(--muted)}
.ok{color:var(--acc)}.bad{color:var(--bad)}.warn{color:var(--warn)}
label{color:var(--muted)}
</style>
</head>
<body>
<header>
  <h1>EDA-Robot v6-1</h1>
  <span id="wifi" class="badge off">...</span>
  <button class="danger" onclick="api('POST','/api/estop')">紧急停止</button>
  <span class="action-note">关 PWM / 功放 / 雷达供电</span>
  <button id="btnShutdown" class="danger" onclick="shutdownDevice()">关机</button>
  <button onclick="refresh()">刷新</button>
</header>
<main>
<section>
  <h2>系统 / I2C</h2>
  <pre id="status">加载中...</pre>
</section>
<section>
  <h2>安全使能</h2>
  <div class="row">
    <button id="btnPwm" class="primary" onclick="togglePwm()">使能 PWM (OE#)</button>
    <button id="btnAmp" onclick="toggleAmp()">功放</button>
    <button onclick="api('POST','/api/beep')">蜂鸣</button>
  </div>
  <pre id="flags"></pre>
</section>
<section>
  <h2>舵机 T3 / T4 (U16)</h2>
  <div class="led-row">
    <label for="servo0">T3</label>
    <input id="servo0" type="range" min="0" max="180" value="90" oninput="onServoSlide(0,this)"/>
    <span id="servoV0" class="led-pct">90°</span>
  </div>
  <div class="led-row">
    <label for="servo1">T4</label>
    <input id="servo1" type="range" min="0" max="180" value="90" oninput="onServoSlide(1,this)"/>
    <span id="servoV1" class="led-pct">90°</span>
  </div>
  <div class="row">
    <button onclick="setAllServo(0)">全 0°</button>
    <button onclick="setAllServo(90)">全 90°</button>
    <button onclick="setAllServo(180)">全 180°</button>
  </div>
</section>
<section>
  <h2>探照灯 (U16→MOSFET)</h2>
  <p class="action-note">LED_1/2 需同时开 LED_ALL（公共地开关）</p>
  <div class="led-row">
    <label for="led0">LED_1</label>
    <input id="led0" type="range" min="0" max="100" value="0" oninput="onLedSlide(0,this)"/>
    <span id="ledV0" class="led-pct">0%</span>
  </div>
  <div class="led-row">
    <label for="led1">LED_2</label>
    <input id="led1" type="range" min="0" max="100" value="0" oninput="onLedSlide(1,this)"/>
    <span id="ledV1" class="led-pct">0%</span>
  </div>
  <div class="led-row">
    <label for="led2">LED_ALL</label>
    <input id="led2" type="range" min="0" max="100" value="0" oninput="onLedSlide(2,this)"/>
    <span id="ledV2" class="led-pct">0%</span>
  </div>
  <div class="row">
    <button onclick="ledsSetAll(0)">全关</button>
    <button onclick="ledsSetAll(100)">全亮</button>
  </div>
</section>
<section>
  <h2>60G 雷达</h2>
  <pre id="radarSum">加载中...</pre>
  <div class="row">
    <button id="btnRadarPwr" onclick="toggleRadarPower()">雷达供电</button>
    <button id="btnRadar" onclick="toggleRadar()">采集</button>
    <a href="/radar" style="color:#58a6ff;font-weight:600">详细调试 →</a>
  </div>
</section>
<section>
  <h2>麦克风 RMS</h2>
  <pre id="mic">-</pre>
  <div class="row"><button onclick="readMic()">采样</button></div>
</section>
<section>
  <h2>OLED（支持常用汉字）</h2>
  <p class="action-note">约 3700 字 16×16；每行约 8 个汉字</p>
  <div class="row">
    <input id="oledText" type="text" style="width:200px" maxlength="40" value="你好机器人"/>
    <button onclick="oled('text')">显示</button>
    <button onclick="oled('clear')">清空</button>
  </div>
</section>
<section class="log-card">
  <h2>设备日志</h2>
  <div class="row">
    <button id="logPause" onclick="toggleLogs()">暂停</button>
    <button onclick="clearLogs()">清空显示</button>
    <label><input id="logFollow" type="checkbox" checked/> 自动滚动</label>
    <span id="logState" class="log-meta">连接中...</span>
  </div>
  <pre id="deviceLog" class="log-console">等待设备日志...</pre>
</section>
<section>
  <h2>Web 烧录 (OTA)</h2>
  <pre id="otaInfo">-</pre>
  <div class="row">
    <input id="otaFile" type="file" accept=".bin,application/octet-stream"/>
    <button id="otaBtn" class="primary" onclick="otaFlash()">上传并烧录</button>
  </div>
  <div class="progress"><i id="otaBar"></i></div>
  <pre id="otaLog">选择 build/eda_robot.bin</pre>
</section>
</main>
<script>
async function api(method,url,body){
  const opt={method,headers:{}};
  if(body!==undefined){opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(body)}
  const r=await fetch(url,opt);
  const t=await r.text();
  let j; try{j=JSON.parse(t)}catch(e){j={ok:false,raw:t}}
  if(!r.ok||j.ok===false){alert((j&&j.error)||t||('HTTP '+r.status));}
  return j;
}
function renderFlags(s){
  document.getElementById('flags').textContent=
    `pwm=${s.pwmEnable}\namp=${s.ampEnable}\nradarPower=${s.radarPower}`;
  document.getElementById('btnPwm').textContent=s.pwmEnable?'PWM 已开':'使能 PWM (OE#)';
  document.getElementById('btnAmp').textContent=s.ampEnable?'功放 已开':'功放';
}
async function refresh(){
  const s=await api('GET','/api/status');
  if(!s)return;
  document.getElementById('wifi').textContent=s.ip||'no-ip';
  document.getElementById('wifi').className='badge'+(s.ip?'':' off');
  document.getElementById('status').innerHTML=
    `FW ${s.fw}  ${s.board||''}\nIP ${s.ip}\nRSSI ${s.rssi}\n`+
    `XL9555 ${s.xl9555?'<span class=ok>OK</span>':'<span class=bad>—</span>'}  `+
    `OLED ${s.oled?'<span class=ok>OK</span>':'<span class=bad>—</span>'}\n`+
    `PCA9685 ${s.pca9685?'<span class=ok>OK</span>':'<span class=bad>—</span>'}  `+
    `I2S ${s.i2s?'<span class=ok>OK</span>':'<span class=warn>—</span>'}\n`+
    `PSRAM ${s.psram?'<span class=ok>'+Math.round((s.psramBytes||0)/1048576)+'MB</span>':'<span class=warn>—</span>'}\n`+
    `I2C: ${(s.i2c||[]).map(x=>'0x'+Number(x).toString(16)).join(', ')||'无（模块未焊/未上电）'}`;
  renderFlags(s);
  const rd=await api('GET','/api/radar');
  if(rd){
    document.getElementById('btnRadarPwr').textContent=rd.power?'供电 已开':'雷达供电';
    document.getElementById('btnRadar').textContent=rd.enabled?'采集 已开':'采集';
    document.getElementById('radarSum').innerHTML=
    `供电 ${rd.power?'<span class=ok>开</span>':'关'}  `+
    `采集 ${rd.enabled?'<span class=ok>开</span>':'关'}  `+
    `UART ${rd.uart?'<span class=ok>就绪</span>':'<span class=warn>—</span>'}  `+
    `OUT ${rd.gpioOut?'高':'低'}\n`+
    (rd.enabled ?
      `存在 ${rd.present?'<span class=ok>是</span>':'否'}  距离 ${rd.range_mm?(rd.range_mm/1000).toFixed(2)+'m':'—'}` :
      '先开供电，再开采集');
  }
}
async function toggleRadarPower(){const r=await api('GET','/api/radar');await api('POST','/api/radar',{power:!r.power});refresh()}
async function toggleRadar(){const r=await api('GET','/api/radar');await api('POST','/api/radar',{on:!r.enabled});refresh()}
async function shutdownDevice(){
  if(!confirm('急停并进入深度睡眠？需断电或按 EN 恢复。'))return;
  const btn=document.getElementById('btnShutdown');
  btn.disabled=true;
  const r=await api('POST','/api/shutdown');
  if(!r||r.ok===false){btn.disabled=false;return}
  clearInterval(refreshTimer);clearInterval(otaTimer);clearInterval(logTimer);
  document.getElementById('wifi').textContent='已关机';
  document.getElementById('wifi').className='badge off';
}
async function togglePwm(){const s=await api('GET','/api/status');await api('POST','/api/pwm',{on:!s.pwmEnable});refresh()}
async function toggleAmp(){const s=await api('GET','/api/status');await api('POST','/api/amp',{on:!s.ampEnable});refresh()}
const servoTimers=[0,0];
function onServoSlide(id,el){
  const angle=Math.max(0,Math.min(180,+el.value||0));
  document.getElementById('servoV'+id).textContent=angle+'°';
  if(servoTimers[id]) clearTimeout(servoTimers[id]);
  servoTimers[id]=setTimeout(()=>setServo(id,angle),40);
}
async function setServo(id,angle){
  angle=Math.max(0,Math.min(180,+angle||0));
  const el=document.getElementById('servo'+id);
  const lab=document.getElementById('servoV'+id);
  if(el) el.value=String(angle);
  if(lab) lab.textContent=angle+'°';
  return api('POST','/api/servo',{id,angle});
}
async function setAllServo(angle){
  for(let i=0;i<2;i++){
    if(servoTimers[i]){clearTimeout(servoTimers[i]);servoTimers[i]=0}
    const j=await setServo(i,angle);
    if(!j||j.ok===false) break;
  }
}
const ledTimers=[0,0,0];
function onLedSlide(id,el){
  const duty=Math.max(0,Math.min(100,+el.value||0));
  document.getElementById('ledV'+id).textContent=duty+'%';
  if(ledTimers[id]) clearTimeout(ledTimers[id]);
  ledTimers[id]=setTimeout(()=>setLed(id,duty,true),40);
}
async function setLed(id,duty,quiet){
  duty=Math.max(0,Math.min(100,+duty||0));
  const el=document.getElementById('led'+id);
  const lab=document.getElementById('ledV'+id);
  if(el) el.value=String(duty);
  if(lab) lab.textContent=duty+'%';
  const r=await fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,duty})});
  const t=await r.text();
  let j; try{j=JSON.parse(t)}catch(e){j={ok:false,raw:t}}
  if(j&&j.pwmEnable){const btn=document.getElementById('btnPwm');if(btn) btn.textContent='PWM 已开'}
  if(!quiet && (!r.ok||j.ok===false)) alert((j&&j.error)||t||('HTTP '+r.status));
  return j;
}
async function ledsSetAll(duty){
  for(let i=0;i<3;i++){
    if(ledTimers[i]){clearTimeout(ledTimers[i]);ledTimers[i]=0}
    const j=await setLed(i,duty,true);
    if(j&&j.ok===false){alert(j.error||'失败');break}
  }
  refresh();
}
async function readMic(){const m=await api('GET','/api/mic');if(m)document.getElementById('mic').textContent=`RMS ${m.rms}  peak ${m.peak}`;}
async function oled(cmd){await api('POST','/api/oled',{cmd,text:document.getElementById('oledText').value})}
async function refreshOta(){
  const o=await api('GET','/api/ota');
  if(!o)return;
  document.getElementById('otaInfo').textContent=
    `FW ${o.fw}\nrunning ${o.running}\nnext ${o.next}  busy=${o.busy}`;
}
async function otaFlash(){
  const f=document.getElementById('otaFile').files[0];
  const log=document.getElementById('otaLog');
  const bar=document.getElementById('otaBar');
  const btn=document.getElementById('otaBtn');
  if(!f){alert('请先选择 .bin');return}
  if(!confirm('上传 '+f.name+' 并重启？'))return;
  btn.disabled=true;bar.style.width='0%';log.textContent='上传中...';
  try{
    await new Promise((resolve,reject)=>{
      const xhr=new XMLHttpRequest();
      xhr.open('POST','/api/ota');
      xhr.setRequestHeader('Content-Type','application/octet-stream');
      xhr.timeout=180000;
      xhr.upload.onprogress=e=>{
        if(e.lengthComputable){const p=Math.round(e.loaded*100/e.total);bar.style.width=p+'%';log.textContent='上传 '+p+'%'}
      };
      xhr.onload=()=>{
        let j;try{j=JSON.parse(xhr.responseText)}catch(e){j={ok:false,raw:xhr.responseText}}
        if(xhr.status>=200&&xhr.status<300&&j.ok!==false){
          bar.style.width='100%';log.textContent='成功，重启中…';setTimeout(()=>location.reload(),5000);resolve(j);
        }else reject(new Error((j&&j.error)||xhr.responseText||('HTTP '+xhr.status)));
      };
      xhr.onerror=()=>reject(new Error('网络错误'));
      xhr.ontimeout=()=>reject(new Error('超时'));
      xhr.send(f);
    });
  }catch(e){log.textContent='失败: '+e.message;alert('OTA 失败: '+e.message);btn.disabled=false}
}
let logSeq=0,logPaused=false,logBusy=false,logLines=[];
async function refreshLogs(){
  if(logPaused||logBusy)return;
  logBusy=true;
  const state=document.getElementById('logState');
  try{
    const r=await fetch('/api/logs?after='+logSeq+'&limit=64');
    const j=await r.json();
    if(!j.ok)throw new Error(j.error||'日志错误');
    for(const e of (j.entries||[])){logLines.push('['+String(e.ms).padStart(8,' ')+'] '+String(e.text).replace(/[\r\n]+$/,''));logSeq=e.seq}
    if(logLines.length>800)logLines.splice(0,logLines.length-800);
    const box=document.getElementById('deviceLog');
    const nearBottom=box.scrollHeight-box.scrollTop-box.clientHeight<24;
    box.textContent=logLines.length?logLines.join('\n'):'暂无日志';
    if(document.getElementById('logFollow').checked&&nearBottom)box.scrollTop=box.scrollHeight;
    state.textContent='已连接 · seq '+logSeq;state.className='log-meta ok';
  }catch(e){state.textContent='失败: '+e.message;state.className='log-meta bad'}
  finally{logBusy=false}
}
function toggleLogs(){logPaused=!logPaused;document.getElementById('logPause').textContent=logPaused?'继续':'暂停';if(!logPaused)refreshLogs()}
function clearLogs(){logLines=[];document.getElementById('deviceLog').textContent='显示已清空'}
refresh();refreshOta();refreshLogs();
const refreshTimer=setInterval(refresh,2000);
const otaTimer=setInterval(refreshOta,5000);
const logTimer=setInterval(refreshLogs,500);
</script>
</body>
</html>)HTML";
