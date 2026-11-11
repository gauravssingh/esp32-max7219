#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <time.h>
#include <Update.h>

// =========================================================================
// ESP32 + MAX7219 4-in-1 Dot Matrix Display
// Web Interface + REST API + Real-time Web Preview
// =========================================================================

static constexpr char kDefaultSsid[] = "DecoFi";
static constexpr char kDefaultPass[] = "tinkupinku";
static constexpr char kApSsid[]      = "matrix-display";
static constexpr char kApPass[]      = "matrix123";
static constexpr char kMdnsHost[]    = "matrix-display";

static constexpr uint8_t kNumModules = 4; // 4x 8x8 matrix = 32x8 pixels
static constexpr uint8_t kDefaultCsPin  = 5;   // IO5 (D8 on D1 Mini)
static constexpr uint8_t kDefaultClkPin = 18;  // IO18 (D5 on D1 Mini)
static constexpr uint8_t kDefaultDinPin = 23;  // IO23 (D7 on D1 Mini)

// --- Parola Effects Mapping ---
struct EffectMap {
  const char* name;
  textEffect_t effect;
};

static const EffectMap kEffects[] = {
  {"scroll_left",     PA_SCROLL_LEFT},
  {"scroll_right",    PA_SCROLL_RIGHT},
  {"print",           PA_PRINT},
  {"scroll_up",       PA_SCROLL_UP},
  {"scroll_down",     PA_SCROLL_DOWN},
  {"wipe",            PA_WIPE},
  {"wipe_cursor",     PA_WIPE_CURSOR},
  {"slice",           PA_SLICE},
  {"mesh",            PA_MESH},
  {"fade",            PA_FADE},
  {"dissolve",        PA_DISSOLVE},
  {"blinds",          PA_BLINDS},
  {"random",          PA_RANDOM},
  {"opening",         PA_OPENING},
  {"opening_cursor",  PA_OPENING_CURSOR},
  {"closing",         PA_CLOSING},
  {"closing_cursor",  PA_CLOSING_CURSOR},
  {"scan_horiz",      PA_SCAN_HORIZ},
  {"scan_vert",       PA_SCAN_VERT},
  {"grow_up",         PA_GROW_UP},
  {"grow_down",       PA_GROW_DOWN}
};
static const size_t kNumEffects = sizeof(kEffects) / sizeof(kEffects[0]);

// --- State ---
static MD_Parola* g_parola = nullptr;
static SemaphoreHandle_t g_lock = nullptr;
static Preferences g_prefs;
static WebServer g_server(80);

static char g_curText[256] = "HELLO ESP32";
static char g_nextText[256] = "HELLO ESP32";
static bool g_textPending = false;

static String g_effectName = "scroll_left";
static textEffect_t g_effectIn = PA_SCROLL_LEFT;
static textEffect_t g_effectOut = PA_SCROLL_LEFT;
static textPosition_t g_align = PA_LEFT;
static uint16_t g_speed = 35;       // ms per frame
static uint16_t g_pause = 1000;     // pause in ms at end
static uint8_t  g_bright = 7;       // 0 to 15
static bool     g_invert = false;
static bool     g_clockMode = false;
static uint32_t g_revertToClockAt = 0;
static int32_t  g_tzOffset = 19800; // default UTC+5:30 (India) in seconds

static uint8_t g_csPin = kDefaultCsPin;
static uint8_t g_clkPin = kDefaultClkPin;
static uint8_t g_dinPin = kDefaultDinPin;
static uint8_t g_hwType = MD_MAX72XX::FC16_HW; // 3 = FC16
static bool    g_useHwSpi = false;              // default: Software SPI for robust 3.3V/5V tolerance on clone chips

enum TestPattern : uint8_t {
  TEST_OFF = 0,
  TEST_ALL_ON = 1,
  TEST_CHECKER = 2,
  TEST_WALK = 3
};
static TestPattern g_testPattern = TEST_OFF;

// Buffer snapshot for web live preview (32 columns of 8-bit vertical pixels)
static uint8_t g_displayBuffer[32] = {0};

// --- Forward Declarations ---
static void loadSettings();
static void saveSettings();
static void initDisplay();
static void updateDisplayMessage();
static textEffect_t findEffect(const String& name);
static textPosition_t parseAlign(const String& a);
static const char* alignToString(textPosition_t a);
static void handleTest();
static void handleConfig();
static void rawSend16(uint8_t reg, uint8_t data);
static void rawInitMax7219();
static void setTestPattern(TestPattern p);
static void checkSerialCommands();

// --- Web UI HTML/CSS/JS (embedded PROGMEM) ---
static const char kIndexHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MAX7219 Dot Matrix Display</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#e6edf3;--dim:#8b949e;--led-red:#ff3b30;--accent:#58a6ff}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:12px;background:var(--bg);color:var(--text);font:14px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;max-width:760px;margin-left:auto;margin-right:auto}
.top-header{text-align:center;margin:8px 0 16px}
.top-header h1{margin:0 0 8px;font-size:20px;font-weight:700;letter-spacing:-.3px}
.status-bar{display:flex;justify-content:center;align-items:center;flex-wrap:wrap;gap:6px}
.status-chip{display:inline-flex;align-items:center;gap:5px;background:#161b22;border:1px solid var(--border);border-radius:16px;padding:4px 10px;font-size:11px;color:var(--dim);text-decoration:none;font-variant-numeric:tabular-nums;transition:all .15s}
.status-chip:hover{border-color:var(--dim);color:var(--text)}
.status-chip.ip-chip{background:rgba(46,160,67,0.12);border-color:rgba(46,160,67,0.38);color:#3fb950;font-weight:600}
.pulse-dot{width:7px;height:7px;border-radius:50%;background:#3fb950;box-shadow:0 0 6px #3fb950;display:inline-block;animation:pulse 2s infinite}
@keyframes pulse{0%{transform:scale(.9);opacity:.8}50%{transform:scale(1.2);opacity:1}100%{transform:scale(.9);opacity:.8}}
.chip-icon{font-size:11px;opacity:.85}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;margin-bottom:12px}
.card h2{margin:0 0 12px;font-size:12px;text-transform:uppercase;letter-spacing:.5px;color:var(--dim);display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px}
.mode-toggles{display:inline-flex;background:#0d1117;border:1px solid var(--border);border-radius:20px;padding:2px;gap:2px}
.mode-btn{background:transparent;border:none;border-radius:14px;padding:3px 9px;font-size:11px;font-weight:600;color:var(--dim);cursor:pointer;transition:all .15s}
.mode-btn:hover{color:var(--text)}
.mode-btn.active{background:#238636;color:#fff;box-shadow:0 0 6px rgba(35,134,54,0.4)}
.mode-btn.sim-active{background:var(--accent);color:#05070a;box-shadow:0 0 6px rgba(88,166,255,0.4)}
.sync-badge{font-size:10px;padding:3px 8px;border-radius:12px;background:#238636;color:#fff;font-weight:600;text-transform:none;white-space:nowrap;display:inline-flex;align-items:center;gap:4px}

.preview-wrap{display:flex;justify-content:center;align-items:center;background:#05070a;border:1px solid #21262d;border-radius:8px;padding:8px 4px;overflow:hidden}
#matrixCanvas{border-radius:4px;background:#000;max-width:100%;height:auto;display:block}

.preview-bar{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px;margin-top:10px}
.color-picker{display:flex;gap:8px;align-items:center}
.dot-btn{width:26px;height:26px;border-radius:50%;border:2px solid transparent;cursor:pointer}
.dot-btn.sel{border-color:#fff;box-shadow:0 0 8px #fff}

.toggle-group{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.toggle-chip{display:inline-flex;align-items:center;gap:5px;background:#1b212c;border:1px solid var(--border);border-radius:20px;padding:6px 12px;font-size:12px;font-weight:600;color:var(--dim);cursor:pointer;transition:all .15s;user-select:none}
.toggle-chip:hover{background:#283141;color:var(--text)}
.toggle-chip:active{transform:scale(0.96)}
.toggle-chip.active{background:rgba(88,166,255,0.18);border-color:var(--accent);color:var(--accent);box-shadow:0 0 10px rgba(88,166,255,0.35)}

.msg-row{display:flex;gap:8px;margin-bottom:12px}
input[type=text]{flex:1;background:#21262d;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:10px 12px;font:inherit;font-size:14px;min-width:0}
button{background:#21262d;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:10px 14px;cursor:pointer;font:inherit;font-weight:600;font-size:13px;transition:all .15s}
button:active{transform:scale(0.97)}
button.primary{background:#238636;border-color:#2ea043;color:#fff;white-space:nowrap}
button.danger{background:#da3633;border-color:#f85149;color:#fff}

.ctrl-grid{display:grid;grid-template-columns:1fr 1fr auto;gap:10px;align-items:center;margin-bottom:12px}
.ctrl-item{display:flex;flex-direction:column;gap:4px}
.ctrl-item label{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px}
select{background:#21262d;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:8px 10px;font:inherit;font-size:13px;width:100%}
.invert-item{display:flex;align-items:center;gap:6px;height:100%;padding-top:16px;font-size:13px;color:var(--text);cursor:pointer}

.sliders-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:12px}
.slider-card{background:#11151c;border:1px solid var(--border);border-radius:6px;padding:8px 10px}
.slider-head{display:flex;justify-content:space-between;align-items:center;font-size:11px;color:var(--dim);margin-bottom:6px}
.slider-card input[type=range]{width:100%;margin:0;accent-color:var(--accent)}

.presets{display:grid;grid-template-columns:repeat(auto-fill,minmax(105px,1fr));gap:6px;margin-top:10px}
.presets button{padding:8px 6px;font-size:12px;background:#1b212c;text-align:center}

.pin-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.pin-box{background:#11151c;border:1px solid var(--border);border-radius:6px;padding:10px;font-size:12px}
.pin-box b{color:var(--accent)}
pre{background:#0a0c10;border:1px solid var(--border);border-radius:6px;padding:10px;font-size:11px;overflow-x:auto;margin:0}

@media(max-width:600px){
  body{padding:8px}
  .card{padding:12px;margin-bottom:10px}
  .msg-row{flex-direction:column}
  .msg-row button{width:100%;padding:12px}
  .ctrl-grid{grid-template-columns:1fr 1fr}
  .invert-item{grid-column:span 2;padding-top:4px}
  .sliders-grid{grid-template-columns:1fr;gap:8px}
  .preview-bar{flex-direction:column;align-items:stretch;gap:10px}
  .color-picker{justify-content:space-between}
  .toggle-group{justify-content:space-between}
  .toggle-chip{flex:1;justify-content:center;padding:8px 10px}
  .dot-btn{width:30px;height:30px}
  .pin-grid{grid-template-columns:1fr}
  .presets{grid-template-columns:1fr 1fr;gap:8px}
  .presets button{padding:10px 8px}
}
</style>
</head>
<body>
<div class="top-header">
  <h1>MAX7219 Dot Matrix Display</h1>
  <div class="status-bar" id="statusBar">
    <a href="http://%IP%/" target="_blank" class="status-chip ip-chip" id="ipChip" title="Device IP (Click to Open)">
      <span class="pulse-dot"></span>
      <span id="ipVal">%IP%</span>
    </a>
    <div class="status-chip" title="Wi-Fi Signal Strength">
      <span class="chip-icon">📶</span>
      <span id="rssiVal">-- dBm</span>
    </div>
    <div class="status-chip" title="Free Heap Memory">
      <span class="chip-icon">⚡</span>
      <span id="heapVal">-- KB</span>
    </div>
    <div class="status-chip" title="Device Uptime">
      <span class="chip-icon">⏱</span>
      <span id="uptimeVal">--</span>
    </div>
  </div>
</div>

<!-- Display Preview Card -->
<div class="card">
  <h2>
    <span>Display Preview (32 &times; 8)</span>
    <div class="mode-toggles">
      <button type="button" class="mode-btn active" id="modeLiveBtn" onclick="setPreviewMode('live')">● Live Sync</button>
      <button type="button" class="mode-btn" id="modeSimBtn" onclick="setPreviewMode('sim')">⚡ JS Preview</button>
    </div>
  </h2>
  <div class="preview-wrap">
    <canvas id="matrixCanvas" width="672" height="176"></canvas>
  </div>
  <div class="preview-bar">
    <div class="color-picker">
      <span style="font-size:12px;color:var(--dim)">Color:</span>
      <div class="dot-btn sel" style="background:#ff3b30" onclick="setColor('#ff3b30','rgba(255,59,48,0.7)')" title="Red"></div>
      <div class="dot-btn" style="background:#30d158" onclick="setColor('#30d158','rgba(48,209,88,0.7)')" title="Green"></div>
      <div class="dot-btn" style="background:#0a84ff" onclick="setColor('#0a84ff','rgba(10,132,255,0.7)')" title="Blue"></div>
      <div class="dot-btn" style="background:#ffd60a" onclick="setColor('#ffd60a','rgba(255,214,10,0.7)')" title="Amber"></div>
      <div class="dot-btn" style="background:#ffffff" onclick="setColor('#ffffff','rgba(255,255,255,0.7)')" title="White"></div>
    </div>
    <div class="toggle-group">
      <button type="button" class="toggle-chip" id="flipHBtn" onclick="toggleFlip('H')">⇄ Flip H</button>
      <button type="button" class="toggle-chip" id="flipVBtn" onclick="toggleFlip('V')">⇅ Flip V</button>
      <span id="fpsCounter" style="font-size:11px;color:var(--dim)">30 FPS</span>
    </div>
  </div>
</div>

<!-- Message Control Card -->
<div class="card">
  <h2>Message &amp; Animation Controls</h2>
  <div class="msg-row">
    <input type="text" id="msgInput" maxlength="250" placeholder="Type message to display..." value="HELLO ESP32">
    <button class="primary" onclick="sendMessage()">Send to Display</button>
  </div>
  <div class="ctrl-grid">
    <div class="ctrl-item">
      <label>Effect</label>
      <select id="effectSel">
        <option value="scroll_left">Scroll Left</option>
        <option value="scroll_right">Scroll Right</option>
        <option value="print">Static Print</option>
        <option value="scroll_up">Scroll Up</option>
        <option value="scroll_down">Scroll Down</option>
        <option value="wipe">Wipe</option>
        <option value="wipe_cursor">Wipe Cursor</option>
        <option value="slice">Slice</option>
        <option value="mesh">Mesh</option>
        <option value="fade">Fade</option>
        <option value="dissolve">Dissolve</option>
        <option value="blinds">Blinds</option>
        <option value="random">Random</option>
        <option value="opening">Opening</option>
        <option value="closing">Closing</option>
        <option value="scan_horiz">Scan Horizontal</option>
        <option value="scan_vert">Scan Vertical</option>
        <option value="grow_up">Grow Up</option>
        <option value="grow_down">Grow Down</option>
      </select>
    </div>
    <div class="ctrl-item">
      <label>Align</label>
      <select id="alignSel">
        <option value="left">Left</option>
        <option value="center">Center</option>
        <option value="right">Right</option>
      </select>
    </div>
    <label class="invert-item"><input type="checkbox" id="invertChk"> Invert LEDs</label>
  </div>
  <div class="sliders-grid">
    <div class="slider-card">
      <div class="slider-head"><span>Speed</span><b id="speedVal">35 ms</b></div>
      <input type="range" id="speedRng" min="10" max="150" value="35">
    </div>
    <div class="slider-card">
      <div class="slider-head"><span>Pause</span><b id="pauseVal">1.0 s</b></div>
      <input type="range" id="pauseRng" min="0" max="5000" step="100" value="1000">
    </div>
    <div class="slider-card">
      <div class="slider-head"><span>Brightness</span><b id="brightVal">7/15</b></div>
      <input type="range" id="brightRng" min="0" max="15" value="7">
    </div>
  </div>
  
  <div style="margin-top:12px">
    <label style="color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.5px">Quick Presets:</label>
    <div class="presets">
      <button onclick="setPreset('WELCOME!')">Welcome</button>
      <button onclick="setPreset('ANTIGRAVITY AI')">Antigravity</button>
      <button onclick="toggleClock()">Toggle Clock</button>
      <button onclick="setPreset('ALERT! ⚠️')">Alert</button>
      <button onclick="setPreset('DecoFi Wi-Fi OK')">Wi-Fi Status</button>
      <button class="danger" onclick="clearDisplay()">Clear Screen</button>
    </div>
  </div>
</div>

<!-- Hardware Diagnostic & Self-Test Card -->
<div class="card">
  <h2>Hardware Diagnostics &amp; Signal Test</h2>
  <div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:12px">
    <button type="button" class="test-btn" id="btnTestAll" onclick="runTestPattern('all')" style="background:#e3b341;color:#000;font-weight:700">💡 Force All LEDs ON</button>
    <button type="button" class="test-btn" id="btnTestCheck" onclick="runTestPattern('checker')" style="background:#21262d;border:1px solid #30363d">🏁 Checkerboard</button>
    <button type="button" class="test-btn" id="btnTestWalk" onclick="runTestPattern('walk')" style="background:#21262d;border:1px solid #30363d">🚶 Walking Dot</button>
    <button type="button" class="test-btn" id="btnTestOff" onclick="runTestPattern('off')" style="background:#238636;color:#fff">▶️ Normal Text Mode</button>
    <div style="display:inline-flex;gap:6px;align-items:center;font-size:12px;color:var(--dim);margin-left:auto">
      <span>SPI Mode:</span>
      <select id="spiModeSel" onchange="changeSpiMode(this.value)" style="width:auto;padding:6px 10px">
        <option value="sw">Software Bit-Bang (Safe ~500kHz)</option>
        <option value="hw">Hardware VSPI (8MHz)</option>
      </select>
    </div>
  </div>
  <div style="background:#0d1117;border:1px solid #30363d;border-radius:6px;padding:10px;font-size:12px;color:#c9d1d9;line-height:1.6">
    <b>Quick Diagnostic Steps:</b><br>
    • Click <b>"Force All LEDs ON"</b>: Sends raw MAX7219 test command (0x0F) to force all 256 LEDs lit. If LEDs stay dark, check power &amp; DIN wire.<br>
    • Click <b>"Checkerboard"</b>: Tests alternating row &amp; column registers (0xAA / 0x55).<br>
    • Click <b>"Normal Text Mode"</b>: Restores standard web message animations.
  </div>
</div>

<!-- Hardware Wiring Pinout Card -->
<div class="card">
  <h2>Hardware Wiring &amp; Power Configuration</h2>
  <div class="pin-grid">
    <div class="pin-box">
      <b>38-Pin ESP32-WROOM-32 Pinout (Right Side)</b><br>
      • <b>DIN</b> &rarr; <b>GPIO 23 (D23)</b> &nbsp;<span style="color:#58a6ff">&larr; MOSI / Data In</span><br>
      • <b>CS</b>  &rarr; <b>GPIO 5  (D5)</b>  &nbsp;&nbsp;&nbsp;<span style="color:#58a6ff">&larr; SS / Chip Select</span><br>
      • <b>CLK</b> &rarr; <b>GPIO 18 (D18)</b> &nbsp;<span style="color:#58a6ff">&larr; SCK / Clock</span><br>
      • <b>GND</b> &rarr; <b>GND</b> pin (Ground)<br>
      • <b>VCC</b> &rarr; <b>VIN / 5V</b> pin (5V USB rail)<br>
      <span style="font-size:11px;color:var(--dim)">D1 Mini aliases: DIN=D7, CS=D8, CLK=D5</span>
    </div>
    <div class="pin-box">
      <b>Module Architecture &amp; Tips</b><br>
      • <b>INPUT Side Only</b>: Connect to <b>DIN</b> end of 4-in-1 board. DOUT is only for cascading.<br>
      • <b>Power</b>: MAX7219 requires <b>5V (VIN)</b>. Avoid 3.3V pin.<br>
      • <b>Signal Level</b>: Software Bit-Bang mode gives clean 3.3V&rarr;5V timing for clone chips.<br>
      • <b>Current</b>: ~200 mA typical scrolling text, ~800 mA peak (all LEDs on).
    </div>
  </div>
</div>

<!-- REST API Documentation -->
<div class="card">
  <h2>REST API Examples</h2>
  <pre># Send a message via curl
curl -X POST -d "text=Hello+World&speed=30&effect=scroll_left" http://%IP%/api/message

# Set brightness (0-15)
curl -X POST -d "brightness=10" http://%IP%/api/brightness

# Read live 32x8 buffer
curl -s http://%IP%/api/status | jq .</pre>
</div>

<script>
let curColor = '#ff3b30';
let curGlow = 'rgba(255,59,48,0.7)';
let lastSync = 0;
let isClock = false;

const canvas = document.getElementById('matrixCanvas');
const ctx = canvas.getContext('2d');

function setColor(c, g){
  curColor = c;
  curGlow = g;
  document.querySelectorAll('.dot-btn').forEach(b => b.classList.toggle('sel', b.style.backgroundColor === c));
}

let lastBuffer = [];

// Draw 32 columns x 8 rows of LEDs
function drawMatrix(buffer) {
  if (buffer) lastBuffer = buffer;
  const cols = 32, rows = 8;
  const pad = 12;
  const dotR = 7;
  const pitchX = 20;
  const pitchY = 19;
  const modGap = 8;

  const flipH = flipHState;
  const flipV = flipVState;

  ctx.fillStyle = '#06080c';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  for (let m = 0; m < 4; m++) {
    const mx = pad + m * (8 * pitchX + modGap) - 4;
    ctx.strokeStyle = '#1b212c';
    ctx.lineWidth = 2;
    ctx.strokeRect(mx, pad - 4, 8 * pitchX + 2, 8 * pitchY + 4);
  }

  for (let x = 0; x < cols; x++) {
    const modIdx = Math.floor(x / 8);
    const cx = pad + x * pitchX + modIdx * modGap + dotR;
    const colIdx = flipH ? (cols - 1 - x) : x;
    const colByte = (lastBuffer && lastBuffer[colIdx] !== undefined) ? lastBuffer[colIdx] : 0;

    for (let y = 0; y < rows; y++) {
      const cy = pad + y * pitchY + dotR;
      const bitIdx = flipV ? (7 - y) : y;
      const on = (colByte & (1 << bitIdx)) !== 0;

      ctx.beginPath();
      ctx.arc(cx, cy, dotR, 0, Math.PI * 2);

      if (on) {
        ctx.fillStyle = curColor;
        ctx.shadowColor = curGlow;
        ctx.shadowBlur = 10;
        ctx.fill();
        ctx.shadowBlur = 0;
        // Inner highlight
        ctx.beginPath();
        ctx.arc(cx - 2, cy - 2, dotR * 0.35, 0, Math.PI * 2);
        ctx.fillStyle = 'rgba(255,255,255,0.6)';
        ctx.fill();
      } else {
        ctx.fillStyle = '#141820';
        ctx.fill();
        ctx.strokeStyle = '#0d1117';
        ctx.lineWidth = 1;
        ctx.stroke();
      }
    }
  }
}

async function fetchStatus() {
  try {
    const res = await fetch('/api/status', {cache:'no-store'});
    if (!res.ok) return;
    const d = await res.json();

    const ipVal = document.getElementById('ipVal');
    const ipChip = document.getElementById('ipChip');
    if (ipVal) ipVal.textContent = d.ip;
    if (ipChip) ipChip.href = 'http://' + d.ip + '/';
    const rssiVal = document.getElementById('rssiVal');
    if (rssiVal) rssiVal.textContent = d.rssi + ' dBm';
    const heapVal = document.getElementById('heapVal');
    if (heapVal) heapVal.textContent = (d.heap / 1024).toFixed(0) + ' KB';
    const uptimeVal = document.getElementById('uptimeVal');
    if (uptimeVal) {
      const s = Math.floor(d.uptime / 1000);
      const m = Math.floor(s / 60);
      const h = Math.floor(m / 60);
      uptimeVal.textContent = h > 0 ? `${h}h ${m%60}m` : (m > 0 ? `${m}m ${s%60}s` : `${s}s`);
    }

    isClock = d.clockMode;
    if (document.activeElement !== document.getElementById('msgInput')) {
      if (!isClock) document.getElementById('msgInput').value = d.text;
    }
    document.getElementById('speedRng').value = d.speed;
    document.getElementById('speedVal').textContent = d.speed + ' ms';
    document.getElementById('pauseRng').value = d.pause;
    document.getElementById('pauseVal').textContent = (d.pause/1000).toFixed(1) + ' s';
    document.getElementById('brightRng').value = d.brightness;
    document.getElementById('brightVal').textContent = d.brightness + '/15';
    document.getElementById('effectSel').value = d.effect;
    document.getElementById('alignSel').value = d.align;
    document.getElementById('invertChk').checked = d.invert;
    if (d.spiMode && document.getElementById('spiModeSel')) {
      document.getElementById('spiModeSel').value = d.spiMode;
    }
    if (d.testPattern !== undefined) {
      currentTestPattern = d.testPattern;
      updateTestButtonsUI();
    }

    if (previewMode === 'live' && d.dots && d.dots.length === 32) {
      drawMatrix(d.dots);
    }
    lastSync = Date.now();
  } catch (e) {
    if (document.getElementById('modeLiveBtn') && previewMode === 'live') {
      document.getElementById('modeLiveBtn').textContent = 'OFFLINE';
      document.getElementById('modeLiveBtn').style.background = '#da3633';
    }
  }
}

async function sendMessage() {
  const body = new URLSearchParams({
    text: document.getElementById('msgInput').value,
    effect: document.getElementById('effectSel').value,
    align: document.getElementById('alignSel').value,
    speed: document.getElementById('speedRng').value,
    pause: document.getElementById('pauseRng').value,
    brightness: document.getElementById('brightRng').value,
    invert: document.getElementById('invertChk').checked ? '1' : '0'
  });
  await fetch('/api/message', {method:'POST', body});
  fetchStatus();
}

function setPreset(t) {
  document.getElementById('msgInput').value = t;
  sendMessage();
}

async function toggleClock() {
  await fetch('/api/message', {
    method: 'POST',
    body: new URLSearchParams({clock: isClock ? '0' : '1'})
  });
  fetchStatus();
}

async function clearDisplay() {
  await fetch('/api/clear', {method:'POST'});
  fetchStatus();
}

let currentTestPattern = 0;
async function runTestPattern(p) {
  const res = await fetch('/api/test', {
    method: 'POST',
    body: new URLSearchParams({pattern: p})
  });
  const d = await res.json();
  currentTestPattern = d.testPattern !== undefined ? d.testPattern : (p === 'off' ? 0 : 1);
  updateTestButtonsUI();
  fetchStatus();
}

function updateTestButtonsUI() {
  const bAll = document.getElementById('btnTestAll');
  const bChk = document.getElementById('btnTestCheck');
  const bWlk = document.getElementById('btnTestWalk');
  const bOff = document.getElementById('btnTestOff');
  if (bAll) bAll.style.outline = (currentTestPattern === 1) ? '2px solid #fff' : 'none';
  if (bChk) bChk.style.outline = (currentTestPattern === 2) ? '2px solid #58a6ff' : 'none';
  if (bWlk) bWlk.style.outline = (currentTestPattern === 3) ? '2px solid #58a6ff' : 'none';
  if (bOff) bOff.style.outline = (currentTestPattern === 0) ? '2px solid #2ea043' : 'none';
}

async function changeSpiMode(m) {
  await fetch('/api/config', {method:'POST', body: new URLSearchParams({spi: m})});
  fetchStatus();
}

// Live range updates
document.getElementById('speedRng').oninput = e => {
  document.getElementById('speedVal').textContent = e.target.value + ' ms';
};
document.getElementById('speedRng').onchange = sendMessage;

document.getElementById('pauseRng').oninput = e => {
  document.getElementById('pauseVal').textContent = (e.target.value/1000).toFixed(1) + ' s';
};
document.getElementById('pauseRng').onchange = sendMessage;

document.getElementById('brightRng').oninput = e => {
  document.getElementById('brightVal').textContent = e.target.value + '/15';
};
document.getElementById('brightRng').onchange = e => {
  fetch('/api/brightness', {method:'POST', body: new URLSearchParams({brightness: e.target.value})});
};

document.getElementById('effectSel').onchange = sendMessage;
document.getElementById('alignSel').onchange = sendMessage;
document.getElementById('invertChk').onchange = sendMessage;
document.getElementById('msgInput').onkeydown = e => { if (e.key === 'Enter') sendMessage(); };
document.getElementById('msgInput').oninput = () => { if (previewMode === 'sim') { simOffset = 0; simPauseUntil = 0; } };

let flipHState = false;
let flipVState = false;

function toggleFlip(axis) {
  if (axis === 'H') flipHState = !flipHState;
  if (axis === 'V') flipVState = !flipVState;
  updateFlipUI();
  localStorage.setItem('flipH', flipHState ? '1' : '0');
  localStorage.setItem('flipV', flipVState ? '1' : '0');
  drawMatrix();
}

function updateFlipUI() {
  const hBtn = document.getElementById('flipHBtn');
  const vBtn = document.getElementById('flipVBtn');
  if (hBtn) hBtn.classList.toggle('active', flipHState);
  if (vBtn) vBtn.classList.toggle('active', flipVState);
}

if (localStorage.getItem('flipH') === '1' || localStorage.getItem('flipH') === 'true') {
  flipHState = true;
}
if (localStorage.getItem('flipV') === '1' || localStorage.getItem('flipV') === 'true') {
  flipVState = true;
}
updateFlipUI();

// --- Client-side 5x7 Dot Matrix JS Simulator ---
let previewMode = 'live';
let simOffset = 0;
let simLastTick = 0;
let simPauseUntil = 0;

const FONT_DATA = {};
(function() {
  const b64 = "AwAAAAFfAwcABwUUfxR/FAVESv9KMgVjEwhkYwU2SUk2SAEHAz5BQQNBQT4FCCocKggFCAg+CAgCYOAECAgICAJgYAVgEAgEAwU+UUlFPgMEAn8FcUlJSUYFQUlJSTYFDwgICH8FT0lJSTEFPklJSTAFAwEBAX8FNklJSTYFBklJST4CbGwCbOwDCBQiBBQUFBQDIhQIBQFZCQkGBT5BXVlOBX4JCQl+BX9JSUk2BT5BQUFBBX9BQUE+BX9JSUlBBX8JCQkBBT5BQUl5BX8ICAh/A0F/QQUwQUFBPwV/CBQiQQV/QEBAQAV/AgwCfwV/BAgQfwU+QUFBPgV/CQkJBgU+QUFhfgV/CRkpRgVGSUlJMQUBAX8BAQU/QEBAPwUfIEAgHwU/QDhAPwVjFAgUYwUDBHgEAwVhUUlFQwN/QUEFAwQIEGADQUF/BQQCAQIEBICAgIADAQIEBDhERHwEf0REOAQ4REREBDhERH8EOFRUWAQEfgUBBBikpHwEfwQEeAF9A4SFfAR/EChEAX8FfAR4BHgEfAQEeAQ4REQ4BPwkJBgEGCQk/AR8BAQIBFhUVDQDBH8EBDxAQHwEHCBAfAU8QDBAPARsEBBsBBygoHwEZFRUTAQINkFBAX8EQUE2CAQCAQIB";
  const bin = atob(b64);
  let p = 0;
  for (let c = 32; c <= 126; c++) {
    const w = bin.charCodeAt(p++);
    const col = [];
    for (let i = 0; i < w; i++) col.push(bin.charCodeAt(p++));
    FONT_DATA[c] = col;
  }
})();

function getColumnsForText(str) {
  const cols = [];
  for (let i = 0; i < str.length; i++) {
    const code = str.charCodeAt(i);
    const glyph = FONT_DATA[code] || [0,0,0];
    for (let b of glyph) cols.push(b);
    cols.push(0);
  }
  return cols;
}

function runSimulator() {
  if (previewMode !== 'sim') return;

  const now = Date.now();
  const speed = parseInt(document.getElementById('speedRng').value) || 35;
  const pause = parseInt(document.getElementById('pauseRng').value) || 1000;
  const effect = document.getElementById('effectSel').value;
  const align = document.getElementById('alignSel').value;
  const msg = document.getElementById('msgInput').value || ' ';

  if (now >= simPauseUntil && (now - simLastTick >= speed)) {
    simLastTick = now;
    let buf = new Array(32).fill(0);

    if (isClock) {
      const t = new Date().toTimeString().slice(0, 8);
      const cols = getColumnsForText(t);
      const start = Math.max(0, Math.floor((32 - cols.length) / 2));
      for (let i = 0; i < cols.length && (start + i) < 32; i++) {
        buf[start + i] = cols[i];
      }
    } else {
      const cols = getColumnsForText(msg);
      const totalLen = cols.length;

      if (effect === 'print') {
        let start = 0;
        if (align === 'center') start = Math.max(0, Math.floor((32 - totalLen) / 2));
        else if (align === 'right') start = Math.max(0, 32 - totalLen);
        for (let i = 0; i < totalLen && (start + i) < 32; i++) {
          buf[start + i] = cols[i];
        }
      } else if (effect === 'scroll_right') {
        simOffset--;
        if (simOffset < -totalLen) {
          simOffset = 32;
          simPauseUntil = now + pause;
        }
        for (let c = 0; c < 32; c++) {
          const idx = c - simOffset;
          buf[c] = (idx >= 0 && idx < totalLen) ? cols[idx] : 0;
        }
      } else {
        simOffset++;
        if (simOffset > totalLen + 32) {
          simOffset = 0;
          simPauseUntil = now + pause;
        }
        for (let c = 0; c < 32; c++) {
          const idx = (simOffset - (31 - c));
          buf[c] = (idx >= 0 && idx < totalLen) ? cols[idx] : 0;
        }
      }
    }
    drawMatrix(buf);
  }

  requestAnimationFrame(runSimulator);
}

function setPreviewMode(mode) {
  previewMode = mode;
  localStorage.setItem('previewMode', mode);
  const liveBtn = document.getElementById('modeLiveBtn');
  const simBtn = document.getElementById('modeSimBtn');
  if (liveBtn) liveBtn.classList.toggle('active', mode === 'live');
  if (simBtn) simBtn.classList.toggle('sim-active', mode === 'sim');

  if (mode === 'sim') {
    simOffset = 0;
    simLastTick = 0;
    simPauseUntil = 0;
    requestAnimationFrame(runSimulator);
  } else {
    fetchStatus();
  }
}

const savedMode = localStorage.getItem('previewMode') || 'live';
setPreviewMode(savedMode);

drawMatrix(new Array(32).fill(0));
fetchStatus();
setInterval(fetchStatus, 250); // 4Hz poll for real-time live preview
</script>
</body>
</html>
)HTML";

// =========================================================================
// Implementation
// =========================================================================

static textEffect_t findEffect(const String& name) {
  for (size_t i = 0; i < kNumEffects; i++) {
    if (name.equalsIgnoreCase(kEffects[i].name)) return kEffects[i].effect;
  }
  return PA_SCROLL_LEFT;
}

static textPosition_t parseAlign(const String& a) {
  if (a.equalsIgnoreCase("right")) return PA_RIGHT;
  if (a.equalsIgnoreCase("center")) return PA_CENTER;
  return PA_LEFT;
}

static const char* alignToString(textPosition_t a) {
  if (a == PA_RIGHT) return "right";
  if (a == PA_CENTER) return "center";
  return "left";
}

static void loadSettings() {
  g_prefs.begin("matrix", true);
  String t = g_prefs.getString("text", "HELLO ESP32");
  snprintf(g_curText, sizeof(g_curText), "%s", t.c_str());
  snprintf(g_nextText, sizeof(g_nextText), "%s", t.c_str());

  g_speed = g_prefs.getUShort("speed", 35);
  g_pause = g_prefs.getUShort("pause", 1000);
  g_bright = g_prefs.getUChar("bright", 7);
  g_invert = g_prefs.getBool("invert", false);
  g_effectName = g_prefs.getString("effect", "scroll_left");
  g_effectIn = findEffect(g_effectName);
  g_effectOut = g_effectIn;
  g_align = (textPosition_t)g_prefs.getUChar("align", (uint8_t)PA_LEFT);
  g_clockMode = g_prefs.getBool("clock", false);
  g_tzOffset = g_prefs.getInt("tz", 19800);
  g_csPin = g_prefs.getUChar("cs", kDefaultCsPin);
  g_clkPin = g_prefs.getUChar("clk", kDefaultClkPin);
  g_dinPin = g_prefs.getUChar("din", kDefaultDinPin);
  g_hwType = g_prefs.getUChar("hw", MD_MAX72XX::FC16_HW);
  g_useHwSpi = g_prefs.getBool("hwSpi", false);
  g_prefs.end();
}

static void saveSettings() {
  g_prefs.begin("matrix", false);
  g_prefs.putString("text", g_curText);
  g_prefs.putUShort("speed", g_speed);
  g_prefs.putUShort("pause", g_pause);
  g_prefs.putUChar("bright", g_bright);
  g_prefs.putBool("invert", g_invert);
  g_prefs.putString("effect", g_effectName);
  g_prefs.putUChar("align", (uint8_t)g_align);
  g_prefs.putBool("clock", g_clockMode);
  g_prefs.putInt("tz", g_tzOffset);
  g_prefs.putUChar("cs", g_csPin);
  g_prefs.putUChar("clk", g_clkPin);
  g_prefs.putUChar("din", g_dinPin);
  g_prefs.putUChar("hw", g_hwType);
  g_prefs.putBool("hwSpi", g_useHwSpi);
  g_prefs.end();
}

// Raw low-level 16-bit command sender to all 4 MAX7219 modules via direct GPIO bit-banging
static void rawSend16(uint8_t reg, uint8_t data) {
  pinMode(g_csPin, OUTPUT);
  pinMode(g_clkPin, OUTPUT);
  pinMode(g_dinPin, OUTPUT);

  digitalWrite(g_csPin, LOW);
  delayMicroseconds(3);
  for (uint8_t dev = 0; dev < kNumModules; dev++) {
    uint16_t word = ((uint16_t)reg << 8) | data;
    for (int8_t b = 15; b >= 0; b--) {
      digitalWrite(g_clkPin, LOW);
      digitalWrite(g_dinPin, (word & (1 << b)) ? HIGH : LOW);
      delayMicroseconds(3);
      digitalWrite(g_clkPin, HIGH);
      delayMicroseconds(3);
    }
  }
  digitalWrite(g_clkPin, LOW);
  delayMicroseconds(3);
  digitalWrite(g_csPin, HIGH);
  delayMicroseconds(3);
}

// Raw low-level initialization (Shutdown OFF, Scan Limit 8, Intensity 15, No Decode)
static void rawInitMax7219() {
  rawSend16(0x0C, 0x01); // Normal operation (exit shutdown)
  rawSend16(0x0F, 0x00); // Test mode OFF
  rawSend16(0x09, 0x00); // Decode mode: raw matrix
  rawSend16(0x0B, 0x07); // Scan limit: all 8 rows
  rawSend16(0x0A, 0x0F); // Max brightness (15/15)
  for (uint8_t r = 1; r <= 8; r++) {
    rawSend16(r, 0x00); // Clear all rows
  }
}

// Custom glyphs for icons:
// Checkmark ✓ (5 columns wide)
static const uint8_t kCheckGlyph[] = { 5, 0x18, 0x30, 0x60, 0x18, 0x06 };
// Alert ! (3 columns wide)
static const uint8_t kAlertGlyph[] = { 3, 0x06, 0x5F, 0x06 };

static void initDisplay() {
  if (g_parola) {
    delete g_parola;
    g_parola = nullptr;
  }
  rawInitMax7219();
  if (g_useHwSpi) {
    g_parola = new MD_Parola((MD_MAX72XX::moduleType_t)g_hwType, g_csPin, kNumModules);
  } else {
    // Software SPI: rock-solid on 3.3V ESP32 with 5V MAX7219 clones
    g_parola = new MD_Parola((MD_MAX72XX::moduleType_t)g_hwType, g_dinPin, g_clkPin, g_csPin, kNumModules);
  }
  g_parola->begin();
  g_parola->addChar(0x80, kCheckGlyph);
  g_parola->addChar(0x81, kAlertGlyph);
  g_parola->setIntensity(g_bright);
  g_parola->setInvert(g_invert);
  g_parola->displayText(g_curText, g_align, g_speed, g_pause, g_effectIn, g_effectOut);
  g_parola->displayReset();
}

static void setTestPattern(TestPattern p) {
  g_testPattern = p;
  if (p == TEST_ALL_ON) {
    rawSend16(0x0C, 0x01); // Normal operation (exit shutdown)
    rawSend16(0x0F, 0x01); // MAX7219 Display Test ON: forces EVERY LED ON on all 4 modules!
    memset(g_displayBuffer, 0xFF, sizeof(g_displayBuffer));
  } else if (p == TEST_CHECKER) {
    rawInitMax7219();
    for (uint8_t r = 1; r <= 8; r++) {
      rawSend16(r, (r % 2 == 0) ? 0xAA : 0x55);
    }
    for (uint8_t c = 0; c < 32; c++) {
      g_displayBuffer[c] = (c % 2 == 0) ? 0xAA : 0x55;
    }
  } else if (p == TEST_WALK) {
    rawInitMax7219();
    memset(g_displayBuffer, 0, sizeof(g_displayBuffer));
  } else {
    // TEST_OFF: exit test mode and re-init Parola
    rawSend16(0x0F, 0x00);
    rawInitMax7219();
    initDisplay();
  }
}

static void updateDisplayMessage() {
  if (!g_parola) return;
  snprintf(g_curText, sizeof(g_curText), "%s", g_nextText);
  g_textPending = false;
  g_effectIn = findEffect(g_effectName);
  g_effectOut = g_effectIn;
  g_parola->setIntensity(g_bright);
  g_parola->setInvert(g_invert);
  g_parola->displayText(g_curText, g_align, g_speed, g_pause, g_effectIn, g_effectOut);
  g_parola->displayReset();
}

// Background FreeRTOS task dedicated to animating the matrix at high frame rate
static void matrixAnimationTask(void*) {
  static uint32_t lastClockUpdate = 0;

  for (;;) {
    xSemaphoreTake(g_lock, portMAX_DELAY);

    if (g_testPattern == TEST_WALK) {
      static uint32_t lastWalk = 0;
      static uint8_t walkCol = 0;
      static uint8_t walkRow = 0;
      if (millis() - lastWalk >= 60) {
        lastWalk = millis();
        memset(g_displayBuffer, 0, sizeof(g_displayBuffer));
        g_displayBuffer[walkCol] = (1 << walkRow);

        for (uint8_t r = 1; r <= 8; r++) {
          rawSend16(r, (r == (walkRow + 1)) ? (1 << (walkCol % 8)) : 0x00);
        }

        walkCol = (walkCol + 1) % 32;
        if (walkCol == 0) {
          walkRow = (walkRow + 1) % 8;
        }
      }
      xSemaphoreGive(g_lock);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    } else if (g_testPattern != TEST_OFF) {
      xSemaphoreGive(g_lock);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Check hardware auto-revert timer to return to clock
    if (g_revertToClockAt > 0 && millis() >= g_revertToClockAt) {
      g_revertToClockAt = 0;
      g_clockMode = true;
    }

    if (g_clockMode) {
      if (millis() - lastClockUpdate >= 1000) {
        lastClockUpdate = millis();
        time_t now = time(nullptr);
        char timeBuf[32];
        if (now < 100000) {
          snprintf(timeBuf, sizeof(timeBuf), "SYNCING");
        } else {
          struct tm tmInfo;
          localtime_r(&now, &tmInfo);
          strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tmInfo);
        }
        if (strcmp(g_curText, timeBuf) != 0) {
          snprintf(g_curText, sizeof(g_curText), "%s", timeBuf);
          g_parola->displayText(g_curText, PA_CENTER, 0, 1000, PA_PRINT, PA_NO_EFFECT);
          g_parola->displayReset();
        }
      }
    } else if (g_textPending) {
      updateDisplayMessage();
    }

    if (g_parola && g_parola->displayAnimate()) {
      if (!g_clockMode) {
        g_parola->displayReset(); // loop animation
      }
    }

    // Capture the 32-column display buffer for web live preview in natural left-to-right order
    // (MD_MAX72XX native display coordinates have Column 0 at the far right and Column 31 at the far left)
    if (g_parola && g_parola->getGraphicObject()) {
      for (uint8_t c = 0; c < 32; c++) {
        g_displayBuffer[c] = g_parola->getGraphicObject()->getColumn(31 - c);
      }
    }

    xSemaphoreGive(g_lock);
    vTaskDelay(pdMS_TO_TICKS(5)); // ~200 Hz animation polling
  }
}

// --- HTTP Handlers ---
static void handleRoot() {
  String html = FPSTR(kIndexHtml);
  html.replace("%IP%", WiFi.localIP().toString());
  g_server.send(200, "text/html; charset=utf-8", html);
}

static void handleStatus() {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  String j = F("{\"ok\":true");
  j += F(",\"text\":\"");
  for (size_t i = 0; i < strlen(g_curText); i++) {
    char c = g_curText[i];
    if ((uint8_t)c == 0x80) { j += "\\u2713"; }
    else if ((uint8_t)c == 0x81) { j += "!"; }
    else if (c == '"' || c == '\\') { j += '\\'; j += c; }
    else if (c == '\n') { j += "\\n"; }
    else if (c == '\r') { j += "\\r"; }
    else if (c == '\t') { j += "\\t"; }
    else if ((uint8_t)c >= 0x20) { j += c; }
  }
  j += F("\",\"effect\":\""); j += g_effectName;
  j += F("\",\"align\":\""); j += alignToString(g_align);
  j += F("\",\"speed\":"); j += g_speed;
  j += F(",\"pause\":"); j += g_pause;
  j += F(",\"brightness\":"); j += g_bright;
  j += F(",\"invert\":"); j += (g_invert ? "true" : "false");
  j += F(",\"clockMode\":"); j += (g_clockMode ? "true" : "false");
  j += F(",\"testPattern\":"); j += (int)g_testPattern;
  j += F(",\"testMode\":"); j += (g_testPattern != TEST_OFF ? "true" : "false");
  j += F(",\"spiMode\":\""); j += (g_useHwSpi ? "hw" : "sw");
  j += F("\",\"cs\":"); j += g_csPin;
  j += F(",\"clk\":"); j += g_clkPin;
  j += F(",\"din\":"); j += g_dinPin;
  j += F(",\"hw\":"); j += g_hwType;
  j += F(",\"ip\":\""); j += WiFi.localIP().toString();
  j += F("\",\"rssi\":"); j += WiFi.RSSI();
  j += F(",\"heap\":"); j += ESP.getFreeHeap();
  j += F(",\"uptime\":"); j += millis();
  j += F(",\"dots\":[");
  for (uint8_t c = 0; c < 32; c++) {
    j += g_displayBuffer[c];
    if (c < 31) j += ',';
  }
  j += F("]}");
  xSemaphoreGive(g_lock);
  g_server.send(200, "application/json", j);
}

static void handleMessage() {
  bool changed = false;

  xSemaphoreTake(g_lock, portMAX_DELAY);

  if (g_server.hasArg("clock")) {
    g_clockMode = (g_server.arg("clock").toInt() != 0 || g_server.arg("clock").equalsIgnoreCase("true"));
    if (g_clockMode) g_revertToClockAt = 0;
    changed = true;
  }

  if (g_server.hasArg("revert_ms")) {
    int r = g_server.arg("revert_ms").toInt();
    if (r > 0) {
      g_revertToClockAt = millis() + (uint32_t)r;
    } else {
      g_revertToClockAt = 0;
    }
  }

  if (g_server.hasArg("text")) {
    String txt = g_server.arg("text");
    txt.replace("\xE2\x9C\x93", "\x80"); // UTF-8 checkmark ✓
    txt.replace("\xE2\x9C\x94", "\x80"); // UTF-8 checkmark ✔
    txt.replace("[v]", "\x80");
    txt.replace("[V]", "\x80");
    txt.replace("[!]", "\x81");
    snprintf(g_nextText, sizeof(g_nextText), "%s", txt.c_str());
    snprintf(g_curText, sizeof(g_curText), "%s", txt.c_str());
    g_textPending = true;
    g_clockMode = false;
    changed = true;
  }

  if (g_server.hasArg("effect")) {
    g_effectName = g_server.arg("effect");
    g_textPending = true;
    changed = true;
  }

  if (g_server.hasArg("align")) {
    g_align = parseAlign(g_server.arg("align"));
    g_textPending = true;
    changed = true;
  }

  if (g_server.hasArg("speed")) {
    int v = g_server.arg("speed").toInt();
    if (v >= 5 && v <= 500) { g_speed = (uint16_t)v; changed = true; }
  }

  if (g_server.hasArg("pause")) {
    int v = g_server.arg("pause").toInt();
    if (v >= 0 && v <= 20000) { g_pause = (uint16_t)v; changed = true; }
  }

  if (g_server.hasArg("brightness")) {
    int v = g_server.arg("brightness").toInt();
    if (v >= 0 && v <= 15) {
      g_bright = (uint8_t)v;
      if (g_parola) g_parola->setIntensity(g_bright);
      changed = true;
    }
  }

  if (g_server.hasArg("invert")) {
    String inv = g_server.arg("invert");
    g_invert = (inv == "1" || inv.equalsIgnoreCase("true"));
    if (g_parola) g_parola->setInvert(g_invert);
    changed = true;
  }

  if (changed) {
    bool shouldSave = true;
    if (g_server.hasArg("save")) {
      String s = g_server.arg("save");
      shouldSave = (s == "1" || s.equalsIgnoreCase("true"));
    } else if (g_server.hasArg("persist")) {
      String p = g_server.arg("persist");
      shouldSave = (p == "1" || p.equalsIgnoreCase("true"));
    }
    if (shouldSave) {
      saveSettings();
    }
  }

  xSemaphoreGive(g_lock);
  handleStatus();
}

static void handleBrightness() {
  if (g_server.hasArg("brightness") || g_server.hasArg("v")) {
    String s = g_server.hasArg("brightness") ? g_server.arg("brightness") : g_server.arg("v");
    int v = s.toInt();
    if (v >= 0 && v <= 15) {
      xSemaphoreTake(g_lock, portMAX_DELAY);
      g_bright = (uint8_t)v;
      if (g_parola) g_parola->setIntensity(g_bright);
      saveSettings();
      xSemaphoreGive(g_lock);
      g_server.send(200, "application/json", "{\"ok\":true,\"brightness\":" + String(g_bright) + "}");
      return;
    }
  }
  g_server.send(400, "text/plain", "brightness 0-15\n");
}

static void handleClear() {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_nextText[0] = '\0';
  g_curText[0] = '\0';
  g_textPending = true;
  g_clockMode = false;
  g_testPattern = TEST_OFF;
  saveSettings();
  xSemaphoreGive(g_lock);
  g_server.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
}

static void handleTest() {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (g_server.hasArg("pattern")) {
    String p = g_server.arg("pattern");
    if (p.equalsIgnoreCase("all") || p.equalsIgnoreCase("test") || p.equalsIgnoreCase("1")) {
      setTestPattern(TEST_ALL_ON);
    } else if (p.equalsIgnoreCase("checker") || p.equalsIgnoreCase("check") || p.equalsIgnoreCase("2")) {
      setTestPattern(TEST_CHECKER);
    } else if (p.equalsIgnoreCase("walk") || p.equalsIgnoreCase("dot") || p.equalsIgnoreCase("3")) {
      setTestPattern(TEST_WALK);
    } else {
      setTestPattern(TEST_OFF);
    }
  } else if (g_server.hasArg("on")) {
    String s = g_server.arg("on");
    bool on = (s == "1" || s.equalsIgnoreCase("true"));
    setTestPattern(on ? TEST_ALL_ON : TEST_OFF);
  } else {
    setTestPattern(g_testPattern == TEST_OFF ? TEST_ALL_ON : TEST_OFF);
  }
  int pat = (int)g_testPattern;
  xSemaphoreGive(g_lock);
  g_server.send(200, "application/json", "{\"ok\":true,\"testPattern\":" + String(pat) + ",\"testMode\":" + (pat != 0 ? "true" : "false") + "}");
}

static void handleConfig() {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  bool reinit = false;
  if (g_server.hasArg("spi")) {
    String s = g_server.arg("spi");
    bool hw = (s == "hw" || s == "hardware" || s == "1");
    if (hw != g_useHwSpi) { g_useHwSpi = hw; reinit = true; }
  }
  if (g_server.hasArg("cs")) {
    uint8_t pin = (uint8_t)g_server.arg("cs").toInt();
    if (pin != g_csPin && pin > 0) { g_csPin = pin; reinit = true; }
  }
  if (g_server.hasArg("clk")) {
    uint8_t pin = (uint8_t)g_server.arg("clk").toInt();
    if (pin != g_clkPin && pin > 0) { g_clkPin = pin; reinit = true; }
  }
  if (g_server.hasArg("din")) {
    uint8_t pin = (uint8_t)g_server.arg("din").toInt();
    if (pin != g_dinPin && pin > 0) { g_dinPin = pin; reinit = true; }
  }
  if (g_server.hasArg("hw")) {
    String h = g_server.arg("hw");
    uint8_t t = g_hwType;
    if (h.equalsIgnoreCase("fc16")) t = MD_MAX72XX::FC16_HW;
    else if (h.equalsIgnoreCase("generic")) t = MD_MAX72XX::GENERIC_HW;
    else if (h.equalsIgnoreCase("parola")) t = MD_MAX72XX::PAROLA_HW;
    else if (h.equalsIgnoreCase("icstation")) t = MD_MAX72XX::ICSTATION_HW;
    else if (h.toInt() > 0) t = (uint8_t)h.toInt();
    if (t != g_hwType) { g_hwType = t; reinit = true; }
  }
  if (reinit) {
    saveSettings();
    initDisplay();
  }
  xSemaphoreGive(g_lock);
  handleStatus();
}

// =========================================================================
// Setup and Main Loop
// =========================================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32 MAX7219 4-in-1 Dot Matrix Display ===");

  g_lock = xSemaphoreCreateMutex();
  loadSettings();
  initDisplay();

  // Wi-Fi setup
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(kApSsid, kApPass, 6);
  Serial.printf("ap: up ssid=%s pass=%s ip=%s\n", kApSsid, kApPass,
                WiFi.softAPIP().toString().c_str());

  // Join DecoFi LAN
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  Preferences wifiPrefs;
  wifiPrefs.begin("wifi", true);
  String ssid = wifiPrefs.getString("ssid", kDefaultSsid);
  String pass = wifiPrefs.getString("pass", kDefaultPass);
  wifiPrefs.end();

  if (ssid.length()) {
    Serial.printf("sta: connecting to %s...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("sta: connected! IP: %s\n", WiFi.localIP().toString().c_str());
      if (MDNS.begin(kMdnsHost)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mdns: http://%s.local/ -> %s\n", kMdnsHost, WiFi.localIP().toString().c_str());
      }
      // Start NTP clock sync
      configTime(g_tzOffset, 0, "pool.ntp.org", "time.nist.gov");
    } else {
      Serial.println("sta: join timed out, running on SoftAP");
    }
  }

  // Web routes
  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/api/status", HTTP_GET, handleStatus);
  g_server.on("/api/message", HTTP_POST, handleMessage);
  g_server.on("/api/brightness", HTTP_POST, handleBrightness);
  g_server.on("/api/clear", HTTP_POST, handleClear);
  g_server.on("/api/test", HTTP_POST, handleTest);
  g_server.on("/api/config", HTTP_POST, handleConfig);

  // Web OTA firmware update endpoint
  g_server.on("/update", HTTP_GET, []() {
    g_server.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset='utf-8'><title>OTA Update</title>"
      "<style>body{background:#0d1117;color:#e6edf3;font-family:sans-serif;padding:30px;max-width:500px;margin:auto}"
      ".card{background:#161b22;border:1px solid #30363d;padding:24px;border-radius:8px}"
      "input[type=file]{margin:16px 0;width:100%}button{background:#238636;color:#fff;border:none;padding:10px 20px;border-radius:6px;cursor:pointer;font-weight:600}"
      "</style></head><body><div class='card'><h2>MAX7219 OTA Firmware Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update' accept='.bin' required><br>"
      "<button type='submit'>Flash Firmware</button></form></div></body></html>");
  });
  g_server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    g_server.sendHeader("Connection", "close");
    g_server.send(200, "application/json", ok ? "{\"ok\":true,\"reboot\":true}" : "{\"ok\":false,\"err\":\"flash_failed\"}");
    if (ok) {
      delay(500);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = g_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("ota: update start %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("ota: update success %u B\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  g_server.onNotFound([]() { g_server.send(404, "text/plain", "Not Found"); });
  g_server.begin();
  Serial.println("http: listening on port 80");

  // FreeRTOS Task pinned to Core 0 for non-blocking 60fps animation
  xTaskCreatePinnedToCore(matrixAnimationTask, "matrix", 8192, nullptr, 2, nullptr, 0);

  Serial.println("ready: web server and display engine active");
  Serial.println("Type 'help' in serial monitor for hardware test commands");
}

static void checkSerialCommands() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd.equalsIgnoreCase("help") || cmd == "?") {
      Serial.println(F("\n=== MAX7219 Hardware Test Commands ==="));
      Serial.println(F("  test all     -> Force ALL 256 LEDs ON (hardware test mode 0x0F)"));
      Serial.println(F("  test check   -> Send Checkerboard pattern (0xAA / 0x55)"));
      Serial.println(F("  test walk    -> Animate walking single dot across matrix"));
      Serial.println(F("  test off     -> Restore normal text animation"));
      Serial.println(F("  pins <din> <cs> <clk> -> Set GPIO pins (default 23 5 18)"));
      Serial.println(F("  spi <sw|hw>  -> Switch Software Bit-Bang vs Hardware VSPI"));
      Serial.println(F("  status       -> Show current pins, mode, and IP"));
      Serial.println(F("======================================\n"));
    } else if (cmd.equalsIgnoreCase("test all") || cmd.equalsIgnoreCase("test 1")) {
      xSemaphoreTake(g_lock, portMAX_DELAY);
      setTestPattern(TEST_ALL_ON);
      xSemaphoreGive(g_lock);
      Serial.println(F(">> TEST: All 256 LEDs ON (Hardware test register 0x0F)"));
    } else if (cmd.equalsIgnoreCase("test check") || cmd.equalsIgnoreCase("test 2")) {
      xSemaphoreTake(g_lock, portMAX_DELAY);
      setTestPattern(TEST_CHECKER);
      xSemaphoreGive(g_lock);
      Serial.println(F(">> TEST: Checkerboard pattern active"));
    } else if (cmd.equalsIgnoreCase("test walk") || cmd.equalsIgnoreCase("test 3")) {
      xSemaphoreTake(g_lock, portMAX_DELAY);
      setTestPattern(TEST_WALK);
      xSemaphoreGive(g_lock);
      Serial.println(F(">> TEST: Walking pixel active"));
    } else if (cmd.equalsIgnoreCase("test off") || cmd.equalsIgnoreCase("test 0")) {
      xSemaphoreTake(g_lock, portMAX_DELAY);
      setTestPattern(TEST_OFF);
      xSemaphoreGive(g_lock);
      Serial.println(F(">> TEST: Normal display restored"));
    } else if (cmd.startsWith("pins ")) {
      int d = 0, c = 0, k = 0;
      if (sscanf(cmd.c_str(), "pins %d %d %d", &d, &c, &k) == 3) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_dinPin = d; g_csPin = c; g_clkPin = k;
        saveSettings();
        initDisplay();
        xSemaphoreGive(g_lock);
        Serial.printf(">> Pins updated: DIN=%d, CS=%d, CLK=%d\n", g_dinPin, g_csPin, g_clkPin);
      }
    } else if (cmd.startsWith("spi ")) {
      String m = cmd.substring(4);
      m.trim();
      xSemaphoreTake(g_lock, portMAX_DELAY);
      g_useHwSpi = (m.equalsIgnoreCase("hw") || m.equalsIgnoreCase("hardware"));
      saveSettings();
      initDisplay();
      xSemaphoreGive(g_lock);
      Serial.printf(">> SPI Mode: %s\n", g_useHwSpi ? "Hardware VSPI (8MHz)" : "Software Bit-Bang (~500kHz)");
    } else if (cmd.equalsIgnoreCase("status")) {
      Serial.printf(">> Status: IP=%s | Pins: DIN=%d, CS=%d, CLK=%d | SPI=%s | Pattern=%d\n",
                    WiFi.localIP().toString().c_str(), g_dinPin, g_csPin, g_clkPin,
                    g_useHwSpi ? "Hardware" : "Software", (int)g_testPattern);
    }
  }
}

void loop() {
  g_server.handleClient();
  checkSerialCommands();

  // Watchdog: auto-reconnect Wi-Fi if connection drops
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck >= 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("wifi: link dropped, reconnecting..."));
      WiFi.reconnect();
    }
  }

  delay(2);
}
