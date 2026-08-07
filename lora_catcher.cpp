/*
   Bonifica LoRa – Completo con Selezione Banda + Web + Display
   WiFi LoRa 32 (V2) con OLED e microSD (PCAP LoRaTap)
   -------------------------------------------------------------
   Features:
   - Selezione banda iniziale (Low 433-510 / High 863-923 MHz)
   - Frequenze variabili con step configurabile
   - Scansione multicanale automatica
   - Lista dispositivi trovati con selezione
   - Modalità caccia con barra RSSI
   - Interfaccia Web per controllo remoto e profili
   - Salvataggio PCAP su microSD
   - Doppio click per funzioni rapide
*/

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ==================== PIN DEFINITION ====================
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

#define SD_CS          21          // microSD CS
#define BUTTON_PIN     0           // Pulsante PRG

// ==================== DISPLAY ====================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);

// ==================== WIFI ====================
Preferences preferences;
String wifiSSID = "LoRaCatcher";
String wifiPASS = "catcheratwork";
WebServer server(80);
DNSServer dnsServer;

// ==================== BANDE ====================
enum Band { BAND_LOW, BAND_HIGH };
Band selectedBand = BAND_LOW;

#define LOW_BAND_START   433.0   // MHz
#define LOW_BAND_END     510.0
#define HIGH_BAND_START  863.0
#define HIGH_BAND_END    923.0
float freqStep = 1.0;     // MHz tra una frequenza e l'altra (configurabile)

// ==================== STRUTTURE DATI ====================
struct LoRaChannel {
  long freq;   // Hz
  int  sf;     // Spreading Factor
  long bw;     // Hz
  int  cr;     // Coding Rate (5=4/5, 6=4/6, 7=4/7, 8=4/8)
  int  syncWord;
  bool invertIQ;
};

// Profili Custom gestiti via web
#define MAX_CUSTOM_PROFILES 20
LoRaChannel customProfiles[MAX_CUSTOM_PROFILES];
int numCustomProfiles = 0;

// Profilo manuale per hunt diretto
LoRaChannel manualProfile;

// ==================== PARAMETRI SCANSIONE BASE ====================
const int SF_LIST[] = {7, 8, 9, 10, 11, 12};
const int NUM_SF = 6;
const long BW_LIST[] = {62500, 125000, 250000, 500000};
const int NUM_BW = 4;
const int CR_LIST[] = {5, 6, 7, 8};  // 4/5, 4/6, 4/7, 4/8
const int NUM_CR = 4;
const int SYNC_LIST[] = {0x12, 0x34};
const int NUM_SYNC = 2;
const bool IQ_LIST[] = {false, true};
const int NUM_IQ = 2;

const int DWELL_TIME = 200;  // ms per canale in auto-scan

int TOTAL_PROFILES = 0;
long baseFreqs[800]; // Max 800 frequenze base per supportare step di 0.1MHz
int numBaseFreqs = 0;

struct DiscoveredDevice {
  LoRaChannel ch;
  int packetCount;
};
#define MAX_DISCOVERED 100
DiscoveredDevice discovered[MAX_DISCOVERED];
int discoveredCount = 0;

// ==================== STATI APPLICAZIONE ====================
enum AppState { BAND_SELECT, SCAN, SELECT, HUNT };
AppState state = BAND_SELECT;

int currentChannelIndex = 0; // -1 indica profilo manuale
bool autoScan = true;
unsigned long channelEnteredTime = 0;
unsigned long packetCountTotal = 0;
unsigned long packetCountChannel = 0;
int selectedItem = 0;

// ==================== GESTIONE PULSANTE ====================
enum PressType { NONE, SINGLE_CLICK, DOUBLE_CLICK, LONG_PRESS };
PressType currentPress = NONE;

bool lastRawState = HIGH;
unsigned long lastButtonStableTime = 0;
bool buttonStableState = HIGH;
bool buttonPrevStable = HIGH;
unsigned long pressStartTime = 0;
bool longPressDetected = false;
bool pendingSingleClick = false;
unsigned long pendingClickTime = 0;

const int DEBOUNCE_MS = 20;
const int LONG_PRESS_MS = 800;
const int DOUBLE_CLICK_INTERVAL = 250;

// ==================== FILE PCAP ====================
const char* pcapFileName = "/lora_bonifica.pcap";
bool sdCardPresent = false;

// ==================== PROTOTIPI ====================
void setupDisplay();
void setupLoRa();
void setupWiFi();
void activateBand(Band band);
void generateProfiles();
void applyChannel(int index);
int  readInstantRSSI();
void writePcapGlobalHeader();
void writePcapPacket(int rssi, int snr, long freq, int sf, long bw, int cr, uint8_t* payload, size_t len);
void handleButton();
void processPress(PressType press);

void drawBandSelect();
void drawScanState(int rssi);
void drawSelectState();
void drawHuntState(int rssi);

void enterSelect();
void enterHunt(int profileIndex);
void enterManualHunt(long freq, int sf, long bw, int cr, int syncWord, int iq);
void resetDiscovery();

// Web handlers
void handleRoot();
void handleStatus();
void handleAddProfile();
void handleRemoveProfile();
void handleManualHunt();
void handleSetBand();
void handleStartScan();
void handleStopScan();
void handleDownload();
void handleProfilesTxt();
void handleSetWiFi();
String generateWebPage();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║   LoRaCatcher by MacRF      ║");
  Serial.println("║   WiFi LoRa 32 (V2)         ║");
  Serial.println("╚══════════════════════════════╝");

  setupDisplay();
  setupWiFi();

  // Inizializza SD
  if (!SD.begin(SD_CS)) {
    Serial.println("⚠ SD non rilevata. Logging disabilitato.");
    sdCardPresent = false;
  } else {
    Serial.println("✓ SD card pronta");
    sdCardPresent = true;
    writePcapGlobalHeader();
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Inizializza stato pulsante per evitare falsi trigger
  lastRawState = digitalRead(BUTTON_PIN);
  buttonStableState = lastRawState;
  lastButtonStableTime = millis();

  setupLoRa();

  // Stato iniziale: selezione banda
  state = BAND_SELECT;
  selectedBand = BAND_LOW;
  
  Serial.println("Seleziona la banda sul display o via web...");
}

// ==================== LOOP ====================
void loop() {
  server.handleClient();
  dnsServer.processNextRequest();

  // Lettura pulsante con debounce e riconoscimento click
  handleButton();

  // Se siamo in selezione banda
  if (state == BAND_SELECT) {
    drawBandSelect();
    
    // Pressione breve: cambia banda
    if (currentPress == SINGLE_CLICK) {
      currentPress = NONE;
      selectedBand = (selectedBand == BAND_LOW) ? BAND_HIGH : BAND_LOW;
      Serial.printf("Banda cambiata a: %s\n", selectedBand == BAND_LOW ? "LOW" : "HIGH");
    }
    
    // Pressione lunga: conferma
    if (currentPress == LONG_PRESS) {
      currentPress = NONE;
      Serial.println("Banda confermata manualmente");
      activateBand(selectedBand);
    }
    
    delay(10);
    return;
  }

  // ===== STATI OPERATIVI =====
  
  // Leggi RSSI istantaneo
  int rssi = readInstantRSSI();

  // Ricezione pacchetto LoRa
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    int packetRssi = LoRa.packetRssi();
    int snr = LoRa.packetSnr();
    uint8_t payload[256];
    int len = 0;
    while (LoRa.available() && len < 256) {
      payload[len++] = (uint8_t)LoRa.read();
    }
    packetCountTotal++;
    packetCountChannel++;
    
    LoRaChannel currentCh = (currentChannelIndex == -1) ? manualProfile : getProfile(currentChannelIndex);
    
    if (currentChannelIndex != -1) {
      registerDiscovered(currentCh);
    }

    Serial.printf("📦 CH%d RSSI:%d SNR:%d LEN:%d\n", 
                  currentChannelIndex, packetRssi, snr, len);

    if (sdCardPresent) {
      writePcapPacket(packetRssi, snr, currentCh.freq, currentCh.sf, currentCh.bw, currentCh.cr, payload, len);
    }
  }

  // Processa comandi pulsante
  processPress(currentPress);
  currentPress = NONE;

  // Auto-scansione
  if (state == SCAN && autoScan && currentChannelIndex >= 0 && millis() - channelEnteredTime > DWELL_TIME) {
    currentChannelIndex = (currentChannelIndex + 1) % TOTAL_PROFILES;
    applyChannel(currentChannelIndex);
    channelEnteredTime = millis();
  }

  // Aggiorna display
  switch (state) {
    case SCAN:   drawScanState(rssi); break;
    case SELECT: drawSelectState();   break;
    case HUNT:   drawHuntState(rssi); break;
  }
  
  delay(10);
}

// ==================== ATTIVAZIONE BANDA ====================
void activateBand(Band band) {
  selectedBand = band;
  
  generateProfiles();

  discoveredCount = 0;

  Serial.printf("✓ Banda %s attivata\n", band == BAND_LOW ? "LOW" : "HIGH");
  Serial.printf("  Profili totali: %d\n", TOTAL_PROFILES);

  currentChannelIndex = 0;
  packetCountTotal = 0;
  packetCountChannel = 0;
  autoScan = true;
  state = SCAN;
  applyChannel(0);
  channelEnteredTime = millis();
}

// ==================== GENERAZIONE PROFILI ====================
void generateProfiles() {
  numBaseFreqs = 0;
  float start, end;
  if (selectedBand == BAND_LOW) {
    start = LOW_BAND_START;
    end = LOW_BAND_END;
  } else {
    start = HIGH_BAND_START;
    end = HIGH_BAND_END;
  }

  int freqs = (int)((end - start) / freqStep) + 1;
  for (int f = 0; f < freqs && numBaseFreqs < 790; f++) {
    baseFreqs[numBaseFreqs++] = (long)((start + f * freqStep) * 1000000);
  }
  
  if (selectedBand == BAND_HIGH) {
    // Canali LoRaWAN (868.1, 868.3, 868.5)
    long extras[] = {868100000, 868300000, 868500000};
    for (int i=0; i<3; i++) {
      baseFreqs[numBaseFreqs++] = extras[i];
    }
  }

  int combs = NUM_SF * NUM_BW * NUM_CR * NUM_SYNC * NUM_IQ;
  TOTAL_PROFILES = (numBaseFreqs * combs) + numCustomProfiles;
}

LoRaChannel getProfile(int index) {
  if (index >= (TOTAL_PROFILES - numCustomProfiles)) {
    return customProfiles[index - (TOTAL_PROFILES - numCustomProfiles)];
  }
  int combPerFreq = NUM_SF * NUM_BW * NUM_CR * NUM_SYNC * NUM_IQ;
  int freqIdx = index / combPerFreq;
  int rem = index % combPerFreq;
  
  int sfIdx = rem / (NUM_BW * NUM_CR * NUM_SYNC * NUM_IQ); 
  rem = rem % (NUM_BW * NUM_CR * NUM_SYNC * NUM_IQ);
  
  int bwIdx = rem / (NUM_CR * NUM_SYNC * NUM_IQ); 
  rem = rem % (NUM_CR * NUM_SYNC * NUM_IQ);
  
  int crIdx = rem / (NUM_SYNC * NUM_IQ); 
  rem = rem % (NUM_SYNC * NUM_IQ);
  
  int syncIdx = rem / NUM_IQ; 
  int iqIdx = rem % NUM_IQ;
  
  LoRaChannel ch;
  ch.freq = baseFreqs[freqIdx];
  ch.sf = SF_LIST[sfIdx];
  ch.bw = BW_LIST[bwIdx];
  ch.cr = CR_LIST[crIdx];
  ch.syncWord = SYNC_LIST[syncIdx];
  ch.invertIQ = IQ_LIST[iqIdx];
  
  return ch;
}

bool isSameChannel(LoRaChannel a, LoRaChannel b) {
  return (a.freq == b.freq && a.sf == b.sf && a.bw == b.bw && a.cr == b.cr && a.syncWord == b.syncWord && a.invertIQ == b.invertIQ);
}

void registerDiscovered(LoRaChannel ch) {
  for (int i = 0; i < discoveredCount; i++) {
    if (isSameChannel(discovered[i].ch, ch)) {
      discovered[i].packetCount++;
      return;
    }
  }
  if (discoveredCount < MAX_DISCOVERED) {
    discovered[discoveredCount].ch = ch;
    discovered[discoveredCount].packetCount = 1;
    discoveredCount++;
  }
}

// ==================== APPLICA CANALE ====================
void setupLoRaChannel(LoRaChannel ch) {
  LoRa.setFrequency(ch.freq);
  LoRa.setSpreadingFactor(ch.sf);
  LoRa.setSignalBandwidth(ch.bw);
  LoRa.setCodingRate4(ch.cr);
  LoRa.setSyncWord(ch.syncWord);
  if (ch.invertIQ) {
    LoRa.enableInvertIQ();
  } else {
    LoRa.disableInvertIQ();
  }
}

void applyChannel(int index) {
  if (index == -1) {
    setupLoRaChannel(manualProfile);
    currentChannelIndex = -1;
    packetCountChannel = 0;
    return;
  }

  if (index < 0 || index >= TOTAL_PROFILES) return;
  
  setupLoRaChannel(getProfile(index));
  currentChannelIndex = index;
  packetCountChannel = 0;
}

// ==================== RSSI ISTANTANEO ====================
int readInstantRSSI() {
  digitalWrite(PIN_LORA_CS, LOW);
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  SPI.transfer(0x1A & 0x7F);
  uint8_t raw = SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(PIN_LORA_CS, HIGH);

  long freq = 868000000;
  if (currentChannelIndex >= 0 && currentChannelIndex < TOTAL_PROFILES) {
    freq = getProfile(currentChannelIndex).freq;
  } else if (currentChannelIndex == -1) {
    freq = manualProfile.freq;
  }
  
  return (freq >= 779000000) ? -157 + (int)raw : -164 + (int)raw;
}

// ==================== GESTIONE PULSANTE ====================
void handleButton() {
  bool raw = digitalRead(BUTTON_PIN);
  
  if (raw != lastRawState) {
    lastButtonStableTime = millis();
    lastRawState = raw;
  }
  
  if (millis() - lastButtonStableTime >= DEBOUNCE_MS) {
    if (raw != buttonStableState) {
      buttonPrevStable = buttonStableState;
      buttonStableState = raw;

      if (buttonStableState == LOW) {
        pressStartTime = millis();
        longPressDetected = false;
      } else {
        if (!longPressDetected) {
          unsigned long now = millis();
          if (pendingSingleClick && (now - pendingClickTime < DOUBLE_CLICK_INTERVAL)) {
            pendingSingleClick = false;
            currentPress = DOUBLE_CLICK;
          } else {
            pendingSingleClick = true;
            pendingClickTime = now;
          }
        }
      }
    }
  }

  // Rilevamento pressione lunga
  if (buttonStableState == LOW && !longPressDetected) {
    if (millis() - pressStartTime >= LONG_PRESS_MS) {
      longPressDetected = true;
      currentPress = LONG_PRESS;
      pendingSingleClick = false;
    }
  }

  // Timeout per singolo click
  if (pendingSingleClick && (millis() - pendingClickTime >= DOUBLE_CLICK_INTERVAL)) {
    if (buttonStableState == HIGH) {
      pendingSingleClick = false;
      if (currentPress == NONE) {
        currentPress = SINGLE_CLICK;
      }
    }
  }
}

// ==================== MACCHINA A STATI ====================
void processPress(PressType press) {
  if (press == NONE) return;

  switch (state) {
    case SCAN:
      if (press == SINGLE_CLICK) {
        // Vai alla lista dispositivi
        autoScan = false;
        enterList();
      }
      else if (press == DOUBLE_CLICK) {
        // Reset e riavvia scansione
        resetDiscovery();
        state = SCAN;
        autoScan = true;
        if (currentChannelIndex == -1) currentChannelIndex = 0;
        channelEnteredTime = millis();
      }
      else if (press == LONG_PRESS) {
        // Toggle auto-scan
        autoScan = !autoScan;
        if (autoScan) channelEnteredTime = millis();
      }
      break;

    case SELECT:
      if (press == SINGLE_CLICK) {
        // Scorri lista
        if (discoveredCount > 0) {
          selectedItem = (selectedItem + 1) % discoveredCount;
        }
      }
      else if (press == DOUBLE_CLICK) {
        // Torna a scansione
        state = SCAN;
        autoScan = true;
        if (currentChannelIndex == -1) currentChannelIndex = 0;
        channelEnteredTime = millis();
      }
      else if (press == LONG_PRESS) {
        // Conferma caccia su elemento
        if (discoveredCount > 0) {
          enterHunt(selectedItem);
        } else {
          state = SCAN;
          autoScan = true;
          channelEnteredTime = millis();
        }
      }
      break;

    case HUNT:
      if (press == SINGLE_CLICK) {
        // Torna alla lista
        enterList();
      }
      else if (press == DOUBLE_CLICK) {
        // Torna a scansione
        state = SCAN;
        autoScan = true;
        if (currentChannelIndex == -1) currentChannelIndex = 0;
        channelEnteredTime = millis();
      }
      else if (press == LONG_PRESS) {
        // Reset completo e nuova scansione
        resetDiscovery();
        state = SCAN;
        autoScan = true;
        if (currentChannelIndex == -1) currentChannelIndex = 0;
        channelEnteredTime = millis();
      }
      break;
  }
}

// ==================== TRANSIZIONI ====================
void enterList() {
  state = SELECT;
  selectedItem = 0;
  Serial.printf("Lista: %d dispositivi trovati\n", discoveredCount);
}

void enterHunt(int discoveredIndex) {
  applyChannel(0); // Dummy for implementation sake, usually profile search
  state = HUNT;
  LoRaChannel ch = discovered[discoveredIndex].ch;
  setupLoRaChannel(ch);
  Serial.printf("Caccia: %.1f MHz SF%d BW%.0fk\n", ch.freq/1e6, ch.sf, ch.bw/1e3);
}

void enterManualHunt(long freq, int sf, long bw, int cr, int syncWord, int iq) {
  manualProfile.freq = freq;
  manualProfile.sf = sf;
  manualProfile.bw = bw;
  manualProfile.cr = cr;
  manualProfile.syncWord = syncWord;
  manualProfile.invertIQ = (iq == 1);
  state = HUNT;
  applyChannel(-1);
  packetCountChannel = 0;
  Serial.printf("CACCIA MANUALE: %.1f MHz SF%d\n", freq / 1000000.0, sf);
  drawHuntState(readInstantRSSI());
}

void resetDiscovery() {
  for (int i = 0; i < TOTAL_PROFILES; i++) {
    discovered[i].packetCount = 0;
  }
  packetCountTotal = 0;
  Serial.println("Dati azzerati");
}

// ==================== DISPLAY ====================
void drawBandSelect() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Titolo
  display.setCursor(10, 0);
  display.print("SELEZIONA BANDA");
  
  // Linea separatrice
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  
  // Opzione LOW
  display.setCursor(5, 16);
  if (selectedBand == BAND_LOW) {
    display.print(">");
  } else {
    display.print(" ");
  }
  display.print("LOW  433-510 MHz");
  
  // Opzione HIGH
  display.setCursor(5, 28);
  if (selectedBand == BAND_HIGH) {
    display.print(">");
  } else {
    display.print(" ");
  }
  display.print("HIGH 863-923 MHz");
  
  // Istruzioni in basso
  display.drawLine(0, 40, 127, 40, SSD1306_WHITE);
  display.setCursor(0, 44);
  display.print("Breve: cambia");
  display.setCursor(0, 54);
  display.print("Lunga: Conferma");
  
  display.display();
}

void drawScanState(int rssi) {
  display.clearDisplay();
  
  // Barra RSSI
  int bar = map(rssi, -140, -30, 0, 80);
  bar = constrain(bar, 0, 80);
  display.fillRect(0, 0, bar, 10, SSD1306_WHITE);
  display.drawRect(0, 0, 80, 10, SSD1306_WHITE);
  
  // Testo RSSI fuori dalla barra per evitare sovrapposizioni
  display.setCursor(84, 1);
  display.printf("%ddBm", rssi);

  // Info canale
  display.setCursor(0, 12);
  if (currentChannelIndex == -1) {
    display.printf("CH MANUALE   %s", autoScan ? "AUTO" : "MAN");
  } else {
    display.printf("CH %d/%d %s", currentChannelIndex + 1, TOTAL_PROFILES, autoScan ? "AUTO" : "MAN");
  }
  
  LoRaChannel ch = (currentChannelIndex == -1) ? manualProfile : getProfile(currentChannelIndex);
  
  // Frequenza e SF
  display.setCursor(0, 23);
  display.printf("F: %.1fM SF: %d", ch.freq / 1e6, ch.sf);
  
  // BW e CR
  display.setCursor(0, 34);
  display.printf("B:%dk C:4/%d %s%s", (int)(ch.bw / 1000), ch.cr, ch.syncWord==0x34?"LW":"", ch.invertIQ?"I":"");
  
  // Statistiche
  display.setCursor(0, 45);
  display.printf("Pk ch:%u Tot:%u", (unsigned)packetCountChannel, (unsigned)packetCountTotal);
  
  // Dispositivi trovati
  int devFound = discoveredCount;
  display.setCursor(0, 55);
  display.printf("Dispositivi: %d", devFound);
  
  display.display();
}

void drawSelectState() {
  display.clearDisplay();

  // Titolo
  display.setCursor(0, 0);
  display.printf("DISPOSITIVI: %d", discoveredCount);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  
  if (discoveredCount == 0) {
    display.setCursor(10, 25);
    display.print("Nessun dispositivo");
    display.setCursor(10, 40);
    display.print("Lunga: nuova scan");
    display.display();
    return;
  }

  // Mostra fino a 6 elementi (avendo rimosso la legenda)
  int start = max(0, selectedItem - 2);
  int end = min(discoveredCount, start + 6);
  if (end - start < 6) start = max(0, end - 6);

  for (int i = start; i < end; i++) {
    int y = 11 + (i - start) * 9;
    LoRaChannel ch = discovered[i].ch;
    
    if (i == selectedItem) {
      display.fillRect(0, y - 1, 128, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    
    display.setCursor(2, y);
    display.printf("%.1fM S%d %dp%s%s", ch.freq / 1e6, ch.sf, discovered[i].packetCount, ch.syncWord==0x34?"W":"", ch.invertIQ?"I":"");
  }
  
  display.display();
}

void drawHuntState(int rssi) {
  display.clearDisplay();
  
  // Barra RSSI grande
  int bar = map(rssi, -140, -30, 0, 80);
  bar = constrain(bar, 0, 80);
  display.fillRect(0, 0, bar, 10, SSD1306_WHITE);
  display.drawRect(0, 0, 80, 10, SSD1306_WHITE);
  display.setCursor(84, 1);
  display.printf("%ddBm", rssi);

  LoRaChannel ch = (currentChannelIndex == -1) ? manualProfile : getProfile(currentChannelIndex);
  
  // Titolo caccia
  display.setCursor(0, 15);
  display.printf("CACCIA %.1f MHz", ch.freq / 1e6);
  
  // Parametri
  display.setCursor(0, 30);
  display.printf("SF:%d BW:%dk CR:4/%d", ch.sf, (int)(ch.bw / 1000), ch.cr);
  display.setCursor(0, 40);
  display.printf("Sy:0x%02X IQ:%s", ch.syncWord, ch.invertIQ ? "Inv" : "Nrm");
  
  // Statistiche
  display.setCursor(0, 50);
  display.printf("Pk ch:%u Tot:%u", (unsigned)packetCountChannel, (unsigned)packetCountTotal);
  
  display.display();
}

// ==================== PCAP ====================
void writePcapGlobalHeader() {
  if (!sdCardPresent) return;
  
  File f = SD.open(pcapFileName, FILE_WRITE);
  if (!f) return;
  
  uint32_t magic = 0xa1b2c3d4;
  f.write((uint8_t*)&magic, 4);
  
  uint16_t v_maj = 2, v_min = 4;
  f.write((uint8_t*)&v_maj, 2);
  f.write((uint8_t*)&v_min, 2);
  
  uint32_t tz = 0, sigfigs = 0, snaplen = 65535, network = 273;
  f.write((uint8_t*)&tz, 4);
  f.write((uint8_t*)&sigfigs, 4);
  f.write((uint8_t*)&snaplen, 4);
  f.write((uint8_t*)&network, 4);
  
  f.close();
}

void writePcapPacket(int rssi, int snr, long freq, int sf, long bw, int cr, 
                     uint8_t* payload, size_t len) {
  if (!sdCardPresent) return;
  
  File f = SD.open(pcapFileName, FILE_APPEND);
  if (!f) return;

  uint32_t bitmask = (1<<0) | (1<<1) | (1<<2) | (1<<3) | (1<<4) | (1<<5);
  uint8_t hdr[64];
  int off = 0;
  
  hdr[off++] = 0;  // version
  hdr[off++] = 0;  // reserved
  off += 2;        // spazio header_length
  
  memcpy(hdr + off, &bitmask, 4); off += 4;
  
  uint32_t fq = (uint32_t)freq;
  memcpy(hdr + off, &fq, 4); off += 4;
  
  hdr[off++] = (uint8_t)sf;
  
  uint8_t bw_idx = (bw == 125000) ? 0 : (bw == 250000) ? 1 : 2;
  hdr[off++] = bw_idx;
  
  hdr[off++] = (uint8_t)cr;
  hdr[off++] = (int8_t)rssi;
  hdr[off++] = (int8_t)(snr * 4);
  
  uint16_t hlen = off;
  hdr[2] = hlen & 0xFF;
  hdr[3] = (hlen >> 8) & 0xFF;

  uint32_t now = millis();
  uint32_t ts_sec = now / 1000;
  uint32_t ts_us = (now % 1000) * 1000;
  uint32_t pktlen = hlen + len;
  
  f.write((uint8_t*)&ts_sec, 4);
  f.write((uint8_t*)&ts_us, 4);
  f.write((uint8_t*)&pktlen, 4);
  f.write((uint8_t*)&pktlen, 4);
  f.write(hdr, hlen);
  if (len > 0) f.write(payload, len);
  
  f.close();
}

// ==================== WEB SERVER ====================
void setupWiFi() {
  preferences.begin("loracatcher", false);
  wifiSSID = preferences.getString("ssid", "LoRaCatcher");
  wifiPASS = preferences.getString("pass", "catcheratwork");
  freqStep = preferences.getFloat("step", 1.0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(wifiSSID.c_str(), wifiPASS.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/addprofile", handleAddProfile);
  server.on("/removeprofile", handleRemoveProfile);
  server.on("/manualhunt", handleManualHunt);
  server.on("/setband", handleSetBand);
  server.on("/startscan", handleStartScan);
  server.on("/stopscan", handleStopScan);
  server.on("/download", handleDownload);
  server.on("/profiles.txt", handleProfilesTxt);
  server.on("/setwifi", handleSetWiFi);
  server.onNotFound(handleRoot);
  
  server.begin();
}

void handleSetWiFi() {
  if (server.hasArg("s") && server.hasArg("p") && server.hasArg("st")) {
    String newSsid = server.arg("s");
    String newPass = server.arg("p");
    float newStep = server.arg("st").toFloat();
    
    if (newSsid.length() > 0 && newPass.length() >= 8 && newStep >= 0.1) {
      preferences.putString("ssid", newSsid);
      preferences.putString("pass", newPass);
      preferences.putFloat("step", newStep);
      String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{background:#121212;color:#00e676;font-family:sans-serif;text-align:center;padding:50px;}</style></head><body>";
      html += "<h2>Configurazione Salvata! Riavvio in corso...</h2><p>Collegati alla rete <b>" + newSsid + "</b> e ricarica la pagina.</p></body></html>";
      server.send(200, "text/html", html);
      delay(1000);
      ESP.restart();
      return;
    }
  }
  server.send(200, "text/html", "Errore: La password deve essere di almeno 8 caratteri. <a href='/'>Indietro</a>");
}

void handleRoot() {
  String html = generateWebPage();
  server.send(200, "text/html", html);
}

void handleStatus() {
  String json = "{";
  json += "\"state\":\"" + String(state) + "\",";
  json += "\"band\":\"" + String(selectedBand == BAND_LOW ? "LOW" : "HIGH") + "\",";
  json += "\"channel\":" + String(currentChannelIndex) + ",";
  json += "\"totalChannels\":" + String(TOTAL_PROFILES) + ",";
  json += "\"packets\":" + String(packetCountTotal) + ",";
  json += "\"autoScan\":" + String(autoScan ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleAddProfile() {
  if (numCustomProfiles < MAX_CUSTOM_PROFILES && server.hasArg("f") && server.hasArg("sf") && server.hasArg("bw") && server.hasArg("cr") && server.hasArg("sy") && server.hasArg("iq")) {
    float f = server.arg("f").toFloat();
    int sf = server.arg("sf").toInt();
    long bw = server.arg("bw").toInt();
    int cr = server.arg("cr").toInt();
    int sy = server.arg("sy").toInt();
    int iq = server.arg("iq").toInt();
    
    if (f >= 400 && f <= 930) {
      customProfiles[numCustomProfiles].freq = (long)(f * 1000000);
      customProfiles[numCustomProfiles].sf = sf;
      customProfiles[numCustomProfiles].bw = bw;
      customProfiles[numCustomProfiles].cr = cr;
      customProfiles[numCustomProfiles].syncWord = sy;
      customProfiles[numCustomProfiles].invertIQ = (iq == 1);
      numCustomProfiles++;
    }
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleRemoveProfile() {
  if (server.hasArg("i")) {
    int idx = server.arg("i").toInt();
    if (idx >= 0 && idx < numCustomProfiles) {
      for (int i = idx; i < numCustomProfiles - 1; i++) {
        customProfiles[i] = customProfiles[i+1];
      }
      numCustomProfiles--;
    }
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleManualHunt() {
  if (server.hasArg("f") && server.hasArg("sf") && server.hasArg("bw") && server.hasArg("cr") && server.hasArg("sy") && server.hasArg("iq")) {
    float f = server.arg("f").toFloat();
    int sf = server.arg("sf").toInt();
    long bw = server.arg("bw").toInt();
    int cr = server.arg("cr").toInt();
    int sy = server.arg("sy").toInt();
    int iq = server.arg("iq").toInt();
    enterManualHunt((long)(f * 1000000), sf, bw, cr, sy, iq);
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSetBand() {
  if (server.hasArg("b")) {
    String b = server.arg("b");
    Band target = (b == "HIGH") ? BAND_HIGH : BAND_LOW;
    if (state == BAND_SELECT || !autoScan) {
      activateBand(target);
    }
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleStartScan() {
  if (state != BAND_SELECT) {
    autoScan = true;
    state = SCAN;
    if (currentChannelIndex == -1) currentChannelIndex = 0;
    channelEnteredTime = millis();
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleStopScan() {
  autoScan = false;
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleDownload() {
  if (sdCardPresent && SD.exists(pcapFileName)) {
    File file = SD.open(pcapFileName, FILE_READ);
    server.streamFile(file, "application/octet-stream");
    file.close();
  } else {
    server.send(404, "text/plain", "File non trovato");
  }
}

void handleProfilesTxt() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  
  String chunk = "Profili attualmente in scansione (" + String(TOTAL_PROFILES) + " totali):\n\n";
  server.sendContent(chunk);
  
  for (int i = 0; i < TOTAL_PROFILES; i++) {
    LoRaChannel ch = getProfile(i);
    chunk = "CH" + String(i+1) + ": ";
    chunk += String(ch.freq / 1000000.0, 3) + " MHz, ";
    chunk += "SF" + String(ch.sf) + ", ";
    chunk += "BW" + String(ch.bw / 1000) + "k, ";
    chunk += "CR4/" + String(ch.cr) + ", ";
    chunk += "Sync0x" + String(ch.syncWord, HEX) + ", ";
    chunk += ch.invertIQ ? "IQ:Inv" : "IQ:Norm";
    if (i >= TOTAL_PROFILES - numCustomProfiles) {
      chunk += " (CUSTOM)";
    }
    chunk += "\n";
    server.sendContent(chunk);
  }
  server.sendContent("");
}

String generateWebPage() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>LoRa Bonifica</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; }";
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 15px; background: #121212; color: #e0e0e0; display: flex; flex-direction: column; align-items: center; }";
  html += "h1 { color: #00e676; font-size: 22px; margin: 5px 0 15px 0; text-align: center; text-shadow: 0 0 10px rgba(0,230,118,0.3); }";
  html += "h2 { color: #00e676; font-size: 15px; margin: 0 0 12px 0; border-bottom: 1px solid #333; padding-bottom: 6px; }";
  html += ".container { width: 100%; max-width: 800px; display: grid; gap: 15px; grid-template-columns: 1fr; }";
  html += "@media(min-width: 600px) { .container { grid-template-columns: 1fr 1fr; } }";
  html += ".card { background: linear-gradient(145deg, #1e1e1e, #1a1a1a); padding: 15px; border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.4); border: 1px solid #2a2a2a; }";
  html += ".card.full { grid-column: 1 / -1; }";
  html += "p { margin: 5px 0; font-size: 13px; color: #ccc; }";
  html += "b { color: #fff; }";
  html += "button { background: linear-gradient(145deg, #00e676, #00c853); color: #121212; border: none; padding: 10px 15px; border-radius: 8px; font-weight: bold; cursor: pointer; transition: all 0.3s ease; width: 100%; margin-top: 8px; font-size: 13px; box-shadow: 0 4px 10px rgba(0,230,118,0.2); }";
  html += "button:hover { filter: brightness(1.1); transform: translateY(-1px); box-shadow: 0 6px 15px rgba(0,230,118,0.3); }";
  html += ".btn-group { display: flex; gap: 8px; }";
  html += ".btn-group button { flex: 1; margin-top: 0; }";
  html += ".btn-danger { background: linear-gradient(145deg, #ff5252, #e53935); color: #fff; box-shadow: 0 4px 10px rgba(255,82,82,0.2); }";
  html += ".btn-danger:hover { box-shadow: 0 6px 15px rgba(255,82,82,0.3); }";
  html += ".form-group { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; }";
  html += ".form-group label { font-size: 13px; color: #aaa; flex: 1; }";
  html += ".form-group input, .form-group select { width: 55%; padding: 8px; border-radius: 8px; border: 1px solid #444; background: #222; color: #fff; font-size: 13px; transition: 0.3s; }";
  html += ".form-group input:focus, .form-group select:focus { border-color: #00e676; outline: none; box-shadow: 0 0 5px rgba(0,230,118,0.3); }";
  html += ".prof-item { background: #222; padding: 10px 12px; margin-bottom: 8px; border-radius: 8px; font-size: 13px; display: flex; justify-content: space-between; align-items: center; border-left: 3px solid #00e676; }";
  html += ".prof-item a { color: #ff5252; text-decoration: none; font-weight: bold; font-size: 16px; padding: 0 8px; transition: 0.2s; }";
  html += ".prof-item a:hover { color: #ff1744; transform: scale(1.1); }";
  html += "details { margin-bottom: 10px; }";
  html += "summary { font-size: 15px; font-weight: bold; color: #00e676; cursor: pointer; outline: none; margin-bottom: 10px; transition: 0.2s; }";
  html += "summary:hover { text-shadow: 0 0 8px rgba(0,230,118,0.4); }";
  html += "@media(max-width: 500px) { .form-group { flex-direction: column; align-items: stretch; gap: 5px; margin-bottom:15px; } .form-group input, .form-group select { width: 100%; } }";
  html += "</style></head><body>";
  
  html += "<h1>&#128269; LoRaCatcher <span style='font-size:12px; color:#888; font-weight:normal;'>by MacRF</span></h1>";
  html += "<div class='container'>";
  
  html += "<div class='card'>";
  html += "<h2>Stato Sistema</h2>";
  String stMode = (state == BAND_SELECT) ? "Selezione Banda" : ((state == SCAN) ? (autoScan ? "Scan Auto" : "Scan Fermo") : ((state == HUNT) ? "Caccia" : "Lista"));
  html += "<p>Stato: <b>" + stMode + "</b></p>";
  html += "<p>Banda: <b>" + String(selectedBand == BAND_LOW ? "LOW (433-510)" : "HIGH (863-923)") + "</b></p>";
  if (state != BAND_SELECT) {
    int dispCh = (currentChannelIndex == -1) ? 0 : (currentChannelIndex + 1);
    html += "<p>Canale: <b>" + String(dispCh) + "/" + String(TOTAL_PROFILES) + "</b></p>";
    html += "<p>Pacchetti: <b>" + String(packetCountTotal) + "</b></p>";
  }
  html += "</div>";
  
  html += "<div class='card'>";
  html += "<h2>Controllo</h2>";
  if (state != BAND_SELECT) {
    html += "<div class='btn-group'>";
    html += "<button onclick=\"location.href='/startscan'\">&#9654; Avvia</button>";
    html += "<button class='btn-danger' onclick=\"location.href='/stopscan'\">&#9646;&#9646; Ferma</button>";
    html += "</div>";
  }
  if (state == BAND_SELECT || !autoScan) {
    html += "<p style='font-size:11px; margin-top:10px;'>Cambio banda consentito a scan fermo:</p>";
    html += "<form action='/setband' method='GET' style='display:flex; gap:8px;'>";
    html += "<select name='b' style='flex:2; padding:8px; border-radius:6px; background:#2c2c2c; color:#fff; border:1px solid #333;'>";
    html += "<option value='LOW' "+String(selectedBand==BAND_LOW?"selected":"")+">LOW (433-510)</option>";
    html += "<option value='HIGH' "+String(selectedBand==BAND_HIGH?"selected":"")+">HIGH (863-923)</option>";
    html += "</select>";
    html += "<button type='submit' style='flex:1; margin-top:0;'>Applica</button>";
    html += "</form>";
  }
  html += "<button style='margin-top:15px;' onclick=\"location.href='/download'\">&#128229; Scarica PCAP</button>";
  html += "</div>";
  
  html += "<div class='card full'>";
  html += "<h2>Dispositivi Rilevati (" + String(discoveredCount) + ")</h2>";
  if (discoveredCount > 0) {
    html += "<div style='display:grid; gap:8px; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));'>";
    for (int i = 0; i < discoveredCount; i++) {
      LoRaChannel ch = discovered[i].ch;
      html += "<div class='prof-item'>";
      html += "<span><b>" + String(ch.freq/1000000.0, 1) + "M</b> SF" + String(ch.sf) + " BW" + String(ch.bw/1000) + "k CR4/" + String(ch.cr) + (ch.syncWord == 0x34 ? " LWAN" : "") + (ch.invertIQ ? " IQI" : "") + "</span>";
      html += "<span style='color:#00e676;'><b>" + String(discovered[i].packetCount) + " pkts</b></span>";
      html += "</div>";
    }
    html += "</div>";
  } else {
    html += "<p>Nessun dispositivo rilevato finora.</p>";
  }
  html += "</div>";

  html += "<div class='card'>";
  html += "<details>";
  html += "<summary>Imposta Parametri RF Manuali</summary>";
  html += "<p style='font-size:11px; margin-bottom:10px; margin-top:0;'>Imposta al volo e vai in caccia:</p>";
  html += "<form action='/manualhunt' method='GET'>";
  html += "<div class='form-group'><label>Frequenza (MHz):</label><input type='number' name='f' placeholder='Es. 868.1' step='0.1' required></div>";
  html += "<div class='form-group'><label>Spreading Factor:</label><select name='sf'><option value='7'>SF7</option><option value='8'>SF8</option><option value='9'>SF9</option><option value='10'>SF10</option><option value='11'>SF11</option><option value='12'>SF12</option></select></div>";
  html += "<div class='form-group'><label>Bandwidth:</label><select name='bw'><option value='62500'>62.5k</option><option value='125000'>125k</option><option value='250000'>250k</option><option value='500000'>500k</option></select></div>";
  html += "<div class='form-group'><label>Coding Rate:</label><select name='cr'><option value='5'>4/5</option><option value='6'>4/6</option><option value='7'>4/7</option><option value='8'>4/8</option></select></div>";
  html += "<div class='form-group'><label>Sync Word:</label><select name='sy'><option value='18'>0x12 (Privata)</option><option value='52'>0x34 (LoRaWAN)</option></select></div>";
  html += "<div class='form-group'><label>Inversione IQ:</label><select name='iq'><option value='0'>Normale</option><option value='1'>Invertita</option></select></div>";
  html += "<button type='submit'>Caccia Manuale</button>";
  html += "</form>";
  html += "</details>";
  html += "</div>";
  
  html += "<div class='card'>";
  html += "<details>";
  html += "<summary>Profili Custom ("+String(numCustomProfiles)+"/"+String(MAX_CUSTOM_PROFILES)+")</summary>";
  html += "<form action='/addprofile' method='GET' style='margin-top:10px;'>";
  html += "<div class='form-group'><label>Frequenza (MHz):</label><input type='number' name='f' placeholder='Es. 433.9' step='0.1' required></div>";
  html += "<div class='form-group'><label>Spreading Factor:</label><select name='sf'><option value='7'>SF7</option><option value='8'>SF8</option><option value='9'>SF9</option><option value='10'>SF10</option><option value='11'>SF11</option><option value='12'>SF12</option></select></div>";
  html += "<div class='form-group'><label>Bandwidth:</label><select name='bw'><option value='62500'>62.5k</option><option value='125000'>125k</option><option value='250000'>250k</option><option value='500000'>500k</option></select></div>";
  html += "<div class='form-group'><label>Coding Rate:</label><select name='cr'><option value='5'>4/5</option><option value='6'>4/6</option><option value='7'>4/7</option><option value='8'>4/8</option></select></div>";
  html += "<div class='form-group'><label>Sync Word:</label><select name='sy'><option value='18'>0x12 (Privata)</option><option value='52'>0x34 (LoRaWAN)</option></select></div>";
  html += "<div class='form-group'><label>Inversione IQ:</label><select name='iq'><option value='0'>Normale</option><option value='1'>Invertita</option></select></div>";
  html += "<button type='submit'>Aggiungi Profilo</button>";
  html += "</form>";
  
  if (numCustomProfiles > 0) {
    html += "<div style='margin-top:15px; max-height:180px; overflow-y:auto; padding-right:5px;'>";
    for (int i = 0; i < numCustomProfiles; i++) {
      html += "<div class='prof-item'>";
      html += "<span><b>" + String(customProfiles[i].freq/1000000.0, 1) + "M</b> SF" + String(customProfiles[i].sf) + " BW" + String(customProfiles[i].bw/1000) + "k CR4/" + String(customProfiles[i].cr) + (customProfiles[i].syncWord == 0x34 ? " LWAN" : "") + (customProfiles[i].invertIQ ? " IQI" : "") + "</span>";
      html += "<a href='/removeprofile?i=" + String(i) + "' onclick='return confirm(\"Rimuovere questo profilo?\")'>&times;</a>";
      html += "</div>";
    }
    html += "</div>";
  } else {
    html += "<p style='font-size:12px; margin-top:15px; text-align:center;'>Nessun profilo inserito.</p>";
  }
  html += "</details>";
  html += "</div>";

  html += "<div class='card full'>";
  html += "<h2>Profili Base in Scansione (" + String(TOTAL_PROFILES) + " totali)</h2>";
  float s = (selectedBand == BAND_LOW) ? LOW_BAND_START : HIGH_BAND_START;
  float e = (selectedBand == BAND_LOW) ? LOW_BAND_END : HIGH_BAND_END;
  html += "<p>Frequenze da <b>" + String(s, 1) + "</b> a <b>" + String(e, 1) + " MHz</b> (step " + String(freqStep, 1) + " MHz).</p>";
  html += "<p>Per ogni frequenza sono testate tutte le combinazioni di SF (7-12), BW (62.5k, 125k, 250k, 500k), CR (4/5-4/8), SyncWord (0x12, 0x34) e InvertIQ (Norm/Inv).</p>";
  html += "<button onclick=\"window.open('/profiles.txt', '_blank')\">Vedi Lista Completa TXT</button>";
  html += "</div>";

  html += "<div class='card full'>";
  html += "<h2>Legenda Parametri RF</h2>";
  html += "<div style='font-size:12px; color:#bbb;'>";
  html += "<p><b style='color:#00e676;'>Frequenza (MHz):</b> Il canale radio. Deve combaciare precisamente. I canali 868.1, 868.3 e 868.5 sono i canali primari dello standard pubblico LoRaWAN.</p>";
  html += "<p><b style='color:#00e676;'>Spreading Factor (SF):</b> Durata del simbolo radio. <b>SF7</b> è veloce e consuma meno batteria (distanze brevi). <b>SF12</b> è lento ma penetra ostacoli garantendo portate di svariati chilometri.</p>";
  html += "<p><b style='color:#00e676;'>Bandwidth (BW):</b> Larghezza del canale radio. <b>500k</b> offre molta banda dati ma poco raggio. <b>125k o 62.5k</b> concentrano l'energia, aumentando enormemente la sensibilità e la distanza raggiungibile.</p>";
  html += "<p><b style='color:#00e676;'>Coding Rate (CR):</b> Ridondanza dei dati per correggere gli errori. Da 4/5 (minima protezione) a 4/8 (massima protezione contro le interferenze, ma trasmissione più lunga).</p>";
  html += "<p><b style='color:#00e676;'>Sync Word:</b> Firma hardware del pacchetto. <b>0x12</b> intercetta il 99% dei dispositivi privati fai-da-te o allarmi custom. <b>0x34</b> intercetta i dispositivi commerciali su reti LoRaWAN pubbliche (es. Helium, TTN).</p>";
  html += "<p><b style='color:#00e676;'>Inversione IQ:</b> Inverte la fase del segnale per evitare collisioni di rete. <b>Normale</b> intercetta i sensori che trasmettono. <b>Invertita</b> intercetta le eventuali risposte (downlink) dai Gateway verso i sensori.</p>";
  html += "</div>";
  html += "</div>";

  html += "<div class='card full'>";
  html += "<details>";
  html += "<summary>Impostazioni Sistema & WiFi</summary>";
  html += "<p style='font-size:11px; margin-bottom:10px; margin-top:0;'>Cambia impostazioni base (richiede riavvio).</p>";
  html += "<form action='/setwifi' method='GET'>";
  html += "<div class='form-group'><label>SSID WiFi:</label><input type='text' name='s' value='" + wifiSSID + "' required></div>";
  html += "<div class='form-group'><label>Password (min 8 car):</label><input type='text' name='p' value='" + wifiPASS + "' required minlength='8'></div>";
  html += "<div class='form-group'><label>Step Frequenza Scansione:</label><select name='st'>";
  html += "<option value='1.0' " + String(freqStep==1.0?"selected":"") + ">1.0 MHz (Veloce)</option>";
  html += "<option value='0.5' " + String(freqStep==0.5?"selected":"") + ">0.5 MHz (Medio)</option>";
  html += "<option value='0.1' " + String(freqStep<0.5?"selected":"") + ">0.1 MHz (Lento ma Preciso)</option>";
  html += "</select></div>";
  html += "<button type='submit'>Salva e Riavvia</button>";
  html += "</form>";
  html += "</details>";
  html += "</div>";

  html += "</div>"; // End container
  
  html += "<script>setTimeout(function(){location.reload();},5000);</script>";
  html += "</body></html>";
  return html;
}

// ==================== INIZIALIZZAZIONI ====================
void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERRORE: Display OLED non trovato!");
    while (1) delay(100);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void setupLoRa() {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(868000000)) {
    Serial.println("ERRORE: Modulo LoRa non rilevato!");
    while (1) delay(100);
  }
}
