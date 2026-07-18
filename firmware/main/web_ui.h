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
input[type=range]{width:140px}
input[type=number],input[type=text]{width:72px;background:#0d1117;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px}
pre{margin:0;white-space:pre-wrap;word-break:break-all;font:12px/1.4 ui-monospace,Consolas,monospace;color:#c9d1d9;max-height:180px;overflow:auto}
.ok{color:var(--acc)}.bad{color:var(--bad)}.warn{color:var(--warn)}
label{color:var(--muted)}
img.cam{max-width:100%;background:#000;border-radius:6px;min-height:120px}
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
    <button onclick="setMotor(1)">正转</button>
    <button onclick="setMotor(-1)">反转</button>
    <button onclick="setMotor(0)">停止</button>
    <button class="danger" onclick="stopAllMotors()">全停</button>
  </div>
</section>
<section>
  <h2>探照灯 (U23 LED0-2)</h2>
  <div class="row">
    <button onclick="setLed(0,100)">LED_1</button>
    <button onclick="setLed(1,100)">LED_2</button>
    <button onclick="setLed(2,100)">LED_ALL</button>
    <button onclick="setLed(0,0);setLed(1,0);setLed(2,0)">全关</button>
  </div>
  <div class="row"><label>占空%</label><input id="ledDuty" type="number" min="0" max="100" value="80"/>
    <button onclick="setLed(2,+ledDuty.value)">ALL@%</button>
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
  <img id="camImg" class="cam" alt="capture"/>
</section>
<section>
  <h2>SPI 屏 ST7796 + 触摸</h2>
  <div class="row">
    <button onclick="api('POST','/api/lcd',{cmd:'init'})">初始化</button>
    <button onclick="api('POST','/api/lcd',{cmd:'on'})">背光开</button>
    <button onclick="api('POST','/api/lcd',{cmd:'off'})">背光关</button>
  </div>
  <div class="row">
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'F800'})">红</button>
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'07E0'})">绿</button>
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'001F'})">蓝</button>
    <button onclick="api('POST','/api/lcd',{cmd:'fill',color:'0000'})">黑</button>
  </div>
  <pre id="touch">-</pre>
  <div class="row"><button onclick="readTouch()">读触摸</button></div>
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
    `pwmEnable(OE low)=${s.pwmEnable}\nstby=${s.motorStby}\namp=${s.ampEnable}\ncam=${s.camera}\nlcd=${s.lcd}`;
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
async function stopAllMotors(){await api('POST','/api/motor/stop_all')}
async function setLed(id,duty){await api('POST','/api/led',{id,duty:+duty})}
async function readMic(){const m=await api('GET','/api/mic');if(m)mic.textContent=`RMS ${m.rms}  peak ${m.peak}`;}
async function oled(cmd){await api('POST','/api/oled',{cmd,text:oledText.value})}
async function camOn(on){await api('POST','/api/camera',{on});refresh()}
async function camSnap(){document.getElementById('camImg').src='/api/camera/capture?t='+Date.now()}
async function readTouch(){const t=await api('GET','/api/touch');if(t)touch.textContent=`irq=${t.irq} valid=${t.valid}\nx=${t.x} y=${t.y} z=${t.z}`}
refresh();
setInterval(refresh,2000);
</script>
</body>
</html>)HTML";
