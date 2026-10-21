/*
  * Project: Smart ESP32 IoT Energy Meter
 * Version: 2.0.0
 * Description: AC voltage (ZMPT101B), current (ACS712), power, energy, cost.
 *              16x2 I2C LCD, EEPROM storage with flash-wear protection,
 *              Blynk IoT dashboard, Telegram alerts + MONTHLY reports.
 *
 * Button (GPIO 4 to GND):
 *   short press        -> next LCD page
 *   hold 1 - 3 s       -> calibrate voltage
 *   hold 3 s or more   -> reset energy & cost
 *
 */

// ======================= USER CONFIGURATION =======================

// 1. Blynk IoT Credentials
#define BLYNK_TEMPLATE_ID   "BLYNK_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "Net Energy Meter"
#define BLYNK_AUTH_TOKEN    "BLYNK_AUTH_TOKEN"

// 2. WiFi Credentials
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

// 3. Telegram Bot Credentials (leave as-is to disable)
const String telegramBotToken = "YOUR_BOT_TOKEN_HERE";
const String telegramChatID   = "YOUR_CHAT_ID_HERE";
#define TELEGRAM_INTERVAL 0   // ms between live reports, 0 = disabled (monthly report always on)

// 4. Time zone (Pakistan = UTC+5)
#define GMT_OFFSET_SEC 18000
#define DAYLIGHT_OFFSET_SEC 0

// 5. Energy, Pricing & Memory Settings
#define COST_PER_KWH 8.0
#define REFERENCE_VOLTAGE 235.1        // real mains voltage measured with a multimeter
#define EEPROM_SAVE_THRESHOLD 0.01     // save only every 0.01 kWh (flash wear protection)
#define ACS_MV_PER_AMP 125             // set for your ACS712 module / divider (185=5A, 100=20A, 66=30A)

// 6. Hardware Pin Assignments (ESP32)
#define ACS_PIN 34
#define ZMPT_PIN 35
#define RESET_BUTTON_PIN 4

// ==================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "time.h"
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "ACS712.h"
#include <ZMPT101B.h>
#include <EEPROM.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

ACS712 ACS(ACS_PIN, 3.3, 4095, ACS_MV_PER_AMP);
ZMPT101B voltageSensor(ZMPT_PIN, 50.0);
LiquidCrystal_I2C lcd(0x27, 16, 2);

float energy_kWh = 0.0;
float volt = 0.0, power = 0.0;
float current_A = 0.0;
float total_cost = 0.0;
float vcal = 1.0;
float smoothed_voltage = 0.0;
float last_saved_energy = 0.0;

// Monthly report tracking
int   savedMonth = 0;            // 1-12, 0 = unknown
float monthStartEnergy = 0.0;

#define EEPROM_SIZE 512
#define ENERGY_ADDRESS 0
#define VCAL_ADDRESS 4
#define MONTH_ADDRESS 8
#define MONTH_START_ADDRESS 12

unsigned long lastTime = 0;
unsigned long lastLcdChange = 0;
unsigned long lastTelegramTime = 0;
unsigned long lastBlynkSend = 0;
unsigned long lastMonthCheck = 0;
int displayPage = 1;

const char* monthNames[] = {"", "January", "February", "March", "April", "May", "June",
                            "July", "August", "September", "October", "November", "December"};

// ---------------------------------------------------------------------
// Telegram
// ---------------------------------------------------------------------
void sendTelegram(const String &text) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (telegramBotToken == "YOUR_BOT_TOKEN_HERE") return;  // not configured

  WiFiClientSecure client;
  client.setInsecure();  // skip certificate validation (simple setup)

  HTTPClient http;
  http.begin(client, "https://api.telegram.org/bot" + telegramBotToken + "/sendMessage");
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(768);
  doc["chat_id"] = telegramChatID;
  doc["text"] = text;

  String payload;
  serializeJson(doc, payload);
  int code = http.POST(payload);
  Serial.print("Telegram HTTP code: ");
  Serial.println(code);
  http.end();
}

void sendLiveReport() {
  String msg = "📊 Energy Report\n";
  msg += "Voltage: " + String(volt, 1) + " V\n";
  msg += "Current: " + String(current_A, 2) + " A\n";
  msg += "Power: " + String(power, 1) + " W\n";
  msg += "Energy: " + String(energy_kWh, 3) + " kWh\n";
  msg += "Cost: Rs. " + String(total_cost, 2);
  sendTelegram(msg);
}

void sendMonthlyReport(int month, float usedKWh) {
  if (usedKWh < 0) usedKWh = 0;
  String msg = "📅 Monthly Energy Report - ";
  msg += monthNames[month];
  msg += "\nUsed: " + String(usedKWh, 3) + " kWh\n";
  msg += "Bill: Rs. " + String(usedKWh * COST_PER_KWH, 2) + "\n";
  msg += "Total meter reading: " + String(energy_kWh, 3) + " kWh";
  sendTelegram(msg);
}

// ---------------------------------------------------------------------
// Monthly tracking
// ---------------------------------------------------------------------
void saveMonthData() {
  EEPROM.writeInt(MONTH_ADDRESS, savedMonth);
  EEPROM.writeFloat(MONTH_START_ADDRESS, monthStartEnergy);
  EEPROM.commit();
}

void checkMonthlyReport() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;  // time not synced yet
  int m = t.tm_mon + 1;

  if (savedMonth < 1 || savedMonth > 12) {   // first run
    savedMonth = m;
    monthStartEnergy = energy_kWh;
    saveMonthData();
    return;
  }

  if (m != savedMonth) {                      // month changed
    sendMonthlyReport(savedMonth, energy_kWh - monthStartEnergy);
    savedMonth = m;
    monthStartEnergy = energy_kWh;
    saveMonthData();
  }
}

// ---------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  EEPROM.begin(EEPROM_SIZE);

  energy_kWh = EEPROM.readFloat(ENERGY_ADDRESS);
  if (isnan(energy_kWh) || energy_kWh < 0.0 || energy_kWh > 1000000.0) energy_kWh = 0.0;
  last_saved_energy = energy_kWh;

  vcal = EEPROM.readFloat(VCAL_ADDRESS);
  if (isnan(vcal) || vcal <= 0.0 || vcal > 10.0) vcal = 1.0;

  savedMonth = EEPROM.readInt(MONTH_ADDRESS);
  monthStartEnergy = EEPROM.readFloat(MONTH_START_ADDRESS);
  if (isnan(monthStartEnergy) || monthStartEnergy < 0.0 || monthStartEnergy > energy_kWh) {
    monthStartEnergy = energy_kWh;
  }

  ACS.autoMidPoint();
  voltageSensor.setSensitivity(500.0f);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Circuit Diagrams");
  lcd.setCursor(2, 1);
  lcd.print("Energy Meter");
  delay(2000);
  lcd.clear();

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.google.com");

  lastTime = millis();
  lastLcdChange = millis();
  lastTelegramTime = millis();
  lastBlynkSend = millis();
}

// ---------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------
void loop() {
  Blynk.run();

  // --- Button: short press = next page, 1-3 s = calibrate, 3 s+ = reset ---
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    unsigned long pressStart = millis();
    int action = 0;

    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
      unsigned long held = millis() - pressStart;
      if (held >= 3000) {
        lcd.setCursor(0, 0);
        lcd.print("Release to RESET");
        action = 2;
      } else if (held >= 1000) {
        lcd.setCursor(0, 0);
        lcd.print("Release to CAL. ");
        action = 1;
      } else {
        action = 0;
      }
      Blynk.run();
      delay(30);
    }

    unsigned long heldTotal = millis() - pressStart;

    if (action == 0 && heldTotal >= 50) {
      // SHORT PRESS: next page
      displayPage++;
      if (displayPage > 3) displayPage = 1;
      lastLcdChange = millis();
      lcd.clear();

    } else if (action == 1) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(" Calibrating... ");

      float raw_v = voltageSensor.getRmsVoltage();
      if (raw_v > 20.0) {
        vcal = REFERENCE_VOLTAGE / raw_v;
        EEPROM.writeFloat(VCAL_ADDRESS, vcal);
        EEPROM.commit();
        lcd.setCursor(0, 1);
        lcd.print("Factor: ");
        lcd.print(vcal, 3);
      } else {
        lcd.setCursor(0, 1);
        lcd.print("Error: No Mains!");
      }
      delay(2000);
      lcd.clear();
      lastLcdChange = millis();

    } else if (action == 2) {
      energy_kWh = 0.0;
      total_cost = 0.0;
      monthStartEnergy = 0.0;
      last_saved_energy = 0.0;
      EEPROM.writeFloat(ENERGY_ADDRESS, energy_kWh);
      EEPROM.writeInt(MONTH_ADDRESS, savedMonth);
      EEPROM.writeFloat(MONTH_START_ADDRESS, monthStartEnergy);
      EEPROM.commit();

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("   DATA RESET   ");
      Blynk.virtualWrite(V3, energy_kWh);
      Blynk.virtualWrite(V4, total_cost);
      delay(1500);
      lcd.clear();
      lastLcdChange = millis();
    }

    lastTime = millis();  // don't count button-hold time as energy
  }

  // 1. Current
  int noise = ACS.getNoisemV();
  float average = 0;
  for (int i = 0; i < 20; i++) {
    average += ACS.mA_AC();
  }
  float mA = (average / 20.0) - noise;
  float amps = mA / 1000.0;
  current_A = (amps > 0.05) ? amps : 0.0;

  // 2. Voltage with digital filter
  float raw_voltage = voltageSensor.getRmsVoltage();
  float calibrated_voltage = raw_voltage * vcal;

  if (calibrated_voltage < 50.0) {
    smoothed_voltage = 0.0;
    volt = 0.0;
  } else {
    if (smoothed_voltage < 50.0) smoothed_voltage = calibrated_voltage;
    smoothed_voltage = (smoothed_voltage * 0.8) + (calibrated_voltage * 0.2);
    volt = smoothed_voltage;
  }

  // 3. Power
  power = volt * current_A;

  // 4. Energy & cost
  unsigned long currentTime = millis();
  float elapsedTimeHours = (currentTime - lastTime) / 3600000.0;
  lastTime = currentTime;
  energy_kWh += (power / 1000.0) * elapsedTimeHours;
  total_cost = energy_kWh * COST_PER_KWH;

  // --- LCD pages (auto-rotate every 4 s, short press also switches) ---
  if (currentTime - lastLcdChange >= 4000) {
    displayPage++;
    if (displayPage > 3) displayPage = 1;
    lastLcdChange = currentTime;
    lcd.clear();
  }

  if (displayPage == 1) {
    lcd.setCursor(0, 0);
    lcd.print("Voltage: ");
    lcd.print(volt, 1);
    lcd.print("V  ");
    lcd.setCursor(0, 1);
    lcd.print("Current: ");
    lcd.print(current_A, 2);
    lcd.print("A  ");
  } else if (displayPage == 2) {
    lcd.setCursor(0, 0);
    lcd.print("Power: ");
    lcd.print(power, 1);
    lcd.print("W  ");
    lcd.setCursor(0, 1);
    lcd.print("Energy: ");
    lcd.print(energy_kWh, 2);
    lcd.print("kWh");
  } else {
    lcd.setCursor(0, 0);
    lcd.print("Total Cost: ");
    lcd.setCursor(0, 1);
    lcd.print("Rs. ");
    lcd.print(total_cost, 2);
    lcd.print("      ");
  }

  // --- Telegram ---
  if (TELEGRAM_INTERVAL > 0 && currentTime - lastTelegramTime >= TELEGRAM_INTERVAL) {
    sendLiveReport();
    lastTelegramTime = currentTime;
  }
  if (currentTime - lastMonthCheck >= 60000) {   // check month once a minute
    checkMonthlyReport();
    lastMonthCheck = currentTime;
  }

  // --- Serial ---
  Serial.print("V: "); Serial.print(volt, 1);
  Serial.print("  I: "); Serial.print(current_A, 3);
  Serial.print("  P: "); Serial.println(power, 1);

  // --- EEPROM wear protection ---
  if (energy_kWh - last_saved_energy >= EEPROM_SAVE_THRESHOLD) {
    EEPROM.writeFloat(ENERGY_ADDRESS, energy_kWh);
    EEPROM.commit();
    last_saved_energy = energy_kWh;
    Serial.println("*** EEPROM SAVED ***");
  }

  // --- Blynk (every 2 s to stay within rate limits) ---
  if (currentTime - lastBlynkSend >= 2000) {
    Blynk.virtualWrite(V0, volt);
    Blynk.virtualWrite(V1, current_A);
    Blynk.virtualWrite(V2, power);
    Blynk.virtualWrite(V3, energy_kWh);
    Blynk.virtualWrite(V4, total_cost);
    lastBlynkSend = currentTime;
  }

  delay(200);
}
