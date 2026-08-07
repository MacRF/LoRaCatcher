# LoRaCatcher

LoRaCatcher è un avanzato strumento di "Signals Intelligence" (SIGINT) e Wardriving progettato specificamente per le reti e i dispositivi basati su tecnologia LoRa. Sviluppato da **MacRF**, trasforma un economico modulo ESP32 con chip radio LoRa in un infallibile "aspirapolvere dell'etere", capace di rilevare, localizzare e catturare letteralmente **qualsiasi** pacchetto LoRa in circolazione.

## 🎯 Panoramica e Funzionalità Principali

I ricevitori normali ascoltano su una singola frequenza con parametri preimpostati. **LoRaCatcher** sfrutta invece un motore procedurale che calcola al volo oltre **25.000 combinazioni radio** permutando frequenze, Spreading Factor, Bandwidth, Coding Rate, Sync Word e Inversione IQ.

- **Catch-All Scanner:** Non gli sfugge nulla. Scansiona lo spettro su bande predefinite (433MHz e 868MHz) cercando trasmissioni di:
  - Dispositivi "fai-da-te" e allarmi privati (Sync Word 0x12)
  - Nodi commerciali su reti pubbliche LoRaWAN come Helium e The Things Network (Sync Word 0x34)
  - Trasmissioni a lunghissimo raggio (BW strette fino a 62.5kHz)
  - Comunicazioni di ritorno (Downlink) dai Gateway verso i sensori (Inversione IQ)
- **Modalità "Fox Hunt" (Caccia):** Una volta scoperto un dispositivo in trasmissione, puoi selezionarlo. LoRaCatcher bloccherà la radio su quell'esatta configurazione e trasformerà il display OLED in un radar di prossimità in tempo reale: la barra grafica RSSI e il valore in dBm ti permetteranno di rintracciare fisicamente la sorgente ("fuochino/fuocherello").
- **Interfaccia Web Remota (Access Point):** LoRaCatcher crea una sua rete WiFi (`LoRaCatcher`). Collegandoti con lo smartphone avrai accesso a una dashboard completa, responsiva e dal design accattivante.
- **Parametri Personalizzati:** Aggiungi profili RF manuali, salta istantaneamente a frequenze specifiche, oppure imposta la precisione di scansione (fino a step chirurgici di 0.1 MHz).
- **Cattura Forense PCAP:** Inserendo una scheda MicroSD, ogni singolo pacchetto intercettato (incluso il payload crudo, RSSI, SNR, e Timestamp) viene salvato in un file standard `.pcap`, pronto per essere aperto e analizzato offline tramite Wireshark.

## 🛠️ Hardware Richiesto

Il codice è scritto in C++ per l'ecosistema Arduino ed è ottimizzato per la popolare scheda di sviluppo di LilyGO:

- **Scheda Principale:** TTGO / LilyGO LoRa32 V2 (o cloni ESP32 equipaggiati con SX1276)
- **Chip Radio:** Semtech SX1276 (supporto 433 MHz, 868 MHz, 915 MHz)
- **Display:** OLED SSD1306 128x64 (Incluso nella scheda)
- **Memoria:** Modulo MicroSD (spesso integrato sul retro della V2, oppure collegato via SPI)

**Connessioni PIN (TTGO LoRa32 V2):**
- **LoRa SPI:** SCK `5`, MISO `19`, MOSI `27`, CS `18`, RST `14`, DIO0 `26`
- **Display I2C:** SDA `4`, SCL `15`, RST `16`
- **MicroSD SPI:** CS `21`
- **Pulsante Hardware (PRG):** PIN `0`

## 🚀 Installazione (Arduino IDE)

1. Aggiungi il supporto ESP32 al Board Manager di Arduino IDE. Seleziona la board `TTGO LoRa32-OLED V1` (o equivalente).
2. Assicurati di aver installato le seguenti librerie tramite il Library Manager:
   - `LoRa` (di Sandeep Mistry)
   - `Adafruit SSD1306` e `Adafruit GFX Library`
3. Copia il file `lora_catcher.cpp` (rinominalo in `.ino` se usi l'IDE standard e non PlatformIO/Arduino-CLI).
4. Compila e carica sulla scheda.

## 📱 Utilizzo base

1. **Accensione:** All'avvio, il display mostrerà la schermata "LoRaCatcher by MacRF".
2. **Selezione Banda:** Usa il pulsante (click singolo) per alternare la banda (LOW 433-510 MHz o HIGH 863-923 MHz). Tieni premuto a lungo (Long Press) per avviare la scansione.
3. **Scansione (Auto Scan):** Il dispositivo passerà in rassegna migliaia di combinazioni ogni secondo. Quando la scritta del contatore dispositivi trovati aumenta, significa che è stato catturato un pacchetto!
4. **Visualizzazione e Caccia:** Tieni premuto il tasto per fermare lo scan e passare alla "Lista". Scorri la lista con click singoli. Una volta sul dispositivo che ti interessa, fai *Doppio Click* per attivare la "Fox Hunt" (radar di prossimità).
5. **Connessione Web:** Collegati al WiFi `LoRaCatcher` (Password default: `catcheratwork`). Apri il browser all'indirizzo IP `192.168.4.1` (di norma). Dalla pagina web potrai gestire remotamente tutto l'apparato, modificare la sensibilità dello step (fino a 0.1 MHz), aggiungere canali manuali e scaricare il dump in formato PCAP.

## 🤝 Contribuzioni
Tutte le pull request sono benvenute. Per cambiamenti architetturali o per l'aggiunta del supporto a chip radio di generazione successiva (es. SX1262), apri prima una Issue per discuterne l'implementazione!

---
*Progetto nato per scopi didattici, ricerca sulla sicurezza informatica (Pentesting IoT) e analisi dell'inquinamento elettromagnetico. L'autore declina ogni responsabilità per usi non conformi alle leggi locali sulle telecomunicazioni.*
