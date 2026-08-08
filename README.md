# LoRaCatcher

LoRaCatcher è un avanzato strumento di "Signals Intelligence" (SIGINT) e Wardriving progettato specificamente per le reti e i dispositivi basati su tecnologia LoRa. Sviluppato da **MacRF**, trasforma un economico modulo ESP32 con chip radio LoRa in un infallibile "aspirapolvere dell'etere", capace di rilevare, localizzare e catturare letteralmente **qualsiasi** pacchetto LoRa in circolazione.

## 🎯 Panoramica e Funzionalità Principali

I ricevitori normali ascoltano su una singola frequenza con parametri preimpostati. **LoRaCatcher** sfrutta invece un motore procedurale che calcola al volo oltre **6.000 combinazioni radio** permutando frequenze, Spreading Factor, Bandwidth, Sync Word e Inversione IQ. (Il Coding Rate viene rilevato dinamicamente in Hardware per massimizzare la velocità di scansione).

- **Catch-All Scanner:** Non gli sfugge nulla. Scansiona lo spettro su bande predefinite (Low 433-510 MHz e High 863-923 MHz) cercando trasmissioni di:
  - Dispositivi "fai-da-te" e allarmi privati (Sync Word 0x12)
  - Nodi commerciali su reti pubbliche LoRaWAN come Helium e The Things Network (Sync Word 0x34)
  - Trasmissioni a lunghissimo raggio (BW strette fino a 62.5kHz)
  - Comunicazioni di ritorno (Downlink) dai Gateway verso i sensori (Inversione IQ)
- **Modalità "Fox Hunt" (Caccia):** Una volta scoperto un dispositivo in trasmissione, puoi selezionarlo. LoRaCatcher bloccherà la radio su quell'esatta configurazione e trasformerà il display OLED in un radar di prossimità in tempo reale: la barra grafica RSSI e il valore in dBm ti permetteranno di rintracciare fisicamente la sorgente ("fuochino/fuocherello").
- **Interfaccia Web Premium a Schede (Tabs):** LoRaCatcher crea una sua rete WiFi (`LoRaCatcher`, Password di default: `catcheratwork`). Collegandoti con lo smartphone (o PC) avrai accesso a una dashboard all'avanguardia con design **Glassmorphism** (trasparenze stile frosted glass e interfacce adattive) **totalmente responsive** su schermi Mobile, Tablet e Desktop. È organizzata in schede navigabili senza ricaricamenti pagina:
  - 📡 **Dashboard:** Barra di avanzamento scansione (con % completata calcolata dinamicamente e stima tempo residuo basata sulla velocità attuale), bottone per la modalità **Loop** (ripresa automatica), interruttore per le **Notifiche Sonore** ad ogni intercettazione, radar RSSI animato e gestione catture in tempo reale.
  - 🎯 **Parametri:** Inserimento profili personalizzati e caccia manuale. Di default il sistema parte pronto per intercettare l'attivissima **High Band** (863-923 MHz).
  - ⚙️ **Sistema:** Gestione file (con comando rapido "**Svuota SD**"), configurazione rete Wi-Fi, regolazione **Tempo di Ascolto (Dwell Time)** personalizzabile per singolo canale (da 50ms a 1000ms), e sistema di update.
- **Aggiornamenti Firmware OTA Avanzati:** Puoi caricare nuove versioni del firmware via Web senza cavi USB. Durante il flash visualizzerai una Progress Bar animata in tempo reale e, in caso di problemi, riceverai messaggi di errore diagnostici in chiaro e in italiano (es. "Spazio Insufficiente", "Controllo MD5 Fallito", "Errore Magic Byte") senza impazzire col debugger seriale.
- **Cattura Forense PCAP Perfetta:** Inserendo una scheda MicroSD, ogni singolo pacchetto intercettato (incluso il payload crudo, RSSI, SNR, e Timestamp) viene salvato in un file standard `.pcap`. Grazie al timestamp sincronizzato in automatico dal browser, i nomi dei file conterranno data e ora reali della bonifica, ed i pacchetti sfrutteranno l'header standard "LoRaTap" per essere analizzati alla perfezione su Wireshark.
- **Status Batteria Ultra-Preciso:** L'interfaccia Web e l'OLED mostrano costantemente la percentuale di carica. L'ADC del microcontrollore è stato affinato chirurgicamente e calibrato (allineando la lettura al tester a 4.13V reali per un'indicazione affidabile fino all'1%).
- **Feedback Visivo Immediato:** Il LED integrato sulla scheda lampeggerà istantaneamente ad ogni pacchetto intercettato.

## 🛠️ Hardware Richiesto

Il codice è scritto in C++ per l'ecosistema Arduino ed è ottimizzato per la popolare scheda di sviluppo di LilyGO:

- **Scheda Principale:** **Heltec ESP32 LoRa V2** oppure **LilyGO T3 LoRa32 V1.6.1** (Quest'ultima ha lo slot MicroSD già incorporato!)
- **Chip Radio:** Semtech SX1276 (supporto 433 MHz, 868 MHz, 915 MHz)
- **Display:** OLED SSD1306 128x64 (Incluso)
- **Memoria:** Modulo MicroSD (Spesso esterno nella Heltec, integrato nella LilyGO T3)

**Selezione della scheda:**
Il codice sorgente include già le preconfigurazioni per i due modelli principali. Alla primissima riga del file `lora_catcher.cpp` troverai questo blocco:
```cpp
// Scegli la tua scheda de-commentando SOLO UNA delle righe seguenti:
//#define BOARD_HELTEC_V2       // Heltec ESP32 LoRa V2
#define BOARD_LILYGO_T3_V1_6    // LilyGO T3 LoRa32 V1.6.1 (con MicroSD)
```
Di default è attiva la **T3_V1.6.1**. Questa configura automaticamente l'OLED (su I2C 21/22), il chip LoRa, e crea un Bus HSPI separato per accedere al comodissimo lettore MicroSD integrato sul retro della scheda.

Se usi invece la **Heltec ESP32 LoRa V2**, ti basterà de-commentare la prima riga e commentare la seconda, e cablare una SD card esterna sui PIN classici:
- **LoRa SPI:** SCK `5`, MISO `19`, MOSI `27`, CS `18`, RST `14`, DIO0 `26`
- **Display I2C:** SDA `4`, SCL `15`, RST `16`
- **MicroSD SPI:** CS `21`
- **Pulsante Hardware (PRG):** PIN `0` (puoi usare il bottone "PRG" già presente sulla Heltec)

**Nota sul Pulsante Fisico (LilyGO T3 V1.6.1):** Poiché questa scheda non espone un pulsante utente generico oltre a quello di Reset, il software si aspetta che tu colleghi un tuo pulsante fisico (switch/arcade) tra il **PIN 4** e il **GND**. Non c'è bisogno di resistenze aggiuntive, il software usa già le resistenze di pull-up interne del chip (`INPUT_PULLUP`). Il PIN 4 è libero da interferenze e comodissimo da cablare sulla fila destra della scheda!

## 🚀 Installazione (Arduino IDE)

1. Aggiungi il supporto ESP32 al Board Manager di Arduino IDE. Seleziona la board `TTGO LoRa32-OLED V1` (o equivalente).
2. Scegli uno **Schema Partizioni (Partition Scheme)** che supporti l'OTA (ad es. `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)`).
3. Assicurati di aver installato le seguenti librerie tramite il Library Manager:
   - `LoRa` (di Sandeep Mistry)
   - `Adafruit SSD1306` e `Adafruit GFX Library`
4. Copia il file `lora_catcher.cpp` (rinominalo in `.ino` se usi l'IDE standard e non PlatformIO/Arduino-CLI).
5. Compila e carica sulla scheda tramite cavo USB per la prima installazione. Successivamente, potrai aggiornare via interfaccia Web!

## 📱 Utilizzo base

1. **Accensione:** All'avvio, il display mostrerà la schermata "LoRaCatcher by MacRF".
2. **Selezione Banda:** Usa il pulsante (click singolo) per alternare la banda (LOW 433-510 MHz o HIGH 863-923 MHz). Tieni premuto a lungo (Long Press) per avviare la scansione.
3. **Scansione (Auto Scan):** Il dispositivo passerà in rassegna migliaia di combinazioni ogni secondo. Quando la scritta del contatore dispositivi trovati aumenta, significa che è stato catturato un pacchetto!
4. **Visualizzazione e Caccia:** Tieni premuto il tasto per fermare lo scan e passare alla "Lista". Scorri la lista con click singoli. Una volta sul dispositivo che ti interessa, fai *Doppio Click* per attivare la "Fox Hunt" (radar di prossimità).
5. **Fox Hunt via Web:** È possibile avviare il pedinamento radio (Radar RSSI in tempo reale) direttamente dall'interfaccia Web cliccando sul pulsante "🎯 Inizia Caccia" vicino al bersaglio rilevato. La WebUI sfrutta l'aggiornamento automatico della barra di segnale sfumata da rosso a ciano, per indicarti visivamente l'avvicinamento al target.

## 📡 LoRa Generator (Strumento di Test)

Insieme al LoRaCatcher, in questo progetto è incluso anche **LoRaGenerator**. Si tratta di uno sketch totalmente separato (file `lora_generator.cpp`) progettato per essere caricato su una seconda scheda (es. Heltec ESP32 LoRa V2). 
Il suo scopo è generare traffico radio controllato per collaudare il Catcher:
- Crea una propria rete WiFi (`LoRaGenerator`, password `generatortest`).
- Dalla sua interfaccia web puoi "plasmare" il pacchetto LoRa (scegliendo frequenza esatta, SF, BW, CR e Sync Word) e iniziare a trasmettere a ripetizione (fino a 10 pacchetti al secondo).
- Usalo come bersaglio artificiale per fare pratica con la modalità "Fox Hunt" e verificare la ricezione dei payload!

## ⚖️ Licenza e Termini d'Uso
**Copyright © MacRF. Tutti i diritti riservati.**

Il presente progetto (`LoRaCatcher`) è un'opera personale. Viene concesso **esclusivamente per uso personale e didattico**.
- ❌ **NON è consentita** la modifica, la distribuzione, la copia o la pubblicazione del codice sorgente (in tutto o in parte).
- ❌ **NON è consentito** l'uso commerciale o a scopo di lucro del software, né l'integrazione in prodotti destinati alla vendita.

---
*Progetto nato per scopi didattici, ricerca sulla sicurezza informatica (Pentesting IoT) e analisi dell'inquinamento elettromagnetico. L'autore declina ogni responsabilità per usi non conformi alle leggi locali sulle telecomunicazioni.*
