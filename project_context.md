# Edge Voice Authentication System — Project Context

**Authors:** Camila Nino Francia & Elbert Lin  
**Platform:** CC3200 LaunchPad (MCU)  
**Course:** EEC172

---

## What It Is
A standalone, buttonless biometric security device. A PIR motion sensor wakes the device when someone approaches, auto-records 3 seconds of voice, uploads to AWS, and displays results on an OLED screen. No buttons, no menus — the spoken keyword is the only control input.

---

## Hardware

| Component | Interface | Role |
|---|---|---|
| CC3200 LaunchPad | — | Core MCU; runs all firmware; boots from flash |
| HC-SR501 PIR Sensor | GPIO | Detects presence; wakes device from idle |
| INMP441 Microphone | I2S | 16-bit PCM @ 16 kHz; 3-second fixed window |
| SSD1306 OLED (128×64) | SPI | Displays state, countdown, results |
| AWS Lambda + S3 | Wi-Fi/HTTPS | STT keyword detection, FFT, profile storage |

**BOM total cost (non-lab parts):** ~$16 (PIR $6 + mic $9.99); rest is lab-provided or free-tier AWS.

---

## Current Pin Assignments (from [pin_mux_config.c](pin_mux_config.c))

| Pin | GPIO / Port-Bit | Mode | Peripheral / Direction | Used By |
|---|---|---|---|---|
| PIN_01 | — | 1 | I2C0_SCL | (reserved, not wired) |
| PIN_02 | GPIO11 (P1.3) | 1 | I2C0_SDA | (reserved, not wired) |
| PIN_05 | — | 7 | GSPI_CLK (SPI master) | OLED SCK |
| PIN_07 | — | 7 | GSPI_MOSI (SPI master) | OLED MOSI |
| PIN_18 | GPIO28 (P2.2) — GPIOA3 bit 0x10 | 0 | GPIO **OUTPUT** | OLED **RESET** |
| PIN_55 | — | 3 | UART0_TX | Debug console (USB-UART) |
| PIN_57 | GPIO2 | 3 | UART0_RX | Debug console (USB-UART) |
| PIN_61 | GPIO6 (P1.6) — GPIOA0 bit 0x40 | 0 | GPIO **OUTPUT** | OLED **DC** (data/command) |
| PIN_62 | GPIO7 (P1.7) — GPIOA0 bit 0x80 | 0 | GPIO **OUTPUT** | OLED **CS** (chip select, SW-controlled) |

**Pins explicitly parked at `PIN_MODE_0` (free for new peripherals):**
PIN_03, PIN_04, PIN_06, PIN_08, PIN_15, PIN_21, PIN_45, PIN_50, PIN_52, PIN_53, PIN_58, PIN_59, PIN_60, PIN_63, PIN_64
(JTAG pins 16/17/19/20 are reserved.)

**Peripheral clocks already enabled:** `GPIOA0`, `GPIOA3`, `I2CA0`, `GSPI`, `UARTA0`.
(McASP / I2S clock will need to be enabled when the mic is added: `PRCMPeripheralClkEnable(PRCM_I2S, PRCM_RUN_MODE_CLK)`.)

---

## Pin Recommendations for Microphone & PIR
Elberts's note: 

(I chose 50 for McAXR1, 63 for McAFSX, 64 for McAXR0, 53 for McACLK. This is because this was how example project wifi_audio_app set things up and also we need pins to be configured correctly for I2S communication, pin choice we also referenced the pinout diagram)
The microphone has 6 pins. clock, serial data, word select, left/right channel choice, Vcc, and GND. Since we are not using dual sound track, left/right channel choice will not be used (connect to GND).

### INMP441 Microphone (I²S / McASP) — exact wiring

The CC3200 SDK's `i2s_if.c` configures **DATA_LINE_1 (McAXR1, PIN_50) as RX** and **DATA_LINE_0 (McAXR0, PIN_64) as TX**. The INMP441's SD line must therefore connect to **PIN_50**, not PIN_64. PIN_64 is configured (and driven as output) but left physically disconnected.

| INMP441 pin | CC3200 pin | CC3200 mux | Direction | Notes |
|---|---|---|---|---|
| **VDD** | 3.3V rail | — | power | INMP441 is 1.8–3.3V tolerant |
| **GND** | GND rail | — | power | Common ground |
| **L/R** | GND rail | — | tie low | Selects LEFT slot; RIGHT will be silent |
| **WS** (LRCLK / frame sync) | **PIN_63** | mode 7 → `McASP0_FSX` | MCU → mic (out) | 16 kHz frame rate |
| **SCK** (bit clock / BCLK) | **PIN_53** | mode 2 → `McASP0_ACLK` | MCU → mic (out) | ~512 kHz |
| **SD** (serial data) | **PIN_50** | mode 6 → `McASP0_AXR1` | mic → MCU (in) | **RX serializer per SDK** |

PIN_64 (`McASP0_AXR0`, mode 7) is configured as the TX serializer because the SDK helper `Audio_Start` only supports `I2S_MODE_RX_TX` (RX-only is broken — `Audio_Start` skips `MAP_I2SEnable` when called with `I2S_MODE_RX` alone). Leave PIN_64 unconnected.


### HC-SR501 PIR Sensor (single digital GPIO input)
PIR output is active-high (3.3V) when motion is detected — any free GPIO can read it. Here, I chose pin 45 as the gpio input pin. 

Only 4 GPIOs on the CC3200 can wake from hibernate: **GPIO2, GPIO4, GPIO11, GPIO13**. Of those, GPIO2 (UART RX) and GPIO11 (I²C SDA) are already taken. That leaves two excellent candidates:

| PIR signal | Suggested CC3200 pin | GPIO# | Why |
|---|---|---|---|
| **OUT** (recommended) | **PIN_04** | GPIO13 | Hibernate-wake capable; on LaunchPad header J1 (easy wiring) |
| **OUT** (alternate) | **PIN_59** | GPIO4 | Also hibernate-wake capable |

Configure as input with `GPIODirModeSet(..., GPIO_DIR_MODE_IN)` and register a rising-edge interrupt. If you implement the stretch-goal PIR debounce (>500 ms sustained), do it in the ISR handler.

**Non-wake-capable fallbacks** (fine if you skip hibernate): PIN_58 (GPIO3), PIN_60 (GPIO5), PIN_63 (GPIO8).

---

## Operating Modes (keyword-driven)

| Keyword | Mode | Action |
|---|---|---|
| `"apple"` | Enroll | Store new voiceprint as User X+1 in AWS |
| `"banana"` | Clear | Delete all enrolled profiles from AWS |
| anything else | Authenticate | Compare voice against stored profiles; PASS/FAIL + score |

---

## Full Pipeline (one interaction cycle)

```
IDLE (PIR: no presence)
  → PIR detects presence
  → OLED: "Get Ready..." (1s pause)
  → OLED: "Recording..." (3s fixed window, I2S PCM capture)
  → OLED: "Uploading..." (send raw PCM to AWS via Wi-Fi)
  → AWS Lambda: STT keyword detection + 1024-pt FFT
      ├── "apple"  → store voiceprint → respond "enrolled"
      ├── "banana" → delete all profiles → respond "cleared"
      └── other    → cosine similarity vs. all stored profiles
                       ≥ 90% → PASS (OLED: "PASS XX% similar to User X")
                       < 90% → FAIL (OLED: "FAIL No matching profile")
  → return to IDLE
```

---

## Key Technical Details

- **FFT:** 1024-point; bins covering 100 Hz – 4 kHz (voice-relevant range)
- **Similarity metric:** Cosine similarity between FFT spectral vectors
- **Auth threshold:** 90% cosine similarity for PASS
- **Auth runs on-device:** CC3200 computes local FFT + cosine similarity; no cloud call during auth
- **Enrollment profiles:** Stored remotely in AWS S3; up to 3 users (target goal)
- **Threshold config:** Readable from flash config

---

## Implementation Goals

**Minimal (MVP):**
- Clean 3s I2S recording at 16 kHz
- Upload PCM to AWS; store FFT profile in flash
- PIR reliably wakes and triggers pipeline
- Local FFT + cosine similarity with OLED score display
- Standalone boot from flash
- AWS STT correctly classifies "apple" / "banana" / other

**Target:**
- Full end-to-end pipeline as described above
- 3 enrolled user profiles; AWS returns closest match
- OLED state labels at every stage

**Stretch:**
- Move enrollment FFT fully on-device (eliminate cloud dependency after bootstrap)
- Log attempts (timestamp, score, PASS/FAIL) to AWS DynamoDB via MQTT
- Liveness check: reject if spectral variance < noise floor (replay attack mitigation)
- PIR debounce: require >500ms sustained presence before wake

---

## AWS Architecture
- **Lambda:** Receives raw PCM, runs STT + FFT, handles enrollment/auth/clear logic
- **S3:** Stores enrolled voiceprint profiles
- **DynamoDB (stretch):** Attempt logging via MQTT

---

## What Is NOT in This Project
- No buttons or physical UI controls
- No on-device STT (STT runs in AWS Lambda)
- No speaker/audio output
- No local profile storage (profiles live in AWS)
