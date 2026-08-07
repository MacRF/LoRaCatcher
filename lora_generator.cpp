#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>

// PINOUT HELTEC ESP32 LORA V2
#define PIN_LORA_SCK   5
#define PIN_LORA_MISO  19
#define PIN_LORA_MOSI  27
#define PIN_LORA_CS    18
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0  26

#define OLED_SDA       4
#define OLED_SCL       15
#define OLED_RST       16
#define OLED_ADDR      0x3C

#define LED_PIN        25
#define BUTTON_PIN     0

Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);
WebServer server(80);

// Parametri di default
float txFreq = 868.1;
int txSF = 7;
long txBW = 125000;
int txCR = 5;
int txSync = 0x12;
bool txIQ = false;
int txInterval = 100;
bool isTransmitting = true;

unsigned long txCount = 0;
unsigned long lastTxTime = 0;
bool paramsChanged = true;

// Web page string
String generateWebPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>LoRa Generator</title>";
  html += "<style>body{font-family:Arial,sans-serif;background-color:#121212;color:#ffffff;padding:20px;}";
  html += ".card{background-color:#1e1e1e;border-radius:10px;padding:20px;margin-bottom:20px;box-shadow:0 4px 6px rgba(0,0,0,0.3);}";
  html += "h1{color:#00e676; margin-top:0;} select, input{width:100%;padding:10px;margin:10px 0;border-radius:5px;border:none;background:#2a2a2a;color:#fff;}";
  html += "button{width:100%;padding:15px;background-color:#00e676;color:#121212;border:none;border-radius:5px;font-size:16px;font-weight:bold;cursor:pointer;}";
  html += "button.stop{background-color:#ff3b30;color:white;}";
  html += "label{font-weight:bold;color:#bbb; font-size:13px;}</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1>LoRa Generator</h1>";
  html += "<p>Status: <b>" + String(isTransmitting ? "<span style='color:#00e676;'>IN TRASMISSIONE</span>" : "<span style='color:#ff3b30;'>IN PAUSA</span>") + "</b></p>";
  html += "<p>Pacchetti inviati: <b>" + String(txCount) + "</b> <a href='/' style='color:#bbb; font-size:12px; margin-left:10px;'>Aggiorna contatore</a></p>";
  
  if (isTransmitting) {
    html += "<form action='/stop' method='GET'><button class='stop' type='submit'>Ferma Trasmissione</button></form>";
  } else {
    html += "<form action='/start' method='GET'><button type='submit'>Avvia Trasmissione</button></form>";
  }
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Parametri Radio</h2>";
  html += "<form action='/set' method='GET'>";
  html += "<label>Frequenza (MHz):</label><input type='number' step='0.1' name='f' value='" + String(txFreq, 1) + "'>";
  
  html += "<label>Spreading Factor:</label><select name='sf'>";
  for (int i=7; i<=12; i++) {
    html += "<option value='" + String(i) + "' " + (txSF==i?"selected":"") + ">SF" + String(i) + "</option>";
  }
  html += "</select>";
  
  html += "<label>Bandwidth (kHz):</label><select name='bw'>";
  html += "<option value='62500' " + String(txBW==62500?"selected":"") + ">62.5</option>";
  html += "<option value='125000' " + String(txBW==125000?"selected":"") + ">125</option>";
  html += "<option value='250000' " + String(txBW==250000?"selected":"") + ">250</option>";
  html += "<option value='500000' " + String(txBW==500000?"selected":"") + ">500</option>";
  html += "</select>";
  
  html += "<label>Coding Rate:</label><select name='cr'>";
  for(int i=5; i<=8; i++) {
    html += "<option value='" + String(i) + "' " + (txCR==i?"selected":"") + ">4/" + String(i) + "</option>";
  }
  html += "</select>";
  
  html += "<label>Sync Word:</label><select name='sy'>";
  html += "<option value='18' " + String(txSync==0x12?"selected":"") + ">0x12 (Privata)</option>";
  html += "<option value='52' " + String(txSync==0x34?"selected":"") + ">0x34 (LoRaWAN)</option>";
  html += "</select>";
  
  html += "<label>Inversione IQ:</label><select name='iq'>";
  html += "<option value='0' " + String(txIQ==false?"selected":"") + ">Normale</option>";
  html += "<option value='1' " + String(txIQ==true?"selected":"") + ">Invertita</option>";
  html += "</select>";

  html += "<label>Intervallo TX (ms):</label><select name='i'>";
  html += "<option value='100' " + String(txInterval==100?"selected":"") + ">100 ms (Fuoco Rapido)</option>";
  html += "<option value='500' " + String(txInterval==500?"selected":"") + ">500 ms</option>";
  html += "<option value='1000' " + String(txInterval==1000?"selected":"") + ">1000 ms (1 sec)</option>";
  html += "<option value='5000' " + String(txInterval==5000?"selected":"") + ">5000 ms (5 sec)</option>";
  html += "</select>";
  
  html += "<button type='submit'>Aggiorna Parametri</button>";
  html += "</form></div>";
  html += "</body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html", generateWebPage()); }
void handleStart() { isTransmitting = true; server.sendHeader("Location", "/"); server.send(302); }
void handleStop() { isTransmitting = false; server.sendHeader("Location", "/"); server.send(302); }
void handleSet() {
  if (server.hasArg("f")) txFreq = server.arg("f").toFloat();
  if (server.hasArg("sf")) txSF = server.arg("sf").toInt();
  if (server.hasArg("bw")) txBW = server.arg("bw").toInt();
  if (server.hasArg("cr")) txCR = server.arg("cr").toInt();
  if (server.hasArg("sy")) txSync = server.arg("sy").toInt();
  if (server.hasArg("iq")) txIQ = server.arg("iq").toInt() == 1;
  if (server.hasArg("i")) txInterval = server.arg("i").toInt();
  paramsChanged = true;
  server.sendHeader("Location", "/"); server.send(302);
}

void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("Errore OLED");
    while (1) delay(100);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 20);
  display.print("LoRa Generator");
  display.display();
  delay(1000);
}

void setupLoRa() {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(868100000)) {
    Serial.println("Errore LoRa");
    while (1) delay(100);
  }
}

void updateOLED() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.printf("GEN: %.1f MHz", txFreq);
  display.setCursor(0, 10);
  display.printf("S%d B%dk C4/%d %s", txSF, (int)(txBW/1000), txCR, txIQ?"Inv":"Nrm");
  display.setCursor(0, 20);
  display.printf("Sy:0x%02X Int:%dms", txSync, txInterval);
  
  display.drawLine(0, 32, 127, 32, SSD1306_WHITE);
  
  display.setCursor(0, 40);
  if (isTransmitting) {
    display.print("Stato: TX IN CORSO");
  } else {
    display.print("Stato: IN PAUSA");
  }
  
  display.setCursor(0, 50);
  display.printf("Pacchetti: %u", txCount);
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  setupDisplay();
  setupLoRa();
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP("LoRaGenerator", "generatortest");
  
  server.on("/", handleRoot);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/set", handleSet);
  server.begin();
  
  Serial.println("LoRa Generator Pronto!");
  updateOLED();
}

void loop() {
  server.handleClient();
  
  if (isTransmitting) {
    if (millis() - lastTxTime >= txInterval) {
      if (paramsChanged) {
        LoRa.setFrequency(txFreq * 1000000.0);
        LoRa.setSpreadingFactor(txSF);
        LoRa.setSignalBandwidth(txBW);
        LoRa.setCodingRate4(txCR);
        LoRa.setSyncWord(txSync);
        if (txIQ) {
          LoRa.enableInvertIQ();
        } else {
          LoRa.disableInvertIQ();
        }
        paramsChanged = false;
        updateOLED();
      }
      
      digitalWrite(LED_PIN, HIGH);
      
      LoRa.beginPacket();
      LoRa.print("TEST_PKG_");
      LoRa.print(txCount);
      LoRa.print("_");
      LoRa.print(millis());
      LoRa.endPacket();
      
      digitalWrite(LED_PIN, LOW);
      
      txCount++;
      lastTxTime = millis();
      
      if (txCount % 5 == 0 || txInterval >= 1000) {
        updateOLED();
      }
    }
  } else {
    if (paramsChanged) {
      paramsChanged = false;
      updateOLED();
    }
  }
}
