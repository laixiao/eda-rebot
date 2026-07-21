#pragma once

// 60G 雷达详细可视化调试页（MS60-1211S80M / AT6010 HCI）
static const char RADAR_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>60G 雷达调试</title>
<style>
:root{--bg:#0f1419;--card:#1a222c;--fg:#e7ecf1;--muted:#8b9aab;--acc:#3dd68c;--warn:#e3b341;--bad:#f85149;--line:#2a3441;--blue:#58a6ff;--orange:#f0883e}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 system-ui,Segoe UI,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;gap:10px;flex-wrap:wrap;align-items:center}
header h1{font-size:16px;margin:0;font-weight:600}
a.back{color:var(--blue);text-decoration:none}
.badge{padding:2px 8px;border-radius:999px;background:#238636;font-size:12px}
.badge.off{background:#6e7681}
.badge.warn{background:#9e6a03}
main{padding:12px;display:grid;gap:12px;grid-template-columns:minmax(280px,1.2fr) minmax(260px,1fr)}
@media(max-width:860px){main{grid-template-columns:1fr}}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
h2{margin:0 0 10px;font-size:12px;color:var(--muted);font-weight:600;letter-spacing:.04em;text-transform:uppercase}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
button{font:inherit;background:#21262d;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:8px 12px;cursor:pointer}
button:hover{border-color:#8b949e}
.kpi{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:8px}
.kpi div{background:#0d1117;border:1px solid var(--line);border-radius:8px;padding:10px}
.kpi b{display:block;font-size:20px;font-variant-numeric:tabular-nums;margin-top:2px}
.kpi span{color:var(--muted);font-size:11px}
.chips{display:flex;flex-wrap:wrap;gap:6px}
.chip{padding:4px 10px;border-radius:999px;background:#21262d;border:1px solid var(--line);font-size:12px}
.chip.on{background:#0d3b24;border-color:#238636;color:var(--acc)}
.chip.hot{background:#3b2208;border-color:#f0883e;color:var(--orange)}
canvas{width:100%;max-width:520px;aspect-ratio:1;background:#0d1117;border:1px solid var(--line);border-radius:8px;display:block;margin:0 auto}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{padding:6px 4px;border-bottom:1px solid var(--line);text-align:left}
th{color:var(--muted);font-weight:500}
pre{margin:0;white-space:pre-wrap;word-break:break-all;font:11px/1.4 ui-monospace,Consolas,monospace;color:#c9d1d9;max-height:140px;overflow:auto}
.ok{color:var(--acc)}.bad{color:var(--bad)}.warn{color:var(--warn)}
.full{grid-column:1/-1}
</style>
</head>
<body>
<header>
  <a class="back" href="/">← 调试首页</a>
  <h1>MS60-1211S80M · 60G 雷达</h1>
  <span id="link" class="badge off">UART</span>
  <span id="acqBadge" class="badge off">采集</span>
  <span id="outBadge" class="badge off">OUT</span>
  <button onclick="cmd('version')">刷新模块信息</button>
</header>
<main>
<section>
  <h2>空间定位（俯视 · 雷达在原点）</h2>
  <canvas id="cv" width="520" height="520"></canvas>
  <div class="row" style="justify-content:center;color:var(--muted);font-size:12px">FOV ±60° · 量程 10m · 点=目标 · 线=轨迹</div>
</section>
<section>
  <h2>实时状态</h2>
  <div class="kpi">
    <div><span>存在</span><b id="kPresent">—</b></div>
    <div><span>距离</span><b id="kRange">—</b></div>
    <div><span>角度</span><b id="kAngle">—</b></div>
    <div><span>手势/态势</span><b id="kGest" style="font-size:16px">—</b></div>
  </div>
  <div class="row chips" id="chips" style="margin-top:10px"></div>
  <h2 style="margin-top:14px">检测标志</h2>
  <pre id="detLine">—</pre>
  <h2 style="margin-top:14px">多目标</h2>
  <table>
    <thead><tr><th>帧内 slot</th><th>距离</th><th>角度</th><th>速度</th></tr></thead>
    <tbody id="objs"><tr><td colspan="4" style="color:var(--muted)">等待数据…</td></tr></tbody>
  </table>
  <h2 style="margin-top:14px">模块信息</h2>
  <pre id="moduleInfo">—</pre>
  <details style="margin-top:14px"><summary style="cursor:pointer;color:var(--muted)">诊断信息</summary>
    <pre id="meta" style="margin-top:8px">—</pre>
  </details>
</section>
<section class="full">
  <h2>说明</h2>
  <pre>接线（临时）：雷达 TX→ENC1_A(IO9/ESP RX)，RX→ENC1_B(IO10/ESP TX)，OUT→ENC3_A，VCC/GND→舵机座电源。
UART 固定使用已验证的 115200 8N1：雷达 TX→IO9，雷达 RX→IO10，正常极性；页面无需配置。
固件每 200ms 自动发送一次只读 0x30 检测查询，关闭浏览器也会持续采集。
主页面“雷达采集”总开关只暂停查询和数据解析，不切断雷达 VCC，UART 仍保持就绪。
0x59/0x30 传输、校验及单目标距离/角度已通过实机验证；TYPE=5 多目标仍待完整验收。
多目标 slot 仅为帧内序号，不是稳定目标 ID。</pre>
</section>
</main>
<script>
const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
let last=null;
function polar(r_mm,a_deg,R){
  const r=Math.min(r_mm/10000,1)*R*0.92;
  const rad=(a_deg-90)*Math.PI/180;
  return [cv.width/2+r*Math.cos(rad), cv.height/2+r*Math.sin(rad)];
}
function draw(s){
  const W=cv.width,H=cv.height,cx=W/2,cy=H/2,R=Math.min(W,H)/2-12;
  ctx.clearRect(0,0,W,H);
  ctx.fillStyle='#0d1117';ctx.fillRect(0,0,W,H);
  // FOV wedge ±60°
  ctx.beginPath();
  ctx.moveTo(cx,cy);
  ctx.arc(cx,cy,R,(-60-90)*Math.PI/180,(60-90)*Math.PI/180);
  ctx.closePath();
  ctx.fillStyle='rgba(88,166,255,0.06)';ctx.fill();
  ctx.strokeStyle='#30363d';ctx.lineWidth=1;
  for(let m=2;m<=10;m+=2){
    const rr=R*0.92*(m/10);
    ctx.beginPath();ctx.arc(cx,cy,rr,(-60-90)*Math.PI/180,(60-90)*Math.PI/180);ctx.stroke();
    ctx.fillStyle='#6e7681';ctx.font='11px sans-serif';
    ctx.fillText(m+'m',cx+4,cy-rr+12);
  }
  // angle ticks
  for(const a of[-60,-30,0,30,60]){
    const[x,y]=polar(10000,a,R);
    ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(x,y);ctx.strokeStyle='#21262d';ctx.stroke();
    ctx.fillStyle='#8b949e';ctx.fillText(a+'°',x-8,y-4);
  }
  // trail
  const trail=s.trail||[];
  for(let i=0;i<trail.length;i++){
    const[x,y]=polar(trail[i].r,trail[i].a,R);
    const a=0.15+0.7*(i/Math.max(1,trail.length-1));
    ctx.beginPath();ctx.arc(x,y,3,0,6.28);
    ctx.fillStyle=`rgba(61,214,140,${a})`;ctx.fill();
  }
  if(trail.length>1){
    ctx.beginPath();
    for(let i=0;i<trail.length;i++){
      const[x,y]=polar(trail[i].r,trail[i].a,R);
      if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
    }
    ctx.strokeStyle='rgba(61,214,140,0.45)';ctx.lineWidth=2;ctx.stroke();
  }
  // targets
  const objs=s.enabled?((s.multiValid&&s.objs&&s.objs.length)?s.objs: (s.primaryValid&&s.range_mm?[ {slot:0,range_mm:s.range_mm,angle_deg:s.angle_deg} ]:[])):[];
  objs.forEach((o,i)=>{
    const[x,y]=polar(o.range_mm,o.angle_deg,R);
    ctx.beginPath();ctx.arc(x,y,8,0,6.28);
    ctx.fillStyle=i===0?'#3dd68c':'#58a6ff';ctx.fill();
    ctx.strokeStyle='#fff';ctx.lineWidth=1.5;ctx.stroke();
    ctx.fillStyle='#e6edf3';ctx.font='12px sans-serif';
    ctx.fillText('slot '+(o.slot??i)+' '+(o.range_mm/1000).toFixed(2)+'m '+o.angle_deg+'°',x+10,y-6);
  });
  // radar origin
  ctx.beginPath();ctx.arc(cx,cy,5,0,6.28);ctx.fillStyle='#f0883e';ctx.fill();
}
function setChips(s){
  const flags=s.enabled?[
    [s.gpioOut,'GPIO OUT'],
    [s.present,'活体/存在'],
    [s.detResult&1,'靠近'],
    [s.detResult&2,'远离'],
    [s.detResult&4,'运动'],
    [s.detResult&8,'微动'],
    [s.detResult&16,'呼吸'],
    [s.gesture&&s.gesture.indexOf('扫')>=0,s.gesture||'手势']
  ]:[[false,'采集已关闭']];
  document.getElementById('chips').innerHTML=flags.map(([on,lab])=>
    `<span class="chip ${on?'on':''} ${lab&&String(lab).indexOf('扫')>=0&&on?'hot':''}">${lab}</span>`).join('');
}
function render(s){
  last=s;
  document.getElementById('link').textContent=s.uart?(s.link?'链路OK':'等待数据'):'UART关';
  document.getElementById('link').className='badge'+(s.uart?(s.link?'':' warn'):' off');
  document.getElementById('acqBadge').textContent=s.enabled?'采集中':'采集已关闭';
  document.getElementById('acqBadge').className='badge'+(s.enabled?'':' off');
  document.getElementById('outBadge').textContent=s.gpioOut?'OUT 高':'OUT 低';
  document.getElementById('outBadge').className='badge'+(s.gpioOut?'':' off');
  document.getElementById('kPresent').innerHTML=!s.enabled?'<span class=warn>停用</span>':(s.present?'<span class=ok>有</span>':'<span class=bad>无</span>');
  document.getElementById('kRange').textContent=s.enabled&&s.range_mm?(s.range_mm/1000).toFixed(2)+' m':'—';
  document.getElementById('kAngle').textContent=s.enabled&&s.angle_deg!=null?s.angle_deg+'°':'—';
  document.getElementById('kGest').textContent=s.enabled?(s.gesture||'—'):'采集已关闭';
  document.getElementById('detLine').textContent=
    `det=${s.det||'-'}  result=0x${(s.detResult||0).toString(16)}  type=${s.reportType}\n`+
    `置信度 range=${s.rbConf} angle=${s.angleConf}  frame=${s.frameIdx}\n`+
    `呼吸=${s.br||0}  心率=${s.hr||0}  velo=${s.velo||0}`;
  const body=document.getElementById('objs');
  if(s.enabled&&s.multiValid&&s.objs&&s.objs.length){
    body.innerHTML=s.objs.map(o=>`<tr><td>${o.slot}</td><td>${(o.range_mm/1000).toFixed(2)} m</td><td>${o.angle_deg}°</td><td>${o.velo||0}</td></tr>`).join('');
  }else if(s.enabled&&s.primaryValid&&s.range_mm){
    body.innerHTML=`<tr><td>主目标</td><td>${(s.range_mm/1000).toFixed(2)} m</td><td>${s.angle_deg}°</td><td>${s.velo||0}</td></tr>`;
  }else body.innerHTML='<tr><td colspan="4" style="color:var(--muted)">无目标</td></tr>';
  document.getElementById('moduleInfo').textContent=
    `链路 ${s.link?'正常':'等待回复'} · 115200 8N1 · RX IO9 / TX IO10\n模块版本 ${s.version||'未读取'}\n`+
    `检测查询 ${s.enabled?'自动 5 Hz':'已暂停'} · 多目标稳定ID=${!!s.idStable}`;
  document.getElementById('meta').textContent=
    `协议 ${s.protocol||'unknown'}\n`+
    `波特率 ${s.baud}  帧 ${s.rxFrames} (59=${s.frames59||0}, 5A=${s.frames5A||0})  字节 ${s.rxBytes}\n`+
    `CRC错 ${s.crcErr}  格式错 ${s.malformedFrames||0}  未知帧 ${s.unknownFrames||0}  丢弃 ${s.discardedBytes||0}  溢出 ${s.droppedBytes||0}\n`+
    `多目标 声明=${s.declaredObjNum||0} 输出=${s.objNum||0} 截断=${!!s.truncated}  UART积压=${s.uartBufferedBytes||0}\n`+
    `最后帧 ${s.lastFrameHex||'-'}\n`+
    `引脚 TX=IO${s.pins&&s.pins.tx}(${s.pins&&s.pins.txLevel}) RX=IO${s.pins&&s.pins.rx}(${s.pins&&s.pins.rxLevel}) OUT=${s.pins&&s.pins.out}`;
  setChips(s);
  draw(s);
}
async function api(method,url,body){
  const opt={method,headers:{}};
  if(body!==undefined){opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(body)}
  const r=await fetch(url,opt); const t=await r.text();
  let j; try{j=JSON.parse(t)}catch(e){j={ok:false,raw:t}}
  if(!r.ok||j.ok===false) throw new Error(j.error||('HTTP '+r.status));
  return j;
}
async function refresh(){
  try{render(await api('GET','/api/radar/live'))}
  catch(e){document.getElementById('link').textContent='API错误';document.getElementById('meta').textContent=String(e)}
}
async function cmd(c){
  await api('POST','/api/radar',{cmd:c});
  refresh();
}
refresh();
setInterval(refresh,200);
</script>
</body>
</html>)HTML";
