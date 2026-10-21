# Smart ESP32 IoT Energy Meter

An ESP32-based meter that measures AC mains voltage and load current, then calculates power, energy (kWh) and cost. It shows the readings on a 16x2 LCD, uploads them to the Blynk cloud dashboard and sends reports to Telegram.


## Features

- Real-time AC voltage (ZMPT101B), current (ACS712) and power (V x I)
- Energy (kWh) and cost accumulation
- 16x2 I2C LCD with 3 auto-rotating pages: Voltage/Current, Power/Energy, Total Cost
- Non-volatile storage in EEPROM, with flash-wear protection (saves only every 0.01 kWh)
- Blynk IoT dashboard on web and mobile
- Telegram report messages
- Push button for voltage calibration and data reset

---

## Components

| Part | Purpose |
|---|---|
| ESP32 dev board | Reads sensors, calculations, cloud and Telegram reports |
| ZMPT101B | AC mains voltage sensor (sensitive, needs calibration) |
| ACS712 | AC current sensor |
| 16x2 LCD + I2C module | Display, only 2 data wires (SDA/SCL) |
| Push button | Calibration / reset |
| Veroboard + header pins | Mounting the ESP32 and sensors |
| Jumper wires | Low-voltage connections |
| AC load circuit | Socket/bulb/appliance to be measured |

---

## Wiring

### ESP32 pins 

| Device | Pin on device | ESP32 pin |
|---|---|---|
| ACS712 | OUT | GPIO 34 |
| ZMPT101B | OUT | GPIO 35 |
| Push button | one leg | GPIO 4 (other leg to GND, uses internal pull-up) |
| LCD (I2C) | SDA | GPIO 21 |
| LCD (I2C) | SCL | GPIO 22 |
| All modules | VCC / GND | See power notes below |

LCD I2C address in the code is `0x27`. If the screen stays blank, try `0x3F` or run an I2C scanner.

### Power notes

- The ACS712 and ZMPT101B modules are often 5V parts. Their output must never exceed **3.3V** at the ESP32 ADC pin. Use a voltage divider on the ACS712 output if you power it from 5V.
- The code's `ACS712 ACS(ACS_PIN, 3.3, 4095, 125)` assumes a particular mV/A value (125). Adjust it to match your module/divider setup (ACS712-5A = 185 mV/A, 20A = 100 mV/A, 30A = 66 mV/A before any divider).

### AC side

1. Take the **phase (live) wire** of the load circuit and connect it through the ACS712 current terminals (in series with the load).
2. Connect the **other wire (neutral)** together with the phase to the ZMPT101B input terminals (measured in parallel across mains).
3. Plug the load into the circuit as normal.

```
Mains Phase ──► ACS712 (IN) ── ACS712 (OUT) ──► Load ──► Neutral
Mains Phase & Neutral ──► ZMPT101B input (parallel)
```

---

## Software Setup

### Libraries (Arduino IDE → Library Manager)

- Blynk
- LiquidCrystal_I2C
- ACS712 (Rob Tillaart)
- ZMPT101B
- ArduinoJson
- ESP32 board package (WiFi, EEPROM, HTTPClient are included)

### Blynk setup

1. Create an account at blynk.cloud.
2. Create a **Template** (e.g. "Net Energy Meter").
3. Under **Datastreams**, create 5 virtual pins:

   | Pin | Name | Type |
   |---|---|---|
   | V0 | Voltage | Double |
   | V1 | Current | Double |
   | V2 | Power | Double |
   | V3 | Total Energy | Double |
   | V4 | Total Cost | Double |

4. Build the **Web Dashboard** with widgets (gauges/labels) linked to those datastreams.
5. Create a **Device** from the template. Copy the **Template ID**, **Template Name** and **Auth Token**.
6. Install the Blynk mobile app, log in with the same account, and add widgets in the mobile dashboard for the same datastreams.

Paste the credentials into the code:

```cpp
#define BLYNK_TEMPLATE_ID   "YOUR_BLYNK_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "Net Energy Meter"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";
```

### Telegram setup

1. In Telegram, open **@BotFather**, press Start, send `/newbot` and follow the prompts.
2. Copy the **bot token** BotFather gives you.
3. Start a chat with your new bot and send any message.
4. Get your **chat ID** (open `https://api.telegram.org/bot<TOKEN>/getUpdates` and read `chat.id`, or use @userinfobot).
5. Put both values in the code:

```cpp
const String telegramBotToken = "YOUR_BOT_TOKEN_HERE";
const String telegramChatID   = "YOUR_CHAT_ID_HERE";
#define TELEGRAM_INTERVAL 0   // ms between live reports, 0 = off
```

A **monthly report** (kWh used and bill for the month that just ended) is sent automatically when the month changes. The meter gets the date from the internet (NTP), so it needs WiFi. The time zone is set by `GMT_OFFSET_SEC` (18000 = UTC+5).

### Pricing

```cpp
#define COST_PER_KWH 8.0   // set to your local tariff
```

---

## Using the Meter

| Action | Result |
|---|---|
| Normal operation | LCD rotates through 3 pages every 4 seconds |
| Short press | Jump to the next LCD page |
| Hold button 1 to 3 s, release | **Voltage calibration** (set `REFERENCE_VOLTAGE` to your real mains voltage, measured with a trusted multimeter) |
| Hold button 3 s or more, release | **Reset** energy and cost to zero |

### Calibrating voltage

1. Measure the real mains voltage with a multimeter and set `REFERENCE_VOLTAGE` in the code, then upload.
2. Adjust the trimmer on the ZMPT101B until the raw reading is reasonable.
3. With mains connected, hold the button 1 to 3 s and release. The calibration factor is stored in EEPROM.

---

## Troubleshooting

- **LCD blank**: adjust the contrast pot on the I2C backpack, check the address (0x27/0x3F).
- **Voltage reads 0**: reading below 50V is treated as "no mains". Check sensor wiring and trimmer.
- **Current always 0**: readings under 0.05 A are zeroed. Make sure the load is in series through the ACS712.
- **No Blynk data**: check WiFi credentials, template ID and auth token.
- **No Telegram messages**: make sure you messaged the bot first and the chat ID is correct.
