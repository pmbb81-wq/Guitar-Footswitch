

# Guitar Footswitch

Arduino Leonardo MIDI footswitch with OLED display, expression pedal support, and dual-mode compatibility (JamVOX + Overloud TH-U).

## Hardware

- Arduino Leonardo (Pro Micro clone)
- 2x momentary footswitch buttons
- M-Audio EX-P expression pedal
- OLED 0.96" I2C display (SSD1306 or SH1106)

## Wiring

| Component | Pin |
|-----------|-----|
| Pedal (wiper) | A0 |
| Pedal GND | GND |
| Pedal 5V | 5V |
| Button A | A2 |
| Button B | A3 |
| OLED SDA | 2 |
| OLED SCL | 3 |

Buttons use INPUT_PULLUP (no resistors needed). Connect between pin and GND.

## Features

- **Dual mode**: JamVOX (CC80/CC81) and Overloud TH-U (CC16/CC17 pulse)
- **Expression pedal**: Auto-calibrating 0-127 range with hysteresis
- **OLED display**: Mode title, CLEAN/LEAD blink, progress bar, help menu
- **Toggles**: Pedal inversion (hold B 3s), Button swap (hold A 3s), FX off (both 2s)
- **Help menu**: 6-item scrollable menu with descriptions

## Required Libraries

- [MIDIUSB](https://github.com/arduino-libraries/MIDIUSB)
- [U8g2](https://github.com/olikraus/u8g2)

## Setup

1. Install Arduino IDE
2. Add libraries via Library Manager
3. Select board: **Arduino Leonardo**
4. Upload `Guitar-Footswitch.ino`

## Overloud TH-U Configuration

1. In Overloud, go to MIDI settings
2. Select **Arduino Leonardo** as MIDI input
3. Set **CC16** → Go To Previous Preset (mode: toggle)
4. Set **CC17** → Go To Next Preset (mode: toggle)
5. Disable "Program Changes recall presets in current bank"



<img width="502" height="515" alt="Zrzut ekranu 2026-08-18 194501" src="https://github.com/user-attachments/assets/e9a77977-f9ba-41dc-9acb-17f1f0f9529b" />


<img width="857" height="589" alt="Zrzut ekranu 2026-08-18 194549" src="https://github.com/user-attachments/assets/64e4d56e-b7af-4bec-a2eb-e6b40ea0222b" />

<img width="1600" height="1600" alt="mikrokontroler-zgodny-z-arduino-leonardo-pro-micro-atmega32u4-16mhz" src="https://github.com/user-attachments/assets/4c9d4cb2-bde6-41ea-8651-aa9958fafa8f" />

<img width="1126" height="1748" alt="1000034049" src="https://github.com/user-attachments/assets/44956a9e-70ad-4128-8335-22392e33429d" />

