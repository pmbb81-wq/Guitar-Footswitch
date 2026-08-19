/*
   ESP32-C3 Super Mini - MIDI Footswitch (BLE MIDI + WiFi Web GUI)
   Port z Arduino Leonardo

   BLE MIDI - działa przez Bluetooth, sparuj z PC/telefonem
   WiFi Web Server - config GUI na http://192.168.4.1 (AP mode)

   ROZNICE vs Arduino:
   - ADC 12-bit (0-4095) zamiast 10-bit (0-1023)
   - Logika 3.3V (pedal EX-P potrzebuje dzielnika 5V->3.3V)
   - BLE MIDI zamiast USB MIDI
   - WiFi WebServer do konfiguracji
   - Inne piny GPIO

   PINY ESP32-C3 Super Mini:
   - GPIO0: Pedał (ADC)
   - GPIO1: Przycisk A (prev)
   - GPIO3: Przycisk B (next)
   - GPIO4: OLED SDA
   - GPIO5: OLED SCL

   WYMAGANIA:
   - Arduino IDE + ESP32 Core 3.x (Espressif Systems)
   - Tools -> Board -> ESP32C3 Dev Module (lub Nologo ESP32C3 Super Mini)
   - Tools -> USB CDC On Boot -> Enabled
   - Libraries: U8g2, BLE-MIDI, NimBLE-Arduino
*/

#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>

// #define DEBUG_ON

#define OLED_SDA 4
#define OLED_SCL 5

U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ OLED_SCL, /* data=*/ OLED_SDA);

BLEMIDI_CREATE_INSTANCE("Guitar Footswitch", MIDI);

WebServer server(80);

#define PEDAL_PIN    0
#define BTN_A_PIN    1
#define BTN_B_PIN    3

#define CC_EXPRESSION    11
#define CC_FOOTSW1       80
#define CC_FOOTSW2       81
#define CC_OVERLOUD_PREV 16
#define CC_OVERLOUD_NEXT 17

#define PEDAL_INTERVAL_MS 20
#define RELEARN_MS       2000
#define MIN_SPREAD       400
#define PEDAL_HYSTERESIS 8
#define PEDAL_RAW_MAX    4095

#define BTN_DEBOUNCE_MS   10
#define PEDAL_TOGGLE_MS   3000
#define PEDAL_MSG_MS      2000
#define BTN_HELP_MS       500

#define HELP_STATE_OFF   0
#define HELP_STATE_MENU  1
#define HELP_STATE_ITEM1 2
#define HELP_STATE_ITEM2 3
#define HELP_STATE_ITEM3 4

#define HELP_MENU_ITEMS  6
#define HELP_VISIBLE     4

#define WIFI_AP_SSID "Guitar-Footswitch"
#define WIFI_AP_PASS "12345678"

int lastCCValue = -1;
unsigned long lastPedalSend = 0;
unsigned long lastOledUpdate = 0;
int lastOledCC = -1;
boolean oledBtnA = false;
boolean oledBtnB = false;
boolean btnABlink = false;
boolean btnBBlink = false;
boolean lastOledPhase = false;
boolean pedalInverted = false;
boolean btnSwap = false;
boolean fxOff = false;
boolean momentaryPulse = false;
boolean msgOn = false;
boolean lastOledMsgOn = false;
boolean lastFxOff = false;
unsigned long msgStart = 0;
char msgBuf[16];

int btnARaw = 1;
int btnBRaw = 1;
int btnAStable = 1;
int btnBStable = 1;
unsigned long btnADebounceStart = 0;
unsigned long btnBDebounceStart = 0;

int pedalMin = 0;
int pedalMax = PEDAL_RAW_MAX;
int winMin = PEDAL_RAW_MAX;
int winMax = 0;
unsigned long windowStart = 0;

int helpState = HELP_STATE_MENU;
int helpIdx = 0;
int lastHelpState = -1;
int lastHelpIdx = -1;

bool bleConnected = false;

const char PAGE_ROOT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Guitar Footswitch</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;padding:10px}
.hdr{background:#16213e;padding:12px;border-radius:8px;text-align:center;margin-bottom:10px}
.hdr h1{font-size:18px;color:#e94560}
.card{background:#16213e;border-radius:8px;padding:14px;margin-bottom:10px}
.card h2{font-size:14px;color:#e94560;margin-bottom:10px;border-bottom:1px solid #0f3460;padding-bottom:6px}
.row{display:flex;align-items:center;justify-content:space-between;padding:6px 0}
.row label{font-size:13px;flex:1}
.btn{background:#0f3460;border:none;color:#eee;padding:10px 16px;border-radius:6px;font-size:13px;cursor:pointer;min-width:80px}
.btn:active{background:#e94560}
.btn.on{background:#e94560}
.btn.off{background:#333}
.status{font-size:12px;color:#aaa;text-align:center;padding:4px}
.meter{width:100%;height:20px;background:#0f3460;border-radius:4px;overflow:hidden;margin:6px 0}
.meter-fill{height:100%;background:#e94560;transition:width 0.1s;border-radius:4px}
.info{font-size:11px;color:#666;text-align:center;margin-top:8px}
.conn{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.conn.on{background:#0f0}
.conn.off{background:#f00}
</style>
</head>
<body>
<div class="hdr">
<h1>Guitar Footswitch</h1>
<div class="status" id="st">BLE: <span class="conn off" id="blec"></span><span id="ble">disconnected</span></div>
</div>

<div class="card">
<h2>Mode</h2>
<div class="row">
<label>Current mode:</label>
<span id="modetxt" style="color:#e94560;font-weight:bold">Jam Vox</span>
</div>
<div class="row">
<label>Set mode:</label>
<button class="btn" onclick="send('/api?set=jamvox')">Jam Vox</button>
<button class="btn" onclick="send('/api?set=overloud')">Overloud</button>
</div>
</div>

<div class="card">
<h2>Expression Pedal</h2>
<div class="meter"><div class="meter-fill" id="pb" style="width:0%"></div></div>
<div class="row">
<label>Pedal value:</label>
<span id="pv">0</span>
</div>
<div class="row">
<label>Pedal invert:</label>
<button class="btn" id="pinv" onclick="send('/api?toggle=pedalInvert')">OFF</button>
</div>
<div class="row">
<label>FX pedal:</label>
<button class="btn" id="fxoff" onclick="send('/api?toggle=fxOff')">ON</button>
</div>
</div>

<div class="card">
<h2>Buttons</h2>
<div class="row">
<label>Button swap:</label>
<button class="btn" id="bswap" onclick="send('/api?toggle=btnSwap')">OFF</button>
</div>
<div class="row">
<label>A = LEAD (prev)</label>
</div>
<div class="row">
<label>B = CLEAN (next)</label>
</div>
</div>

<div class="card">
<h2>Info</h2>
<div class="row"><label>IP Address:</label><span id="ip">-</span></div>
<div class="row"><label>Uptime:</label><span id="up">-</span></div>
</div>

<div class="info">BLE MIDI: "Guitar Footswitch" | Connect via Bluetooth</div>

<script>
function send(u){fetch(u).then(r=>r.json()).then(j=>{update(j)}).catch(e=>{})}
function update(j){
  if(j.pedal!==undefined){document.getElementById('pb').style.width=(j.pedal*100/127)+'%';document.getElementById('pv').textContent=j.pedal}
  if(j.pedalInvert!==undefined)document.getElementById('pinv').textContent=j.pedalInvert?'ON':'OFF';
  if(j.pedalInvert!==undefined)document.getElementById('pinv').className='btn '+(j.pedalInvert?'on':'off');
  if(j.fxOff!==undefined)document.getElementById('fxoff').textContent=j.fxOff?'OFF (muted)':'ON';
  if(j.fxOff!==undefined)document.getElementById('fxoff').className='btn '+(j.fxOff?'on':'off');
  if(j.btnSwap!==undefined)document.getElementById('bswap').textContent=j.btnSwap?'ON':'OFF';
  if(j.btnSwap!==undefined)document.getElementById('bswap').className='btn '+(j.btnSwap?'on':'off');
  if(j.mode!==undefined)document.getElementById('modetxt').textContent=j.mode=='overloud'?'Overloud TH-U':'Jam Vox';
  if(j.bleConnected!==undefined){document.getElementById('blec').className='conn '+(j.bleConnected?'on':'off');document.getElementById('ble').textContent=j.bleConnected?'connected':'disconnected'}
  if(j.ip!==undefined)document.getElementById('ip').textContent=j.ip;
  if(j.uptime!==undefined){var s=j.uptime,m=Math.floor(s/60),h=Math.floor(m/60);m=m%60;document.getElementById('up').textContent=h+'h '+m+'m'}
}
setInterval(function(){send('/api?poll=1')},1000);
send('/api?poll=1');
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", PAGE_ROOT);
}

void handleAPI() {
  String resp = "{";
  bool poll = server.hasArg("poll");

  if (server.hasArg("set")) {
    String mode = server.arg("set");
    if (mode == "jamvox") { momentaryPulse = false; fxOff = false; }
    else if (mode == "overloud") { momentaryPulse = true; fxOff = true; }
  }

  if (server.hasArg("toggle")) {
    String t = server.arg("toggle");
    if (t == "pedalInvert") pedalInverted = !pedalInverted;
    else if (t == "btnSwap") btnSwap = !btnSwap;
    else if (t == "fxOff") fxOff = !fxOff;
  }

  resp += "\"mode\":\"" + String(momentaryPulse ? "overloud" : "jamvox") + "\",";
  resp += "\"pedal\":" + String((lastCCValue < 0) ? 0 : lastCCValue) + ",";
  resp += "\"pedalInvert\":" + String(pedalInverted ? "true" : "false") + ",";
  resp += "\"fxOff\":" + String(fxOff ? "true" : "false") + ",";
  resp += "\"btnSwap\":" + String(btnSwap ? "true" : "false") + ",";
  resp += "\"bleConnected\":" + String(bleConnected ? "true" : "false") + ",";
  resp += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  resp += "\"uptime\":" + String(millis() / 1000);
  resp += "}";

  server.send(200, "application/json", resp);
}

void setup() {
  pinMode(BTN_A_PIN, INPUT_PULLUP);
  pinMode(BTN_B_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  windowStart = millis();
  u8g2.begin();

  MIDI.begin(MIDI_CHANNEL_OMNI);
  BLEMIDI.setHandleConnected([]() { bleConnected = true; });
  BLEMIDI.setHandleDisconnected([]() { bleConnected = false; });

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  server.on("/", handleRoot);
  server.on("/api", handleAPI);
  server.begin();

#ifdef DEBUG_ON
  Serial.begin(115200);
  Serial.println("--- ESP32-C3 footswitch ready ---");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
#endif
}


void loop() {
  server.handleClient();
  MIDI.read();
  readPedal();
  readButtons();
  updateOled();
#ifdef DEBUG_ON
  testMode();
#endif
}


void testMode() {
  static boolean testOn = false;
  static unsigned long lastTestSend = 0;

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't' || c == 'T') {
      testOn = !testOn;
      Serial.println(testOn ? "TEST MODE ON" : "TEST MODE OFF");
    }
  }

  if (testOn && millis() - lastTestSend >= 500) {
    MIDI.sendControlChange(CC_OVERLOUD_PREV, 127, 1);
    MIDI.sendControlChange(CC_OVERLOUD_NEXT, 127, 1);
    Serial.println("Sent CC16+CC17=127");
    lastTestSend = millis();
  }
}


void readPedal() {
  if (helpState != HELP_STATE_OFF) return;
  if (millis() - lastPedalSend < PEDAL_INTERVAL_MS) return;

  int raw = analogRead(PEDAL_PIN);

  if (millis() - windowStart >= RELEARN_MS) {
    if (winMax - winMin >= MIN_SPREAD) {
      pedalMin = winMin;
      pedalMax = winMax;
    }
    winMin = PEDAL_RAW_MAX;
    winMax = 0;
    windowStart = millis();
  }
  if (raw < winMin) winMin = raw;
  if (raw > winMax) winMax = raw;

  long scaled = (long)(raw - pedalMin) * 127L / (long)(pedalMax - pedalMin);
  if (scaled < 0) scaled = 0;
  if (scaled > 127) scaled = 127;
  if (pedalInverted) scaled = 127 - scaled;
  if (!fxOff && abs((int)scaled - lastCCValue) >= PEDAL_HYSTERESIS) {
    MIDI.sendControlChange(CC_EXPRESSION, (byte)scaled, 1);
    lastCCValue = (int)scaled;
  }
  lastPedalSend = millis();
}


void sendButtonA(boolean pressed) {
  byte cc = btnSwap ? CC_OVERLOUD_NEXT : CC_OVERLOUD_PREV;
  if (!momentaryPulse) cc = btnSwap ? CC_FOOTSW2 : CC_FOOTSW1;
  if (pressed) {
    btnABlink = true;
    btnBBlink = false;
    MIDI.sendControlChange(cc, 127, 1);
    if (momentaryPulse) MIDI.sendControlChange(cc, 0, 1);
  }
}

void sendButtonB(boolean pressed) {
  byte cc = btnSwap ? CC_OVERLOUD_PREV : CC_OVERLOUD_NEXT;
  if (!momentaryPulse) cc = btnSwap ? CC_FOOTSW1 : CC_FOOTSW2;
  if (pressed) {
    btnBBlink = true;
    btnABlink = false;
    MIDI.sendControlChange(cc, 127, 1);
    if (momentaryPulse) MIDI.sendControlChange(cc, 0, 1);
  }
}


void readButtons() {
  unsigned long t = millis();
  int aNow = (digitalRead(BTN_A_PIN) == LOW) ? 1 : 0;
  int bNow = (digitalRead(BTN_B_PIN) == LOW) ? 1 : 0;

  if (aNow != btnARaw) { btnARaw = aNow; btnADebounceStart = t; }
  if (aNow == btnARaw && t - btnADebounceStart >= BTN_DEBOUNCE_MS && aNow != btnAStable) btnAStable = aNow;
  if (bNow != btnBRaw) { btnBRaw = bNow; btnBDebounceStart = t; }
  if (bNow == btnBRaw && t - btnBDebounceStart >= BTN_DEBOUNCE_MS && bNow != btnBStable) btnBStable = bNow;

  oledBtnA = (btnAStable == 1);
  oledBtnB = (btnBStable == 1);

  static int prevStableA = 0;
  static int prevStableB = 0;

  if (helpState != HELP_STATE_OFF) {
    if (helpState == HELP_STATE_MENU) {
      static unsigned long helpAPressStart = 0;
      static unsigned long helpBPressStart = 0;

      if (oledBtnA && !prevStableA) helpAPressStart = t;
      if (oledBtnB && !prevStableB) helpBPressStart = t;

      if (!oledBtnA && prevStableA) {
        if (helpAPressStart > 0 && t - helpAPressStart >= BTN_HELP_MS) {
          if (helpIdx == 5) {
            helpState = HELP_STATE_OFF;
          } else if (helpIdx == 3) {
            momentaryPulse = false;
            helpState = HELP_STATE_OFF;
          } else if (helpIdx == 4) {
            momentaryPulse = true;
            fxOff = true;
            helpState = HELP_STATE_OFF;
          } else {
            helpState = HELP_STATE_ITEM1 + helpIdx;
          }
        } else {
          helpIdx = (helpIdx + 1) % HELP_MENU_ITEMS;
        }
        helpAPressStart = 0;
      }
      if (!oledBtnB && prevStableB) {
        if (helpBPressStart > 0 && t - helpBPressStart >= BTN_HELP_MS) {
          if (helpIdx == 5) {
            helpState = HELP_STATE_OFF;
          } else if (helpIdx == 3) {
            momentaryPulse = false;
            helpState = HELP_STATE_OFF;
          } else if (helpIdx == 4) {
            momentaryPulse = true;
            fxOff = true;
            helpState = HELP_STATE_OFF;
          } else {
            helpState = HELP_STATE_ITEM1 + helpIdx;
          }
        } else {
          helpIdx = (helpIdx + HELP_MENU_ITEMS - 1) % HELP_MENU_ITEMS;
        }
        helpBPressStart = 0;
      }
    } else {
      if ((oledBtnA && !prevStableA) || (oledBtnB && !prevStableB)) {
        helpState = HELP_STATE_MENU;
      }
    }
    prevStableA = oledBtnA;
    prevStableB = oledBtnB;
    return;
  }

  static unsigned long btnAPressStart = 0;
  static unsigned long btnBPressStart = 0;
  static boolean btnALongFired = false;
  static boolean btnBLongFired = false;
  static unsigned long btnBothStart = 0;
  static boolean btnBothFired = false;

  if (oledBtnA && oledBtnB) {
    if (btnBothStart == 0) btnBothStart = t;
    if (!btnBothFired && (t - btnBothStart >= 2000)) {
      btnBothFired = true;
      fxOff = !fxOff;
      msgStart = t;
      msgOn = true;
      snprintf(msgBuf, sizeof(msgBuf), fxOff ? "PED FX OFF" : "PED FX ON");
    }
    prevStableA = oledBtnA;
    prevStableB = oledBtnB;
    return;
  } else {
    btnBothStart = 0;
    btnBothFired = false;
  }

  if (oledBtnA && !prevStableA) {
    btnAPressStart = t;
    btnALongFired = false;
    sendButtonA(true);
  }
  if (oledBtnA && !btnALongFired && (t - btnAPressStart >= PEDAL_TOGGLE_MS)) {
    btnALongFired = true;
    sendButtonA(false);
    btnSwap = !btnSwap;
    msgStart = t;
    msgOn = true;
    snprintf(msgBuf, sizeof(msgBuf), btnSwap ? "A-B SWAP" : "A-B NORM");
    btnABlink = true;
    btnBBlink = false;
  }
  if (!oledBtnA && prevStableA) sendButtonA(false);
  prevStableA = oledBtnA;

  if (oledBtnB && !prevStableB) {
    btnBPressStart = t;
    btnBLongFired = false;
    sendButtonB(true);
  }
  if (oledBtnB && !btnBLongFired && (t - btnBPressStart >= PEDAL_TOGGLE_MS)) {
    btnBLongFired = true;
    sendButtonB(false);
    pedalInverted = !pedalInverted;
    msgStart = t;
    msgOn = true;
    snprintf(msgBuf, sizeof(msgBuf), pedalInverted ? "PEDAL REV" : "PEDAL NORM");
    btnBBlink = true;
    btnABlink = false;
  }
  if (!oledBtnB && prevStableB) sendButtonB(false);
  prevStableB = oledBtnB;
}


void drawHelpMenu() {
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawBox(0, 0, 128, 10);
  u8g2.setColorIndex(0);
  u8g2.drawStr(34, 8, "HELP MENU");
  u8g2.setColorIndex(1);

  const char *items[] = {
    "1. LEAD Button", "2. CLEAN Button", "3. Both Buttons",
    "4. Jam Vox", "5. Overloud", "6. EXIT"
  };

  int visStart = 0;
  if (helpIdx >= visStart + HELP_VISIBLE) visStart = helpIdx - HELP_VISIBLE + 1;
  if (helpIdx < visStart) visStart = helpIdx;

  for (int i = 0; i < HELP_VISIBLE && (visStart + i) < HELP_MENU_ITEMS; i++) {
    int idx = visStart + i;
    int y = 22 + i * 10;
    if (idx == helpIdx) { u8g2.drawBox(0, y - 8, 128, 10); u8g2.setColorIndex(0); }
    u8g2.drawStr(10, y, items[idx]);
    if (idx == helpIdx) u8g2.setColorIndex(1);
  }
  u8g2.drawStr(2, 63, momentaryPulse ? "tryb: PULSE" : "tryb: SINGLE");
}

void drawHelpItem1() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 10); u8g2.setColorIndex(0);
  u8g2.drawStr(28, 8, "LEAD BUTTON"); u8g2.setColorIndex(1);
  u8g2.drawStr(0, 22, "Krotkie wcisniecie:");
  u8g2.drawStr(6, 34, "CC80 = poprzedni");
  u8g2.drawStr(6, 46, "preset (prev)");
  u8g2.drawStr(0, 58, "Przytrzymaj 3s =");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(70, 62, "zamiana A/B");
}

void drawHelpItem2() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 10); u8g2.setColorIndex(0);
  u8g2.drawStr(24, 8, "CLEAN BUTTON"); u8g2.setColorIndex(1);
  u8g2.drawStr(0, 22, "Krotkie wcisniecie:");
  u8g2.drawStr(6, 34, "CC81 = nastepny");
  u8g2.drawStr(6, 46, "preset (next)");
  u8g2.drawStr(0, 58, "Przytrzymaj 3s =");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(70, 62, "odwroc pedal");
}

void drawHelpItem3() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 10); u8g2.setColorIndex(0);
  u8g2.drawStr(25, 8, "BOTH BUTTONS"); u8g2.setColorIndex(1);
  u8g2.drawStr(0, 22, "Przytrzymaj oba");
  u8g2.drawStr(0, 34, "przyciski razem");
  u8g2.drawStr(0, 46, "przez 2 sekundy:");
  u8g2.drawStr(6, 58, "toggle FX pedalu");
}


void updateOled() {
  if (helpState != HELP_STATE_OFF) {
    if (helpState == lastHelpState && helpIdx == lastHelpIdx) return;
    lastHelpState = helpState;
    lastHelpIdx = helpIdx;
    u8g2.firstPage();
    do {
      if (helpState == HELP_STATE_MENU) drawHelpMenu();
      else if (helpState == HELP_STATE_ITEM1) drawHelpItem1();
      else if (helpState == HELP_STATE_ITEM2) drawHelpItem2();
      else if (helpState == HELP_STATE_ITEM3) drawHelpItem3();
    } while (u8g2.nextPage());
    return;
  }

  lastHelpState = -1;
  lastHelpIdx = -1;
  if (millis() - lastOledUpdate < 100) return;

  int cc = (lastCCValue < 0) ? 0 : lastCCValue;
  boolean phase = ((millis() / 500) % 2) == 0;
  if (msgOn && millis() - msgStart >= PEDAL_MSG_MS) msgOn = false;
  boolean changed = (cc != lastOledCC) || (phase != lastOledPhase) || (msgOn != lastOledMsgOn) || (fxOff != lastFxOff);
  lastOledPhase = phase; lastOledMsgOn = msgOn; lastFxOff = fxOff;
  if (!changed) { lastOledUpdate = millis(); return; }

  char buf[8];
  snprintf(buf, sizeof(buf), "%3d", cc);
  boolean showA = !btnABlink || phase;
  boolean showB = !btnBBlink || phase;

  u8g2.firstPage();
  do {
    u8g2.drawBox(0, 0, 128, 10);
    u8g2.setColorIndex(0);
    u8g2.setFont(u8g2_font_6x10_tf);
    if (bleConnected) {
      u8g2.drawStr(momentaryPulse ? 28 : 13, 8, momentaryPulse ? "OVERLOUD TH3" : "Jam Vox-M-Audio FX");
    } else {
      u8g2.drawStr(16, 8, "BLE: not connected");
    }
    u8g2.setColorIndex(1);

    if (msgOn) {
      u8g2.setColorIndex(0); u8g2.drawBox(0, 14, 128, 28); u8g2.setColorIndex(1);
      u8g2.setFont(u8g2_font_10x20_tf);
      int w = u8g2.getStrWidth(msgBuf);
      u8g2.drawStr((128 - w) / 2, 34, msgBuf);
    } else {
      u8g2.setFont(u8g2_font_10x20_tf);
      if (showB) { u8g2.setColorIndex(0); u8g2.drawBox(0, 14, 64, 28); u8g2.setColorIndex(1); u8g2.drawStr(12, 34, "CLEAN"); }
      if (showA) { u8g2.setColorIndex(0); u8g2.drawBox(64, 14, 64, 28); u8g2.setColorIndex(1); u8g2.drawStr(74, 34, "LEAD"); }
    }

    if (fxOff) {
      u8g2.drawBox(0, 48, 128, 16); u8g2.setColorIndex(0);
      u8g2.setFont(u8g2_font_6x10_tf);
      int w = u8g2.getStrWidth("PED FX OFF");
      u8g2.drawStr((128 - w) / 2, 58, "PED FX OFF"); u8g2.setColorIndex(1);
    } else {
      u8g2.drawBox(0, 48, 128, 16); u8g2.setColorIndex(0);
      int pw = (cc * 128) / 127;
      u8g2.drawBox(pw, 48, 128 - pw, 16); u8g2.setColorIndex(1);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawBox(54, 50, 20, 12); u8g2.setColorIndex(0);
      u8g2.drawStr(55, 60, buf); u8g2.setColorIndex(1);
    }
  } while (u8g2.nextPage());
  lastOledCC = cc;
  lastOledUpdate = millis();
}
