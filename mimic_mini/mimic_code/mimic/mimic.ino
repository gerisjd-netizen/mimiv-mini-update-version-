#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <EEPROM.h>
#include "esp_sleep.h"

// --- EEPROM CÍMEK ---
#define EEPROM_SIZE 2
#define EEPROM_SAVE_FORMAT_ADDR 0
#define EEPROM_LED_PIN_ADDR 1

// --- IRREMOTE CONFIGURATION ---
#define IR_SEND_INTERNAL_PIN 3   // Belső IR adó LED: GPIO 3
#define IR_SEND_EXTERNAL_PIN 21  // Külső IR adó adatvonala: GPIO 21 (D6)
#define IR_RECEIVE_PIN 20        // IR vevő modul: GPIO 20
#define RAW_BUFFER_LENGTH 400    // Puffer méret a nyers pulzusokhoz

// --- SEBESSÉG BEÁLLÍTÁS ---
const uint16_t IR_GAP_MS = 50; 

#include <IRremote.hpp>

// --- PINOUT DEFINITIONS ---
#define SD_CS   4   // D2 láb = GPIO 4
#define SD_CLK  8   // D8 láb = GPIO 8
#define SD_MISO 9   // D9 láb = GPIO 9
#define SD_MOSI 10  // D10 láb = GPIO 10
#define ANALOG_PIN 2 // Gomb-létra: GPIO 2 (ADC1_CH2)

// Deep sleep inaktivitási időkorlát (30 másodperc)
#define SLEEP_TIMEOUT_MS 30000 
uint32_t lastActivityTime = 0;

// SD Kártya állapotának figyelése
bool sdInitialized = false;

// OLED Kijelző (XIAO ESP32-C3: SDA=6, SCL=7)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// --- AUTO-REPEAT GOMB MÁTRIX VÁLTOZÓK ---
enum ButtonType { BTN_NONE, BTN_LEFT, BTN_RIGHT, BTN_DOWN, BTN_UP };

ButtonType lastPhysicalButton = BTN_NONE;
unsigned long buttonPressStart = 0;
unsigned long lastRepeatTime = 0;
bool isHolding = false;

const unsigned long HOLD_DELAY_MS = 350;     // Ennyi nyomva tartás után indul a gyorstüzelés
const unsigned long REPEAT_INTERVAL_MS = 100; // Ismétlési sebesség (ms)

// Gomb trigger jelzők a loop-nak
bool rightTriggered = false;
bool downTriggered  = false;
bool leftTriggered   = false;
bool upTriggered     = false;

// Nyers gombállapotok az olyan folyamatos mozgásokhoz, mint a Pong
bool rightRaw = false;
bool downRaw  = false;
bool leftRaw   = false;
bool upRaw     = false;

// SD Fájllista (IR, Univ és Jegyzetekhez)
#define MAX_FILES 50
String fileList[MAX_FILES];
int totalFiles = 0;
int selectedIndex = 0;

// Több jelet tartalmazó fájlok almenüje
#define MAX_SUB_SIGNALS 40
String subSignalNames[MAX_SUB_SIGNALS];
int totalSubSignals = 0;
int selectedSubIndex = 0;
String currentMultiFilePath = "";

// Jegyzet megjelenítéshez sorok
#define MAX_NOTE_LINES 100
String noteLines[MAX_NOTE_LINES];
int totalNoteLines = 0;
int noteScrollIndex = 0;

// Beállítások opciók
bool saveAsFlipper = true; // true = Flipper .ir, false = Raw .txt
bool useExternalIR = false; // true = External, false = Internal

// Rendszerállapotok
enum SystemState {
  MAIN_MENU,
  IR_MENU,
  LOAD_IR_LIST,
  LOAD_UNIV_LIST,
  SUB_IR_LIST,
  SAVE_IR_WAIT,
  LOAD_NOTE_LIST,
  VIEW_NOTE,
  SETTINGS_MENU,
  GAME_PONG,
  STATUS_MSG
};

SystemState currentState = MAIN_MENU;

// Menü indexek
int mainMenuIndex = 0; 
const int MAIN_MENU_TOTAL = 4; // 0=IR Signal, 1=Notes, 2=Games, 3=Settings

int irMenuIndex = 0;
const int IR_MENU_TOTAL = 3; // 0=Load IR, 1=Universal IR, 2=Save IR

int settingsMenuIndex = 0;
const int SETTINGS_MENU_TOTAL = 2; // 0=IR LED, 1=Save Format

String statusMessage = "";
uint32_t statusTimer = 0;

// --- PONG JÁTÉK VÁLTOZÓK ---
int playerY = 24;
int cpuY = 24;
const int paddleHeight = 16;
const int paddleWidth = 3;

float ballX = 64.0;
float ballY = 32.0;
float ballSpeedX = 2.5;
float ballSpeedY = 1.5;

int playerScore = 0;
int cpuScore = 0;

// --- PROTOTÍPUSOK ---
void playAllSignalsInUniversalFile(String filePath);

// --- EEPROM BEÁLLÍTÁSOK BETÖLTÉSE ÉS MENTÉSE ---
void loadSettings() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("EEPROM Init Failed!");
    return;
  }
  
  uint8_t formatVal = EEPROM.read(EEPROM_SAVE_FORMAT_ADDR);
  saveAsFlipper = (formatVal == 1) ? true : false;

  uint8_t ledVal = EEPROM.read(EEPROM_LED_PIN_ADDR);
  useExternalIR = (ledVal == 1) ? true : false;
}

void saveSettings() {
  EEPROM.write(EEPROM_SAVE_FORMAT_ADDR, saveAsFlipper ? 1 : 0);
  EEPROM.write(EEPROM_LED_PIN_ADDR, useExternalIR ? 1 : 0);
  EEPROM.commit();
}

// --- DEEP SLEEP INDÍTÁSA ---
void goToSleep() {
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  
  Wire.beginTransmission(0x3C);
  Wire.write(0x00);
  Wire.write(0xAE);
  Wire.endTransmission();

  pinMode(ANALOG_PIN, INPUT_PULLUP);
  delay(50); 

  esp_deep_sleep_enable_gpio_wakeup(1ULL << ANALOG_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// --- ANALÓG GOMB-LÉTRA BEOLVASÁSA AUTO-REPEAT FUNKCIÓVAL ---
void readButtons() {
  rightTriggered = false;
  downTriggered  = false;
  leftTriggered   = false;
  upTriggered     = false;

  rightRaw = false;
  downRaw  = false;
  leftRaw   = false;
  upRaw     = false;

  uint32_t mVolts = analogReadMilliVolts(ANALOG_PIN);
  float voltage = mVolts / 1000.0;

  ButtonType currentPhysicalButton = BTN_NONE;

  if (voltage < 0.10) {
    currentPhysicalButton = BTN_LEFT;
    leftRaw = true;
  } else if (voltage >= 0.15 && voltage <= 0.45) {
    currentPhysicalButton = BTN_RIGHT;
    rightRaw = true;
  } else if (voltage >= 0.65 && voltage <= 0.95) {
    currentPhysicalButton = BTN_DOWN;
    downRaw = true;
  } else if (voltage >= 1.45 && voltage <= 1.85) {
    currentPhysicalButton = BTN_UP;
    upRaw = true;
  }

  unsigned long now = millis();

  if (currentPhysicalButton != BTN_NONE) {
    lastActivityTime = now; // Inaktivitás számláló frissítése

    if (currentPhysicalButton != lastPhysicalButton) {
      // ÚJ GOMBNYOMÁS! Azonnali trigger
      lastPhysicalButton = currentPhysicalButton;
      buttonPressStart = now;
      lastRepeatTime = now;
      isHolding = false;

      if (currentPhysicalButton == BTN_LEFT)  leftTriggered = true;
      if (currentPhysicalButton == BTN_RIGHT) rightTriggered = true;
      if (currentPhysicalButton == BTN_DOWN)  downTriggered = true;
      if (currentPhysicalButton == BTN_UP)    upTriggered = true;
    } 
    else {
      // FOLYAMATOSAN NYOMVA TARTVA
      if (!isHolding && (now - buttonPressStart >= HOLD_DELAY_MS)) {
        isHolding = true;
        lastRepeatTime = now;

        if (currentPhysicalButton == BTN_LEFT)  leftTriggered = true;
        if (currentPhysicalButton == BTN_RIGHT) rightTriggered = true;
        if (currentPhysicalButton == BTN_DOWN)  downTriggered = true;
        if (currentPhysicalButton == BTN_UP)    upTriggered = true;
      } 
      else if (isHolding && (now - lastRepeatTime >= REPEAT_INTERVAL_MS)) {
        lastRepeatTime = now;

        if (currentPhysicalButton == BTN_LEFT)  leftTriggered = true;
        if (currentPhysicalButton == BTN_RIGHT) rightTriggered = true;
        if (currentPhysicalButton == BTN_DOWN)  downTriggered = true;
        if (currentPhysicalButton == BTN_UP)    upTriggered = true;
      }
    }
  } else {
    // ELENGEDVE
    lastPhysicalButton = BTN_NONE;
    isHolding = false;
  }
}

// --- SD KÁRTYÁRÓL FÁJLLISTA BEOLVASÁSA ---
void loadFileListFromFolder(const char* folderPath) {
  totalFiles = 0;
  selectedIndex = 0;

  if (!sdInitialized) {
    statusMessage = "NO SD CARD!";
    currentState = STATUS_MSG;
    statusTimer = millis();
    return;
  }

  if (!SD.exists(folderPath)) {
    SD.mkdir(folderPath);
  }

  File dir = SD.open(folderPath);
  if (!dir || !dir.isDirectory()) return;

  dir.rewindDirectory();

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;

    String fname = String(entry.name());
    bool isDir = entry.isDirectory();
    entry.close();

    if (!isDir) {
      int lastSlash = fname.lastIndexOf('/');
      if (lastSlash >= 0) {
        fname = fname.substring(lastSlash + 1);
      }

      String fnameLower = fname;
      fnameLower.toLowerCase();

      if ((fnameLower.endsWith(".txt") || fnameLower.endsWith(".ir")) && !fname.startsWith(".")) {
        if (totalFiles < MAX_FILES) {
          fileList[totalFiles] = fname;
          totalFiles++;
        }
      }
    }
  }
  dir.close();
}

// --- JEGYZET BEOLVASÁSA ÉS TÖRDELÉSE ---
void loadAndWrapNote(String filename) {
  totalNoteLines = 0;
  noteScrollIndex = 0;

  if (!sdInitialized) return;

  String filePath = "/notes/" + filename;
  File noteFile = SD.open(filePath, FILE_READ);

  if (!noteFile) return;

  u8g2.setFont(u8g2_font_6x10_tr);
  int maxPixelWidth = 120; 

  String currentLine = "";

  while (noteFile.available() && totalNoteLines < MAX_NOTE_LINES) {
    char c = noteFile.read();

    if (c == '\r') continue;

    if (c == '\n') {
      noteLines[totalNoteLines++] = currentLine;
      currentLine = "";
      continue;
    }

    String testLine = currentLine + c;
    if (u8g2.getStrWidth(testLine.c_str()) <= maxPixelWidth) {
      currentLine = testLine;
    } else {
      if (currentLine.length() > 0) {
        noteLines[totalNoteLines++] = currentLine;
      }
      currentLine = String(c);
    }
  }

  if (currentLine.length() > 0 && totalNoteLines < MAX_NOTE_LINES) {
    noteLines[totalNoteLines++] = currentLine;
  }

  noteFile.close();
}

// --- SORSZÁMOZÁS AZ ÚJ IR FÁJLNAK ---
int getNextFileNumber() {
  int maxNum = 0;
  for (int i = 0; i < totalFiles; i++) {
    String fname = fileList[i];
    int dotIdx = fname.indexOf('.');
    if (dotIdx > 0) {
      int num = fname.substring(0, dotIdx).toInt();
      if (num > maxNum) {
        maxNum = num;
      }
    }
  }
  return maxNum + 1;
}

// --- IR JEL MENTÉSE ---
void saveIRSignal() {
  if (!sdInitialized) {
    statusMessage = "NO SD CARD!";
    currentState = STATUS_MSG;
    statusTimer = millis();
    return;
  }

  loadFileListFromFolder("/ir_files"); 
  int newNum = getNextFileNumber();
  
  if (saveAsFlipper && IrReceiver.decodedIRData.protocol != UNKNOWN) {
    String filePath = "/ir_files/" + String(newNum) + ".ir";
    File irFile = SD.open(filePath, FILE_WRITE);

    if (irFile) {
      irFile.println("Filetype: IR signals file");
      irFile.println("Version: 1");
      irFile.println("# ");
      irFile.print("name: ");
      irFile.println(String(newNum));
      irFile.println("type: parsed");

      String protName = getProtocolString(IrReceiver.decodedIRData.protocol);
      irFile.print("protocol: ");
      irFile.println(protName);

      uint32_t addr = IrReceiver.decodedIRData.address;
      uint32_t cmd  = IrReceiver.decodedIRData.command;

      char addrBuf[12];
      char cmdBuf[12];
      snprintf(addrBuf, sizeof(addrBuf), "%02X 00 00 00", (unsigned int)(addr & 0xFF));
      snprintf(cmdBuf, sizeof(cmdBuf), "%02X 00 00 00", (unsigned int)(cmd & 0xFF));

      irFile.print("address: ");
      irFile.println(addrBuf);
      irFile.print("command: ");
      irFile.println(cmdBuf);

      irFile.close();
      statusMessage = "SAVED: " + String(newNum) + ".ir";
    } else {
      statusMessage = "SD WRITE ERROR!";
    }
  } 
  else {
    uint16_t length = IrReceiver.irparams.rawlen - 1;
    if (length > RAW_BUFFER_LENGTH) length = RAW_BUFFER_LENGTH;

    String filePath = "/ir_files/" + String(newNum) + ".txt";
    File irFile = SD.open(filePath, FILE_WRITE);

    if (irFile) {
      for (unsigned int i = 1; i <= length; i++) {
        irFile.print(IrReceiver.irparams.rawbuf[i] * MICROS_PER_TICK);
        if (i < length) irFile.print(",");
      }
      irFile.close();
      statusMessage = "SAVED: " + String(newNum) + ".txt";
    } else {
      statusMessage = "SD WRITE ERROR!";
    }
  }

  currentState = STATUS_MSG;
  statusTimer = millis();
}

// --- SCROLLBAR RAJZOLÁSA ---
void drawScrollbar(int currentIdx, int totalItems, int visibleItems) {
  if (totalItems <= visibleItems) return;

  int trackX = 124;
  int trackY = 16;
  int trackHeight = 46;
  int trackWidth = 3;

  u8g2.drawVLine(trackX + 1, trackY, trackHeight);

  int thumbHeight = max(6, (trackHeight * visibleItems) / totalItems);
  int thumbY = trackY + ((trackHeight - thumbHeight) * currentIdx) / (totalItems - 1);

  u8g2.drawBox(trackX, thumbY, trackWidth, thumbHeight);
}

// --- FLIPPER HEX PARSER ---
uint32_t parseFlipperHexBytes(String hexStr) {
  uint32_t result = 0;
  int byteIdx = 0;
  hexStr.trim();
  
  while (hexStr.length() > 0 && byteIdx < 4) {
    int spaceIdx = hexStr.indexOf(' ');
    String byteStr = (spaceIdx != -1) ? hexStr.substring(0, spaceIdx) : hexStr;
    if (spaceIdx != -1) {
      hexStr = hexStr.substring(spaceIdx + 1);
      hexStr.trim();
    } else {
      hexStr = "";
    }
    
    uint32_t val = strtoul(byteStr.c_str(), NULL, 16);
    result |= (val << (byteIdx * 8));
    byteIdx++;
  }
  return result;
}

// --- IR KÜLDŐ ---
void executeParsedIR(String protocol, uint32_t address, uint32_t command) {
  protocol.toLowerCase();
  protocol.trim();
  
  if (protocol == "nec" || protocol == "necext") {
    IrSender.sendNEC(address & 0xFFFF, command & 0xFFFF, 0);
  } 
  else if (protocol == "samsung" || protocol == "samsung32") {
    IrSender.sendSamsung(address & 0xFFFF, command & 0xFFFF, 0);
  } 
  else if (protocol == "sirc" || protocol == "sony") {
    IrSender.sendSony(address & 0xFFFF, command & 0xFF, 2);
  } 
  else if (protocol == "rc5") {
    IrSender.sendRC5(address & 0xFF, command & 0xFF, 0);
  } 
  else if (protocol == "rc6") {
    IrSender.sendRC6(address & 0xFF, command & 0xFF, 0);
  }
}

// --- MEGSZÁMOLJA A JELEKET EGY FÁJLBAN ---
int countAndParseSignalsInFile(String filePath) {
  totalSubSignals = 0;
  File irFile = SD.open(filePath, FILE_READ);
  if (!irFile) return 0;

  while (irFile.available()) {
    String line = irFile.readStringUntil('\n');
    line.trim();

    if (line.startsWith("Name: ") || line.startsWith("name: ")) {
      if (totalSubSignals < MAX_SUB_SIGNALS) {
        subSignalNames[totalSubSignals] = line.substring(6);
        totalSubSignals++;
      }
    }
  }
  irFile.close();
  return totalSubSignals;
}

// --- ADOTT SORSZÁMOZÁSÚ JEL FELDOLGOZÁSA ÉS KIKÜLDÉSE ---
void processAndSendIRFile(String filePath, String displayName, int targetSignalIndex = 0) {
  File irFile = SD.open(filePath, FILE_READ);

  if (!irFile) {
    statusMessage = "NOT FOUND!";
    currentState = STATUS_MSG;
    statusTimer = millis();
    return;
  }

  String headerSample = "";
  while (irFile.available() && headerSample.length() < 120) {
    headerSample += (char)irFile.read();
  }
  irFile.seek(0);

  IrReceiver.stop();
  IrSender.setSendPin(useExternalIR ? IR_SEND_EXTERNAL_PIN : IR_SEND_INTERNAL_PIN);

  if (headerSample.indexOf("Protocol:") >= 0 || headerSample.indexOf("protocol:") >= 0 || headerSample.indexOf("Filetype:") >= 0) {
    String name = "";
    String type = "";
    String protocol = "";
    uint32_t address = 0;
    uint32_t command = 0;
    uint16_t frequency = 38000;
    
    uint16_t rawBuffer[RAW_BUFFER_LENGTH];
    size_t rawLen = 0;
    
    int currentSignalIdx = -1;
    bool insideTargetSignal = false;

    while (irFile.available()) {
      String line = irFile.readStringUntil('\n');
      line.trim();
      if (line.endsWith("\r")) line.remove(line.length() - 1);

      if (line.startsWith("Name: ") || line.startsWith("name: ")) {
        currentSignalIdx++;
        if (currentSignalIdx == targetSignalIndex) {
          insideTargetSignal = true;
          name = line.substring(6);
        } else if (currentSignalIdx > targetSignalIndex) {
          break;
        }
      } 
      else if (insideTargetSignal) {
        if (line.startsWith("Type: ") || line.startsWith("type: ")) {
          type = line.substring(6);
        } 
        else if (line.startsWith("Protocol: ") || line.startsWith("protocol: ")) {
          protocol = line.substring(10);
        } 
        else if (line.startsWith("Address: ") || line.startsWith("address: ")) {
          address = parseFlipperHexBytes(line.substring(9));
        } 
        else if (line.startsWith("Command: ") || line.startsWith("command: ")) {
          command = parseFlipperHexBytes(line.substring(9));
        } 
        else if (line.startsWith("Frequency: ") || line.startsWith("frequency: ")) {
          frequency = line.substring(11).toInt();
        } 
        else if (line.startsWith("Data: ") || line.startsWith("data: ")) {
          String dataStr = line.substring(6);
          rawLen = 0;
          dataStr.trim();
          
          while (dataStr.length() > 0 && rawLen < RAW_BUFFER_LENGTH) {
            int spaceIdx = dataStr.indexOf(' ');
            String valStr = (spaceIdx != -1) ? dataStr.substring(0, spaceIdx) : dataStr;
            if (spaceIdx != -1) {
              dataStr = dataStr.substring(spaceIdx + 1);
              dataStr.trim();
            } else {
              dataStr = "";
            }
            rawBuffer[rawLen++] = valStr.toInt();
          }
        }
      }
    }

    if (insideTargetSignal) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_ncenB08_tr);
      u8g2.drawStr(34, 12, "MIMIC V2");
      u8g2.drawLine(0, 14, 127, 14);

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(20, 28, "SENDING...");
      u8g2.drawStr(10, 46, name.length() > 0 ? name.c_str() : displayName.c_str());

      if (useExternalIR) {
        u8g2.drawStr(10, 60, "[EXT LED]");
      } else {
        u8g2.drawStr(10, 60, "[INT LED]");
      }

      u8g2.sendBuffer();

      if (type == "parsed" || protocol.length() > 0) {
        executeParsedIR(protocol, address, command);
      } 
      else if (type == "raw" && rawLen > 0) {
        IrSender.sendRaw(rawBuffer, rawLen, frequency > 0 ? (frequency / 1000) : 38);
      }

      delay(IR_GAP_MS);
    }
  } 
  else {
    uint16_t buffer[RAW_BUFFER_LENGTH];
    uint16_t count = 0;

    while (irFile.available() && count < RAW_BUFFER_LENGTH) {
      String valStr = irFile.readStringUntil(',');
      valStr.trim();
      if (valStr.length() > 0) {
        buffer[count] = valStr.toInt();
        count++;
      }
    }

    if (count > 0) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_ncenB08_tr);
      u8g2.drawStr(34, 12, "MIMIC V2");
      u8g2.drawLine(0, 14, 127, 14);

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(24, 32, "SENDING...");
      u8g2.drawStr(10, 48, displayName.c_str());
      u8g2.sendBuffer();

      IrSender.sendRaw(buffer, count, 38);
      delay(IR_GAP_MS);
    }
  }

  irFile.close();
  IrReceiver.start();
}

// --- UNIVERSAL OFF: AZ ÖSSZES JEL LEJÁTSZÁSA ---
void playAllSignalsInUniversalFile(String filePath) {
  File irFile = SD.open(filePath, FILE_READ);
  if (!irFile) {
    statusMessage = "NOT FOUND!";
    currentState = STATUS_MSG;
    statusTimer = millis();
    return;
  }

  IrReceiver.stop();
  IrSender.setSendPin(useExternalIR ? IR_SEND_EXTERNAL_PIN : IR_SEND_INTERNAL_PIN);

  String fullHeaderCheck = "";
  while (irFile.available() && fullHeaderCheck.length() < 300) {
    fullHeaderCheck += (char)irFile.read();
  }
  irFile.seek(0);

  bool isFlipperFormat = (fullHeaderCheck.indexOf("Filetype:") >= 0 || 
                          fullHeaderCheck.indexOf("protocol:") >= 0 || 
                          fullHeaderCheck.indexOf("Protocol:") >= 0 ||
                          fullHeaderCheck.indexOf("type:") >= 0);

  if (isFlipperFormat) {
    String name = "";
    String type = "";
    String protocol = "";
    uint32_t address = 0;
    uint32_t command = 0;
    uint16_t frequency = 38000;
    
    uint16_t rawBuffer[RAW_BUFFER_LENGTH];
    size_t rawLen = 0;

    while (irFile.available()) {
      String line = irFile.readStringUntil('\n');
      line.trim();
      if (line.endsWith("\r")) line.remove(line.length() - 1);

      if (line.startsWith("Name: ") || line.startsWith("name: ")) {
        name = line.substring(6);
      } 
      else if (line.startsWith("Type: ") || line.startsWith("type: ")) {
        type = line.substring(6);
      } 
      else if (line.startsWith("Protocol: ") || line.startsWith("protocol: ")) {
        protocol = line.substring(10);
      } 
      else if (line.startsWith("Address: ") || line.startsWith("address: ")) {
        address = parseFlipperHexBytes(line.substring(9));
      } 
      else if (line.startsWith("Command: ") || line.startsWith("command: ")) {
        command = parseFlipperHexBytes(line.substring(9));
      } 
      else if (line.startsWith("Frequency: ") || line.startsWith("frequency: ")) {
        frequency = line.substring(11).toInt();
      } 
      else if (line.startsWith("Data: ") || line.startsWith("data: ")) {
        String dataStr = line.substring(6);
        rawLen = 0;
        dataStr.trim();
        
        while (dataStr.length() > 0 && rawLen < RAW_BUFFER_LENGTH) {
          int spaceIdx = dataStr.indexOf(' ');
          String valStr = (spaceIdx != -1) ? dataStr.substring(0, spaceIdx) : dataStr;
          if (spaceIdx != -1) {
            dataStr = dataStr.substring(spaceIdx + 1);
            dataStr.trim();
          } else {
            dataStr = "";
          }
          rawBuffer[rawLen++] = valStr.toInt();
        }
      }

      if ((line.startsWith("#") || line.length() == 0 || !irFile.available()) && (protocol.length() > 0 || rawLen > 0)) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(34, 12, "MIMIC V2");
        u8g2.drawLine(0, 14, 127, 14);

        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(10, 28, "UNIV SENDING");
        u8g2.drawStr(10, 46, name.length() > 0 ? name.c_str() : "Sending...");

        if (useExternalIR) {
          u8g2.drawStr(10, 60, "[EXT LED]");
        } else {
          u8g2.drawStr(10, 60, "[INT LED]");
        }
        u8g2.sendBuffer();

        protocol.trim();
        type.trim();

        if (type == "parsed" || protocol.length() > 0) {
          executeParsedIR(protocol, address, command);
        } 
        else if (type == "raw" && rawLen > 0) {
          IrSender.sendRaw(rawBuffer, rawLen, frequency > 0 ? (frequency / 1000) : 38);
        }

        delay(IR_GAP_MS); 

        name = "";
        type = "";
        protocol = "";
        address = 0;
        command = 0;
        rawLen = 0;
      }
    }
  } else {
    uint16_t buffer[RAW_BUFFER_LENGTH];
    uint16_t count = 0;

    while (irFile.available() && count < RAW_BUFFER_LENGTH) {
      String valStr = irFile.readStringUntil(',');
      valStr.trim();
      if (valStr.length() > 0) {
        buffer[count] = valStr.toInt();
        count++;
      }
    }

    if (count > 0) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_ncenB08_tr);
      u8g2.drawStr(34, 12, "MIMIC V2");
      u8g2.drawLine(0, 14, 127, 14);

      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(20, 32, "UNIV SENDING");
      u8g2.sendBuffer();

      IrSender.sendRaw(buffer, count, 38);
      delay(IR_GAP_MS);
    }
  }

  irFile.close();
  IrReceiver.start();

  statusMessage = "UNIV. FINISHED";
  currentState = STATUS_MSG;
  statusTimer = millis();
}

// --- IR FÁJL KIVÁLASZTÁS LOAD-BÓL ---
void handleIRFileSelect(String filename) {
  if (!sdInitialized) return;

  String filePath = "/ir_files/" + filename;
  int signalsFound = countAndParseSignalsInFile(filePath);

  if (signalsFound > 1) {
    currentMultiFilePath = filePath;
    selectedSubIndex = 0;
    currentState = SUB_IR_LIST;
  } else {
    processAndSendIRFile(filePath, filename, 0);
  }
}

// --- UNIVERSAL FÁJL KIVÁLASZTÁS ---
void handleUniversalFileSelect(String filename) {
  if (!sdInitialized) return;

  String filePath = "/universal/" + filename;
  playAllSignalsInUniversalFile(filePath);
}

// --- PONG JÁTÉK ---
void resetPongBall() {
  ballX = 64.0;
  ballY = 32.0;
  ballSpeedX = (random(0, 2) == 0) ? 2.2 : -2.2;
  ballSpeedY = random(-15, 15) / 10.0;
}

void resetPongGame() {
  playerY = 24;
  cpuY = 24;
  playerScore = 0;
  cpuScore = 0;
  resetPongBall();
}

void updateAndDrawPong() {
  // A Pong folyamatos gombnyomást (nyers állapotot) igényel a folyamatos mozgáshoz
  if (upRaw && playerY > 0) {
    playerY -= 3;
  }
  if (downRaw && playerY < (64 - paddleHeight)) {
    playerY += 3;
  }

  if (cpuY + (paddleHeight / 2) < ballY - 2 && cpuY < (64 - paddleHeight)) {
    cpuY += 2;
  } else if (cpuY + (paddleHeight / 2) > ballY + 2 && cpuY > 0) {
    cpuY -= 2;
  }

  ballX += ballSpeedX;
  ballY += ballSpeedY;

  if (ballY <= 0 || ballY >= 62) {
    ballSpeedY = -ballSpeedY;
  }

  if (ballX <= (2 + paddleWidth) && ballY >= playerY && ballY <= (playerY + paddleHeight)) {
    ballSpeedX = -ballSpeedX;
    ballSpeedY += (ballY - (playerY + paddleHeight / 2.0)) * 0.15;
  }

  if (ballX >= (125 - paddleWidth) && ballY >= cpuY && ballY <= (cpuY + paddleHeight)) {
    ballSpeedX = -ballSpeedX;
    ballSpeedY += (ballY - (cpuY + paddleHeight / 2.0)) * 0.15;
  }

  if (ballX < 0) {
    cpuScore++;
    resetPongBall();
  } else if (ballX > 128) {
    playerScore++;
    resetPongBall();
  }

  u8g2.clearBuffer();

  for (int y = 0; y < 64; y += 6) {
    u8g2.drawVLine(64, y, 3);
  }

  u8g2.drawBox(2, playerY, paddleWidth, paddleHeight);
  u8g2.drawBox(123, cpuY, paddleWidth, paddleHeight);

  u8g2.drawBox((int)ballX, (int)ballY, 3, 3);

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(48, 10);
  u8g2.print(playerScore);
  u8g2.setCursor(72, 10);
  u8g2.print(cpuScore);

  u8g2.sendBuffer();
}

// --- OLED MEGJELENÍTÉS ---
void updateDisplay() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(34, 12, "MIMIC V2");
  u8g2.drawLine(0, 14, 127, 14);

  u8g2.setFont(u8g2_font_6x10_tr);

  // --- MAIN MENU ---
  if (currentState == MAIN_MENU) {
    const char* menuItems[] = {"IR Signal", "Notes", "Games", "Settings"};

    int topIndex = mainMenuIndex - 1;
    if (topIndex < 0) topIndex = 0;
    if (topIndex + 2 >= MAIN_MENU_TOTAL) topIndex = max(0, MAIN_MENU_TOTAL - 2);

    int yPos = 32;
    for (int i = topIndex; i < topIndex + 2 && i < MAIN_MENU_TOTAL; i++) {
      if (i == mainMenuIndex) {
        u8g2.drawStr(0, yPos, ">");
        u8g2.drawStr(10, yPos, menuItems[i]);
      } else {
        u8g2.drawStr(10, yPos, menuItems[i]);
      }
      yPos += 18;
    }
    drawScrollbar(mainMenuIndex, MAIN_MENU_TOTAL, 2);
  }
  // --- IR MENU ---
  else if (currentState == IR_MENU) {
    const char* irMenuItems[] = {"Load IR", "Universal IR", "Save IR"};

    int topIndex = irMenuIndex - 1;
    if (topIndex < 0) topIndex = 0;
    if (topIndex + 2 >= IR_MENU_TOTAL) topIndex = max(0, IR_MENU_TOTAL - 2);

    int yPos = 32;
    for (int i = topIndex; i < topIndex + 2 && i < IR_MENU_TOTAL; i++) {
      if (i == irMenuIndex) {
        u8g2.drawStr(0, yPos, ">");
        u8g2.drawStr(10, yPos, irMenuItems[i]);
      } else {
        u8g2.drawStr(10, yPos, irMenuItems[i]);
      }
      yPos += 18;
    }
    drawScrollbar(irMenuIndex, IR_MENU_TOTAL, 2);
  }
  // --- SETTINGS MENU ---
  else if (currentState == SETTINGS_MENU) {
    if (settingsMenuIndex == 0) {
      u8g2.drawStr(0, 32, ">");
    }
    u8g2.drawStr(10, 32, "LED: ");
    u8g2.drawStr(46, 32, useExternalIR ? "EXTERNAL" : "INTERNAL");

    if (settingsMenuIndex == 1) {
      u8g2.drawStr(0, 50, ">");
    }
    u8g2.drawStr(10, 50, "Save: ");
    u8g2.drawStr(46, 50, saveAsFlipper ? "FLIPPER" : "RAW");
  }
  // --- LOAD IR LIST & UNIVERSAL LIST & NOTE LIST ---
  else if (currentState == LOAD_IR_LIST || currentState == LOAD_UNIV_LIST || currentState == LOAD_NOTE_LIST) {
    if (totalFiles == 0) {
      u8g2.drawStr(10, 36, "No files found!");
      u8g2.drawStr(0, 52, "Press LEFT to back");
    } else {
      int topIndex = selectedIndex - 1;
      if (topIndex < 0) topIndex = 0;
      if (topIndex + 3 >= totalFiles && totalFiles >= 3) topIndex = max(0, totalFiles - 3);

      int yPos = 28;
      for (int i = topIndex; i < topIndex + 3 && i < totalFiles; i++) {
        if (i == selectedIndex) {
          u8g2.drawStr(0, yPos, ">");
          u8g2.drawStr(10, yPos, fileList[i].c_str());
        } else {
          u8g2.drawStr(10, yPos, fileList[i].c_str());
        }
        yPos += 13;
      }

      drawScrollbar(selectedIndex, totalFiles, 3);
    }
  }
  // --- SUB IR LIST ---
  else if (currentState == SUB_IR_LIST) {
    if (totalSubSignals == 0) {
      u8g2.drawStr(10, 36, "No signals found!");
    } else {
      int topIndex = selectedSubIndex - 1;
      if (topIndex < 0) topIndex = 0;
      if (topIndex + 3 >= totalSubSignals && totalSubSignals >= 3) topIndex = max(0, totalSubSignals - 3);

      int yPos = 28;
      for (int i = topIndex; i < topIndex + 3 && i < totalSubSignals; i++) {
        if (i == selectedSubIndex) {
          u8g2.drawStr(0, yPos, ">");
          u8g2.drawStr(10, yPos, subSignalNames[i].c_str());
        } else {
          u8g2.drawStr(10, yPos, subSignalNames[i].c_str());
        }
        yPos += 13;
      }

      drawScrollbar(selectedSubIndex, totalSubSignals, 3);
    }
  }
  // --- VIEW NOTE ---
  else if (currentState == VIEW_NOTE) {
    if (totalNoteLines == 0) {
      u8g2.drawStr(10, 36, "Empty Note!");
    } else {
      int yPos = 26;
      for (int i = noteScrollIndex; i < noteScrollIndex + 4 && i < totalNoteLines; i++) {
        u8g2.drawStr(0, yPos, noteLines[i].c_str());
        yPos += 11;
      }
      drawScrollbar(noteScrollIndex, totalNoteLines, 4);
    }
  }
  // --- SAVE IR WAIT ---
  else if (currentState == SAVE_IR_WAIT) {
    u8g2.drawStr(10, 32, "WAITING FOR IR...");
    u8g2.drawStr(0, 50, "Press LEFT to cancel");
  }
  // --- STATUS MSG ---
  else if (currentState == STATUS_MSG) {
    u8g2.drawStr(10, 38, statusMessage.c_str());
  }

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);

  loadSettings(); 

  pinMode(IR_SEND_EXTERNAL_PIN, OUTPUT);
  IrSender.begin(IR_SEND_INTERNAL_PIN);
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  Wire.begin(6, 7);
  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();

  Wire.beginTransmission(0x3C);
  Wire.write(0x00);
  Wire.write(0xAF);
  Wire.endTransmission();

  analogSetAttenuation(ADC_11db);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("SD Card Error!");
    sdInitialized = false;
  } else {
    sdInitialized = true;
    if (!SD.exists("/ir_files")) SD.mkdir("/ir_files");
    if (!SD.exists("/notes")) SD.mkdir("/notes");
    if (!SD.exists("/universal")) SD.mkdir("/universal");
  }

  lastActivityTime = millis();
}

void loop() {
  readButtons();

  if (millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
    goToSleep();
  }

  if (currentState == STATUS_MSG && (millis() - statusTimer > 1500)) {
    currentState = MAIN_MENU; 
  }

  // --- 1. MAIN MENU ---
  if (currentState == MAIN_MENU) {
    if (downTriggered) {
      mainMenuIndex = (mainMenuIndex + 1) % MAIN_MENU_TOTAL;
    } 
    else if (upTriggered) {
      mainMenuIndex = (mainMenuIndex - 1 + MAIN_MENU_TOTAL) % MAIN_MENU_TOTAL;
    } 
    else if (rightTriggered) {
      if (mainMenuIndex == 0) {
        currentState = IR_MENU;
        irMenuIndex = 0;
      }
      else if (mainMenuIndex == 1) {
        loadFileListFromFolder("/notes");
        currentState = LOAD_NOTE_LIST;
      }
      else if (mainMenuIndex == 2) {
        resetPongGame();
        currentState = GAME_PONG;
      }
      else if (mainMenuIndex == 3) {
        currentState = SETTINGS_MENU;
        settingsMenuIndex = 0;
      }
    }
  }

  // --- 2. IR MENU ---
  else if (currentState == IR_MENU) {
    if (downTriggered) {
      irMenuIndex = (irMenuIndex + 1) % IR_MENU_TOTAL;
    } 
    else if (upTriggered) {
      irMenuIndex = (irMenuIndex - 1 + IR_MENU_TOTAL) % IR_MENU_TOTAL;
    } 
    else if (rightTriggered) {
      if (irMenuIndex == 0) {
        loadFileListFromFolder("/ir_files");
        currentState = LOAD_IR_LIST;
      } 
      else if (irMenuIndex == 1) {
        loadFileListFromFolder("/universal");
        currentState = LOAD_UNIV_LIST;
      } 
      else if (irMenuIndex == 2) {
        currentState = SAVE_IR_WAIT;
        IrReceiver.start();
      }
    } 
    else if (leftTriggered) {
      currentState = MAIN_MENU;
    }
  }

  // --- 3. SETTINGS MENU ---
  else if (currentState == SETTINGS_MENU) {
    if (downTriggered) {
      settingsMenuIndex = (settingsMenuIndex + 1) % SETTINGS_MENU_TOTAL;
    } 
    else if (upTriggered) {
      settingsMenuIndex = (settingsMenuIndex - 1 + SETTINGS_MENU_TOTAL) % SETTINGS_MENU_TOTAL;
    } 
    else if (rightTriggered) {
      if (settingsMenuIndex == 0) {
        useExternalIR = !useExternalIR;
        saveSettings();
      } else if (settingsMenuIndex == 1) {
        saveAsFlipper = !saveAsFlipper;
        saveSettings();
      }
    } 
    else if (leftTriggered) {
      currentState = MAIN_MENU;
    }
  }

  // --- 4. LOAD IR LIST / LOAD UNIV LIST ---
  else if (currentState == LOAD_IR_LIST || currentState == LOAD_UNIV_LIST) {
    if (downTriggered) {
      if (totalFiles > 0) selectedIndex = (selectedIndex + 1) % totalFiles;
    } 
    else if (upTriggered) {
      if (totalFiles > 0) selectedIndex = (selectedIndex - 1 + totalFiles) % totalFiles;
    } 
    else if (rightTriggered) {
      if (totalFiles > 0) {
        if (currentState == LOAD_IR_LIST) {
          handleIRFileSelect(fileList[selectedIndex]);
        } else if (currentState == LOAD_UNIV_LIST) {
          handleUniversalFileSelect(fileList[selectedIndex]);
        }
      }
    } 
    else if (leftTriggered) {
      currentState = IR_MENU; 
    }
  }

  // --- 5. SUB IR LIST ---
  else if (currentState == SUB_IR_LIST) {
    if (downTriggered) {
      if (totalSubSignals > 0) selectedSubIndex = (selectedSubIndex + 1) % totalSubSignals;
    } 
    else if (upTriggered) {
      if (totalSubSignals > 0) selectedSubIndex = (selectedSubIndex - 1 + totalSubSignals) % totalSubSignals;
    } 
    else if (rightTriggered) {
      if (totalSubSignals > 0) processAndSendIRFile(currentMultiFilePath, subSignalNames[selectedSubIndex], selectedSubIndex);
    } 
    else if (leftTriggered) {
      currentState = LOAD_IR_LIST;
    }
  }

  // --- 6. LOAD NOTE LIST ---
  else if (currentState == LOAD_NOTE_LIST) {
    if (downTriggered) {
      if (totalFiles > 0) selectedIndex = (selectedIndex + 1) % totalFiles;
    } 
    else if (upTriggered) {
      if (totalFiles > 0) selectedIndex = (selectedIndex - 1 + totalFiles) % totalFiles;
    } 
    else if (rightTriggered) {
      if (totalFiles > 0) {
        loadAndWrapNote(fileList[selectedIndex]);
        currentState = VIEW_NOTE;
      }
    } 
    else if (leftTriggered) {
      currentState = MAIN_MENU;
    }
  }

  // --- 7. VIEW NOTE ---
  else if (currentState == VIEW_NOTE) {
    if (downTriggered) {
      if (noteScrollIndex + 4 < totalNoteLines) noteScrollIndex++;
    } 
    else if (upTriggered) {
      if (noteScrollIndex > 0) noteScrollIndex--;
    } 
    else if (leftTriggered) {
      currentState = LOAD_NOTE_LIST;
    }
  }

  // --- 8. SAVE IR WAIT ---
  else if (currentState == SAVE_IR_WAIT) {
    if (IrReceiver.decode()) {
      saveIRSignal();
      IrReceiver.resume();
    }

    if (leftTriggered) {
      currentState = IR_MENU;
    }
  }

  // --- 9. PONG GAME ---
  else if (currentState == GAME_PONG) {
    updateAndDrawPong();

    if (leftTriggered) {
      currentState = MAIN_MENU;
    }
  }

  // --- DISPLAY REFRESH ---
  if (currentState != GAME_PONG) {
    updateDisplay();
  }

  delay(20);
}