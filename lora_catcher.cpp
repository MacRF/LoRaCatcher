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
#include <Update.h>
#include <sys/time.h>

// ==================== CONFIGURAZIONE SCHEDA ====================
// Scegli la tua scheda de-commentando (rimuovendo le doppie sbarre) SOLO UNA delle righe seguenti:
//#define BOARD_HELTEC_V2       // Heltec ESP32 LoRa V2
#define BOARD_LILYGO_T3_V1_6    // LilyGO T3 LoRa32 V1.6.1 (con slot MicroSD integrato)

// ==================== PIN DEFINITION ====================
#if defined(BOARD_LILYGO_T3_V1_6)
  // Pinout specifico per LilyGO T3 LoRa32 V1.6.1
  #define PIN_LORA_SCK   5
  #define PIN_LORA_MISO  19
  #define PIN_LORA_MOSI  27
  #define PIN_LORA_CS    18
  #define PIN_LORA_RST   23
  #define PIN_LORA_DIO0  26

  #define OLED_SDA       21
  #define OLED_SCL       22
  #define OLED_RST       -1
  #define OLED_ADDR      0x3C

  #define SD_SCK         14
  #define SD_MISO        2
  #define SD_MOSI        15
  #define SD_CS          13
  
  #define BUTTON_PIN     4   // Collega un pulsante fisico tra il PIN 4 e GND (Il PIN 4 è libero e supporta INPUT_PULLUP)
  #define BATTERY_PIN    35  // ADC Batteria (partitore di tensione interno)
  #define BATTERY_DIVIDER 2.0 // Ratio del partitore (Spesso 100k/100k su LilyGO)
  #define LED_PIN        25  // LED verde integrato
#else
  // Pinout specifico per Heltec ESP32 LoRa V2
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

  #define SD_CS          21
  
  #define BUTTON_PIN     0   // Sulla Heltec V2 usa il pulsante integrato "PRG" (GPIO 0)
  #define BATTERY_PIN    37  // ADC Batteria su Heltec (solitamente 37)
  #define BATTERY_EN_PIN 21  // Pin per abilitare la lettura batteria (Vext)
  #define BATTERY_DIVIDER 3.2 // Ratio del partitore su Heltec (220k/100k)
  #define LED_PIN        25  // LED bianco integrato su Heltec V2
#endif

#if defined(BOARD_LILYGO_T3_V1_6)
SPIClass sdSPI(HSPI);
#endif

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
const int CR_LIST[] = {5};  // 4/5 (il chip rileva automaticamente gli altri CR nell'header esplicito)
const int NUM_CR = 1;
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
int currentRssi = -140;
int savedScannerIndex = 0;

// ==================== STATI APPLICAZIONE ====================
enum AppState { BAND_SELECT, SCAN, SELECT, HUNT };
AppState state = BAND_SELECT;

unsigned long lastPacketTime = 0;

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
String pcapFileName = "/lora_bonifica.pcap";
bool timeSynced = false;
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
void openNewPcapFile();
void writePcapPacket(int rssi, int snr, long freq, int sf, long bw, int cr, int syncWord, uint8_t* payload, size_t len);void handleButton();
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
void handleRestartScan();
void handleStopScan();
void handleDownload();
void handleDelete();
void handleClearList();
void handleWebHunt();
void handleProfilesTxt();
void handleSetWiFi();
String generateWebPage();

int getBatteryPercentage() {
  static unsigned long lastBatReadTime = 0;
  static float filteredVoltage = -1.0;

  // Aggiorna fisicamente la lettura solo ogni 2 secondi per evitare letture troppo ravvicinate
  if (filteredVoltage < 0 || millis() - lastBatReadTime > 2000) {
#if defined(BATTERY_EN_PIN)
    pinMode(BATTERY_EN_PIN, OUTPUT);
    digitalWrite(BATTERY_EN_PIN, LOW); // Attiva il circuito di lettura batteria su Heltec
    delay(10); // Attesa per stabilizzare la tensione
#endif

    // Esegue una media rapida di 8 campioni per eliminare il rumore di fondo dell'ADC
    long sum = 0;
    for (int i = 0; i < 8; i++) {
      sum += analogRead(BATTERY_PIN);
      delay(2);
    }
    float raw = sum / 8.0;
    float currentVoltage = (raw / 4095.0) * 3.3 * BATTERY_DIVIDER;
    
#if defined(BATTERY_EN_PIN)
    digitalWrite(BATTERY_EN_PIN, HIGH); // Spegne per risparmiare energia
#endif

    // Applica un filtro passa-basso esponenziale (20% nuovo, 80% storico) per smorzare i salti di voltaggio
    if (filteredVoltage < 0) {
      filteredVoltage = currentVoltage;
    } else {
      filteredVoltage = (currentVoltage * 0.2) + (filteredVoltage * 0.8);
    }
    
    lastBatReadTime = millis();
  }

  // Mappatura LiPo 3.2V (0%) - 4.2V (100%)
  if (filteredVoltage >= 4.2) return 100;
  if (filteredVoltage <= 3.2) return 0;
  return (int)((filteredVoltage - 3.2) * 100.0);
}

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
#if defined(BOARD_LILYGO_T3_V1_6)
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
#else
  if (!SD.begin(SD_CS)) {
#endif
    Serial.println("⚠ SD non rilevata. Logging disabilitato.");
    sdCardPresent = false;
  } else {
    Serial.println("✓ SD card pronta");
    sdCardPresent = true;
    openNewPcapFile();
    writePcapGlobalHeader();
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Inizializza stato pulsante per evitare falsi trigger
  lastRawState = digitalRead(BUTTON_PIN);
  buttonStableState = lastRawState;
  lastButtonStableTime = millis();
  
  pinMode(BATTERY_PIN, INPUT);
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

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
    currentRssi = packetRssi;
    uint8_t payload[256];
    int len = 0;
    while (LoRa.available() && len < 256) {
      payload[len++] = (uint8_t)LoRa.read();
    }
    packetCountTotal++;
    packetCountChannel++;
    
    LoRaChannel currentCh = (currentChannelIndex == -1) ? manualProfile : getProfile(currentChannelIndex);
    
    // Registra o aggiorna solo se stiamo scansionando
    if (state == SCAN && currentChannelIndex != -1) {
      registerDiscovered(currentCh);
    }

    Serial.printf("📦 CH%d RSSI:%d SNR:%d LEN:%d\n", 
                  currentChannelIndex, packetRssi, snr, len);

    if (sdCardPresent) {
      writePcapPacket(packetRssi, snr, currentCh.freq, currentCh.sf, currentCh.bw, currentCh.cr, currentCh.syncWord, payload, len);
    }
    
    digitalWrite(LED_PIN, HIGH);
    lastPacketTime = millis();
  }

  // Spegne il LED dopo 50 millisecondi per creare un effetto "flash"
  if (millis() - lastPacketTime > 50) {
    digitalWrite(LED_PIN, LOW);
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
    
    delay(50); // Attendi l'assestamento hardware per scartare pacchetti fantasma
    // Forza lo svuotamento del buffer per eliminare pacchetti "fantasma"
    while(LoRa.parsePacket() > 0) {
      while(LoRa.available()) LoRa.read();
    }
    return;
  }

  if (index < 0 || index >= TOTAL_PROFILES) return;
  
  setupLoRaChannel(getProfile(index));
  currentChannelIndex = index;
  packetCountChannel = 0;
  
  delay(50); // Attendi l'assestamento hardware per scartare pacchetti fantasma
  // Forza lo svuotamento del buffer per eliminare pacchetti "fantasma" ricevuti sul canale precedente
  while(LoRa.parsePacket() > 0) {
    while(LoRa.available()) LoRa.read();
  }
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
        currentChannelIndex = savedScannerIndex;
        applyChannel(currentChannelIndex); // Sintonizza fisicamente la radio subito
        channelEnteredTime = millis();
      }
      else if (press == LONG_PRESS) {
        // Reset completo e nuova scansione
        resetDiscovery();
        state = SCAN;
        autoScan = true;
        currentChannelIndex = savedScannerIndex;
        applyChannel(currentChannelIndex); // Sintonizza fisicamente la radio subito
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
  state = HUNT;
  LoRaChannel ch = discovered[discoveredIndex].ch;
  manualProfile = ch;
  if (currentChannelIndex != -1) savedScannerIndex = currentChannelIndex;
  currentChannelIndex = -1; // Usa il manualProfile durante la caccia
  packetCountChannel = 0;   // Azzera il contatore di sessione
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
  if (currentChannelIndex != -1) savedScannerIndex = currentChannelIndex;
  currentChannelIndex = -1;
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
  
  // Dispositivi trovati e Batteria
  display.setCursor(0, 55);
  display.printf("Disp: %d", discoveredCount);
  
  display.setCursor(96, 55);
  display.printf("B:%d%%", getBatteryPercentage());
  
  display.display();
}

void drawSelectState() {
  display.clearDisplay();

  // Titolo e Batteria
  display.setCursor(0, 0);
  display.printf("DISP: %d", discoveredCount);
  display.setCursor(96, 0);
  display.printf("B:%d%%", getBatteryPercentage());
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
  
  display.setTextColor(SSD1306_WHITE); // Ripristina il colore per le altre schermate
  display.display();
}

void drawHuntState(int rssi) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
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
  
  // Statistiche e Batteria
  display.setCursor(0, 50);
  display.printf("Pk c:%u T:%u", (unsigned)packetCountChannel, (unsigned)packetCountTotal);
  
  display.setCursor(96, 50);
  display.printf("B:%d%%", getBatteryPercentage());
  
  display.display();
}

// ==================== PCAP ====================
void openNewPcapFile() {
  if (!sdCardPresent) return;
  
  if (timeSynced) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    char buf[64];
    strftime(buf, sizeof(buf), "/lora_bonifica_%Y%m%d_%H%M%S.pcap", tm_info);
    pcapFileName = String(buf);
  } else {
    int index = 1;
    while (true) {
      String name = "/lora_bonifica_" + String(index) + ".pcap";
      if (!SD.exists(name)) {
        pcapFileName = name;
        break;
      }
      index++;
    }
  }
  
  writePcapGlobalHeader();
}

void writePcapGlobalHeader() {
  if (!sdCardPresent) return;
  
  File f = SD.open(pcapFileName.c_str(), FILE_WRITE);
  if (!f) return;
  
  uint32_t magic = 0xa1b2c3d4;
  f.write((uint8_t*)&magic, 4);
  
  uint16_t v_maj = 2, v_min = 4;
  f.write((uint8_t*)&v_maj, 2);
  f.write((uint8_t*)&v_min, 2);
  
  uint32_t tz = 0, sigfigs = 0, snaplen = 65535, network = 270; // 270 is LINKTYPE_LORATAP
  f.write((uint8_t*)&tz, 4);
  f.write((uint8_t*)&sigfigs, 4);
  f.write((uint8_t*)&snaplen, 4);
  f.write((uint8_t*)&network, 4);
  
  f.close();
}

void writePcapPacket(int rssi, int snr, long freq, int sf, long bw, int cr, int syncWord, 
                     uint8_t* payload, size_t len) {
  if (!sdCardPresent) return;
  
  File f = SD.open(pcapFileName.c_str(), FILE_APPEND);
  if (!f) return;

  uint8_t hdr[15];
  
  hdr[0] = 0;  // version (v0)
  hdr[1] = 0;  // padding
  
  uint16_t hlen = sizeof(hdr);
  hdr[2] = (hlen >> 8) & 0xFF; // length high byte
  hdr[3] = hlen & 0xFF;        // length low byte
  
  uint32_t fq = (uint32_t)freq;
  hdr[4] = (fq >> 24) & 0xFF;
  hdr[5] = (fq >> 16) & 0xFF;
  hdr[6] = (fq >> 8) & 0xFF;
  hdr[7] = fq & 0xFF;
  
  uint8_t bw_idx = 0;
  if (bw == 125000) bw_idx = 0;
  else if (bw == 250000) bw_idx = 1;
  else if (bw == 500000) bw_idx = 2;
  else if (bw == 62500) bw_idx = 3;
  
  hdr[8] = bw_idx;
  hdr[9] = (uint8_t)sf;
  
  hdr[10] = (int8_t)rssi; // rssi_packet
  hdr[11] = 0; // rssi_max (not used)
  hdr[12] = 0; // rssi_current (not used)
  hdr[13] = (int8_t)(snr * 4); // SNR in 0.25dB steps
  hdr[14] = (uint8_t)syncWord;
  
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
  server.on("/restartscan", handleRestartScan);
  server.on("/stopscan", handleStopScan);
  server.on("/download", handleDownload);
  server.on("/delete", handleDelete);
  server.on("/clear", handleClearList);
  server.on("/webhunt", handleWebHunt);
  server.on("/profiles.txt", handleProfilesTxt);
  server.on("/setwifi", handleSetWiFi);
  
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", "<html><body><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form></body></html>");
  });
  
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "UPDATE FAILED" : "UPDATE SUCCESS - REBOOTING");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/settime", HTTP_GET, []() {
    if (server.hasArg("t")) {
      struct timeval tv;
      tv.tv_sec = server.arg("t").toInt();
      tv.tv_usec = 0;
      settimeofday(&tv, NULL);
      if (!timeSynced) {
        timeSynced = true;
        if (sdCardPresent && pcapFileName.startsWith("/lora_bonifica_")) {
           openNewPcapFile();
        }
      }
    }
    server.send(200, "text/plain", "OK");
  });

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
      generateProfiles(); // Aggiorna il numero totale per lo scanner
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
      generateProfiles(); // Aggiorna il numero totale per lo scanner
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
    currentChannelIndex = savedScannerIndex; // Riprende da dove era rimasto
    applyChannel(currentChannelIndex);       // Sintonizza fisicamente la radio subito
    channelEnteredTime = millis();
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleRestartScan() {
  if (state != BAND_SELECT) {
    autoScan = true;
    state = SCAN;
    currentChannelIndex = 0;
    savedScannerIndex = 0;
    applyChannel(currentChannelIndex);
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
  String fName = pcapFileName;
  if (server.hasArg("f")) {
    fName = server.arg("f");
  }
  
  if (sdCardPresent && SD.exists(fName)) {
    File file = SD.open(fName, FILE_READ);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + fName.substring(fName.lastIndexOf('/') + 1) + "\"");
    server.streamFile(file, "application/octet-stream");
    file.close();
  } else {
    server.send(404, "text/plain", "File non trovato");
  }
}

void handleDelete() {
  if (server.hasArg("f") && sdCardPresent) {
    String fName = server.arg("f");
    if (SD.exists(fName)) {
      SD.remove(fName);
    }
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleClearList() {
  discoveredCount = 0;
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleWebHunt() {
  if (server.hasArg("i")) {
    int id = server.arg("i").toInt();
    if (id >= 0 && id < discoveredCount) {
      enterHunt(id);
    }
  }
  server.sendHeader("Location", "/");
  server.send(302);
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
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 15px; background: #0b0c10; color: #c5c6c7; display: flex; flex-direction: column; align-items: center; }";
  html += "h1 { color: #00bfff; font-size: 24px; margin: 5px 0 15px 0; text-align: center; text-shadow: 0 0 12px rgba(0,191,255,0.4); font-weight: 600; }";
  html += "h2 { color: #00bfff; font-size: 16px; margin: 0 0 12px 0; border-bottom: 1px solid rgba(255,255,255,0.05); padding-bottom: 6px; font-weight: 500; }";
  html += ".container { width: 100%; max-width: 800px; display: grid; gap: 16px; grid-template-columns: 1fr; }";
  html += "@media(min-width: 600px) { .container { grid-template-columns: 1fr 1fr; } }";
  html += ".card { background: rgba(30,30,32,0.6); backdrop-filter: blur(10px); padding: 16px; border-radius: 14px; box-shadow: 0 8px 24px rgba(0,0,0,0.6); border: 1px solid rgba(255,255,255,0.03); }";
  html += ".card.full { grid-column: 1 / -1; }";
  html += "p { margin: 6px 0; font-size: 13px; color: #a0a0a5; }";
  html += "b { color: #ffffff; font-weight: 600; }";
  html += "button { background: linear-gradient(135deg, #007AFF, #00bfff); color: #fff; border: none; padding: 12px 16px; border-radius: 10px; font-weight: 600; cursor: pointer; transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1); width: 100%; margin-top: 8px; font-size: 14px; box-shadow: 0 4px 12px rgba(0,122,255,0.3); }";
  html += "button:hover { transform: translateY(-2px); box-shadow: 0 6px 16px rgba(0,122,255,0.5); filter: brightness(1.1); }";
  html += "button:active { transform: translateY(1px); }";
  html += ".btn-group { display: flex; gap: 10px; }";
  html += ".btn-group button { flex: 1; margin-top: 0; }";
  html += ".btn-danger { background: linear-gradient(135deg, #ff3b30, #ff453a); box-shadow: 0 4px 12px rgba(255,59,48,0.3); }";
  html += ".btn-danger:hover { box-shadow: 0 6px 16px rgba(255,59,48,0.5); }";
  html += ".btn-secondary { background: linear-gradient(135deg, #48484a, #3a3a3c); box-shadow: 0 4px 12px rgba(0,0,0,0.2); }";
  html += ".btn-secondary:hover { box-shadow: 0 6px 16px rgba(0,0,0,0.4); }";
  html += ".form-group { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }";
  html += ".form-group label { font-size: 13px; color: #a1a1a6; flex: 1; font-weight: 500; }";
  html += ".form-group input, .form-group select { width: 55%; padding: 10px; border-radius: 8px; border: 1px solid #333; background: rgba(0,0,0,0.3); color: #fff; font-size: 13px; transition: 0.3s; }";
  html += ".form-group input:focus, .form-group select:focus { border-color: #007AFF; outline: none; box-shadow: 0 0 0 2px rgba(0,122,255,0.3); }";
  html += ".prof-item { background: rgba(0,0,0,0.2); padding: 12px; margin-bottom: 10px; border-radius: 10px; font-size: 13px; display: flex; justify-content: space-between; align-items: center; border-left: 4px solid #00bfff; border-top: 1px solid rgba(255,255,255,0.02); border-right: 1px solid rgba(255,255,255,0.02); border-bottom: 1px solid rgba(255,255,255,0.02); transition: 0.2s; }";
  html += ".prof-item:hover { background: rgba(0,0,0,0.4); transform: translateX(2px); }";
  html += ".prof-item a { color: #ff453a; text-decoration: none; font-weight: bold; font-size: 18px; padding: 0 10px; transition: 0.2s; line-height:1; }";
  html += ".prof-item a:hover { color: #ff6961; transform: scale(1.2); }";
  html += "details { margin-bottom: 10px; background: rgba(0,0,0,0.15); padding: 10px; border-radius: 10px; }";
  html += "summary { font-size: 15px; font-weight: 600; color: #00bfff; cursor: pointer; outline: none; transition: 0.2s; user-select: none; margin-bottom:10px; }";
  html += "summary:hover { text-shadow: 0 0 10px rgba(0,191,255,0.4); }";
  html += ".status-badge { background: rgba(0,191,255,0.15); color: #00bfff; padding: 4px 8px; border-radius: 6px; font-size: 12px; font-weight: bold; border: 1px solid rgba(0,191,255,0.3); }";
  html += ".glass-panel { background: rgba(0,0,0,0.3); border-radius: 8px; padding: 12px; margin-bottom: 12px; border: 1px solid rgba(255,255,255,0.03); }";
  html += "@media(max-width: 500px) { .form-group { flex-direction: column; align-items: stretch; gap: 6px; margin-bottom:16px; } .form-group input, .form-group select { width: 100%; } }";
  
  // TABS CSS
  html += ".tabs { display: flex; width: 100%; max-width: 800px; margin-bottom: 20px; background: rgba(0,0,0,0.4); border-radius: 12px; padding: 4px; border: 1px solid rgba(255,255,255,0.05); }";
  html += ".tab-btn { flex: 1; padding: 12px 5px; text-align: center; color: #a1a1a6; font-size: 13px; font-weight: 600; cursor: pointer; border-radius: 8px; transition: 0.3s; background: transparent; }";
  html += ".tab-btn.active { background: rgba(0,191,255,0.15); color: #00bfff; box-shadow: 0 4px 10px rgba(0,0,0,0.2); }";
  html += ".tab-content { display: none; width: 100%; max-width: 800px; animation: fadeIn 0.3s ease; }";
  html += ".tab-content.active { display: block; }";
  html += "@keyframes fadeIn { from { opacity: 0; transform: translateY(5px); } to { opacity: 1; transform: translateY(0); } }";
  
  html += "</style></head><body>";
  
  int bat = getBatteryPercentage();
  html += "<div id='bat-indicator' style='position:absolute; top:16px; right:16px; font-size:13px; color:#00bfff; font-weight:bold; background:rgba(0,191,255,0.15); padding:4px 8px; border-radius:12px;'>&#128267; " + String(bat) + "%</div>";
  
  html += "<div style='display:flex; justify-content:center; align-items:center; margin-bottom:20px; width:100%; max-width:800px;'>";
  html += "<h1 style='margin:0; font-size:28px;'>&#128269; LoRaCatcher <span style='font-size:12px; color:#a1a1a6; font-weight:500; vertical-align:middle;'>by MacRF</span></h1>";
  html += "</div>";

  // TABS NAVIGATION
  html += "<div class='tabs'>";
  html += "<div class='tab-btn' onclick=\"openTab('tab-dash', this)\">&#128225; Dashboard</div>";
  html += "<div class='tab-btn' onclick=\"openTab('tab-param', this)\">&#127919; Parametri</div>";
  html += "<div class='tab-btn' onclick=\"openTab('tab-sys', this)\">&#9881; Sistema</div>";
  html += "</div>";
  
  // ====================== TAB 1: DASHBOARD ======================
  html += "<div id='tab-dash' class='tab-content'>";
  html += "<div class='container'>";
  
  if (state == HUNT) {
    LoRaChannel ch = (currentChannelIndex == -1) ? manualProfile : getProfile(currentChannelIndex);
    html += "<div class='card full' style='border: 1px solid rgba(0,191,255,0.4); background: linear-gradient(145deg, #0a111a, #070b12);'>";
    html += "<h2 style='color:#00bfff; text-align:center; border:none; margin-bottom:8px; font-size:20px;'>&#127919; CACCIA IN CORSO</h2>";
    html += "<div class='glass-panel' style='text-align:center;'>";
    html += "<p style='margin:0;'>Bersaglio: <b style='color:#fff; font-size:16px;'>" + String(ch.freq/1000000.0, 1) + " MHz</b></p>";
    html += "<p style='margin:4px 0 0 0; font-size:12px;'>SF" + String(ch.sf) + " | BW" + String(ch.bw/1000) + "k | CR4/" + String(ch.cr) + "</p>";
    html += "</div>";
    
    int rssiPercent = map(currentRssi, -140, -30, 0, 100);
    rssiPercent = constrain(rssiPercent, 0, 100);
    String barColor = rssiPercent > 70 ? "#00bfff" : (rssiPercent > 35 ? "#ff9f0a" : "#ff3b30");
    
    html += "<div style='width:100%; background:rgba(0,0,0,0.5); border-radius:12px; height:28px; margin:20px 0; overflow:hidden; border:1px solid rgba(255,255,255,0.05); box-shadow:inset 0 2px 4px rgba(0,0,0,0.5);'>";
    html += "<div style='width:" + String(rssiPercent) + "%; background: linear-gradient(90deg, " + barColor + ", " + barColor + "ee); height:100%; transition:width 0.3s cubic-bezier(0.4, 0, 0.2, 1); box-shadow: 0 0 12px " + barColor + "; border-radius:12px;'></div>";
    html += "</div>";
    html += "<h1 style='text-align:center; font-size:54px; margin:10px 0; text-shadow:0 0 20px " + barColor + "aa; color:" + barColor + "; line-height:1;'>" + String(currentRssi) + " <span style='font-size:20px; vertical-align:middle; color:#a1a1a6; text-shadow:none;'>dBm</span></h1>";
    
    html += "<p style='text-align:center; color:#a1a1a6; font-size:14px;'>Pacchetti estratti: <b style='color:#fff;'>" + String(packetCountChannel) + "</b></p>";
    html += "<button class='btn-danger' onclick=\"location.href='/startscan'\" style='margin-top:20px; font-size:15px; padding:14px;'>&#9646;&#9646; Ferma Caccia e Torna allo Scan</button>";
    html += "</div>";
  }
  
  html += "<div class='card full' style='display:flex; flex-direction:column; gap:15px;'>";
  String stMode = (state == BAND_SELECT) ? "Selezione Banda" : ((state == SCAN) ? (autoScan ? "Scan Auto" : "Scan Fermo") : ((state == HUNT) ? "Caccia" : "Lista"));
  
  html += "<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;'>";
  html += "<h2 style='border:none; margin:0;'>Pannello di Controllo</h2>";
  html += "<span class='status-badge'>" + stMode + "</span>";
  html += "</div>";

  html += "<div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(100px, 1fr)); gap:10px;'>";
  html += "<div class='glass-panel' style='margin:0; text-align:center;'><p style='margin:0; font-size:11px; color:#888;'>Banda</p><b style='font-size:14px;'>" + String(selectedBand == BAND_LOW ? "LOW" : "HIGH") + "</b></div>";
  if (state != BAND_SELECT) {
    int dispCh = (currentChannelIndex == -1) ? 0 : (currentChannelIndex + 1);
    html += "<div class='glass-panel' style='margin:0; text-align:center;'><p style='margin:0; font-size:11px; color:#888;'>Canale</p><b style='font-size:14px;'>" + String(dispCh) + " / " + String(TOTAL_PROFILES) + "</b></div>";
    html += "<div class='glass-panel' style='margin:0; text-align:center;'><p style='margin:0; font-size:11px; color:#888;'>Pacchetti</p><b style='font-size:14px; color:#00bfff;'>" + String(packetCountTotal) + "</b></div>";
  }
  html += "</div>";

  if (state != BAND_SELECT) {
    html += "<div class='btn-group' style='margin-top:5px;'>";
    html += "<button onclick=\"location.href='/startscan'\">&#9654; Avvia</button>";
    html += "<button class='btn-secondary' onclick=\"location.href='/restartscan'\">&#8635; Ricomincia</button>";
    html += "<button class='btn-danger' onclick=\"location.href='/stopscan'\">&#9646;&#9646; Ferma</button>";
    html += "</div>";
  }
  
  if (state == BAND_SELECT || !autoScan) {
    html += "<div class='glass-panel' style='margin-top:5px;'>";
    html += "<p style='font-size:12px; margin:0 0 8px 0; color:#a1a1a6;'>Cambio banda consentito a scan fermo:</p>";
    html += "<form action='/setband' method='GET' style='display:flex; gap:10px; align-items:center;'>";
    html += "<select name='b' style='flex:2; padding:10px; border-radius:8px; background:rgba(0,0,0,0.5); color:#fff; border:1px solid #333;'>";
    html += "<option value='LOW' "+String(selectedBand==BAND_LOW?"selected":"")+">LOW (433-510 MHz)</option>";
    html += "<option value='HIGH' "+String(selectedBand==BAND_HIGH?"selected":"")+">HIGH (863-923 MHz)</option>";
    html += "</select>";
    html += "<button type='submit' style='flex:1; margin:0;'>Applica</button>";
    html += "</form>";
    html += "</div>";
  }
  html += "<button class='btn-secondary' onclick=\"location.href='/download'\">&#128229; Scarica PCAP (" + String(packetCountTotal) + " pkt)</button>";
  html += "</div>";
  
  html += "<div class='card full' style='border-top: 3px solid #00bfff;'>";
  html += "<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>";
  html += "<h2 style='border:none; margin:0;'>Dispositivi Rilevati (" + String(discoveredCount) + ")</h2>";
  html += "<button class='btn-secondary' onclick=\"location.href='/clear'\" style='width:auto; padding:6px 12px; font-size:12px; margin:0;'>&#128465; Svuota Lista</button>";
  html += "</div>";
  
  if (discoveredCount > 0) {
    html += "<div style='display:grid; gap:12px; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));'>";
    for (int i = 0; i < discoveredCount; i++) {
      LoRaChannel ch = discovered[i].ch;
      html += "<div class='prof-item' style='flex-direction:column; align-items:stretch;'>";
      html += "<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;'>";
      html += "<span style='font-size:16px; font-weight:bold; color:#fff;'>" + String(ch.freq/1000000.0, 1) + " MHz</span>";
      html += "<span style='color:#00bfff; font-weight:bold; background:rgba(0,191,255,0.15); padding:3px 8px; border-radius:6px; font-size:12px;'>" + String(discovered[i].packetCount) + " PKTS</span>";
      html += "</div>";
      html += "<div style='font-size:12px; color:#a1a1a6; margin-bottom:12px;'>SF" + String(ch.sf) + " | BW" + String(ch.bw/1000) + "k | CR4/" + String(ch.cr) + (ch.syncWord == 0x34 ? " | LWAN" : "") + (ch.invertIQ ? " | IQ:Inv" : "") + "</div>";
      html += "<button onclick=\"location.href='/webhunt?i=" + String(i) + "'\" style='margin:0; padding:8px; font-size:13px; font-weight:600;'>&#127919; Inizia Caccia</button>";
      html += "</div>";
    }
    html += "</div>";
  } else {
    html += "<div class='glass-panel' style='text-align:center; padding:30px 10px;'>";
    html += "<p style='font-size:15px; color:#888;'>Inizia la scansione per trovare dispositivi LoRa.</p>";
    html += "</div>";
  }
  html += "</div>";
  
  html += "</div>"; // End container per Tab 1
  html += "</div>"; // End Tab 1

  // ====================== TAB 2: PARAMETRI ======================
  html += "<div id='tab-param' class='tab-content'>";
  html += "<div class='container'>";
  
  html += "<div class='card full'>";
  html += "<h2>&#127919; Imposta Parametri Manuali</h2>";
  html += "<p style='font-size:12px; margin:5px 0 15px 0;'>Imposta frequenza ed entra direttamente in caccia:</p>";
  html += "<form action='/manualhunt' method='GET'>";
  html += "<div class='form-group'><label>Frequenza (MHz):</label><input type='number' name='f' placeholder='Es. 868.1' step='any' required></div>";
  html += "<div class='form-group'><label>Spreading Factor:</label><select name='sf'><option value='7'>SF7</option><option value='8'>SF8</option><option value='9'>SF9</option><option value='10'>SF10</option><option value='11'>SF11</option><option value='12'>SF12</option></select></div>";
  html += "<div class='form-group'><label>Bandwidth:</label><select name='bw'><option value='62500'>62.5k</option><option value='125000'>125k</option><option value='250000'>250k</option><option value='500000'>500k</option></select></div>";
  html += "<div class='form-group'><label>Coding Rate:</label><select name='cr'><option value='5'>4/5</option><option value='6'>4/6</option><option value='7'>4/7</option><option value='8'>4/8</option></select></div>";
  html += "<div class='form-group'><label>Sync Word:</label><select name='sy'><option value='18'>0x12 (Privata)</option><option value='52'>0x34 (LoRaWAN)</option></select></div>";
  html += "<div class='form-group'><label>Inversione IQ:</label><select name='iq'><option value='0'>Normale</option><option value='1'>Invertita</option></select></div>";
  html += "<button type='submit'>Caccia Manuale</button>";
  html += "</form>";
  html += "</div>";
  
  html += "<div class='card full'>";
  html += "<h2>&#10133; Profili Custom ("+String(numCustomProfiles)+"/"+String(MAX_CUSTOM_PROFILES)+")</h2>";
  html += "<form action='/addprofile' method='GET' style='margin-top:10px;'>";
  html += "<div class='form-group'><label>Frequenza (MHz):</label><input type='number' name='f' placeholder='Es. 433.9' step='any' required></div>";
  html += "<div class='form-group'><label>Spreading Factor:</label><select name='sf'><option value='7'>SF7</option><option value='8'>SF8</option><option value='9'>SF9</option><option value='10'>SF10</option><option value='11'>SF11</option><option value='12'>SF12</option></select></div>";
  html += "<div class='form-group'><label>Bandwidth:</label><select name='bw'><option value='62500'>62.5k</option><option value='125000'>125k</option><option value='250000'>250k</option><option value='500000'>500k</option></select></div>";
  html += "<div class='form-group'><label>Coding Rate:</label><select name='cr'><option value='5'>4/5</option><option value='6'>4/6</option><option value='7'>4/7</option><option value='8'>4/8</option></select></div>";
  html += "<div class='form-group'><label>Sync Word:</label><select name='sy'><option value='18'>0x12 (Privata)</option><option value='52'>0x34 (LoRaWAN)</option></select></div>";
  html += "<div class='form-group'><label>Inversione IQ:</label><select name='iq'><option value='0'>Normale</option><option value='1'>Invertita</option></select></div>";
  html += "<button class='btn-secondary' type='submit'>Aggiungi Profilo</button>";
  html += "</form>";
  
  if (numCustomProfiles > 0) {
    html += "<div style='margin-top:15px; max-height:180px; overflow-y:auto; padding-right:5px;'>";
    for (int i = 0; i < numCustomProfiles; i++) {
      html += "<div class='prof-item' style='border-left-color:#ff9f0a; padding:8px 12px;'>";
      html += "<span style='font-size:12px;'><b>" + String(customProfiles[i].freq/1000000.0, 1) + "M</b> SF" + String(customProfiles[i].sf) + " BW" + String(customProfiles[i].bw/1000) + "k CR4/" + String(customProfiles[i].cr) + "</span>";
      html += "<a href='/removeprofile?i=" + String(i) + "' onclick='return confirm(\"Rimuovere questo profilo?\")' title='Rimuovi'>&times;</a>";
      html += "</div>";
    }
    html += "</div>";
  } else {
    html += "<p style='font-size:12px; margin-top:15px; text-align:center;'>Nessun profilo inserito.</p>";
  }
  html += "</div>";
  
  html += "<div class='card full'>";
  html += "<details>";
  html += "<summary>&#128214; Legenda Parametri RF & Profili Generati</summary>";
  html += "<div class='glass-panel'>";
  float s = (selectedBand == BAND_LOW) ? LOW_BAND_START : HIGH_BAND_START;
  float e = (selectedBand == BAND_LOW) ? LOW_BAND_END : HIGH_BAND_END;
  html += "<p>Profili Base in Scansione: <b>" + String(TOTAL_PROFILES) + " totali</b>.</p>";
  html += "<p>Frequenze da <b>" + String(s, 1) + "</b> a <b>" + String(e, 1) + " MHz</b> (step " + String(freqStep, 1) + " MHz).</p>";
  html += "<p style='font-size:12px;'>Per ogni frequenza sono testate le combinazioni di SF (7-12), BW (62.5k, 125k, 250k, 500k), CR (4/5-4/8), SyncWord (0x12, 0x34) e InvertIQ (Norm/Inv).</p>";
  html += "<button class='btn-secondary' style='width:auto; padding:6px 12px; font-size:12px;' onclick=\"window.open('/profiles.txt', '_blank')\">Vedi Lista TXT</button>";
  html += "</div>";
  
  html += "<div style='font-size:12px; color:#a1a1a6;'>";
  html += "<p><b style='color:#00bfff;'>Frequenza:</b> Il canale radio. I canali 868.1, 868.3 e 868.5 sono i primari dello standard pubblico LoRaWAN.</p>";
  html += "<p><b style='color:#00bfff;'>Spreading Factor (SF):</b> Durata del simbolo radio. SF7 è veloce (distanze brevi). SF12 è lento ma garantisce portate di svariati km.</p>";
  html += "<p><b style='color:#00bfff;'>Bandwidth (BW):</b> 500k offre molta banda ma poco raggio. 125k o 62.5k aumentano la sensibilità e la distanza.</p>";
  html += "<p><b style='color:#00bfff;'>Coding Rate (CR):</b> Ridondanza errori. Da 4/5 a 4/8.</p>";
  html += "<p><b style='color:#00bfff;'>Sync Word:</b> 0x12 (Privata) o 0x34 (LoRaWAN pubblica).</p>";
  html += "<p><b style='color:#00bfff;'>Inversione IQ:</b> Normale per sensori (Uplink), Invertita per Gateway (Downlink).</p>";
  html += "</div>";
  html += "</details>";
  html += "</div>";

  html += "</div>"; // End container per Tab 2
  html += "</div>"; // End Tab 2

  // ====================== TAB 3: SISTEMA ======================
  html += "<div id='tab-sys' class='tab-content'>";
  html += "<div class='container'>";
  
  if (sdCardPresent) {
    html += "<div class='card full'>";
    html += "<h2>&#128190; Gestione Memoria SD</h2>";
    html += "<div class='glass-panel'>";
    File root = SD.open("/");
    if (root) {
      File file = root.openNextFile();
      if (!file) {
        html += "<p style='color:#888;'>Nessun file presente sulla SD.</p>";
      }
      while(file) {
        if (!file.isDirectory()) {
          String fName = file.name();
          if (!fName.startsWith("/")) fName = "/" + fName;
          if (fName.endsWith(".pcap")) {
            html += "<div style='display:flex; justify-content:space-between; align-items:center; background:rgba(0,0,0,0.4); padding:6px 10px; border-radius:6px; margin-bottom:4px;'>";
            html += "<span style='font-size:12px; color:#ccc; word-break:break-all;'>" + fName + " <small>(" + String(file.size()/1024) + " KB)</small></span>";
            html += "<button class='btn-danger' style='width:auto; margin:0; padding:4px 8px; font-size:11px;' onclick=\"if(confirm('Eliminare " + fName + "?')) location.href='/delete?f=" + fName + "'\">Elimina</button>";
            html += "</div>";
          }
        }
        file = root.openNextFile();
      }
    } else {
      html += "<p style='color:#ff3b30;'>Errore di lettura cartella radice.</p>";
    }
    html += "</div>";
    html += "</div>";
  }

  html += "<div class='card full'>";
  html += "<h2>&#9881; Configurazione Base & WiFi</h2>";
  html += "<p style='font-size:11px; margin-bottom:10px; margin-top:0;'>Richiede il riavvio del dispositivo.</p>";
  html += "<form action='/setwifi' method='GET'>";
  html += "<div class='form-group'><label>SSID WiFi:</label><input type='text' name='s' value='" + wifiSSID + "' required></div>";
  html += "<div class='form-group'><label>Password (min 8 car):</label><input type='text' name='p' value='" + wifiPASS + "' required minlength='8'></div>";
  html += "<div class='form-group'><label>Step Frequenza Scansione:</label><select name='st'>";
  html += "<option value='1.0' " + String(freqStep==1.0?"selected":"") + ">1.0 MHz (Veloce)</option>";
  html += "<option value='0.5' " + String(freqStep==0.5?"selected":"") + ">0.5 MHz (Medio)</option>";
  html += "<option value='0.1' " + String(freqStep<0.5?"selected":"") + ">0.1 MHz (Lento ma Preciso)</option>";
  html += "</select></div>";
  html += "<button class='btn-secondary' type='submit'>Salva e Riavvia</button>";
  html += "</form>";
  html += "</div>";

  html += "<div class='card full'>";
  html += "<h2>&#128640; Aggiornamento Firmware OTA</h2>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' required style='margin-bottom:10px; width: 100%; border:none; padding:10px; background:rgba(0,0,0,0.3); border-radius:8px; color:#ccc;'><br>";
  html += "<p style='font-size:11px; color:#ff9f0a; margin-top:0;'>Assicurati che lo schema partizioni (su Arduino IDE) supporti l'OTA (es. Minimal SPIFFS).</p>";
  html += "<button type='submit' class='btn-danger'>Carica e Aggiorna Firmware</button>";
  html += "</form>";
  html += "</div>";


  
  html += "</div>"; // End container per Tab 3
  html += "</div>"; // End Tab 3
  
  // SCRIPT SEZIONE
  html += "<script>";
  html += "let autoRefresh;";
  
  html += "async function doRefresh() {";
  html += "  try {";
  html += "    let res = await fetch('/');";
  html += "    let txt = await res.text();";
  html += "    let doc = new DOMParser().parseFromString(txt, 'text/html');";
  html += "    let newDash = doc.getElementById('tab-dash');";
  html += "    if (newDash) document.getElementById('tab-dash').innerHTML = newDash.innerHTML;";
  html += "    let newBat = doc.getElementById('bat-indicator');";
  html += "    if (newBat) document.getElementById('bat-indicator').innerHTML = newBat.innerHTML;";
  html += "  } catch(e) {}";
  html += "  manageRefresh();";
  html += "}";

  html += "function manageRefresh() {";
  html += "  clearTimeout(autoRefresh);";
  html += "  let activeTab = sessionStorage.getItem('activeTab') || 'tab-dash';";
  html += "  if(activeTab === 'tab-dash') {";
  html += "    autoRefresh = setTimeout(doRefresh, 4000);";
  html += "  }";
  html += "}";
  
  html += "function openTab(tabId, btn) {";
  html += "  document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));";
  html += "  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));";
  html += "  document.getElementById(tabId).classList.add('active');";
  html += "  if(btn) btn.classList.add('active');";
  html += "  else {";
  html += "    if(tabId === 'tab-param') document.querySelectorAll('.tab-btn')[1].classList.add('active');";
  html += "    else if(tabId === 'tab-sys') document.querySelectorAll('.tab-btn')[2].classList.add('active');";
  html += "    else document.querySelectorAll('.tab-btn')[0].classList.add('active');";
  html += "  }";
  html += "  sessionStorage.setItem('activeTab', tabId);";
  html += "  manageRefresh();";
  html += "}";
  
  html += "function saveDetails() {";
  html += "  let dState = {};";
  html += "  document.querySelectorAll('details').forEach((d, i) => { dState[i] = d.open; });";
  html += "  sessionStorage.setItem('dState', JSON.stringify(dState));";
  html += "}";
  
  html += "function loadDetails() {";
  html += "  let st = sessionStorage.getItem('dState');";
  html += "  if(st) {";
  html += "    let dState = JSON.parse(st);";
  html += "    document.querySelectorAll('details').forEach((d, i) => { if(dState[i]) d.open = true; });";
  html += "  }";
  html += "}";

  html += "window.onload = () => {";
  html += "  let activeTab = sessionStorage.getItem('activeTab') || 'tab-dash';";
  html += "  openTab(activeTab, null);";
  html += "  loadDetails();";
  html += "  document.querySelectorAll('details').forEach(d => { d.addEventListener('toggle', () => { if(d.open) clearTimeout(autoRefresh); }) });";
  html += "  document.querySelectorAll('input, select, button').forEach(e => {";
  html += "    e.addEventListener('focus', () => clearTimeout(autoRefresh));";
  html += "    e.addEventListener('mousedown', () => clearTimeout(autoRefresh));";
  html += "  });";
  html += "  if(!sessionStorage.getItem('timeSent')) {";
  html += "    let ts = Math.floor(Date.now()/1000);";
  html += "    fetch('/settime?t='+ts).then(() => sessionStorage.setItem('timeSent', '1'));";
  html += "  }";
  html += "};";
  html += "</script>";
  html += "</body></html>";
  return html;
}



// ==================== INIZIALIZZAZIONI ====================
void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (OLED_RST != -1) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
      Serial.println("ERRORE: Display OLED non trovato!");
      while (1) delay(100);
    }
  } else {
    // La T3_V1_6 non usa il pin di reset per l'OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false)) {
      Serial.println("ERRORE: Display OLED non trovato!");
      while (1) delay(100);
    }
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
