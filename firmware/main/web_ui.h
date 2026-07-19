#pragma once

// 单页调试面板，内嵌 PROGMEM
static const char INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>EDA Robot Debug</title>
<style>
:root{--bg:#121418;--card:#1c2128;--fg:#e6edf3;--muted:#8b949e;--acc:#3fb950;--warn:#d29922;--bad:#f85149;--line:#30363d}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 system-ui,Segoe UI,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:14px 16px;border-bottom:1px solid var(--line);display:flex;gap:12px;flex-wrap:wrap;align-items:center}
header h1{font-size:16px;margin:0;font-weight:600}
.badge{padding:2px 8px;border-radius:999px;background:#238636;font-size:12px}
.badge.off{background:#6e7681}
main{padding:12px;display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
h2{margin:0 0 10px;font-size:13px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
button,input,select{font:inherit}
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
.ok{color:var(--acc)}.bad{color:var(--bad)}.warn{color:var(--warn)}
label{color:var(--muted)}
img.cam{max-width:100%;width:100%;aspect-ratio:4/3;object-fit:contain;background:#0d1117;border:1px solid var(--line);border-radius:6px;display:block}
.cam-wrap{position:relative;background:#0d1117;border:1px solid var(--line);border-radius:6px;overflow:hidden}
.cam-wrap:not(.has-img) img.cam{visibility:hidden;border:0}
.cam-ph{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;color:var(--muted);pointer-events:none}
.cam-ph svg{opacity:.55}
.cam-ph span{font-size:12px}
.cam-wrap.has-img .cam-ph{display:none}
</style>
</head>
<body>
<header>
  <h1>EDA-RobotPro Web Debug</h1>
  <span id="wifi" class="badge off">...</span>
  <button class="danger" onclick="api('POST','/api/estop')">紧急停止</button>
  <button onclick="refresh()">刷新状态</button>
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
    <button id="btnMtr" class="primary" onclick="toggleStby()">使能电机 STBY</button>
  </div>
  <div class="row">
    <button id="btnAmp" onclick="toggleAmp()">功放 SD_MODE</button>
    <button onclick="api('POST','/api/beep')">蜂鸣测试</button>
  </div>
  <pre id="flags"></pre>
</section>
<section>
  <h2>舵机 T3-T7 (U16)</h2>
  <div class="row"><label>通道</label>
    <select id="servoId"><option value="0">T3 ch11</option><option value="1">T4 ch12</option><option value="2">T5 ch13</option><option value="3">T6 ch14</option><option value="4">T7 ch15</option></select>
    <label>角度</label><input id="servoAng" type="number" min="0" max="180" value="90"/>
    <button onclick="setServo()">设置</button>
  </div>
  <div class="row">
    <button onclick="setAllServo(0)">全 0°</button>
    <button onclick="setAllServo(90)">全 90°</button>
    <button onclick="setAllServo(180)">全 180°</button>
  </div>
</section>
<section>
  <h2>电机 0-3 (U23→TB6612)</h2>
  <div class="row"><label>电机</label>
    <select id="motorId"><option>0</option><option>1</option><option>2</option><option>3</option></select>
    <label>占空%</label><input id="motorDuty" type="number" min="0" max="100" value="40"/>
  </div>
  <div class="row">
    <button onpointerdown="startMotor(1)" onpointerup="stopMotorHold()" onpointerleave="stopMotorHold()">按住正转</button>
    <button onpointerdown="startMotor(-1)" onpointerup="stopMotorHold()" onpointerleave="stopMotorHold()">按住反转</button>
    <button onclick="stopMotorHold()">停止</button>
    <button class="danger" onclick="stopAllMotors()">全停</button>
  </div>
</section>
<section>
  <h2>探照灯 (U23 LED0-2)</h2>
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
  <h2>编码器</h2>
  <pre id="enc">-</pre>
  <div class="row"><button onclick="api('POST','/api/encoders/reset')">清零</button></div>
</section>
<section>
  <h2>麦克风 RMS</h2>
  <pre id="mic">-</pre>
  <div class="row"><button onclick="readMic()">采样</button></div>
</section>
<section>
  <h2>OLED</h2>
  <div class="row">
    <input id="oledText" type="text" style="width:160px" maxlength="21" value="Hello Robot"/>
    <button onclick="oled('text')">显示</button>
    <button onclick="oled('clear')">清空</button>
    <button onclick="oled('fill')">全亮</button>
  </div>
</section>
<section>
  <h2>摄像头 OV2640</h2>
  <div class="row">
    <button class="primary" onclick="camOn(true)">开启</button>
    <button onclick="camOn(false)">关闭</button>
    <button onclick="camSnap()">抓拍</button>
    <a href="/stream" target="_blank" style="color:#58a6ff">MJPEG 流</a>
  </div>
  <div id="camWrap" class="cam-wrap">
    <img id="camImg" class="cam" alt="capture" onload="camShow(true)" onerror="camShow(false)"/>
    <div class="cam-ph" aria-hidden="true">
      <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
        <path d="M4 7h3l2-2h6l2 2h3a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2z"/>
        <circle cx="12" cy="13" r="3.5"/>
        <line x1="3" y1="3" x2="21" y2="21"/>
      </svg>
      <span>无画面 · 开启后抓拍</span>
    </div>
  </div>
</section>
<section>
  <h2>SPI 屏 ST7796 + 触摸</h2>
  <div class="row">
    <button onclick="api('POST','/api/lcd',{cmd:'init'})">初始化</button>
    <button onclick="api('POST','/api/lcd',{cmd:'on'})">背光开</button>
    <button onclick="api('POST','/api/lcd',{cmd:'off'})">背光关</button>
    <button class="primary" onclick="api('POST','/api/lcd',{cmd:'demo'})">演示画面</button>
  </div>
  <div class="row">
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'F800'})">红</button>
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'07E0'})">绿</button>
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'001F'})">蓝</button>
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'0000'})">黑</button>
  </div>
  <label>屏上文字（ASCII，用 \n 换行）</label>
  <div class="row">
    <input id="lcdText" type="text" style="width:100%;max-width:280px" value="Hello EDA Robot"/>
  </div>
  <div class="row">
    <label>x</label><input id="lcdX" type="number" value="8"/>
    <label>y</label><input id="lcdY" type="number" value="80"/>
    <label>scale</label><input id="lcdScale" type="number" min="1" max="6" value="2"/>
  </div>
  <div class="row">
    <label>fg</label><input id="lcdFg" type="text" value="FFFF" style="width:64px"/>
    <label>bg</label><input id="lcdBg" type="text" value="0000" style="width:64px"/>
  </div>
  <div class="row">
    <button class="primary" onclick="lcdDrawText(false)">显示文字</button>
    <button onclick="lcdDrawText(true)">清屏后显示</button>
    <button onclick="lcdShowStatus()">显示状态信息</button>
  </div>
  <pre id="touch">-</pre>
  <div class="row"><button onclick="readTouch()">读触摸</button></div>
</section>
<section>
  <h2>Web 烧录 (OTA)</h2>
  <pre id="otaInfo">-</pre>
  <div class="row">
    <input id="otaFile" type="file" accept=".bin,application/octet-stream"/>
    <button id="otaBtn" class="primary" onclick="otaFlash()">上传并烧录</button>
  </div>
  <div class="progress"><i id="otaBar"></i></div>
  <pre id="otaLog">选择 build/eda_robot.bin，无需串口。烧录成功后自动重启。</pre>
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
    `pwmEnable(OE low)=${s.pwmEnable}\nstby=${s.motorStby}\nmotorMask=${s.motorActiveMask||0}\n`+
    `motorFailsafe=${s.motorFailsafeMs||0}ms\namp=${s.ampEnable}\ncam=${s.camera}\nstream=${s.streaming}\nlcd=${s.lcd}`;
  document.getElementById('btnPwm').textContent=s.pwmEnable?'PWM 已开 (点关闭)':'使能 PWM (OE#)';
  document.getElementById('btnMtr').textContent=s.motorStby?'STBY 已开 (点关闭)':'使能电机 STBY';
  document.getElementById('btnAmp').textContent=s.ampEnable?'功放 已开 (点关闭)':'功放 SD_MODE';
}
async function refresh(){
  const s=await api('GET','/api/status');
  if(!s)return;
  document.getElementById('wifi').textContent=s.ip||'no-ip';
  document.getElementById('wifi').className='badge'+(s.ip?'':' off');
  document.getElementById('status').innerHTML=
    `FW ${s.fw}\nIP ${s.ip}\nRSSI ${s.rssi}\n`+
    `XL9555 ${s.xl9555?'<span class=ok>OK</span>':'<span class=bad>FAIL</span>'}  `+
    `OLED ${s.oled?'<span class=ok>OK</span>':'<span class=bad>FAIL</span>'}\n`+
    `PCA U16 ${s.pcaServo?'<span class=ok>OK</span>':'<span class=bad>FAIL</span>'}  `+
    `U23 ${s.pcaMotor?'<span class=ok>OK</span>':'<span class=bad>FAIL</span>'}\n`+
    `LCD ${s.lcd?'<span class=ok>OK</span>':'<span class=bad>OFF</span>'}  `+
    `PSRAM ${s.psram?'<span class=ok>'+Math.round((s.psramBytes||0)/1048576)+'MB</span>':'<span class=warn>FAIL</span>'}\n`+
    `CAM ${s.camera?'<span class=ok>ON</span>':'<span class=warn>OFF</span>'}\n`+
    `I2C: ${(s.i2c||[]).map(x=>'0x'+x.toString(16)).join(', ')}`;
  renderFlags(s);
  const e=await api('GET','/api/encoders');
  if(e) document.getElementById('enc').textContent=
    `ENC1 ${e.enc1}  ENC2 ${e.enc2}\nENC3 ${e.enc3}  ENC4 ${e.enc4}\nrawIO0 0x${(e.xlPort0||0).toString(16)}`;
}
async function togglePwm(){const s=await api('GET','/api/status');await api('POST','/api/pwm',{on:!s.pwmEnable});refresh()}
async function toggleStby(){const s=await api('GET','/api/status');await api('POST','/api/stby',{on:!s.motorStby});refresh()}
async function toggleAmp(){const s=await api('GET','/api/status');await api('POST','/api/amp',{on:!s.ampEnable});refresh()}
async function setServo(){
  await api('POST','/api/servo',{id:+servoId.value,angle:+servoAng.value});
}
async function setAllServo(a){
  for(let i=0;i<5;i++) await api('POST','/api/servo',{id:i,angle:a});
}
async function setMotor(dir){
  await api('POST','/api/motor',{id:+motorId.value,dir,duty:+motorDuty.value});
}
let motorTimer=0,motorGeneration=0;
async function startMotor(dir){
  const generation=++motorGeneration;
  stopMotorTimer();
  await setMotor(dir);
  if(generation===motorGeneration) motorTimer=setInterval(()=>setMotor(dir),500);
}
function stopMotorTimer(){if(motorTimer){clearInterval(motorTimer);motorTimer=0}}
async function stopMotorHold(){motorGeneration++;stopMotorTimer();await setMotor(0)}
async function stopAllMotors(){await api('POST','/api/motor/stop_all')}
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
  if(j&&j.pwmEnable){
    const btn=document.getElementById('btnPwm');
    if(btn) btn.textContent='PWM 已开 (点关闭)';
  }
  if(!quiet && (!r.ok||j.ok===false)) alert((j&&j.error)||t||('HTTP '+r.status));
  return j;
}
async function ledsSetAll(duty){
  for(let i=0;i<3;i++){
    if(ledTimers[i]){clearTimeout(ledTimers[i]);ledTimers[i]=0}
    const j=await setLed(i,duty,true);
    if(j&&j.ok===false){alert(j.error||'探照灯设置失败');break}
  }
  refresh();
}
async function readMic(){const m=await api('GET','/api/mic');if(m)mic.textContent=`RMS ${m.rms}  peak ${m.peak}`;}
async function oled(cmd){await api('POST','/api/oled',{cmd,text:oledText.value})}
function camShow(ok){
  document.getElementById('camWrap').classList.toggle('has-img',!!ok);
}
async function camOn(on){
  await api('POST','/api/camera',{on});
  if(!on){
    document.getElementById('camImg').removeAttribute('src');
    camShow(false);
  }
  refresh();
}
async function camSnap(){
  camShow(false);
  document.getElementById('camImg').src='/api/camera/capture?t='+Date.now();
}
async function lcdDrawText(clear){
  await api('POST','/api/lcd',{
    cmd:'text',
    text:document.getElementById('lcdText').value,
    x:+document.getElementById('lcdX').value,
    y:+document.getElementById('lcdY').value,
    scale:+document.getElementById('lcdScale').value||2,
    color:document.getElementById('lcdFg').value||'FFFF',
    bg:document.getElementById('lcdBg').value||'0000',
    clear:!!clear
  });
}
async function lcdShowStatus(){
  const s=await api('GET','/api/status');
  if(!s)return;
  const t=
    'EDA-RobotPro\n'+
    'FW '+s.fw+'\n'+
    'IP '+(s.ip||'no-ip')+'\n'+
    'RSSI '+(s.rssi??'?')+'\n'+
    'LCD '+(s.lcd?'OK':'OFF')+' CAM '+(s.camera?'ON':'OFF');
  await api('POST','/api/lcd',{cmd:'text',text:t,x:8,y:8,scale:2,color:'FFFF',bg:'0000',clear:true});
}
async function readTouch(){const t=await api('GET','/api/touch');if(t)touch.textContent=`irq=${t.irq} valid=${t.valid}\nx=${t.x} y=${t.y} z=${t.z}`}
async function refreshOta(){
  const o=await api('GET','/api/ota');
  if(!o)return;
  document.getElementById('otaInfo').textContent=
    `FW ${o.fw}\nrunning ${o.running} @0x${(o.runningOffset||0).toString(16)}\n`+
    `next ${o.next} (${Math.round((o.nextSize||0)/1048576)}MB)  busy=${o.busy}`;
}
async function otaFlash(){
  const f=document.getElementById('otaFile').files[0];
  const log=document.getElementById('otaLog');
  const bar=document.getElementById('otaBar');
  const btn=document.getElementById('otaBtn');
  if(!f){alert('请先选择 .bin 固件');return}
  if(!confirm('上传 '+f.name+' ('+Math.round(f.size/1024)+'KB) 并重启？\n勿断电、勿刷新页面。'))return;
  btn.disabled=true;bar.style.width='0%';log.textContent='上传中...';
  try{
    await new Promise((resolve,reject)=>{
      const xhr=new XMLHttpRequest();
      xhr.open('POST','/api/ota');
      xhr.setRequestHeader('Content-Type','application/octet-stream');
      xhr.timeout=180000;
      xhr.upload.onprogress=e=>{
        if(e.lengthComputable){
          const p=Math.round(e.loaded*100/e.total);
          bar.style.width=p+'%';
          log.textContent='上传 '+p+'%  ('+e.loaded+'/'+e.total+')';
        }
      };
      xhr.onload=()=>{
        let j;try{j=JSON.parse(xhr.responseText)}catch(e){j={ok:false,raw:xhr.responseText}}
        if(xhr.status>=200&&xhr.status<300&&j.ok!==false){
          bar.style.width='100%';
          log.textContent='烧录成功，设备重启中… 约 5s 后刷新页面。\n'+JSON.stringify(j);
          setTimeout(()=>location.reload(),5000);
          resolve(j);
        }else reject(new Error((j&&j.error)||xhr.responseText||('HTTP '+xhr.status)));
      };
      xhr.onerror=()=>reject(new Error('网络错误'));
      xhr.ontimeout=()=>reject(new Error('超时'));
      xhr.send(f);
    });
  }catch(e){
    log.textContent='失败: '+e.message;
    alert('OTA 失败: '+e.message);
    btn.disabled=false;
  }
}
refresh();
refreshOta();
setInterval(refresh,2000);
setInterval(refreshOta,5000);
</script>
</body>
</html>)HTML";
