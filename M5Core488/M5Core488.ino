#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <sys/time.h>

#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#define HAVE_SNTP_STATUS 1
#elif __has_include(<sntp.h>)
#include <sntp.h>
#define HAVE_SNTP_STATUS 1
#else
#define HAVE_SNTP_STATUS 0
#endif

#include <EspUsbHost.h>
#include "USB.h"
#include "USBMSC.h"

// ============================================================
// Version
// ============================================================
#define APP_VERSION "Rev.1D.1"
#define APP_NAME    "M5Core488"

// ============================================================
// CoreS3 microSD
// ============================================================
#define SD_SPI_CS_PIN    4
#define SD_SPI_SCK_PIN  36
#define SD_SPI_MISO_PIN 35
#define SD_SPI_MOSI_PIN 37
#define SD_FREQ 25000000

#define MACRO_DIR         "/macros"
#define LOG_DIR           "/logs"
#define CONFIG_DIR        "/config"
#define WIFI_CONFIG_FILE  "/config/wifi.ini"
#define MACRO_CONFIG_FILE "/config/macros.ini"
#define UPLOAD_TEMP_FILE  "/macros/.upload.tmp"

// ============================================================
// Macro Engine
// ============================================================
#define AR488_NORMAL_TIMEOUT_MS  150
#define AR488_LOCAL_TIMEOUT_MS   500
#define AR488_READ_TIMEOUT_MS   3500
#define AR488_IDLE_TIMEOUT_MS     80
#define MAX_WAIT_MS           600000UL

// ============================================================
// Wi-Fi / NTP
// ============================================================
#define WIFI_CONNECT_TIMEOUT_MS 10000UL
#define NTP_SYNC_TIMEOUT_MS     10000UL
#define WIFI_RECONNECT_INTERVAL_MS 15000UL
#define MAX_NTP_RESYNC_HOURS        720
#define MAX_AUTO_AR488_SECONDS        60

// ============================================================
// Lists
// ============================================================
#define MAX_MACROS        40
#define MACROS_PER_PAGE    5
#define MAX_WEB_LOGS      50
#define MAX_MACRO_BYTES 65536UL

struct WifiConfig {
  String ssid;
  String password;
  String timezone = "UTC+00:00";
  String ntp1 = "pool.ntp.org";
  String ntp2 = "time.google.com";
  String ntp3 = "time.cloudflare.com";
  uint16_t ntpResyncHours = 24;  // 0 = boot only
  bool webEnable = false;
  uint16_t webPort = 80;
  String hostname = "m5core488";
  uint8_t autoAr488Seconds = 5;  // 0 = stay at mode selection
};

WifiConfig wifiConfig;

enum TimeSource { TIME_UNSYNCED, TIME_RTC, TIME_NTP };
TimeSource timeSource = TIME_UNSYNCED;

bool rtcAvailable = false;
bool rtcTimeValid = false;
bool wifiConfigLoaded = false;
bool timezoneValid = true;
bool resyncConfigValid = true;
bool autoAr488ConfigValid = true;
bool ntpSyncOK = false;
int utcOffsetMinutes = 0;
uint32_t lastNtpAttemptMillis = 0;
uint32_t lastWiFiReconnectAttemptMillis = 0;
bool lastWiFiConnectedState = false;

// ============================================================
// CSV Logger
// ============================================================
File logFile;
String currentLogDate;
String currentLogPath;
String currentMacroName;
bool loggerFault = false;

// ============================================================
// Macros
// ============================================================
String macroFiles[MAX_MACROS];
bool macroEnabled[MAX_MACROS];
bool macroHasLoopSection[MAX_MACROS];
bool macroFormatValid[MAX_MACROS];
int macroCount = 0;
int selectedMacro = -1;
int pageStart = 0;

// ============================================================
// USB Host / AR488
// ============================================================
EspUsbHost usbHost;
EspUsbHostCdcSerial AR488(usbHost);

// ============================================================
// USB Device / MSC
// ============================================================
USBMSC MSC;

// ============================================================
// Operating mode
// ============================================================
enum OperatingMode { MODE_SELECT, MODE_AR488, MODE_USB_SD };
OperatingMode currentMode = MODE_SELECT;

bool sdReady = false;
bool lastAR488Connected = false;
bool macroBusy = false;
bool uiWaitingForReturn = false;

// Loop execution state
bool loopActive = false;
bool loopStopRequested = false;
String loopMacroPath;
String loopMacroName;
uint32_t loopCycle = 0;
uint32_t loopTouchEnableMillis = 0;

// Auto-start AR488 MODE after boot (0 = disabled)
bool autoAr488Armed = false;
uint32_t autoAr488DeadlineMillis = 0;
int lastAutoAr488SecondsShown = -1;

// Incremented whenever a macro finishes/aborts.
// The Web UI uses this to refresh the log list even for very short runs.
uint32_t macroCompletionCounter = 0;

// ============================================================
// Web
// ============================================================
WebServer* webServer = nullptr;
bool webServerRunning = false;
bool mdnsRunning = false;

bool webRunRequested = false;
String webRequestedMacroPath;
bool webLoopRequested = false;
String webRequestedLoopPath;

File uploadTempFile;
bool uploadOK = false;
String uploadError;
String uploadTargetName;
uint32_t uploadReceivedBytes = 0;

// ============================================================
// Macro format types
//
// IMPORTANT:
// These types are intentionally declared before the first function body.
// Arduino's .ino preprocessor auto-generates function prototypes near the
// top of the translation unit. Keeping MacroLayout visible here prevents
// the generated inspectMacroLayout() prototype from referring to an
// undeclared type.
// ============================================================
enum MacroSectionType {
  MACRO_SECTION_NONE,
  MACRO_SECTION_SETUP,
  MACRO_SECTION_LOOP
};

struct MacroLayout {
  bool valid = true;
  bool sectioned = false;
  bool hasSetup = false;
  bool hasLoop = false;
  uint32_t setupStart = 0;
  uint32_t setupEnd = 0;
  uint32_t loopStart = 0;
  uint32_t loopEnd = 0;
  int setupFirstLine = 1;
  int loopFirstLine = 1;
  uint32_t setupExecutableLines = 0;
  uint32_t loopExecutableLines = 0;
  String error;
};

// ============================================================
// Touch areas
// ============================================================
struct ButtonArea { int x, y, w, h; };
ButtonArea btnAR488 = {30, 70, 260, 60};
ButtonArea btnUSBSD = {30, 150, 260, 60};
ButtonArea btnPrev  = {0, 194, 64, 46};
ButtonArea btnNext  = {64, 194, 64, 46};
ButtonArea btnRun   = {128, 194, 96, 46};
ButtonArea btnLoop  = {224, 194, 96, 46};

// ============================================================
// Generic helpers
// ============================================================
bool touched(const ButtonArea& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

bool macroOperationLocked() {
  return macroBusy || webRunRequested || webLoopRequested || loopActive;
}

void drawButton(const ButtonArea& b, const char* text, bool enabled = true) {
  uint16_t color = enabled ? TFT_WHITE : TFT_DARKGREY;
  M5.Display.drawRect(b.x + 2, b.y + 2, b.w - 4, b.h - 4, color);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(text, b.x + b.w / 2, b.y + b.h / 2);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

void removeUtf8Bom(String& line) {
  if (line.length() >= 3 &&
      (uint8_t)line[0] == 0xEF &&
      (uint8_t)line[1] == 0xBB &&
      (uint8_t)line[2] == 0xBF) {
    line.remove(0, 3);
  }
}

bool parseUnsignedInteger(const String& value, uint32_t& result) {
  if (value.length() == 0) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  result = strtoul(value.c_str(), nullptr, 10);
  return true;
}

bool parseBoolValue(String value, bool& result) {
  value.trim();
  value.toLowerCase();
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    result = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    result = false;
    return true;
  }
  return false;
}

String getBaseName(const String& path) {
  int slash1 = path.lastIndexOf('/');
  int slash2 = path.lastIndexOf('\\');
  int slash = max(slash1, slash2);
  return slash >= 0 ? path.substring(slash + 1) : path;
}

String getMacroTitle(const String& path) {
  String title = getBaseName(path);
  String lower = title;
  lower.toLowerCase();
  if (lower.endsWith(".mac")) title.remove(title.length() - 4);
  return title;
}

bool isSafeLeafName(const String& name) {
  if (name.length() == 0 || name.length() > 96) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (name.indexOf("..") >= 0) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    uint8_t c = (uint8_t)name[i];
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool isSafeMacroName(const String& name) {
  if (!isSafeLeafName(name)) return false;
  if (name.indexOf('=') >= 0 || name.startsWith("#") || name.startsWith(";")) return false;
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mac");
}

bool isSafeLogName(const String& name) {
  if (!isSafeLeafName(name)) return false;
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".csv");
}

String htmlEscape(const String& src) {
  String out;
  out.reserve(src.length() + 16);
  for (size_t i = 0; i < src.length(); ++i) {
    char c = src[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String jsonEscape(const String& src) {
  String out;
  out.reserve(src.length() + 16);
  for (size_t i = 0; i < src.length(); ++i) {
    uint8_t c = (uint8_t)src[i];
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else {
          out += (char)c;
        }
        break;
    }
  }
  return out;
}

String urlEncode(const String& src) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(src.length() * 3);
  for (size_t i = 0; i < src.length(); ++i) {
    uint8_t c = (uint8_t)src[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// ============================================================
// SD
// ============================================================
bool initSD() {
  if (sdReady) return true;
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, SD_FREQ)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  sdReady = true;
  return true;
}

bool ensureDirectory(const char* path) {
  if (!initSD()) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

bool ensureLogDirectory() { return ensureDirectory(LOG_DIR); }
bool ensureConfigDirectory() { return ensureDirectory(CONFIG_DIR); }


// ============================================================
// Macro format inspection
//
// Legacy format:
//   every executable line is run once by RUN.
//
// Sectioned format:
//   [setup] = executed once at the beginning
//   [loop]  = executed once by RUN, repeatedly by LOOP
//
// Executable lines outside sections are rejected once section syntax is used.
// ============================================================
bool inspectMacroLayout(const String& path, MacroLayout& layout) {
  layout = MacroLayout();
  if (!initSD()) {
    layout.valid = false;
    layout.error = "SD unavailable";
    return false;
  }

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    layout.valid = false;
    layout.error = "Macro not found";
    return false;
  }

  MacroSectionType currentSection = MACRO_SECTION_NONE;
  bool outsideExecutableFound = false;
  int outsideExecutableLine = 0;
  bool unknownSectionFound = false;
  String unknownSectionText;
  int unknownSectionLine = 0;
  int lineNumber = 0;

  while (file.available()) {
    uint32_t lineStart = (uint32_t)file.position();
    String line = file.readStringUntil('\n');
    uint32_t lineEnd = (uint32_t)file.position();
    ++lineNumber;
    line.trim();
    if (lineNumber == 1) {
      removeUtf8Bom(line);
      line.trim();
    }

    if (line.length() == 0 || line.startsWith("#")) continue;

    String lower = line;
    lower.toLowerCase();

    if (lower == "[setup]") {
      layout.sectioned = true;
      if (layout.hasSetup) {
        layout.valid = false;
        layout.error = "Duplicate [setup] at line " + String(lineNumber);
        break;
      }
      if (layout.hasLoop) {
        layout.valid = false;
        layout.error = "[setup] must appear before [loop]";
        break;
      }
      layout.hasSetup = true;
      layout.setupStart = lineEnd;
      layout.setupFirstLine = lineNumber + 1;
      currentSection = MACRO_SECTION_SETUP;
      continue;
    }

    if (lower == "[loop]") {
      layout.sectioned = true;
      if (layout.hasLoop) {
        layout.valid = false;
        layout.error = "Duplicate [loop] at line " + String(lineNumber);
        break;
      }
      if (currentSection == MACRO_SECTION_SETUP) layout.setupEnd = lineStart;
      layout.hasLoop = true;
      layout.loopStart = lineEnd;
      layout.loopFirstLine = lineNumber + 1;
      currentSection = MACRO_SECTION_LOOP;
      continue;
    }

    if (line.startsWith("[") && line.endsWith("]")) {
      unknownSectionFound = true;
      if (unknownSectionLine == 0) {
        unknownSectionText = line;
        unknownSectionLine = lineNumber;
      }
      if (currentSection != MACRO_SECTION_NONE) {
        // Count it as content for section validation, but do not send it.
        if (currentSection == MACRO_SECTION_SETUP) ++layout.setupExecutableLines;
        else if (currentSection == MACRO_SECTION_LOOP) ++layout.loopExecutableLines;
      } else {
        outsideExecutableFound = true;
        if (outsideExecutableLine == 0) outsideExecutableLine = lineNumber;
      }
      continue;
    }

    if (currentSection == MACRO_SECTION_NONE) {
      outsideExecutableFound = true;
      if (outsideExecutableLine == 0) outsideExecutableLine = lineNumber;
    } else if (currentSection == MACRO_SECTION_SETUP) {
      ++layout.setupExecutableLines;
    } else if (currentSection == MACRO_SECTION_LOOP) {
      ++layout.loopExecutableLines;
    }
  }

  uint32_t fileSize = (uint32_t)file.size();
  file.close();

  if (!layout.valid) return false;

  if (layout.hasSetup && layout.setupEnd == 0) layout.setupEnd = fileSize;
  if (layout.hasLoop) layout.loopEnd = fileSize;

  if (layout.sectioned && unknownSectionFound) {
    layout.valid = false;
    layout.error = "Unknown section " + unknownSectionText + " at line " + String(unknownSectionLine);
    return false;
  }

  if (layout.sectioned && outsideExecutableFound) {
    layout.valid = false;
    layout.error = "Command outside [setup]/[loop] at line " + String(outsideExecutableLine);
    return false;
  }

  if (layout.hasLoop && layout.loopExecutableLines == 0) {
    layout.valid = false;
    layout.error = "[loop] section is empty";
    return false;
  }

  return true;
}

// ============================================================
// Time
// ============================================================
bool parseUtcOffset(const String& text, int& resultMinutes) {
  String s = text;
  s.trim();
  s.toUpperCase();
  if (s == "UTC") { resultMinutes = 0; return true; }
  if (s.length() != 9 || !s.startsWith("UTC") || s[6] != ':') return false;
  char sign = s[3];
  if (sign != '+' && sign != '-') return false;
  if (!isDigit(s[4]) || !isDigit(s[5]) || !isDigit(s[7]) || !isDigit(s[8])) return false;
  int hours = s.substring(4, 6).toInt();
  int minutes = s.substring(7, 9).toInt();
  if (hours > 14 || minutes > 59 || (hours == 14 && minutes != 0)) return false;
  resultMinutes = hours * 60 + minutes;
  if (sign == '-') resultMinutes = -resultMinutes;
  return true;
}

const char* getTimeSourceName() {
  switch (timeSource) {
    case TIME_NTP: return "NTP";
    case TIME_RTC: return "RTC";
    default: return "---";
  }
}

bool systemTimeValid() {
  return time(nullptr) > 1704067200; // 2024-01-01 UTC
}

String getUtcTimestampMs() {
  if (!systemTimeValid()) return "UNSYNCED";
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return "UNSYNCED";
  struct tm utcTime;
  gmtime_r(&tv.tv_sec, &utcTime);
  long ms = tv.tv_usec / 1000L;
  char buffer[40];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
           utcTime.tm_year + 1900, utcTime.tm_mon + 1, utcTime.tm_mday,
           utcTime.tm_hour, utcTime.tm_min, utcTime.tm_sec, ms);
  return String(buffer);
}

String getUtcDateCompact() {
  if (!systemTimeValid()) return "UNSYNCED";
  time_t now = time(nullptr);
  struct tm utcTime;
  gmtime_r(&now, &utcTime);
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%04d%02d%02d",
           utcTime.tm_year + 1900, utcTime.tm_mon + 1, utcTime.tm_mday);
  return String(buffer);
}

String getDisplayDateTimeString() {
  if (!systemTimeValid()) return "----/--/-- --:--:--";
  time_t displayTime = time(nullptr) + ((time_t)utcOffsetMinutes * 60);
  struct tm t;
  gmtime_r(&displayTime, &t);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%04d/%02d/%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buffer);
}

String getDisplayTimeString() {
  if (!systemTimeValid()) return "--:--:--";
  time_t displayTime = time(nullptr) + ((time_t)utcOffsetMinutes * 60);
  struct tm t;
  gmtime_r(&displayTime, &t);
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buffer);
}

// ============================================================
// Macro enable config
// Missing entry = enabled
// ============================================================
void loadMacroEnableConfig() {
  for (int i = 0; i < macroCount; ++i) macroEnabled[i] = true;
  if (!initSD()) return;
  File file = SD.open(MACRO_CONFIG_FILE, FILE_READ);
  if (!file) return;

  bool first = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (first) { removeUtf8Bom(line); line.trim(); first = false; }
    if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;
    int eq = line.indexOf('=');
    if (eq < 1) continue;
    String name = line.substring(0, eq);
    String value = line.substring(eq + 1);
    name.trim(); value.trim();
    bool enabled;
    if (!parseBoolValue(value, enabled)) continue;
    for (int i = 0; i < macroCount; ++i) {
      if (getBaseName(macroFiles[i]) == name) {
        macroEnabled[i] = enabled;
        break;
      }
    }
  }
  file.close();
}

bool saveMacroEnableConfig() {
  if (!ensureConfigDirectory()) return false;
  const char* tempPath = "/config/macros.tmp";
  if (SD.exists(tempPath)) SD.remove(tempPath);
  File file = SD.open(tempPath, FILE_WRITE);
  if (!file) return false;
  file.println("# M5Core488 macro enable state");
  file.println("# 1=enabled, 0=disabled");
  for (int i = 0; i < macroCount; ++i) {
    file.print(getBaseName(macroFiles[i]));
    file.print('=');
    file.println(macroEnabled[i] ? "1" : "0");
  }
  file.flush();
  file.close();
  if (SD.exists(MACRO_CONFIG_FILE)) SD.remove(MACRO_CONFIG_FILE);
  return SD.rename(tempPath, MACRO_CONFIG_FILE);
}

int findMacroIndexByName(const String& name) {
  for (int i = 0; i < macroCount; ++i) {
    if (getBaseName(macroFiles[i]) == name) return i;
  }
  return -1;
}

// ============================================================
// Scan /macros/*.mac
// ============================================================
bool scanMacros() {
  macroCount = 0;
  selectedMacro = -1;
  pageStart = 0;
  if (!initSD()) return false;
  File dir = SD.open(MACRO_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  File entry;
  while ((entry = dir.openNextFile())) {
    if (!entry.isDirectory() && macroCount < MAX_MACROS) {
      String name = getBaseName(entry.name());
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".mac")) {
        macroFiles[macroCount++] = String(MACRO_DIR) + "/" + name;
      }
    }
    entry.close();
  }
  dir.close();

  for (int i = 0; i < macroCount - 1; ++i) {
    for (int j = i + 1; j < macroCount; ++j) {
      String a = getMacroTitle(macroFiles[i]);
      String b = getMacroTitle(macroFiles[j]);
      a.toLowerCase(); b.toLowerCase();
      if (a.compareTo(b) > 0) {
        String t = macroFiles[i]; macroFiles[i] = macroFiles[j]; macroFiles[j] = t;
      }
    }
  }

  for (int i = 0; i < macroCount; ++i) {
    MacroLayout layout;
    macroFormatValid[i] = inspectMacroLayout(macroFiles[i], layout);
    macroHasLoopSection[i] = macroFormatValid[i] && layout.hasLoop;
  }

  loadMacroEnableConfig();
  if (macroCount > 0) selectedMacro = 0;
  return true;
}

// ============================================================
// CSV Logger
// ============================================================
String csvQuote(String value) {
  value.replace("\r", "");
  value.replace("\n", "\\n");
  value.replace("\"", "\"\"");
  return "\"" + value + "\"";
}

void closeLogFile() {
  if (logFile) {
    logFile.flush();
    logFile.close();
  }
  currentLogDate = "";
  currentLogPath = "";
}

bool openLogForCurrentUtcDay() {
  if (!ensureLogDirectory()) { loggerFault = true; return false; }
  String utcDate = getUtcDateCompact();
  if (logFile && currentLogDate == utcDate) return true;

  closeLogFile();
  currentLogDate = utcDate;
  currentLogPath = String(LOG_DIR) + "/" + utcDate + ".csv";
  bool newFile = !SD.exists(currentLogPath.c_str());
  logFile = SD.open(currentLogPath.c_str(), FILE_APPEND);
  if (!logFile) { loggerFault = true; currentLogPath = ""; return false; }

  if (newFile || logFile.size() == 0) {
    size_t written = logFile.println(
      "timestamp_utc,clock_source,macro,line,event,command,response");
    logFile.flush();
    if (written == 0) { loggerFault = true; return false; }
  }
  return true;
}

bool writeLogRecord(int lineNumber, const String& event,
                    const String& command, const String& response) {
  if (!openLogForCurrentUtcDay()) return false;
  String row;
  row.reserve(256 + command.length() + response.length());
  row += csvQuote(getUtcTimestampMs()); row += ',';
  row += csvQuote(getTimeSourceName()); row += ',';
  row += csvQuote(currentMacroName); row += ',';
  row += String(lineNumber); row += ',';
  row += csvQuote(event); row += ',';
  row += csvQuote(command); row += ',';
  row += csvQuote(response);
  size_t written = logFile.println(row);
  logFile.flush();
  if (written == 0) { loggerFault = true; return false; }
  return true;
}

bool beginMacroLog(const String& macroName) {
  currentMacroName = macroName;
  loggerFault = false;
  if (!openLogForCurrentUtcDay()) return false;
  return writeLogRecord(0, "START", "", "");
}

void finishMacroLog(bool success, const String& result) {
  if (logFile) writeLogRecord(0, success ? "END" : "ABORT", "", result);
  closeLogFile();
  currentMacroName = "";
}

// ============================================================
// Wi-Fi config
// ============================================================
bool loadWifiConfig() {
  if (!initSD()) return false;
  File file = SD.open(WIFI_CONFIG_FILE, FILE_READ);
  if (!file) return false;
  int lineNumber = 0;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    ++lineNumber;
    line.trim();
    if (lineNumber == 1) { removeUtf8Bom(line); line.trim(); }
    if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;
    int separator = line.indexOf('=');
    if (separator < 0) continue;
    String key = line.substring(0, separator);
    String value = line.substring(separator + 1);
    key.trim(); value.trim(); key.toLowerCase();

    if (key == "ssid") wifiConfig.ssid = value;
    else if (key == "password") wifiConfig.password = value;
    else if (key == "timezone") wifiConfig.timezone = value;
    else if (key == "ntp1") wifiConfig.ntp1 = value;
    else if (key == "ntp2") wifiConfig.ntp2 = value;
    else if (key == "ntp3") wifiConfig.ntp3 = value;
    else if (key == "ntp_resync_hours") {
      uint32_t hours = 0;
      if (parseUnsignedInteger(value, hours) && hours <= MAX_NTP_RESYNC_HOURS) {
        wifiConfig.ntpResyncHours = hours;
      } else {
        wifiConfig.ntpResyncHours = 24;
        resyncConfigValid = false;
      }
    }
    else if (key == "web_enable") {
      bool b;
      if (parseBoolValue(value, b)) wifiConfig.webEnable = b;
    }
    else if (key == "web_port") {
      uint32_t port = 0;
      if (parseUnsignedInteger(value, port) && port >= 1 && port <= 65535) {
        wifiConfig.webPort = (uint16_t)port;
      }
    }
    else if (key == "hostname") {
      if (value.length() > 0 && value.length() <= 32) wifiConfig.hostname = value;
    }
    else if (key == "auto_ar488_seconds") {
      uint32_t seconds = 0;
      if (parseUnsignedInteger(value, seconds) && seconds <= MAX_AUTO_AR488_SECONDS) {
        wifiConfig.autoAr488Seconds = (uint8_t)seconds;
      } else {
        wifiConfig.autoAr488Seconds = 5;
        autoAr488ConfigValid = false;
      }
    }
  }
  file.close();

  int parsedOffset = 0;
  timezoneValid = parseUtcOffset(wifiConfig.timezone, parsedOffset);
  if (timezoneValid) {
    utcOffsetMinutes = parsedOffset;
  } else {
    wifiConfig.timezone = "UTC+00:00";
    utcOffsetMinutes = 0;
  }
  return true;
}

void initializeRtcState() {
  rtcAvailable = M5.Rtc.isEnabled();
  if (!rtcAvailable) {
    rtcTimeValid = false;
    timeSource = TIME_UNSYNCED;
    return;
  }
  auto dt = M5.Rtc.getDateTime();
  rtcTimeValid = dt.date.year >= 2024 && dt.date.year <= 2099 &&
                 dt.date.month >= 1 && dt.date.month <= 12 &&
                 dt.date.date >= 1 && dt.date.date <= 31;
  timeSource = rtcTimeValid ? TIME_RTC : TIME_UNSYNCED;
}

void stopWiFi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
}

bool ensureWiFiConnected(bool showStatus) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wifiConfig.ssid.length() == 0) return false;

  if (showStatus) M5.Display.print("WiFi : ");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiConfig.ssid.c_str(), wifiConfig.password.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    M5.update();
    delay(200);
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (showStatus) M5.Display.println(ok ? "CONNECTED" : "FAILED");
  // In Web mode keep STA alive so a router that boots later can be rejoined.
  if (!ok && !wifiConfig.webEnable) stopWiFi();
  return ok;
}

bool syncTimeFromNtp(bool showStatus) {
  ntpSyncOK = false;
  lastNtpAttemptMillis = millis();
  if (!ensureWiFiConnected(showStatus)) return false;

  if (showStatus) M5.Display.print("NTP  : ");
  configTime(0, 0, wifiConfig.ntp1.c_str(), wifiConfig.ntp2.c_str(), wifiConfig.ntp3.c_str());

  bool syncOK = false;
  uint32_t ntpStart = millis();
#if HAVE_SNTP_STATUS
  while (millis() - ntpStart < NTP_SYNC_TIMEOUT_MS) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      syncOK = true;
      break;
    }
    M5.update();
    delay(100);
  }
#else
  delay(2000);
  syncOK = systemTimeValid();
#endif

  if (!syncOK) {
    if (showStatus) M5.Display.println("FAILED");
    if (!wifiConfig.webEnable) stopWiFi();
    return false;
  }

  if (showStatus) M5.Display.println("SYNC OK");
  if (rtcAvailable) {
    time_t t = time(nullptr) + 1;
    while (t > time(nullptr)) delay(1);
    struct tm utcTime;
    gmtime_r(&t, &utcTime);
    M5.Rtc.setDateTime(&utcTime);
    rtcTimeValid = true;
    if (showStatus) M5.Display.println("RTC  : UPDATED");
  }

  timeSource = TIME_NTP;
  ntpSyncOK = true;
  if (!wifiConfig.webEnable) stopWiFi();
  return true;
}

void initializeTime() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.println("TIME INITIALIZATION");
  M5.Display.println("-------------------");

  initializeRtcState();
  M5.Display.print("RTC  : ");
  if (rtcAvailable && rtcTimeValid) M5.Display.println("OK");
  else if (rtcAvailable) M5.Display.println("INVALID");
  else M5.Display.println("NOT FOUND");

  M5.Display.print("CFG  : ");
  wifiConfigLoaded = loadWifiConfig();
  if (wifiConfigLoaded) M5.Display.println("OK");
  else {
    M5.Display.println("NOT FOUND");
    wifiConfig.timezone = "UTC+00:00";
    utcOffsetMinutes = 0;
  }

  M5.Display.print("TZ   : ");
  M5.Display.println(timezoneValid ? wifiConfig.timezone : "INVALID -> UTC");

  M5.Display.print("SYNC : ");
  if (!resyncConfigValid) M5.Display.println("INVALID -> 24h");
  else if (wifiConfig.ntpResyncHours == 0) M5.Display.println("BOOT ONLY");
  else { M5.Display.print(wifiConfig.ntpResyncHours); M5.Display.println(" h"); }

  M5.Display.print("WEB  : ");
  M5.Display.println(wifiConfig.webEnable ? "ENABLE" : "OFF");

  M5.Display.print("AUTO : ");
  if (!autoAr488ConfigValid) M5.Display.println("INVALID -> 5s");
  else if (wifiConfig.autoAr488Seconds == 0) M5.Display.println("OFF");
  else { M5.Display.print(wifiConfig.autoAr488Seconds); M5.Display.println(" s -> AR488"); }

  if (wifiConfigLoaded && wifiConfig.ssid.length() > 0) syncTimeFromNtp(true);
  else {
    M5.Display.println("NTP  : SKIPPED");
    lastNtpAttemptMillis = millis();
  }

  M5.Display.println();
  M5.Display.print("TIME : "); M5.Display.println(getDisplayDateTimeString());
  M5.Display.print("SRC  : "); M5.Display.println(getTimeSourceName());
  M5.Display.print("UTC  : "); M5.Display.println(getUtcTimestampMs());
  delay(1500);
}

// ============================================================
// Display
// ============================================================
void drawMainTime() {
  M5.Display.fillRect(0, 36, 320, 18, TFT_BLACK);
  M5.Display.setCursor(10, 39);
  M5.Display.setTextSize(1);
  M5.Display.print(getDisplayDateTimeString());
  M5.Display.print(' ');
  M5.Display.print(getTimeSourceName());
}

int getAutoAr488RemainingSeconds() {
  if (!autoAr488Armed) return 0;
  int32_t remainingMs = (int32_t)(autoAr488DeadlineMillis - millis());
  if (remainingMs <= 0) return 0;
  return (remainingMs + 999) / 1000;
}

void drawAutoAr488Status() {
  M5.Display.fillRect(0, 56, 320, 13, TFT_BLACK);
  M5.Display.setCursor(10, 57);
  M5.Display.setTextSize(1);

  if (autoAr488Armed) {
    M5.Display.print("AUTO AR488 IN ");
    M5.Display.print(getAutoAr488RemainingSeconds());
    M5.Display.print("s  (tap mode to override)");
  } else if (wifiConfig.autoAr488Seconds == 0) {
    M5.Display.print("AUTO AR488: OFF");
  } else if (currentMode == MODE_SELECT) {
    M5.Display.print("AUTO AR488: CANCELLED");
  }
}

void armAutoAr488() {
  autoAr488Armed = false;
  lastAutoAr488SecondsShown = -1;
  if (wifiConfig.autoAr488Seconds == 0) return;
  autoAr488DeadlineMillis = millis() + (uint32_t)wifiConfig.autoAr488Seconds * 1000UL;
  autoAr488Armed = true;
}

void cancelAutoAr488() {
  autoAr488Armed = false;
  lastAutoAr488SecondsShown = -1;
}

void drawModeMenu() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println(APP_NAME);
  M5.Display.setTextSize(1);
  drawMainTime();
  drawAutoAr488Status();
  drawButton(btnAR488, "AR488 MODE");
  drawButton(btnUSBSD, "USB SD MODE");

  M5.Display.setCursor(10, 214);
  M5.Display.print(wifiConfig.timezone);
  M5.Display.print(" RTC:UTC");
  M5.Display.setCursor(10, 226);
  if (webServerRunning && WiFi.status() == WL_CONNECTED) {
    M5.Display.print("WEB ");
    M5.Display.print(WiFi.localIP());
    if (wifiConfig.webPort != 80) {
      M5.Display.print(':');
      M5.Display.print(wifiConfig.webPort);
    }
  } else if (wifiConfig.webEnable) {
    M5.Display.print("WEB WAIT");
  } else {
    M5.Display.print("WEB OFF");
  }
}

void drawAR488Status() {
  M5.Display.fillRect(0, 24, 320, 20, TFT_BLACK);
  M5.Display.setCursor(5, 28);
  M5.Display.setTextSize(1);
  M5.Display.print(AR488.connected() ? "AR488:ON" : "AR488:WAIT");
  M5.Display.print(" MAC:"); M5.Display.print(macroCount);
  M5.Display.print(' '); M5.Display.print(getDisplayTimeString());
  M5.Display.print(' '); M5.Display.print(getTimeSourceName());
}

void drawMacroSelector() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 5);
  M5.Display.println("AR488 MACRO SELECT");
  M5.Display.drawFastHLine(0, 20, 320, TFT_WHITE);
  drawAR488Status();

  if (macroCount == 0) {
    M5.Display.setCursor(20, 90);
    M5.Display.println("No .mac files found.");
    M5.Display.setCursor(20, 110);
    M5.Display.println("Put files in /macros/");
  }

  const int listY = 48;
  const int rowHeight = 29;
  for (int row = 0; row < MACROS_PER_PAGE; ++row) {
    int index = pageStart + row;
    if (index >= macroCount) break;
    int y = listY + row * rowHeight;
    bool selected = index == selectedMacro;
    if (selected) M5.Display.fillRect(4, y, 312, rowHeight - 2, TFT_DARKGREY);
    bool usable = macroEnabled[index] && macroFormatValid[index];
    M5.Display.drawRect(4, y, 312, rowHeight - 2,
                        usable ? TFT_WHITE : TFT_DARKGREY);
    M5.Display.setTextColor(usable ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(10, y + 8);
    if (!macroFormatValid[index]) M5.Display.print("ERR   ");
    else if (!macroEnabled[index]) M5.Display.print("OFF   ");
    else if (macroHasLoopSection[index]) M5.Display.print("ON [L] ");
    else M5.Display.print("ON    ");
    String title = getMacroTitle(macroFiles[index]);
    if (title.length() > 30) title = title.substring(0, 27) + "...";
    M5.Display.print(title);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  bool prevEnabled = pageStart > 0;
  bool nextEnabled = pageStart + MACROS_PER_PAGE < macroCount;
  bool baseRunEnabled = selectedMacro >= 0 && selectedMacro < macroCount &&
                        AR488.connected() && macroEnabled[selectedMacro] &&
                        macroFormatValid[selectedMacro] && !macroOperationLocked();
  bool loopEnabled = baseRunEnabled && macroHasLoopSection[selectedMacro];
  drawButton(btnPrev, "<", prevEnabled);
  drawButton(btnNext, ">", nextEnabled);
  drawButton(btnRun, "RUN", baseRunEnabled);
  drawButton(btnLoop, "LOOP", loopEnabled);
}

// ============================================================
// Web server helpers
// ============================================================
void sendBusy(const char* what) {
  if (!webServer) return;
  String msg = String(what) + " is unavailable while a macro or loop operation is active.";
  webServer->send(409, "text/plain; charset=utf-8", msg);
}

void sendRedirectHome() {
  if (!webServer) return;
  webServer->sendHeader("Location", "/", true);
  webServer->send(303, "text/plain", "");
}

String webBaseUrlText() {
  if (WiFi.status() != WL_CONNECTED) return "offline";
  String s = "http://" + WiFi.localIP().toString();
  if (wifiConfig.webPort != 80) s += ":" + String(wifiConfig.webPort);
  return s + "/";
}

String webCss() {
  return
    "body{font-family:system-ui,-apple-system,sans-serif;margin:20px;max-width:900px;color:#222;}"
    "h1{font-size:1.5rem}h2{margin-top:1.8rem}table{border-collapse:collapse;width:100%;}"
    "th,td{border-bottom:1px solid #ddd;padding:8px;text-align:left;vertical-align:middle;}"
    "code{background:#f3f3f3;padding:2px 4px;border-radius:4px;}"
    ".ok{color:#087f23}.bad{color:#b00020}.muted{color:#777}.busy{color:#b26a00;font-weight:600;}"
    "button,.btn{display:inline-block;padding:6px 10px;margin:2px;border:1px solid #888;border-radius:6px;"
    "background:#fafafa;color:#222;text-decoration:none;font-size:.9rem;}"
    "button:disabled,input:disabled{opacity:.45}.disabled-link{pointer-events:none;opacity:.45;}"
    ".rowform{display:inline}.upload{padding:12px;border:1px solid #ddd;border-radius:8px;}"
    ".live{font-size:.8rem;color:#777;margin-left:.5rem;}"
    ".loopbox{padding:12px;border:1px solid #ddd;border-radius:8px;}";
}

String buildLogTableHtml() {
  if (macroOperationLocked()) return "<p class='busy'>Macro running. Log download is locked until completion.</p>";
  if (!initSD() || !SD.exists(LOG_DIR)) return "<p class='muted'>No logs.</p>";

  String names[MAX_WEB_LOGS];
  int count = 0;
  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "<p class='muted'>No logs.</p>";
  }

  File entry;
  while ((entry = dir.openNextFile())) {
    if (!entry.isDirectory() && count < MAX_WEB_LOGS) {
      String name = getBaseName(entry.name());
      if (isSafeLogName(name)) names[count++] = name;
    }
    entry.close();
  }
  dir.close();

  for (int i = 0; i < count - 1; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (names[i].compareTo(names[j]) < 0) {
        String t = names[i]; names[i] = names[j]; names[j] = t;
      }
    }
  }

  if (count == 0) return "<p class='muted'>No logs.</p>";
  String html = "<table><tr><th>File</th><th>Action</th></tr>";
  for (int i = 0; i < count; ++i) {
    html += "<tr><td><code>" + htmlEscape(names[i]) + "</code></td><td>";
    html += "<a class='btn' data-lockable='1' href='/log/download?name=" + urlEncode(names[i]) + "'>Download</a>";
    html += "</td></tr>";
  }
  html += "</table>";
  return html;
}

String currentModeText();
String autoAr488WebText();
String getLoopStatusText();

String buildHomePage() {
  String html;
  html.reserve(18000);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" APP_NAME "</title><style>" + webCss() + "</style></head><body>";
  html += "<h1>" APP_NAME " <span class='muted'>" APP_VERSION "</span>";
  html += "<span class='live' id='live-indicator'>LIVE</span></h1>";

  html += "<h2>Status</h2><table>";
  html += "<tr><th>AR488</th><td id='status-ar488' class='" + String(AR488.connected() ? "ok'>CONNECTED" : "bad'>DISCONNECTED") + "</td></tr>";
  html += "<tr><th>Mode</th><td id='status-mode'>" + currentModeText() + "</td></tr>";
  html += "<tr><th>Auto AR488</th><td id='status-auto'>" + autoAr488WebText() + "</td></tr>";

  String macroText;
  if (loopActive) macroText = "LOOPING " + loopMacroName;
  else if (macroBusy) macroText = "RUNNING " + currentMacroName;
  else if (webRunRequested) macroText = "RUN QUEUED";
  else if (webLoopRequested) macroText = "LOOP QUEUED";
  else macroText = "IDLE";
  html += "<tr><th>Macro</th><td id='status-macro' class='" + String(macroOperationLocked() ? "busy'>" : "'>") + htmlEscape(macroText) + "</td></tr>";

  String loopText;
  if (loopActive) {
    loopText = loopStopRequested ? "STOP REQUESTED" : "RUNNING";
    loopText += "  ";
    loopText += loopMacroName;
    loopText += "  cycle ";
    loopText += String(loopCycle);
  } else if (webLoopRequested) {
    loopText = "QUEUED";
  } else {
    loopText = "IDLE";
  }
  html += "<tr><th>Loop</th><td id='status-loop'>" + htmlEscape(loopText) + "</td></tr>";
  html += "<tr><th>Time</th><td id='status-time'>" + htmlEscape(getDisplayDateTimeString()) + "</td></tr>";
  html += "<tr><th>Clock source</th><td id='status-clock'>" + String(getTimeSourceName()) + "</td></tr>";
  html += "<tr><th>UTC</th><td><code id='status-utc'>" + htmlEscape(getUtcTimestampMs()) + "</code></td></tr>";
  html += "<tr><th>SD</th><td id='status-sd' class='" + String(sdReady ? "ok'>OK" : "bad'>ERROR") + "</td></tr>";
  if (WiFi.status() == WL_CONNECTED) {
    html += "<tr><th>Wi-Fi</th><td id='status-wifi' class='ok'>CONNECTED &nbsp; " + WiFi.localIP().toString();
    html += " &nbsp; " + String(WiFi.RSSI()) + " dBm</td></tr>";
  } else {
    html += "<tr><th>Wi-Fi</th><td id='status-wifi' class='bad'>DISCONNECTED</td></tr>";
  }
  html += "</table>";

  html += "<h2>Loop control</h2><div class='loopbox'>";
  html += "<div id='loop-summary'>" + htmlEscape(loopText) + "</div>";
  html += "<form class='rowform' method='post' action='/loop/stop'>";
  html += "<button id='loop-stop-button' type='submit'";
  if (!(loopActive || webLoopRequested)) html += " disabled";
  html += ">STOP AFTER CYCLE</button></form>";
  html += "<p class='muted'>STOP does not interrupt the current cycle. It prevents the next cycle from starting.</p></div>";

  bool locked = macroOperationLocked();
  html += "<h2>Macros</h2>";
  if (macroCount == 0) {
    html += "<p class='muted'>No .mac files found.</p>";
  } else {
    html += "<table><tr><th>Macro</th><th>State</th><th>Format</th><th>Actions</th></tr>";
    for (int i = 0; i < macroCount; ++i) {
      String name = getBaseName(macroFiles[i]);
      html += "<tr><td><code>" + htmlEscape(name) + "</code></td>";
      html += "<td class='" + String(macroEnabled[i] ? "ok'>ON" : "muted'>OFF") + "</td>";
      if (!macroFormatValid[i]) html += "<td class='bad'>ERROR</td><td>";
      else if (macroHasLoopSection[i]) html += "<td class='ok'>SETUP / LOOP</td><td>";
      else html += "<td class='muted'>LEGACY / RUN</td><td>";

      if (!locked) {
        html += "<a class='btn' data-lockable='1' href='/macro/download?name=" + urlEncode(name) + "'>Download</a>";
        html += "<form class='rowform' method='post' action='/macro/toggle'>";
        html += "<input type='hidden' name='name' value='" + htmlEscape(name) + "'>";
        html += "<button data-lockable='1' type='submit'>" + String(macroEnabled[i] ? "OFF" : "ON") + "</button></form>";

        bool canRun = currentMode == MODE_AR488 && AR488.connected() && macroEnabled[i] && macroFormatValid[i];
        html += "<form class='rowform' method='post' action='/macro/run'>";
        html += "<input type='hidden' name='name' value='" + htmlEscape(name) + "'>";
        html += "<button data-run='1' data-enabled='" + String((macroEnabled[i] && macroFormatValid[i]) ? "1" : "0") + "' type='submit'";
        if (!canRun) html += " disabled";
        html += ">RUN</button></form>";

        bool canLoop = canRun && macroHasLoopSection[i];
        html += "<form class='rowform' method='post' action='/macro/loop'>";
        html += "<input type='hidden' name='name' value='" + htmlEscape(name) + "'>";
        html += "<button data-loop='1' data-enabled='" + String((macroEnabled[i] && macroFormatValid[i]) ? "1" : "0") + "' data-hasloop='" + String(macroHasLoopSection[i] ? "1" : "0") + "' type='submit'";
        if (!canLoop) html += " disabled";
        html += ">LOOP</button></form>";
      } else {
        html += "<span class='muted'>Locked while running</span>";
      }
      html += "</td></tr>";
    }
    html += "</table>";
  }

  html += "<p class='muted'><b>Sectioned macro:</b> [setup] runs once. [loop] runs once with RUN, or repeatedly with LOOP. Legacy macros without sections remain RUN-only.</p>";

  html += "<h3>Upload macro</h3>";
  if (locked) {
    html += "<p class='busy'>Upload is locked while a macro or loop is active.</p>";
  } else {
    html += "<form class='upload' method='post' action='/macro/upload' enctype='multipart/form-data'>";
    html += "<input id='upload-file' data-lockable='1' type='file' name='file' accept='.mac' required> ";
    html += "<button id='upload-button' data-lockable='1' type='submit'>Upload</button>";
    html += "<p class='muted'>Existing .mac is replaced safely. One .bak backup is kept. Section syntax is validated before installation.</p></form>";
  }

  html += "<h2>Logs</h2>";
  html += buildLogTableHtml();
  html += "<p class='muted'>Web: " + htmlEscape(webBaseUrlText()) + "</p>";

  html += "<script>";
  html += "let completionCounter=" + String(macroCompletionCounter) + ";";
  html += "function text(id,v){const e=document.getElementById(id);if(e)e.textContent=v;}";
  html += "function state(id,ok,good,bad){const e=document.getElementById(id);if(!e)return;e.textContent=ok?good:bad;e.className=ok?'ok':'bad';}";
  html += "function lockUi(s){const locked=s.macro_locked;document.querySelectorAll('[data-lockable]').forEach(e=>{";
  html += "if(e.tagName==='A'){e.classList.toggle('disabled-link',locked);e.setAttribute('aria-disabled',locked?'true':'false');}else e.disabled=locked;});";
  html += "document.querySelectorAll('[data-run]').forEach(b=>{const en=b.dataset.enabled==='1';b.disabled=locked||!en||!s.ar488_connected||s.mode!=='AR488';});";
  html += "document.querySelectorAll('[data-loop]').forEach(b=>{const en=b.dataset.enabled==='1';const hl=b.dataset.hasloop==='1';b.disabled=locked||!en||!hl||!s.ar488_connected||s.mode!=='AR488';});";
  html += "const sb=document.getElementById('loop-stop-button');if(sb)sb.disabled=!(s.loop_active||s.loop_queued);}";
  html += "async function poll(){try{const r=await fetch('/api/status?t='+Date.now(),{cache:'no-store'});if(!r.ok)throw new Error();const s=await r.json();";
  html += "state('status-ar488',s.ar488_connected,'CONNECTED','DISCONNECTED');text('status-mode',s.mode_text);text('status-auto',s.auto_ar488_text);";
  html += "const m=document.getElementById('status-macro');if(m){m.textContent=s.macro_text;m.className=s.macro_locked?'busy':'';}";
  html += "text('status-loop',s.loop_text);text('loop-summary',s.loop_text);text('status-time',s.time_local);text('status-clock',s.clock_source);text('status-utc',s.utc);state('status-sd',s.sd_ok,'OK','ERROR');";
  html += "const w=document.getElementById('status-wifi');if(w){w.textContent=s.wifi_text;w.className=s.wifi_connected?'ok':'bad';}";
  html += "lockUi(s);const li=document.getElementById('live-indicator');if(li){li.textContent='LIVE';li.className='live';}";
  html += "if(s.macro_completion_counter!==completionCounter){location.reload();return;}completionCounter=s.macro_completion_counter;";
  html += "}catch(e){const li=document.getElementById('live-indicator');if(li){li.textContent='OFFLINE';li.className='live bad';}}}";
  html += "setInterval(poll,1000);poll();</script>";
  html += "</body></html>";
  return html;
}

String currentModeApiName() {
  if (currentMode == MODE_AR488) return "AR488";
  if (currentMode == MODE_USB_SD) return "USB_SD";
  return "SELECT";
}

String currentModeText() {
  if (currentMode == MODE_AR488) return "AR488 MODE";
  if (currentMode == MODE_USB_SD) return "USB SD MODE";
  return "SELECT";
}

String autoAr488WebText() {
  if (autoAr488Armed) return String(getAutoAr488RemainingSeconds()) + " s";
  if (wifiConfig.autoAr488Seconds == 0) return "OFF";
  if (currentMode == MODE_AR488) return "ACTIVE";
  return "READY";
}

String getLoopStatusText() {
  if (loopActive) {
    String s = loopStopRequested ? "STOP REQUESTED  " : "RUNNING  ";
    s += loopMacroName;
    s += "  cycle ";
    s += String(loopCycle);
    return s;
  }
  if (webLoopRequested) return "QUEUED";
  return "IDLE";
}

void handleApiStatus() {
  if (!webServer) return;
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  bool locked = macroOperationLocked();

  String macroText;
  if (loopActive) macroText = "LOOPING " + loopMacroName;
  else if (macroBusy) macroText = "RUNNING " + currentMacroName;
  else if (webRunRequested) macroText = "RUN QUEUED";
  else if (webLoopRequested) macroText = "LOOP QUEUED";
  else macroText = "IDLE";

  String wifiText;
  if (wifiConnected) {
    wifiText = "CONNECTED  ";
    wifiText += WiFi.localIP().toString();
    wifiText += "  ";
    wifiText += String(WiFi.RSSI());
    wifiText += " dBm";
  } else {
    wifiText = "DISCONNECTED";
  }

  String json;
  json.reserve(1024);
  json += "{";
  json += "\"version\":\"" + jsonEscape(String(APP_VERSION)) + "\",";
  json += "\"ar488_connected\":" + String(AR488.connected() ? "true" : "false") + ",";
  json += "\"mode\":\"" + currentModeApiName() + "\",";
  json += "\"mode_text\":\"" + jsonEscape(currentModeText()) + "\",";
  json += "\"auto_ar488_armed\":" + String(autoAr488Armed ? "true" : "false") + ",";
  json += "\"auto_ar488_remaining\":" + String(getAutoAr488RemainingSeconds()) + ",";
  json += "\"auto_ar488_text\":\"" + jsonEscape(autoAr488WebText()) + "\",";
  json += "\"macro_busy\":" + String(macroBusy ? "true" : "false") + ",";
  json += "\"macro_queued\":" + String(webRunRequested ? "true" : "false") + ",";
  json += "\"macro_locked\":" + String(locked ? "true" : "false") + ",";
  json += "\"macro_text\":\"" + jsonEscape(macroText) + "\",";
  json += "\"loop_active\":" + String(loopActive ? "true" : "false") + ",";
  json += "\"loop_queued\":" + String(webLoopRequested ? "true" : "false") + ",";
  json += "\"loop_stop_requested\":" + String(loopStopRequested ? "true" : "false") + ",";
  json += "\"loop_cycle\":" + String(loopCycle) + ",";
  json += "\"loop_text\":\"" + jsonEscape(getLoopStatusText()) + "\",";
  json += "\"macro_completion_counter\":" + String(macroCompletionCounter) + ",";
  json += "\"time_local\":\"" + jsonEscape(getDisplayDateTimeString()) + "\",";
  json += "\"clock_source\":\"" + jsonEscape(getTimeSourceName()) + "\",";
  json += "\"utc\":\"" + jsonEscape(getUtcTimestampMs()) + "\",";
  json += "\"sd_ok\":" + String(sdReady ? "true" : "false") + ",";
  json += "\"wifi_connected\":" + String(wifiConnected ? "true" : "false") + ",";
  json += "\"wifi_text\":\"" + jsonEscape(wifiText) + "\"";
  json += "}";

  webServer->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  webServer->send(200, "application/json; charset=utf-8", json);
}

void handleHome() {
  webServer->send(200, "text/html; charset=utf-8", buildHomePage());
}

void handleMacroDownload() {
  if (macroOperationLocked()) { sendBusy("Macro download"); return; }
  String name = webServer->arg("name");
  if (!isSafeMacroName(name)) { webServer->send(400, "text/plain", "Invalid macro name"); return; }
  int index = findMacroIndexByName(name);
  if (index < 0) { webServer->send(404, "text/plain", "Macro not found"); return; }
  File file = SD.open(macroFiles[index].c_str(), FILE_READ);
  if (!file) { webServer->send(500, "text/plain", "Open failed"); return; }
  webServer->sendHeader("Content-Disposition",
                        "attachment; filename*=UTF-8''" + urlEncode(name));
  webServer->streamFile(file, "text/plain; charset=utf-8");
  file.close();
}

void handleLogDownload() {
  if (macroOperationLocked()) { sendBusy("Log download"); return; }
  String name = webServer->arg("name");
  if (!isSafeLogName(name)) { webServer->send(400, "text/plain", "Invalid log name"); return; }
  String path = String(LOG_DIR) + "/" + name;
  if (!SD.exists(path.c_str())) { webServer->send(404, "text/plain", "Log not found"); return; }
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) { webServer->send(500, "text/plain", "Open failed"); return; }
  webServer->sendHeader("Content-Disposition",
                        "attachment; filename*=UTF-8''" + urlEncode(name));
  webServer->streamFile(file, "text/csv; charset=utf-8");
  file.close();
}

void handleMacroToggle() {
  if (macroOperationLocked()) { sendBusy("Macro state change"); return; }
  String name = webServer->arg("name");
  if (!isSafeMacroName(name)) { webServer->send(400, "text/plain", "Invalid macro name"); return; }
  int index = findMacroIndexByName(name);
  if (index < 0) { webServer->send(404, "text/plain", "Macro not found"); return; }
  macroEnabled[index] = !macroEnabled[index];
  if (!saveMacroEnableConfig()) {
    macroEnabled[index] = !macroEnabled[index];
    webServer->send(500, "text/plain", "Could not save macros.ini");
    return;
  }
  if (currentMode == MODE_AR488 && !uiWaitingForReturn) drawMacroSelector();
  sendRedirectHome();
}

void handleMacroRun() {
  if (macroOperationLocked()) { sendBusy("Macro run"); return; }
  String name = webServer->arg("name");
  if (!isSafeMacroName(name)) { webServer->send(400, "text/plain", "Invalid macro name"); return; }
  int index = findMacroIndexByName(name);
  if (index < 0) { webServer->send(404, "text/plain", "Macro not found"); return; }
  if (!macroEnabled[index]) { webServer->send(409, "text/plain", "Macro is OFF"); return; }
  if (!macroFormatValid[index]) { webServer->send(409, "text/plain", "Macro section syntax is invalid"); return; }
  if (currentMode != MODE_AR488) {
    webServer->send(409, "text/plain", "Select AR488 MODE on the CoreS3 first");
    return;
  }
  if (!AR488.connected()) {
    webServer->send(503, "text/plain", "AR488 is not connected");
    return;
  }

  selectedMacro = index;
  pageStart = (index / MACROS_PER_PAGE) * MACROS_PER_PAGE;
  webRequestedMacroPath = macroFiles[index];
  webRunRequested = true;
  drawMacroSelector();
  sendRedirectHome();
}

void handleMacroLoop() {
  if (macroOperationLocked()) { sendBusy("Macro loop"); return; }
  String name = webServer->arg("name");
  if (!isSafeMacroName(name)) { webServer->send(400, "text/plain", "Invalid macro name"); return; }
  int index = findMacroIndexByName(name);
  if (index < 0) { webServer->send(404, "text/plain", "Macro not found"); return; }
  if (!macroEnabled[index]) { webServer->send(409, "text/plain", "Macro is OFF"); return; }
  if (!macroFormatValid[index]) { webServer->send(409, "text/plain", "Macro section syntax is invalid"); return; }
  if (!macroHasLoopSection[index]) { webServer->send(409, "text/plain", "[loop] section not found"); return; }
  if (currentMode != MODE_AR488) {
    webServer->send(409, "text/plain", "Select AR488 MODE on the CoreS3 first");
    return;
  }
  if (!AR488.connected()) {
    webServer->send(503, "text/plain", "AR488 is not connected");
    return;
  }

  selectedMacro = index;
  pageStart = (index / MACROS_PER_PAGE) * MACROS_PER_PAGE;
  webRequestedLoopPath = macroFiles[index];
  webLoopRequested = true;
  drawMacroSelector();
  sendRedirectHome();
}

void handleLoopStop() {
  if (webLoopRequested && !loopActive) {
    webLoopRequested = false;
    webRequestedLoopPath = "";
    sendRedirectHome();
    return;
  }
  if (!loopActive) {
    webServer->send(409, "text/plain; charset=utf-8", "No loop is active");
    return;
  }
  loopStopRequested = true;
  sendRedirectHome();
}

void resetUploadState() {
  if (uploadTempFile) uploadTempFile.close();
  if (SD.exists(UPLOAD_TEMP_FILE)) SD.remove(UPLOAD_TEMP_FILE);
  uploadOK = false;
  uploadError = "";
  uploadTargetName = "";
  uploadReceivedBytes = 0;
}

void handleMacroUploadData() {
  HTTPUpload& upload = webServer->upload();

  if (upload.status == UPLOAD_FILE_START) {
    resetUploadState();
    if (macroOperationLocked()) { uploadError = "Macro is running or queued"; return; }
    if (!ensureDirectory(MACRO_DIR)) { uploadError = "Macro directory unavailable"; return; }

    uploadTargetName = getBaseName(upload.filename);
    if (!isSafeMacroName(uploadTargetName)) { uploadError = "Only safe .mac filenames are allowed"; return; }
    uploadTempFile = SD.open(UPLOAD_TEMP_FILE, FILE_WRITE);
    if (!uploadTempFile) { uploadError = "Could not create temporary file"; return; }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadError.length() > 0 || !uploadTempFile) return;
    uploadReceivedBytes += upload.currentSize;
    if (uploadReceivedBytes > MAX_MACRO_BYTES) {
      uploadError = "Macro file is too large";
      uploadTempFile.close();
      SD.remove(UPLOAD_TEMP_FILE);
      return;
    }
    if (uploadTempFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
      uploadError = "SD write failed";
      uploadTempFile.close();
      SD.remove(UPLOAD_TEMP_FILE);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadTempFile) {
      uploadTempFile.flush();
      uploadTempFile.close();
    }
    if (uploadError.length() > 0) {
      SD.remove(UPLOAD_TEMP_FILE);
      return;
    }
    if (uploadReceivedBytes == 0) {
      uploadError = "Empty macro file";
      SD.remove(UPLOAD_TEMP_FILE);
      return;
    }

    MacroLayout uploadedLayout;
    if (!inspectMacroLayout(UPLOAD_TEMP_FILE, uploadedLayout)) {
      uploadError = "Macro syntax error: " + uploadedLayout.error;
      SD.remove(UPLOAD_TEMP_FILE);
      return;
    }

    String finalPath = String(MACRO_DIR) + "/" + uploadTargetName;
    String backupPath = finalPath + ".bak";
    bool hadOld = SD.exists(finalPath.c_str());

    if (SD.exists(backupPath.c_str())) SD.remove(backupPath.c_str());
    if (hadOld && !SD.rename(finalPath.c_str(), backupPath.c_str())) {
      uploadError = "Could not create backup";
      SD.remove(UPLOAD_TEMP_FILE);
      return;
    }

    if (!SD.rename(UPLOAD_TEMP_FILE, finalPath.c_str())) {
      uploadError = "Could not install uploaded macro";
      if (hadOld && SD.exists(backupPath.c_str())) SD.rename(backupPath.c_str(), finalPath.c_str());
      SD.remove(UPLOAD_TEMP_FILE);
      return;
    }

    uploadOK = true;
    scanMacros();
    if (currentMode == MODE_AR488 && !uiWaitingForReturn) drawMacroSelector();
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadError = "Upload aborted";
    if (uploadTempFile) uploadTempFile.close();
    SD.remove(UPLOAD_TEMP_FILE);
  }
}

void handleMacroUploadComplete() {
  if (macroOperationLocked() && !uploadOK) {
    webServer->send(409, "text/plain; charset=utf-8", "Macro is running");
    return;
  }
  if (!uploadOK) {
    String msg = uploadError.length() ? uploadError : "Upload failed";
    webServer->send(400, "text/plain; charset=utf-8", msg);
    return;
  }
  sendRedirectHome();
}

void startWebServer() {
  if (webServerRunning || !wifiConfig.webEnable || WiFi.status() != WL_CONNECTED) return;
  webServer = new WebServer(wifiConfig.webPort);
  if (!webServer) return;

  webServer->on("/", HTTP_GET, handleHome);
  webServer->on("/api/status", HTTP_GET, handleApiStatus);
  webServer->on("/macro/download", HTTP_GET, handleMacroDownload);
  webServer->on("/log/download", HTTP_GET, handleLogDownload);
  webServer->on("/macro/toggle", HTTP_POST, handleMacroToggle);
  webServer->on("/macro/run", HTTP_POST, handleMacroRun);
  webServer->on("/macro/loop", HTTP_POST, handleMacroLoop);
  webServer->on("/loop/stop", HTTP_POST, handleLoopStop);
  webServer->on("/macro/upload", HTTP_POST, handleMacroUploadComplete, handleMacroUploadData);
  webServer->on("/favicon.ico", HTTP_GET, []() { webServer->send(204); });
  webServer->onNotFound([]() { webServer->send(404, "text/plain", "Not found"); });
  webServer->begin();
  webServerRunning = true;

  if (MDNS.begin(wifiConfig.hostname.c_str())) {
    MDNS.addService("http", "tcp", wifiConfig.webPort);
    mdnsRunning = true;
  }
}

void stopWebServer() {
  if (webServer) {
    webServer->stop();
    delete webServer;
    webServer = nullptr;
  }
  webServerRunning = false;
  if (mdnsRunning) {
    MDNS.end();
    mdnsRunning = false;
  }
}

void ensureWebServer() {
  if (!wifiConfig.webEnable || currentMode == MODE_USB_SD) return;
  if (WiFi.status() == WL_CONNECTED && !webServerRunning) startWebServer();
}

void handleWebServer() {
  if (webServerRunning && webServer) webServer->handleClient();
}

// Keep the Web interface recoverable after an AP/router restart.
// Reconnection is intentionally suspended while a macro is running so
// Wi-Fi recovery cannot disturb GPIB timing.
void serviceWiFiConnection() {
  if (!wifiConfig.webEnable || currentMode == MODE_USB_SD || wifiConfig.ssid.length() == 0) return;

  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    if (!lastWiFiConnectedState) {
      lastWiFiConnectedState = true;
      // If boot-time NTP failed because the router was still starting,
      // synchronize as soon as network service actually returns.
      if (!ntpSyncOK && !macroBusy) syncTimeFromNtp(false);
    }
    ensureWebServer();
    return;
  }

  if (lastWiFiConnectedState) {
    lastWiFiConnectedState = false;
    stopWebServer();
  }

  if (macroBusy) return;
  if (millis() - lastWiFiReconnectAttemptMillis < WIFI_RECONNECT_INTERVAL_MS) return;

  lastWiFiReconnectAttemptMillis = millis();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiConfig.ssid.c_str(), wifiConfig.password.c_str());
}

// ============================================================
// Periodic NTP + RTC correction
// ============================================================
void servicePeriodicNtp() {
  if (!wifiConfigLoaded || wifiConfig.ssid.length() == 0 ||
      wifiConfig.ntpResyncHours == 0 || currentMode == MODE_USB_SD || macroBusy || loopActive) return;

  uint32_t intervalMs = (uint32_t)wifiConfig.ntpResyncHours * 3600000UL;
  if (millis() - lastNtpAttemptMillis < intervalMs) return;

  M5.Display.fillRect(0, 0, 320, 20, TFT_BLACK);
  M5.Display.setCursor(5, 5);
  M5.Display.setTextSize(1);
  M5.Display.print("NTP RESYNC...");
  syncTimeFromNtp(false);
  ensureWebServer();

  if (currentMode == MODE_SELECT) drawModeMenu();
  else if (currentMode == MODE_AR488) drawMacroSelector();
}

// ============================================================
// Local loop STOP request
//
// During LOOP, a screen tap requests a graceful stop after the current
// [loop] cycle. It never aborts the current instrument transaction.
// ============================================================
void serviceLoopStopTouch() {
  if (!loopActive || loopStopRequested) return;
  if ((int32_t)(millis() - loopTouchEnableMillis) < 0) return;
  auto detail = M5.Touch.getDetail();
  if (detail.wasClicked()) {
    loopStopRequested = true;
  }
}

// ============================================================
// AR488 I/O
// ============================================================
String receiveAR488(uint32_t idleTimeoutMs, uint32_t totalTimeoutMs) {
  String response;
  uint32_t startTime = millis();
  uint32_t lastRxTime = millis();
  bool received = false;

  while (millis() - startTime < totalTimeoutMs) {
    M5.update();
    handleWebServer();
    serviceLoopStopTouch();
    while (AR488.available() > 0) {
      int c = AR488.read();
      if (c >= 0) {
        response += (char)c;
        received = true;
        lastRxTime = millis();
      }
    }
    if (received && millis() - lastRxTime >= idleTimeoutMs) break;
    delay(1);
  }
  return response;
}

String executeAR488Line(const String& command) {
  while (AR488.available() > 0) AR488.read();
  AR488.print(command);
  AR488.print("\r");

  uint32_t timeout = AR488_NORMAL_TIMEOUT_MS;
  if (command.startsWith("++read") || command.startsWith("++spoll")) timeout = AR488_READ_TIMEOUT_MS;
  else if (command.startsWith("++")) timeout = AR488_LOCAL_TIMEOUT_MS;

  return receiveAR488(AR488_IDLE_TIMEOUT_MS, timeout);
}

void showMacroResult(const String& command, String response) {
  M5.Display.print("> ");
  M5.Display.println(command);
  response.replace("\r", "");
  if (response.length() > 0) {
    M5.Display.print("< ");
    M5.Display.println(response);
  } else {
    M5.Display.println("< [no response]");
  }
  M5.Display.println();
}

bool executeLocalCommand(const String& line) {
  String lower = line;
  lower.toLowerCase();
  if (!lower.startsWith("@wait")) {
    M5.Display.println("*** UNKNOWN LOCAL COMMAND ***");
    M5.Display.println(line);
    return false;
  }

  String value = line.substring(5);
  value.trim();
  if (value.length() == 0) {
    M5.Display.println("*** @wait ERROR ***");
    M5.Display.println("Missing milliseconds");
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') {
      M5.Display.println("*** @wait ERROR ***");
      M5.Display.print("Invalid: "); M5.Display.println(value);
      return false;
    }
  }

  uint32_t waitMs = strtoul(value.c_str(), nullptr, 10);
  if (waitMs > MAX_WAIT_MS) {
    M5.Display.println("*** @wait ERROR ***");
    M5.Display.println("Maximum = 600000 ms");
    return false;
  }

  M5.Display.print("[WAIT] "); M5.Display.print(waitMs); M5.Display.println(" ms");
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    M5.update();
    handleWebServer();
    serviceLoopStopTouch();
    if (!AR488.connected()) {
      M5.Display.println("*** AR488 LOST ***");
      return false;
    }
    delay(5);
  }
  return true;
}

void waitForReturnTap() {
  M5.Display.println();
  M5.Display.println("Tap screen to return");
  uiWaitingForReturn = true;
  delay(250);
  while (true) {
    M5.update();
    handleWebServer();
    auto detail = M5.Touch.getDetail();
    if (detail.wasPressed() || webRunRequested || webLoopRequested) break;
    delay(10);
  }
  uiWaitingForReturn = false;
  drawMacroSelector();
}

void finishRunUi(bool waitForTap) {
  macroBusy = false;
  ++macroCompletionCounter;
  if (waitForTap) waitForReturnTap();
  else {
    uint32_t until = millis() + 700;
    while ((int32_t)(until - millis()) > 0) {
      M5.update();
      handleWebServer();
      delay(10);
    }
    drawMacroSelector();
  }
}

void drawMacroExecutionScreen(const String& filename, const char* modeText,
                              const char* phaseText, uint32_t cycle) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.print(APP_NAME);
  M5.Display.print(" ");
  M5.Display.println(modeText);
  M5.Display.println("----------------");
  M5.Display.print("File : "); M5.Display.println(getBaseName(filename));
  if (phaseText && phaseText[0]) {
    M5.Display.print("Phase: "); M5.Display.println(phaseText);
  }
  if (cycle > 0) {
    M5.Display.print("Cycle: "); M5.Display.println(cycle);
  }
  M5.Display.print("UTC  : "); M5.Display.println(getUtcTimestampMs());
  if (loopActive) {
    M5.Display.println("Tap screen = STOP after cycle");
    if (loopStopRequested) M5.Display.println("STOP REQUESTED");
  }
  M5.Display.println();
}

bool executeMacroRange(const String& filename,
                       uint32_t startPos, uint32_t endPos, int firstLine,
                       bool rejectSectionHeaders, String& failureReason) {
  File file = SD.open(filename.c_str(), FILE_READ);
  if (!file) {
    failureReason = "MACRO NOT FOUND";
    return false;
  }
  if (!file.seek(startPos)) {
    file.close();
    failureReason = "MACRO SEEK ERROR";
    return false;
  }

  int lineNumber = firstLine - 1;
  while (file.available() && (uint32_t)file.position() < endPos) {
    M5.update();
    handleWebServer();
    serviceLoopStopTouch();

    uint32_t lineStart = (uint32_t)file.position();
    if (lineStart >= endPos) break;

    String line = file.readStringUntil('\n');
    ++lineNumber;
    line.trim();
    if (lineNumber == 1) {
      removeUtf8Bom(line);
      line.trim();
    }
    if (line.length() == 0 || line.startsWith("#")) continue;

    if (!AR488.connected()) {
      M5.Display.println();
      M5.Display.println("*** AR488 LOST ***");
      writeLogRecord(lineNumber, "ERROR", line, "AR488 LOST");
      file.close();
      failureReason = "AR488 LOST";
      return false;
    }

    if (line.startsWith("@")) {
      M5.Display.print("> "); M5.Display.println(line);
      bool localOK = executeLocalCommand(line);
      if (localOK) {
        writeLogRecord(lineNumber, "LOCAL", line, "OK");
      } else {
        writeLogRecord(lineNumber, "ERROR", line, "LOCAL COMMAND FAILED");
        M5.Display.println();
        M5.Display.println("*** MACRO ABORTED ***");
        file.close();
        failureReason = "LOCAL COMMAND FAILED";
        return false;
      }
      M5.Display.println();
      continue;
    }

    // Section headers should never be inside a section execution range. If a
    // sectioned file changed after validation, fail safely instead of sending
    // a header to GPIB. Legacy macros retain their original raw behavior.
    if (rejectSectionHeaders && line.startsWith("[") && line.endsWith("]")) {
      writeLogRecord(lineNumber, "ERROR", line, "UNEXPECTED SECTION HEADER");
      file.close();
      failureReason = "MACRO CHANGED DURING RUN";
      return false;
    }

    String response = executeAR488Line(line);
    showMacroResult(line, response);
    writeLogRecord(lineNumber, "AR488", line, response);
    delay(50);
  }

  file.close();
  return true;
}

bool runMacro(const String& filename, bool waitForTap) {
  if (macroOperationLocked()) return false;
  macroBusy = true;

  MacroLayout layout;
  bool layoutOK = inspectMacroLayout(filename, layout);
  drawMacroExecutionScreen(filename, "RUN", "", 0);

  String macroName = getMacroTitle(filename);
  bool logStarted = beginMacroLog(macroName);
  M5.Display.print("LOG : ");
  if (logStarted) M5.Display.println(currentLogPath);
  else {
    M5.Display.println("ERROR");
    M5.Display.println("Macro will continue.");
  }
  M5.Display.println();

  if (!layoutOK) {
    M5.Display.println("MACRO FORMAT ERROR");
    M5.Display.println(layout.error);
    writeLogRecord(0, "ERROR", "", layout.error);
    finishMacroLog(false, "MACRO FORMAT ERROR");
    finishRunUi(waitForTap);
    return false;
  }

  String failureReason;
  bool ok = true;

  if (!layout.sectioned) {
    File sizeFile = SD.open(filename.c_str(), FILE_READ);
    if (!sizeFile) {
      failureReason = "MACRO NOT FOUND";
      ok = false;
    } else {
      uint32_t fileSize = (uint32_t)sizeFile.size();
      sizeFile.close();
      M5.Display.println("MACRO : LEGACY RUN");
      M5.Display.println("================");
      ok = executeMacroRange(filename, 0, fileSize, 1, false, failureReason);
    }
  } else {
    if (layout.hasSetup) {
      drawMacroExecutionScreen(filename, "RUN", "SETUP", 0);
      writeLogRecord(0, "SETUP_START", "[setup]", "");
      ok = executeMacroRange(filename, layout.setupStart, layout.setupEnd,
                             layout.setupFirstLine, true, failureReason);
      if (ok) writeLogRecord(0, "SETUP_END", "[setup]", "OK");
    }

    if (ok && layout.hasLoop) {
      drawMacroExecutionScreen(filename, "RUN", "LOOP", 1);
      writeLogRecord(0, "CYCLE_START", "1", "");
      ok = executeMacroRange(filename, layout.loopStart, layout.loopEnd,
                             layout.loopFirstLine, true, failureReason);
      if (ok) writeLogRecord(0, "CYCLE_END", "1", "OK");
    }
  }

  if (!ok) {
    M5.Display.println();
    M5.Display.println("*** MACRO ABORTED ***");
    if (loggerFault) M5.Display.println("LOG WRITE ERROR");
    finishMacroLog(false, failureReason.length() ? failureReason : "FAILED");
    finishRunUi(waitForTap);
    return false;
  }

  finishMacroLog(true, "OK");
  M5.Display.println("================");
  M5.Display.println("MACRO COMPLETE");
  M5.Display.println(loggerFault ? "LOG : WRITE ERROR" : "LOG : SAVED");
  finishRunUi(waitForTap);
  return true;
}

bool runMacroLoop(const String& filename, bool waitForTap) {
  if (macroOperationLocked()) return false;

  MacroLayout layout;
  if (!inspectMacroLayout(filename, layout) || !layout.valid || !layout.hasLoop) {
    macroBusy = true;
    drawMacroExecutionScreen(filename, "LOOP", "FORMAT ERROR", 0);
    String macroName = getMacroTitle(filename);
    beginMacroLog(macroName);
    String reason = layout.error.length() ? layout.error : "[loop] section not found";
    M5.Display.println(reason);
    writeLogRecord(0, "ERROR", "", reason);
    finishMacroLog(false, reason);
    finishRunUi(waitForTap);
    return false;
  }

  loopActive = true;
  loopStopRequested = false;
  loopMacroPath = filename;
  loopMacroName = getMacroTitle(filename);
  loopCycle = 0;
  loopTouchEnableMillis = millis() + 500;
  macroBusy = true;

  drawMacroExecutionScreen(filename, "LOOP", layout.hasSetup ? "SETUP" : "READY", 0);
  bool logStarted = beginMacroLog(loopMacroName);
  M5.Display.print("LOG : ");
  if (logStarted) M5.Display.println(currentLogPath);
  else {
    M5.Display.println("ERROR");
    M5.Display.println("Loop will continue.");
  }
  M5.Display.println();

  String failureReason;
  bool ok = true;

  if (layout.hasSetup) {
    writeLogRecord(0, "SETUP_START", "[setup]", "");
    ok = executeMacroRange(filename, layout.setupStart, layout.setupEnd,
                           layout.setupFirstLine, true, failureReason);
    if (ok) writeLogRecord(0, "SETUP_END", "[setup]", "OK");
  }

  macroBusy = false;

  // A STOP request during [setup] prevents cycle 1 from starting.
  while (ok && !loopStopRequested) {
    if (!AR488.connected()) {
      failureReason = "AR488 LOST";
      ok = false;
      break;
    }

    ++loopCycle;
    macroBusy = true;
    drawMacroExecutionScreen(filename, "LOOP", "LOOP", loopCycle);
    writeLogRecord(0, "CYCLE_START", String(loopCycle), "");

    ok = executeMacroRange(filename, layout.loopStart, layout.loopEnd,
                           layout.loopFirstLine, true, failureReason);

    if (ok) writeLogRecord(0, "CYCLE_END", String(loopCycle), "OK");
    macroBusy = false;

    if (!ok || loopStopRequested) break;

    // Between cycles we may safely recover Web/Wi-Fi without injecting work
    // into the middle of an instrument transaction.
    serviceWiFiConnection();
    ensureWebServer();
    handleWebServer();
    M5.update();
    serviceLoopStopTouch();
    delay(10);
  }

  macroBusy = true;
  if (ok) {
    writeLogRecord(0, "LOOP_STOP", String(loopCycle), "STOPPED");
    finishMacroLog(true, "STOPPED");
    M5.Display.println("================");
    M5.Display.println("LOOP STOPPED");
    M5.Display.print("Cycles: "); M5.Display.println(loopCycle);
    M5.Display.println(loggerFault ? "LOG : WRITE ERROR" : "LOG : SAVED");
  } else {
    writeLogRecord(0, "ERROR", "", failureReason);
    finishMacroLog(false, failureReason.length() ? failureReason : "FAILED");
    M5.Display.println();
    M5.Display.println("*** LOOP ABORTED ***");
    M5.Display.println(failureReason);
    if (loggerFault) M5.Display.println("LOG WRITE ERROR");
  }

  loopActive = false;
  loopStopRequested = false;
  loopMacroPath = "";
  loopMacroName = "";
  macroBusy = false;

  ++macroCompletionCounter;
  if (waitForTap) waitForReturnTap();
  else {
    uint32_t until = millis() + 700;
    while ((int32_t)(until - millis()) > 0) {
      M5.update();
      handleWebServer();
      delay(10);
    }
    drawMacroSelector();
  }
  return ok;
}

// ============================================================
// AR488 MODE
// ============================================================
void startAR488Mode() {
  cancelAutoAr488();
  currentMode = MODE_AR488;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(1);
  M5.Display.println("AR488 MODE");
  M5.Display.println("----------------");

  M5.Display.print("SD       : ");
  if (!initSD()) { M5.Display.println("ERROR"); return; }
  M5.Display.println("OK");

  M5.Display.print("LOGGER   : ");
  M5.Display.println(ensureLogDirectory() ? "OK" : "ERROR");

  M5.Display.print("MACROS   : ");
  if (!scanMacros()) { M5.Display.println("ERROR"); return; }
  M5.Display.println(macroCount);

  M5.Power.setUsbOutput(true);
  delay(200);
  AR488.begin(115200);
  if (!usbHost.begin()) {
    M5.Display.print("USB HOST : ERROR ");
    M5.Display.println(usbHost.lastErrorName());
    return;
  }
  M5.Display.println("USB HOST : OK");
  delay(500);
  lastAR488Connected = AR488.connected();
  drawMacroSelector();
}

void loopAR488() {
  static uint32_t lastTimeDraw = 0;
  bool connected = AR488.connected();

  if (connected != lastAR488Connected) {
    lastAR488Connected = connected;
    drawAR488Status();
    bool baseRunEnabled = selectedMacro >= 0 && selectedMacro < macroCount &&
                          connected && macroEnabled[selectedMacro] &&
                          macroFormatValid[selectedMacro] && !macroOperationLocked();
    drawButton(btnRun, "RUN", baseRunEnabled);
    drawButton(btnLoop, "LOOP", baseRunEnabled && macroHasLoopSection[selectedMacro]);
  }

  if (millis() - lastTimeDraw >= 1000) {
    lastTimeDraw = millis();
    drawAR488Status();
  }

  auto detail = M5.Touch.getDetail();
  if (!detail.wasClicked()) return;
  int x = detail.x;
  int y = detail.y;
  const int listY = 48;
  const int rowHeight = 29;

  if (y >= listY && y < listY + MACROS_PER_PAGE * rowHeight) {
    int row = (y - listY) / rowHeight;
    int index = pageStart + row;
    if (index < macroCount) {
      selectedMacro = index;
      drawMacroSelector();
      return;
    }
  }

  if (touched(btnPrev, x, y)) {
    if (pageStart > 0) {
      pageStart -= MACROS_PER_PAGE;
      if (pageStart < 0) pageStart = 0;
      selectedMacro = pageStart;
      drawMacroSelector();
    }
    return;
  }

  if (touched(btnNext, x, y)) {
    if (pageStart + MACROS_PER_PAGE < macroCount) {
      pageStart += MACROS_PER_PAGE;
      selectedMacro = pageStart;
      drawMacroSelector();
    }
    return;
  }

  if (touched(btnRun, x, y)) {
    if (selectedMacro >= 0 && selectedMacro < macroCount && AR488.connected() &&
        macroEnabled[selectedMacro] && macroFormatValid[selectedMacro] && !macroOperationLocked()) {
      runMacro(macroFiles[selectedMacro], true);
    }
    return;
  }

  if (touched(btnLoop, x, y)) {
    if (selectedMacro >= 0 && selectedMacro < macroCount && AR488.connected() &&
        macroEnabled[selectedMacro] && macroFormatValid[selectedMacro] &&
        macroHasLoopSection[selectedMacro] && !macroOperationLocked()) {
      runMacroLoop(macroFiles[selectedMacro], true);
    }
    return;
  }
}

// ============================================================
// USB MSC
// ============================================================
static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  uint8_t* dst = reinterpret_cast<uint8_t*>(buffer);
  uint8_t sectorBuffer[512];
  uint32_t bytePosition = lba * 512UL + offset;
  uint32_t remaining = bufsize;

  while (remaining > 0) {
    uint32_t sector = bytePosition / 512UL;
    uint32_t sectorOffset = bytePosition % 512UL;
    uint32_t copySize = min(remaining, (uint32_t)(512UL - sectorOffset));
    if (!SD.readRAW(sectorBuffer, sector)) return -1;
    memcpy(dst, sectorBuffer + sectorOffset, copySize);
    dst += copySize;
    bytePosition += copySize;
    remaining -= copySize;
  }
  return bufsize;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  uint8_t sectorBuffer[512];
  uint32_t bytePosition = lba * 512UL + offset;
  uint32_t remaining = bufsize;
  uint8_t* src = buffer;

  while (remaining > 0) {
    uint32_t sector = bytePosition / 512UL;
    uint32_t sectorOffset = bytePosition % 512UL;
    uint32_t copySize = min(remaining, (uint32_t)(512UL - sectorOffset));
    if (sectorOffset != 0 || copySize != 512) {
      if (!SD.readRAW(sectorBuffer, sector)) return -1;
    }
    memcpy(sectorBuffer + sectorOffset, src, copySize);
    if (!SD.writeRAW(sectorBuffer, sector)) return -1;
    src += copySize;
    bytePosition += copySize;
    remaining -= copySize;
  }
  return bufsize;
}

static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) {
  return true;
}

void startUsbSdMode() {
  cancelAutoAr488();
  currentMode = MODE_USB_SD;
  webRunRequested = false;
  webRequestedMacroPath = "";
  webLoopRequested = false;
  webRequestedLoopPath = "";
  closeLogFile();
  resetUploadState();
  stopWebServer();
  stopWiFi();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.println("USB SD MODE");
  M5.Display.println("-----------");

  M5.Power.setUsbOutput(false);
  delay(200);
  M5.Display.print("SD : ");
  if (!initSD()) { M5.Display.println("ERROR"); return; }
  M5.Display.println("OK");

  uint32_t sectorSize = SD.sectorSize();
  uint32_t sectorCount = SD.numSectors();
  M5.Display.printf("Size: %llu MB\n", SD.cardSize() / 1024ULL / 1024ULL);

  MSC.vendorID("AR488");
  MSC.productID("CoreS3 SD");
  MSC.productRevision("0.1");
  MSC.onRead(mscRead);
  MSC.onWrite(mscWrite);
  MSC.onStartStop(mscStartStop);
  MSC.mediaPresent(true);
  MSC.isWritable(true);

  if (!MSC.begin(sectorCount, sectorSize)) {
    M5.Display.println("MSC : ERROR");
    return;
  }
  USB.begin();
  M5.Display.println("MSC : READY");
  M5.Display.println();
  M5.Display.println("Connect PC");
  M5.Display.println();
  M5.Display.println("PC owns SD");
  M5.Display.println("Reboot when done");
}

// ============================================================
// Main menu
// ============================================================
void loopModeSelect() {
  static uint32_t lastTimeDraw = 0;
  if (millis() - lastTimeDraw >= 1000) {
    lastTimeDraw = millis();
    drawMainTime();
    drawAutoAr488Status();
  }

  // No local operation is required after a power recovery when enabled.
  if (autoAr488Armed && (int32_t)(autoAr488DeadlineMillis - millis()) <= 0) {
    cancelAutoAr488();
    startAR488Mode();
    return;
  }

  auto detail = M5.Touch.getDetail();
  if (!detail.wasClicked()) return;
  int x = detail.x;
  int y = detail.y;
  if (touched(btnAR488, x, y)) { cancelAutoAr488(); startAR488Mode(); return; }
  if (touched(btnUSBSD, x, y)) { cancelAutoAr488(); startUsbSdMode(); return; }
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  initializeTime();
  initSD();
  ensureDirectory(MACRO_DIR);
  ensureLogDirectory();
  scanMacros();
  ensureWebServer();
  lastWiFiConnectedState = (WiFi.status() == WL_CONNECTED);
  armAutoAr488();
  drawModeMenu();
}

void loop() {
  M5.update();
  serviceWiFiConnection();
  ensureWebServer();
  handleWebServer();

  if (webRunRequested && !macroBusy && !loopActive) {
    if (currentMode == MODE_AR488 && AR488.connected()) {
      String path = webRequestedMacroPath;
      webRunRequested = false;
      webRequestedMacroPath = "";
      runMacro(path, false);
    } else {
      // Never leave a remote RUN request armed for a later reconnect.
      webRunRequested = false;
      webRequestedMacroPath = "";
    }
  }

  if (webLoopRequested && !macroBusy && !loopActive) {
    if (currentMode == MODE_AR488 && AR488.connected()) {
      String path = webRequestedLoopPath;
      webLoopRequested = false;
      webRequestedLoopPath = "";
      runMacroLoop(path, false);
    } else {
      webLoopRequested = false;
      webRequestedLoopPath = "";
    }
  }

  switch (currentMode) {
    case MODE_SELECT: loopModeSelect(); break;
    case MODE_AR488: loopAR488(); break;
    case MODE_USB_SD: break;
  }

  if (currentMode != MODE_USB_SD) servicePeriodicNtp();
  delay(5);
}
