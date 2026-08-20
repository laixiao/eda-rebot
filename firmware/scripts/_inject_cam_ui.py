# Inject v5 camera + SPI LCD/touch panels into current FAN web_ui.h
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1] / "main"
repo = root.parents[1]
v5 = subprocess.check_output(
    ["git", "show", "56479b0:firmware/main/web_ui.h"], cwd=repo
).decode("utf-8")
cur = (root / "web_ui.h").read_text(encoding="utf-8")

i0 = v5.find("img.cam{")
end = v5.find("</style>", i0)
if i0 < 0 or end < 0:
    raise SystemExit("CSS not found")
css = v5[i0:end].rstrip() + "\n"

h0 = v5.find("<section>\n  <h2>摄像头")
h1 = v5.find('<section class="log-card">', h0)
if h0 < 0 or h1 < 0:
    raise SystemExit("HTML not found")
html = v5[h0:h1]

j0 = v5.find("function camShow")
j1 = v5.find("async function readTouch", j0)
j2 = v5.find("\nasync function ", j1 + 10)
if j2 < 0:
    j2 = v5.find("\nfunction ", j1 + 10)
if j0 < 0 or j1 < 0 or j2 < 0:
    raise SystemExit("JS not found")
js = v5[j0:j2].rstrip() + "\n"

if "img.cam{" not in cur:
    if "</style>" not in cur:
        raise SystemExit("no </style>")
    cur = cur.replace("</style>", css + "</style>", 1)

if 'id="camImg"' not in cur:
    oled = cur.find("<h2>OLED")
    if oled < 0:
        raise SystemExit("no OLED heading")
    end_oled = cur.find("</section>", oled)
    if end_oled < 0:
        raise SystemExit("no OLED </section>")
    insert_at = end_oled + len("</section>")
    cur = cur[:insert_at] + "\n" + html + cur[insert_at:]

if "async function camOn" not in cur:
    if "</script>" not in cur:
        raise SystemExit("no </script>")
    cur = cur.replace("</script>", js + "</script>", 1)

(root / "web_ui.h").write_text(cur, encoding="utf-8")
text = (root / "web_ui.h").read_text(encoding="utf-8")
for k in ["摄像头 OV5640", "camImg", "camOn", "SPI 屏", "readTouch", "img.cam"]:
    print(f"{k}: {k in text}")
print("size", len(text))
