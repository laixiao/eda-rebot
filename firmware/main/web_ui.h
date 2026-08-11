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
.badge.warn{background:#9e6a03}
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
.span-all{grid-column:1/-1}
.radar-card{display:flex;flex-direction:column;gap:12px}
.radar-head{display:flex;flex-wrap:wrap;gap:10px 16px;align-items:center;justify-content:space-between}
.radar-head h2{margin:0}
.radar-head-left{display:flex;flex-wrap:wrap;gap:8px 10px;align-items:center;min-width:0}
.radar-status{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.radar-actions{margin:0}
.radar-body{display:grid;grid-template-columns:minmax(240px,380px) 1fr;gap:14px;align-items:start}
@media(max-width:900px){.radar-body{grid-template-columns:1fr}}
.radar-viz{min-width:0}
.radar-viz .hint{text-align:center;color:var(--muted);font-size:11px;margin:6px 0 0}
.radar-side{min-width:0;display:flex;flex-direction:column;gap:10px}
.fan-box{background:#0d1117;border:1px solid var(--line);border-radius:8px;padding:10px}
.fan-box .fan-title{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;margin-bottom:6px}
.fan-box .fan-line{font:12px/1.45 ui-monospace,Consolas,monospace;color:#c9d1d9}
.radar-side pre{max-height:none;overflow:visible}
.kpi{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px;margin-top:10px}
.kpi div{background:#0d1117;border:1px solid var(--line);border-radius:8px;padding:8px}
.kpi b{display:block;font-size:15px;font-variant-numeric:tabular-nums;margin-top:2px}
.kpi span{color:var(--muted);font-size:11px}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.chip{padding:3px 8px;border-radius:999px;background:#21262d;border:1px solid var(--line);font-size:11px}
.chip.on{background:#0d3b24;border-color:#238636;color:var(--acc)}
.chip.hot{background:#3b2208;border-color:#f0883e;color:#f0883e}
#cv{width:100%;height:auto;aspect-ratio:1;max-height:min(52vh,420px);background:#0d1117;border:1px solid var(--line);border-radius:8px;display:block}
.radar-side table{width:100%;border-collapse:collapse;font-size:12px}
.radar-side td,.radar-side th{padding:4px;border-bottom:1px solid var(--line);text-align:left}
.radar-side th{color:var(--muted);font-weight:500}
.log-card .log-console{min-height:220px;height:min(40vh,420px);max-height:none;background:#0d1117;border:1px solid var(--line);border-radius:6px;padding:8px;white-space:pre;overflow:auto}
.log-meta{font-size:12px;color:var(--muted)}
.ok{color:var(--acc)}.bad{color:var(--bad)}.warn{color:var(--warn)}
label{color:var(--muted)}
.ota-card input[type=file]{max-width:100%}
.rec-btn{width:72px;height:72px;border-radius:50%;display:inline-flex;align-items:center;justify-content:center;padding:0;background:#21262d;border:2px solid var(--line)}
.rec-btn svg{width:32px;height:32px;fill:var(--fg)}
.rec-btn.recording{background:#3b1212;border-color:var(--bad);animation:recPulse 1.2s ease-in-out infinite}
.rec-btn.recording svg{fill:var(--bad)}
@keyframes recPulse{0%,100%{box-shadow:0 0 0 0 rgba(248,81,73,.45)}50%{box-shadow:0 0 0 12px rgba(248,81,73,0)}}
.rec-clip{display:flex;gap:10px;align-items:center;margin-top:10px;padding:10px;background:#0d1117;border:1px solid var(--line);border-radius:8px;cursor:pointer}
.rec-clip:hover{border-color:#8b949e}
.rec-clip .play-ico{width:40px;height:40px;border-radius:50%;background:#238636;border:none;display:inline-flex;align-items:center;justify-content:center;flex-shrink:0}
.rec-clip .play-ico svg{width:18px;height:18px;fill:#fff;margin-left:2px}
.rec-clip b{display:block;font-size:13px}
#recTimer{font-variant-numeric:tabular-nums;color:var(--muted);font-size:12px;min-height:1.2em;text-align:center}
.switch{display:inline-flex;align-items:center;gap:10px;cursor:pointer;user-select:none;color:var(--fg)}
.switch input{position:absolute;opacity:0;width:0;height:0}
.switch .track{position:relative;width:44px;height:24px;background:#6e7681;border-radius:999px;transition:background .15s;flex-shrink:0}
.switch .track::after{content:"";position:absolute;top:2px;left:2px;width:20px;height:20px;background:#fff;border-radius:50%;transition:transform .15s}
.switch input:checked+.track{background:#238636}
.switch input:checked+.track::after{transform:translateX(20px)}
.switch input:disabled+.track{opacity:.45}
.radar-actions .switch{padding:6px 10px;border-radius:8px;border:1px solid transparent;gap:8px}
.radar-actions .switch-pwr{background:#3b2208;border-color:#9e6a03;color:#f0c674}
.radar-actions .switch-pwr input:checked+.track{background:#d29922}
.radar-actions .switch-acq{background:#0d2d4a;border-color:#1f6feb;color:#79c0ff}
.radar-actions .switch-acq input:checked+.track{background:#1f6feb}
.radar-actions .switch-fan{background:#0d3b24;border-color:#238636;color:#3fb950}
.radar-actions .switch-fan input:checked+.track{background:#238636}
.radar-actions .switch-gest{background:#0d2f2f;border-color:#1b9e9e;color:#56d4c8}
.radar-actions .switch-gest input:checked+.track{background:#1b9e9e}
.rec-voice{margin-top:12px;padding-top:12px;border-top:1px solid var(--line)}
.rec-voice .switch-voice{padding:6px 10px;border-radius:8px;border:1px solid #238636;background:#0d3b24;color:#3fb950;gap:8px}
.rec-voice .switch-voice input:checked+.track{background:#238636}
header .switch-periph{padding:6px 10px;border-radius:8px;border:1px solid var(--line);background:#21262d;color:var(--muted);gap:8px}
header .switch-periph input:checked+.track{background:#da3633}
header .switch-periph.on{border-color:#f85149;background:#3b1212;color:#ff7b72}
</style>
</head>
<body>
<header>
  <h1>EDA-Robot v6-1</h1>
  <span id="wifi" class="badge off">...</span>
  <label class="switch switch-periph" title="关 PWM / 功放 / 雷达供电；关后再开可恢复">
    <input type="checkbox" id="swPeriphOff" onchange="setPeriphOff(this.checked)"/>
    <span class="track"></span>
    <span id="labPeriphOff">关闭所有外设</span>
  </label>
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
  </div>
  <pre id="flags"></pre>
</section>
<section class="ota-card">
  <h2>升级固件（救援 OTA）</h2>
  <pre id="otaInfo">-</pre>
  <p class="action-note">主系统不能直接覆盖自己。点下方按钮进救援页，再上传 <code>eda_robot.bin</code> 大包（含语音模型）。</p>
  <div class="row">
    <button id="rescueBtn" class="primary" onclick="enterRescue()">进入救援升级</button>
  </div>
  <pre id="otaLog">屏幕会显示步骤与 IP</pre>
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
  <p class="action-note">LED_1/2 需同时开 LED_ALL（公共地）；雷达联动只控 LED_1</p>
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
  <h2>录音 / 语音</h2>
  <div class="row" style="justify-content:center;margin-top:8px">
    <button id="btnRec" class="rec-btn" onclick="toggleRec()" title="录音" aria-label="录音">
      <svg id="recIcon" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 14a3 3 0 0 0 3-3V6a3 3 0 1 0-6 0v5a3 3 0 0 0 3 3zm5-3a5 5 0 0 1-10 0H5a7 7 0 0 0 6 6.92V21h2v-3.08A7 7 0 0 0 19 11h-2z"/></svg>
    </button>
  </div>
  <div id="recTimer"></div>
  <p id="recHint" class="action-note" style="text-align:center">点击麦克风开始，再次点击停止（最长约 12 秒）</p>
  <div id="recClip" class="rec-clip" hidden onclick="playRec()">
    <span class="play-ico" title="扬声器播放"><svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg></span>
    <div>
      <b>录音片段</b>
      <div id="recMeta" class="action-note">—</div>
    </div>
  </div>
  <div class="rec-voice">
    <div class="row" style="margin:0">
      <label class="switch switch-voice" title="伪唤醒「你好爱妃」→ 开/关/大一点/小一点/最大/最小/中等（默认关；录音时会暂停识别）">
        <input id="swVoice" type="checkbox" onchange="setVoice(this.checked)"/>
        <span class="track"></span>
        <span id="labVoice">语音</span>
      </label>
    </div>
    <div id="voiceStatus" class="fan-line" style="margin-top:8px">加载中...</div>
  </div>
</section>
<section>
  <h2>扬声器</h2>
  <div class="row">
    <label class="switch" title="MAX98357 SD · XL IO1_6">
      <input id="swAmp" type="checkbox" onchange="setAmp(this.checked)"/>
      <span class="track"></span>
      <span id="labAmp">功放</span>
    </label>
    <button onclick="api('POST','/api/beep')">蜂鸣</button>
  </div>
  <p class="action-note">功放开关 = MAX98357 <b>SD</b>（XL9555 IO1_6 / SPK_SD）：开=芯片使能，关=关断省电。播放/蜂鸣若关着会临时打开再关，滑块状态不变。</p>
  <div class="led-row">
    <span>音量</span>
    <input id="vol" type="range" min="0" max="100" value="100" oninput="onVolSlide(this)"/>
    <span id="volV" class="led-pct">100</span>
  </div>
  <p class="action-note">上传音频（任意常见格式，会转成 16 kHz），经板载功放播放</p>
  <div class="row">
    <input id="playFile" type="file" accept="audio/*,.wav,.mp3,.ogg,.m4a"/>
    <button id="playBtn" class="primary" onclick="uploadPlay()">上传并播放</button>
  </div>
  <pre id="spkStatus">—</pre>
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
<section class="span-all radar-card" id="radarPanel">
  <div class="radar-head">
    <div class="radar-head-left">
      <h2>60G 雷达 · LED_1 风扇</h2>
      <div class="radar-status" title="链路 / 供电 / OUT">
        <span id="rdLink" class="badge off">等待数据</span>
        <span id="rdPwr" class="badge off">未供电</span>
        <span id="rdOut" class="badge off">OUT 低</span>
      </div>
    </div>
    <div class="row radar-actions">
      <label class="switch switch-pwr" title="Q4 雷达 3V3（开电即查询）">
        <input id="swRadarPwr" type="checkbox" onchange="setRadarPower(this.checked)"/>
        <span class="track"></span>
        <span id="labRadarPwr">供电</span>
      </label>
      <label class="switch switch-fan" title="有人→按档位开；无人 2s×3 才关。可与手势切档同时开">
        <input id="swFanAuto" type="checkbox" onchange="setFanAuto(this.checked)"/>
        <span class="track"></span>
        <span id="labFanAuto">控风扇</span>
      </label>
      <label class="switch switch-gest" title="近距≤0.45m 停留2s 循环档位 关→50%→100；只管档位，不关控风扇">
        <input id="swFanGest" type="checkbox" onchange="setFanGesture(this.checked)"/>
        <span class="track"></span>
        <span id="labFanGest">手势切档</span>
      </label>
    </div>
  </div>
  <div class="radar-body">
    <div class="radar-viz">
      <canvas id="cv" width="420" height="420"></canvas>
      <div class="hint">FOV ±60° · 10m</div>
      <div class="kpi">
        <div><span>存在</span><b id="kPresent">—</b></div>
        <div><span>距离</span><b id="kRange">—</b></div>
        <div><span>角度</span><b id="kAngle">—</b></div>
        <div><span>手势</span><b id="kGest" style="font-size:13px">—</b></div>
      </div>
      <div class="chips" id="rdChips"></div>
    </div>
    <div class="radar-side">
      <div class="fan-box">
        <div class="fan-title">风扇联动</div>
        <div id="fanStatus" class="fan-line">加载中...</div>
      </div>
      <div>
        <h2 style="margin:0 0 6px">目标</h2>
        <table>
          <thead><tr><th>slot</th><th>距离</th><th>角度</th><th>速度</th></tr></thead>
          <tbody id="objs"><tr><td colspan="4" style="color:var(--muted)">等待数据…</td></tr></tbody>
        </table>
      </div>
      <details open>
        <summary style="cursor:pointer;color:var(--muted);font-size:12px">检测摘要</summary>
        <pre id="detLine" style="margin-top:6px">—</pre>
      </details>
      <details>
        <summary style="cursor:pointer;color:var(--muted);font-size:12px">模块 / 诊断</summary>
        <div class="row" style="margin-top:6px">
          <button onclick="radarCmd('version')" title="向 MS60 发 0xFE 读 SDK/硬件版本">读模块版本</button>
        </div>
        <pre id="moduleInfo" style="margin-top:6px">—</pre>
        <pre id="rdMeta" style="margin-top:6px">—</pre>
      </details>
    </div>
  </div>
</section>
<section class="span-all log-card">
  <h2>设备日志</h2>
  <div class="row">
    <button id="logPause" onclick="toggleLogs()">暂停</button>
    <button onclick="clearLogs()">清空显示</button>
    <label><input id="logFollow" type="checkbox" checked/> 自动滚动</label>
    <span id="logState" class="log-meta">连接中...</span>
  </div>
  <pre id="deviceLog" class="log-console">等待设备日志...</pre>
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
    `pwm=${s.pwmEnable}\namp=${s.ampEnable}\nvol=${s.volume??100}\nradarPower=${s.radarPower}\nperipheralsOff=${!!s.peripheralsOff}`;
  document.getElementById('btnPwm').textContent=s.pwmEnable?'PWM 已开':'使能 PWM (OE#)';
  const amp=document.getElementById('swAmp');
  const lab=document.getElementById('labAmp');
  if(amp) amp.checked=!!s.ampEnable;
  if(lab) lab.textContent=s.ampEnable?'功放 开':'功放';
  const vol=document.getElementById('vol');
  const volV=document.getElementById('volV');
  if(vol && document.activeElement!==vol){
    const v=Math.max(0,Math.min(100,+(s.volume??100)));
    vol.value=v;
    if(volV) volV.textContent=v;
  }
  const po=document.getElementById('swPeriphOff');
  const labPo=document.getElementById('labPeriphOff');
  const wrapPo=po&&po.closest('.switch-periph');
  if(po && document.activeElement!==po) po.checked=!!s.peripheralsOff;
  if(labPo) labPo.textContent=s.peripheralsOff?'已关闭所有外设':'关闭所有外设';
  if(wrapPo) wrapPo.classList.toggle('on',!!s.peripheralsOff);
}
async function setPeriphOff(on){
  const sw=document.getElementById('swPeriphOff');
  if(sw) sw.disabled=true;
  try{
    const j=await api('POST','/api/estop',{on:!!on});
    if(!j||j.ok===false){if(sw) sw.checked=!on}
  }finally{if(sw) sw.disabled=false}
  refresh();
}
function phaseLabel(p){
  return ({disabled:'未启用',idle:'待命',arming:'确认有人中',on:'风扇已开',holdoff:'确认无人中'}[p])||p||'—';
}
function gestPhaseLabel(p){
  return ({disabled:'未启用',no_power:'雷达未供电',idle:'待命',holding:'近距停留中',wait_leave:'已切档·移开手'}[p])||p||'—';
}
const ledTimers=[0,0,0];
function renderFan(f){
  const box=document.getElementById('fanStatus');
  if(!f){box.textContent='—';return}
  const sw=document.getElementById('swFanAuto');
  const lab=document.getElementById('labFanAuto');
  if(sw && document.activeElement!==sw) sw.checked=!!f.auto;
  if(lab) lab.textContent=f.auto?'控风扇 开':'控风扇';
  const swG=document.getElementById('swFanGest');
  const labG=document.getElementById('labFanGest');
  if(swG && document.activeElement!==swG) swG.checked=!!f.gesture;
  if(labG) labG.textContent=f.gesture?'手势切档 开':'手势切档';
  let prog='';
  const needOn=f.need||1;
  const needOff=f.offNeed||f.confirm||3;
  if(f.phase==='arming')
    prog=`有人 ${f.progress||0}/${needOn}（每2s）`;
  else if(f.phase==='holdoff')
    prog=`无人 ${f.progress||0}/${needOff}（每2s）`;
  else if(f.phase==='on')
    prog='保持开';
  else if(f.phase==='idle')
    prog='待命';
  else
    prog='联动关';
  const lv=['关','50%','100%'][f.gestLevel??0]||'—';
  let gest='';
  if(f.gesture){
    const hold=Math.min(f.gestProgressMs||0,f.gestNeedMs||2000);
    const need=f.gestNeedMs||2000;
    const rng=f.gestRangeMm?((f.gestRangeMm/1000).toFixed(2)+' m'):'—';
    gest=`手势档 <b>${f.gear??0}%</b>（${lv}）· <b>${gestPhaseLabel(f.gestPhase)}</b> · ${hold}/${need} ms · 距 ${rng}<br>`;
  }
  box.innerHTML=
    `<b>${phaseLabel(f.phase)}</b> · LED_1 ${f.on?'<span class=ok>开</span>':'关'} · ${prog}<br>`+
    gest+
    `强度 ${f.intensity??0}% · 记忆 ${f.savedIntensity??'—'}%（${f.savedLed1??'—'}/${f.savedLedAll??'—'}）<br>`+
    `判定：${f.reason||'—'}<br>`+
    `上次：${f.lastAction||'—'}`;
}
function renderVoice(v){
  const box=document.getElementById('voiceStatus');
  if(!box) return;
  if(!v){box.textContent='—';return}
  const sw=document.getElementById('swVoice');
  const lab=document.getElementById('labVoice');
  if(sw && document.activeElement!==sw) sw.checked=!!v.enabled;
  if(lab) lab.textContent=v.enabled?'语音 开':'语音';
  const st=!v.enabled?'已关闭':(v.ok?(v.listening?'聆听命令中':(v.paused?'暂停(录音中)':'待命')):'未就绪');
  const mb=v.modelBytes?Math.round(v.modelBytes/1048576*10)/10+'MB':'—';
  box.innerHTML=
    `<b>${st}</b> · 模型 ${mb}<br>`+
    `伪唤醒：你好爱妃 → 随机语音 → 开/关 · 大一点/小一点 · 最大/最小/中等风<br>`+
    `${v.last||'—'}`;
}
async function setVoice(on){
  const sw=document.getElementById('swVoice');
  if(sw) sw.disabled=true;
  try{
    const j=await api('POST','/api/voice',{on:!!on});
    if(!j||j.ok===false){if(sw) sw.checked=!on}
  }finally{if(sw) sw.disabled=false}
  refresh();
}
function syncLedsFromStatus(s){
  const leds=s.leds||[];
  for(let i=0;i<3;i++){
    if(ledTimers[i]) continue;
    if(typeof leds[i]!=='number') continue;
    const el=document.getElementById('led'+i);
    const lab=document.getElementById('ledV'+i);
    if(el) el.value=String(leds[i]);
    if(lab) lab.textContent=leds[i]+'%';
  }
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
  renderFan(s.fan);
  renderVoice(s.voice);
  syncLedsFromStatus(s);
  const rd=await api('GET','/api/radar');
  if(rd){
    const pwr=document.getElementById('swRadarPwr');
    const labP=document.getElementById('labRadarPwr');
    if(pwr && document.activeElement!==pwr) pwr.checked=!!rd.power;
    if(labP) labP.textContent=rd.power?'供电 开':'供电';
  }
}
async function setFanAuto(on){
  const sw=document.getElementById('swFanAuto');
  if(sw) sw.disabled=true;
  try{
    const j=await api('POST','/api/fan',{auto:!!on});
    if(!j||j.ok===false){if(sw) sw.checked=!on}
  }finally{if(sw) sw.disabled=false}
  refresh();
}
async function setFanGesture(on){
  const sw=document.getElementById('swFanGest');
  if(sw) sw.disabled=true;
  try{
    const j=await api('POST','/api/fan',{gesture:!!on});
    if(!j||j.ok===false){if(sw) sw.checked=!on}
  }finally{if(sw) sw.disabled=false}
  refresh();
}
async function setRadarPower(on){
  const sw=document.getElementById('swRadarPwr');
  if(sw) sw.disabled=true;
  try{
    const j=await api('POST','/api/radar',{power:!!on});
    if(!j||j.ok===false){if(sw) sw.checked=!on}
  }finally{if(sw) sw.disabled=false}
  refresh();
}
async function shutdownDevice(){
  if(!confirm('关闭所有外设并进入深度睡眠？需断电或按 EN 恢复。'))return;
  const btn=document.getElementById('btnShutdown');
  btn.disabled=true;
  const r=await api('POST','/api/shutdown');
  if(!r||r.ok===false){btn.disabled=false;return}
  clearInterval(refreshTimer);clearInterval(otaTimer);clearInterval(logTimer);clearInterval(radarLiveTimer);
  document.getElementById('wifi').textContent='已关机';
  document.getElementById('wifi').className='badge off';
}
async function togglePwm(){const s=await api('GET','/api/status');await api('POST','/api/pwm',{on:!s.pwmEnable});refresh()}
async function setAmp(on){
  const sw=document.getElementById('swAmp');
  if(sw) sw.disabled=true;
  try{
    const j=await api('POST','/api/amp',{on:!!on});
    if(!j||j.ok===false){if(sw) sw.checked=!on}
  }finally{if(sw) sw.disabled=false}
  refresh();
}
let volTimer=0;
function onVolSlide(el){
  const v=Math.max(0,Math.min(100,+el.value||0));
  document.getElementById('volV').textContent=v;
  clearTimeout(volTimer);
  volTimer=setTimeout(async()=>{
    await api('POST','/api/amp',{volume:v});
    refresh();
  },120);
}
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
function onLedSlide(id,el){
  const duty=Math.max(0,Math.min(100,+el.value||0));
  document.getElementById('ledV'+id).textContent=duty+'%';
  if(ledTimers[id]) clearTimeout(ledTimers[id]);
  // 占位非 0：debounce 期间 refresh() 不覆盖本地拖动
  ledTimers[id]=setTimeout(()=>{ledTimers[id]=0;setLed(id,duty,true)},40);
}
async function setLed(id,duty,quiet){
  duty=Math.max(0,Math.min(100,+duty||0));
  if(ledTimers[id]){clearTimeout(ledTimers[id]);ledTimers[id]=0}
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
let recOn=false,recTimer=null,recStartedAt=0;
function fmtMs(ms){
  const s=Math.floor(ms/1000),m=Math.floor(s/60),r=s%60;
  return m+':'+String(r).padStart(2,'0')+'.'+String(Math.floor((ms%1000)/100));
}
function setRecUi(on){
  const btn=document.getElementById('btnRec');
  const hint=document.getElementById('recHint');
  btn.className='rec-btn'+(on?' recording':'');
  hint.textContent=on?'录音中…再次点击停止':'点击麦克风开始，再次点击停止（最长约 12 秒）';
  if(recTimer){clearInterval(recTimer);recTimer=null}
  if(on){
    recStartedAt=Date.now();
    document.getElementById('recTimer').textContent='0:00.0';
    recTimer=setInterval(async ()=>{
      document.getElementById('recTimer').textContent=fmtMs(Date.now()-recStartedAt);
      try{
        const r=await fetch('/api/rec');
        const j=await r.json();
        if(j&&j.ok!==false&&!j.recording&&recOn){
          recOn=false;setRecUi(false);
          if(j.ready) showRecClip(j);
        }
      }catch(e){}
    },400);
  }else{
    document.getElementById('recTimer').textContent='';
  }
}
function showRecClip(j){
  const el=document.getElementById('recClip');
  el.hidden=false;
  const sec=((j.ms||0)/1000).toFixed(1);
  document.getElementById('recMeta').textContent=
    `${sec} 秒 · ${j.samples||0} 样点 · 点击经扬声器播放`;
}
async function toggleRec(){
  const btn=document.getElementById('btnRec');
  btn.disabled=true;
  try{
    if(!recOn){
      const j=await api('POST','/api/rec',{on:true});
      if(j&&j.ok!==false){recOn=true;setRecUi(true);document.getElementById('recClip').hidden=true}
    }else{
      const j=await api('POST','/api/rec',{on:false});
      recOn=false;setRecUi(false);
      if(j&&j.ready) showRecClip(j);
      else if(j&&j.ok!==false) alert('未录到有效音频');
    }
  }finally{btn.disabled=false}
}
async function playRec(){
  document.getElementById('spkStatus').textContent='播放录音中…';
  const j=await api('POST','/api/play');
  document.getElementById('spkStatus').textContent=
    (j&&j.ok!==false)?('播放完成 · '+((j.samples||0)/16000).toFixed(1)+'s'):'播放失败';
  refresh();
}
function writeWav(pcm16){
  const dataBytes=pcm16.byteLength;
  const buf=new ArrayBuffer(44+dataBytes);
  const v=new DataView(buf);
  const w=(o,s)=>{for(let i=0;i<s.length;i++)v.setUint8(o+i,s.charCodeAt(i))};
  w(0,'RIFF');v.setUint32(4,36+dataBytes,true);w(8,'WAVE');w(12,'fmt ');
  v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);
  v.setUint32(24,16000,true);v.setUint32(28,32000,true);v.setUint16(32,2,true);
  v.setUint16(34,16,true);w(36,'data');v.setUint32(40,dataBytes,true);
  new Uint8Array(buf,44).set(new Uint8Array(pcm16.buffer,pcm16.byteOffset,pcm16.byteLength));
  return buf;
}
async function fileToWav16k(file){
  const ab=await file.arrayBuffer();
  const AC=window.AudioContext||window.webkitAudioContext;
  const ac=new AC();
  let decoded;
  try{decoded=await ac.decodeAudioData(ab.slice(0))}
  finally{try{await ac.close()}catch(e){}}
  const rate=16000;
  const n=Math.max(1,Math.ceil(decoded.duration*rate));
  const offline=new OfflineAudioContext(1,n,rate);
  const mono=offline.createBuffer(1,decoded.length,decoded.sampleRate);
  const dst=mono.getChannelData(0);
  const nCh=decoded.numberOfChannels;
  for(let i=0;i<decoded.length;i++){
    let s=0;for(let c=0;c<nCh;c++)s+=decoded.getChannelData(c)[i];
    dst[i]=s/nCh;
  }
  const src=offline.createBufferSource();
  src.buffer=mono;src.connect(offline.destination);src.start();
  const rendered=await offline.startRendering();
  const f32=rendered.getChannelData(0);
  const maxSamples=16000*12;
  const take=Math.min(f32.length,maxSamples);
  const pcm=new Int16Array(take);
  for(let i=0;i<take;i++){
    const x=Math.max(-1,Math.min(1,f32[i]));
    pcm[i]=(x<0?x*32768:x*32767)|0;
  }
  return writeWav(pcm);
}
async function uploadPlay(){
  const f=document.getElementById('playFile').files[0];
  const btn=document.getElementById('playBtn');
  const st=document.getElementById('spkStatus');
  if(!f){alert('请先选择音频文件');return}
  btn.disabled=true;st.textContent='转码中…';
  try{
    const wav=await fileToWav16k(f);
    st.textContent='上传播放 '+(wav.byteLength/1024).toFixed(0)+' KB…';
    const r=await fetch('/api/play/upload',{method:'POST',headers:{'Content-Type':'audio/wav'},body:wav});
    const t=await r.text();
    let j;try{j=JSON.parse(t)}catch(e){j={ok:false,raw:t}}
    if(!r.ok||j.ok===false) throw new Error((j&&j.error)||t||('HTTP '+r.status));
    st.textContent='播放完成 · '+((j.samples||0)/16000).toFixed(1)+'s';
    refresh();
  }catch(e){st.textContent='失败: '+e.message;alert('播放失败: '+e.message)}
  finally{btn.disabled=false}
}
async function oled(cmd){await api('POST','/api/oled',{cmd,text:document.getElementById('oledText').value})}
async function refreshOta(){
  const o=await api('GET','/api/ota');
  if(!o)return;
  document.getElementById('otaInfo').textContent=
    `FW ${o.fw}\n运行 ${o.running} (${Math.round((o.runningSize||0)/1048576)}MB)\n`+
    `救援 factory ${Math.round((o.factorySize||0)/1048576)}MB · 主槽 ota_0 ${Math.round((o.ota0Size||0)/1048576)}MB\n`+
    (o.hint||'');
}
async function enterRescue(){
  if(!confirm('将重启进入救援模式，屏幕会显示升级步骤。继续？')) return;
  const log=document.getElementById('otaLog');
  log.textContent='正在进入救援…';
  try{
    const j=await api('POST','/api/rescue',{});
    log.textContent='已请求重启。数秒后打开同一 IP，按屏幕步骤上传大包。';
    if(j&&j.reboot) setTimeout(()=>location.reload(),3000);
  }catch(e){ log.textContent='失败: '+e.message; }
}
async function otaFlash(){ alert('请改用「进入救援升级」'); }
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
const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
function polar(r_mm,a_deg,R){
  const r=Math.min(r_mm/10000,1)*R*0.92;
  const rad=(a_deg-90)*Math.PI/180;
  return [cv.width/2+r*Math.cos(rad), cv.height/2+r*Math.sin(rad)];
}
function drawRadar(s){
  const W=cv.width,H=cv.height,cx=W/2,cy=H/2,R=Math.min(W,H)/2-12;
  ctx.clearRect(0,0,W,H);
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,W,H);
  ctx.beginPath();ctx.moveTo(cx,cy);
  ctx.arc(cx,cy,R,(-60-90)*Math.PI/180,(60-90)*Math.PI/180);ctx.closePath();
  ctx.fillStyle='rgba(88,166,255,0.06)';ctx.fill();
  ctx.strokeStyle='#30363d';ctx.lineWidth=1;
  for(let m=2;m<=10;m+=2){
    const rr=R*0.92*(m/10);
    ctx.beginPath();ctx.arc(cx,cy,rr,(-60-90)*Math.PI/180,(60-90)*Math.PI/180);ctx.stroke();
    ctx.fillStyle='#6e7681';ctx.font='11px sans-serif';ctx.fillText(m+'m',cx+4,cy-rr+12);
  }
  for(const a of[-60,-30,0,30,60]){
    const[x,y]=polar(10000,a,R);
    ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(x,y);ctx.strokeStyle='#21262d';ctx.stroke();
    ctx.fillStyle='#8b949e';ctx.fillText(a+'°',x-8,y-4);
  }
  const trail=s.trail||[];
  for(let i=0;i<trail.length;i++){
    const[x,y]=polar(trail[i].r,trail[i].a,R);
    const a=0.15+0.7*(i/Math.max(1,trail.length-1));
    ctx.beginPath();ctx.arc(x,y,3,0,6.28);ctx.fillStyle=`rgba(61,214,140,${a})`;ctx.fill();
  }
  if(trail.length>1){
    ctx.beginPath();
    for(let i=0;i<trail.length;i++){
      const[x,y]=polar(trail[i].r,trail[i].a,R);
      if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
    }
    ctx.strokeStyle='rgba(61,214,140,0.45)';ctx.lineWidth=2;ctx.stroke();
  }
  const powered=!!(s.power||s.enabled);
  const objs=powered?((s.multiValid&&s.objs&&s.objs.length)?s.objs:(s.primaryValid&&s.range_mm?[{slot:0,range_mm:s.range_mm,angle_deg:s.angle_deg}]:[])):[];
  objs.forEach((o,i)=>{
    const[x,y]=polar(o.range_mm,o.angle_deg,R);
    ctx.beginPath();ctx.arc(x,y,8,0,6.28);
    ctx.fillStyle=i===0?'#3dd68c':'#58a6ff';ctx.fill();
    ctx.strokeStyle='#fff';ctx.lineWidth=1.5;ctx.stroke();
    ctx.fillStyle='#e6edf3';ctx.font='12px sans-serif';
    ctx.fillText('slot '+(o.slot??i)+' '+(o.range_mm/1000).toFixed(2)+'m '+o.angle_deg+'°',x+10,y-6);
  });
  ctx.beginPath();ctx.arc(cx,cy,5,0,6.28);ctx.fillStyle='#f0883e';ctx.fill();
}
function setRadarChips(s){
  const powered=!!(s.power||s.enabled);
  const flags=powered?[
    [s.gpioOut,'GPIO OUT'],[s.present,'活体/存在'],[s.detResult&1,'靠近'],[s.detResult&2,'远离'],
    [s.detResult&4,'运动'],[s.detResult&8,'微动'],[s.detResult&16,'呼吸'],
    [s.gesture&&s.gesture.indexOf('扫')>=0,s.gesture||'手势']
  ]:[[false,'未供电']];
  document.getElementById('rdChips').innerHTML=flags.map(([on,lab])=>
    `<span class="chip ${on?'on':''} ${lab&&String(lab).indexOf('扫')>=0&&on?'hot':''}">${lab}</span>`).join('');
}
function renderRadarLive(s){
  const powered=!!(s.power||s.enabled);
  document.getElementById('rdLink').textContent=s.uart?(s.link?'链路OK':'等待数据'):'UART关';
  document.getElementById('rdLink').className='badge'+(s.uart?(s.link?'':' warn'):' off');
  const pwrEl=document.getElementById('rdPwr');
  if(pwrEl){
    pwrEl.textContent=powered?'供电中':'未供电';
    pwrEl.className='badge'+(powered?'':' off');
  }
  document.getElementById('rdOut').textContent=s.gpioOut?'OUT 高':'OUT 低';
  document.getElementById('rdOut').className='badge'+(s.gpioOut?'':' off');
  document.getElementById('kPresent').innerHTML=!powered?'<span class=warn>未供电</span>':(s.present?'<span class=ok>有</span>':'<span class=bad>无</span>');
  document.getElementById('kRange').textContent=powered&&s.range_mm?(s.range_mm/1000).toFixed(2)+' m':'—';
  document.getElementById('kAngle').textContent=powered&&s.angle_deg!=null?s.angle_deg+'°':'—';
  document.getElementById('kGest').textContent=powered?(s.gesture||'—'):'未供电';
  document.getElementById('detLine').textContent=
    `det=${s.det||'-'} result=0x${(s.detResult||0).toString(16)} type=${s.reportType}  `+
    `置信度 r=${s.rbConf} a=${s.angleConf} frame=${s.frameIdx}  呼吸=${s.br||0} 心率=${s.hr||0}`;
  const body=document.getElementById('objs');
  if(powered&&s.multiValid&&s.objs&&s.objs.length){
    body.innerHTML=s.objs.map(o=>`<tr><td>${o.slot}</td><td>${(o.range_mm/1000).toFixed(2)} m</td><td>${o.angle_deg}°</td><td>${o.velo||0}</td></tr>`).join('');
  }else if(powered&&s.primaryValid&&s.range_mm){
    body.innerHTML=`<tr><td>主目标</td><td>${(s.range_mm/1000).toFixed(2)} m</td><td>${s.angle_deg}°</td><td>${s.velo||0}</td></tr>`;
  }else body.innerHTML='<tr><td colspan="4" style="color:var(--muted)">无目标</td></tr>';
  document.getElementById('moduleInfo').textContent=
    `链路 ${s.link?'正常':'等待'} · RX IO10 / TX IO9 · 版本 ${s.version||'未读'}\n`+
    `查询 ${powered?'自动 5 Hz':'未供电'} · 多目标稳定ID=${!!s.idStable}`;
  document.getElementById('rdMeta').textContent=
    `协议 ${s.protocol||'-'} 波特率 ${s.baud} 帧 ${s.rxFrames} (59=${s.frames59||0}) 字节 ${s.rxBytes}\n`+
    `CRC错 ${s.crcErr} 格式错 ${s.malformedFrames||0} 未知 ${s.unknownFrames||0} 丢弃 ${s.discardedBytes||0}\n`+
    `多目标 声明=${s.declaredObjNum||0} 输出=${s.objNum||0} 截断=${!!s.truncated}\n`+
    `最后帧 ${s.lastFrameHex||'-'}`;
  setRadarChips(s);
  drawRadar(s);
}
async function refreshRadarLive(){
  try{
    const r=await fetch('/api/radar/live');
    const j=await r.json();
    if(!r.ok||j.ok===false) throw new Error(j.error||('HTTP '+r.status));
    renderRadarLive(j);
  }catch(e){
    document.getElementById('rdLink').textContent='API错误';
    document.getElementById('rdMeta').textContent=String(e);
  }
}
async function radarCmd(c){await api('POST','/api/radar',{cmd:c});refreshRadarLive()}
refresh();refreshOta();refreshLogs();refreshRadarLive();
const refreshTimer=setInterval(refresh,2500);
const otaTimer=setInterval(refreshOta,8000);
const logTimer=setInterval(refreshLogs,1000);
const radarLiveTimer=setInterval(refreshRadarLive,400);
</script>
</body>
</html>)HTML";
