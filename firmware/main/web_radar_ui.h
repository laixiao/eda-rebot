#pragma once

// /radar 已并入首页「设备日志」右侧；此页仅跳转锚点，保留旧书签兼容
static const char RADAR_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<meta http-equiv="refresh" content="0;url=/#radarPanel"/>
<title>60G 雷达调试</title>
</head>
<body style="font:14px system-ui;background:#121418;color:#e6edf3;padding:24px">
  <p>雷达调试已整合到首页设备日志右侧。</p>
  <p><a href="/#radarPanel" style="color:#58a6ff">前往首页调试区 →</a></p>
  <script>location.replace('/#radarPanel');</script>
</body>
</html>)HTML";
