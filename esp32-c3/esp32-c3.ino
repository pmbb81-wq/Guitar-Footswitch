/*
   ESP32-C3 Super Mini - MIDI Footswitch
   Port z Arduino Leonardo

   Dziala tak samo jak wersja Arduino:
   - 2 przyciski (prev/next preset)
   - Pedał ekspresji (CC11)
   - OLED 0.96" I2C
   - Dual mode: JamVOX (CC80/81) + Overloud (CC16/17 pulse)

   ROZNICE vs Arduino:
   - ADC 12-bit (0-4095) zamiast 10-bit (0-1023)
   - Logika 3.3V (pedal EX-P potrzebuje dzielnika 5V->3.3V)
   - USB MIDI przez TinyUSB (nie MIDIUSB)
   - Inne piny GPIO

   PINY ESP32-C3 Super Mini:
   - GPIO0: Pedał (ADC)
   - GPIO1: Przycisk A (prev)
   - GPIO3: Przycisk B (next)
   - GPIO4: OLED SDA
   - GPIO5: OLED SCL
   - GPIO8: LED onboard (nie uzywany)

   WYMAGANIA:
   - Arduino IDE + ESP32 Core 3.x (Espressif Systems)
   - Tools -> USB Mode -> TinyUSB
   - Tools -> USB CDC On Boot -> Enabled
   - Libraries: U8g2, USB-MIDI (lathoub)
*/

#include <USB-MIDI.h>
#include <Wire.h>
#include <U8g2lib.h>

// #define DEBUG_ON

#define OLED_SDA 4
#define OLED_SCL 5

U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ OLED_SCL, /* data=*/ OLED_SDA);

USBMIDI_CREATE_INSTANCE(0, MIDI);

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


void setup() {
  pinMode(BTN_A_PIN, INPUT_PULLUP);
  pinMode(BTN_B_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  windowStart = millis();
  u8g2.begin();
  MIDI.begin(MIDI_CHANNEL_OMNI);
#ifdef DEBUG_ON
  Serial.begin(115200);
  Serial.println("--- ESP32-C3 footswitch ready ---");
#endif
}


void loop() {
  MIDI.read();
  readPedal();
  readButtons();
  updateOled();
  testMode();
}


void testMode() {
#ifdef DEBUG_ON
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
    controlChange(0, CC_OVERLOUD_PREV, 127);
    controlChange(0, CC_OVERLOUD_NEXT, 127);
    Serial.println("Sent CC16+CC17=127");
    lastTestSend = millis();
  }
#endif
}


void readPedal() {
  if (helpState != HELP_STATE_OFF) return;
  if (millis() - lastPedalSend < PEDAL_INTERVAL_MS) return;

  int raw = analogRead(PEDAL_PIN);

  if (millis() - windowStart >= RELEARN_MS) {
    if (winMax - winMin >= MIN_SPREAD) {
      pedalMin = winMin;
      pedalMax = winMax;
#ifdef DEBUG_ON
      Serial.print("Range: min=");
      Serial.print(pedalMin);
      Serial.print(" max=");
      Serial.println(pedalMax);
#endif
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
    controlChange(0, CC_EXPRESSION, (byte)scaled);
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
    controlChange(0, cc, 127);
    controlChange(0, cc, 0);
  }
#ifdef DEBUG_ON
  if (pressed) { Serial.print("BTN A -> CC"); Serial.print(cc); Serial.println(" pulse"); }
#endif
}

void sendButtonB(boolean pressed) {
  byte cc = btnSwap ? CC_OVERLOUD_PREV : CC_OVERLOUD_NEXT;
  if (!momentaryPulse) cc = btnSwap ? CC_FOOTSW1 : CC_FOOTSW2;
  if (pressed) {
    btnBBlink = true;
    btnABlink = false;
    controlChange(0, cc, 127);
    controlChange(0, cc, 0);
  }
#ifdef DEBUG_ON
  if (pressed) { Serial.print("BTN B -> CC"); Serial.print(cc); Serial.println(" pulse"); }
#endif
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
    u8g2.drawStr(momentaryPulse ? 28 : 13, 8, momentaryPulse ? "OVERLOUD TH3" : "Jam Vox-M-Audio FX");
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


void controlChange(byte channel, byte control, byte value) {
  MIDI.sendControlChange(control, value, channel + 1);
}
