#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include <SoftwareSerial.h>
#include <ESP8266mDNS.h>

const char* ssid = "Arko";
const char* password = "Arko#1124";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 6 * 3600;
const int daylightOffset_sec = 0;

ESP8266WebServer server(80);

#define MEGA_RX_PIN 0
#define MEGA_TX_PIN 2
SoftwareSerial MegaSerial(MEGA_RX_PIN, MEGA_TX_PIN);

const int gateButtonPin = 15;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

int availableSlots = 0;
String emptySlots = "";
String filledSlots = "";
const int TOTAL_SLOTS = 7;

struct LogEntry {
  String timestamp;
  String messageType;
  String uid;
  String action;
  String status;
  String details;
};
const int MAX_LOG_ENTRIES = 50;
LogEntry logEntries[MAX_LOG_ENTRIES];
int logIndex = 0;

enum GateState { IDLE, OPENING, OPENED, GATE_CLOSING, GATE_CLOSED };
GateState gateState = IDLE;
unsigned long gateStateChangeTime = 0;

String lastEntryUID = "";
String gateDirection = "";

struct CarHistoryEntry {
  String uid;
  String entryTimestamp;
  String exitTimestamp;
  bool isActive;
  float cost;
};

const int MAX_CAR_HISTORY_ENTRIES = 100;
CarHistoryEntry carHistory[MAX_CAR_HISTORY_ENTRIES];
int carHistoryCount = 0;

const float HOURLY_RATE = 100.0;

void handleRoot();
void handleData();
void handleOpenGate();
void handleNotFound();
void processMessage(String message);
String getCurrentTimestamp();
time_t convertTimestampToTimeT(String timestampStr);
String getMegaLikeTimestamp();
void addLogEntry(String messageType, String uid, String action, String status, String details = "");
bool listContains(const String& list, int slotNumber);
void updateCarHistoryEntry(String uid, bool isEntry);
void updateGateStateDisplay();
float calculateParkingCost(time_t entryTime, time_t exitTime);

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP8266 Initializing ---");

  MegaSerial.begin(9600);

  pinMode(gateButtonPin, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int wifi_retries = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retries < 40) {
    delay(250);
    wifi_retries++;
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("smartparking")) {
      Serial.println("MDNS responder started");
      MDNS.addService("http", "tcp", 80);
      Serial.println("Access at http://smartparking.local/");
    } else {
      Serial.println("Error setting up MDNS responder!");
    }

  } else {
    Serial.println("\nFailed to connect to WiFi. Web server and NTP will not function.");
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    time_t now = time(nullptr);
    int ntp_retries = 0;
    while (now < 8 * 3600 * 2 && ntp_retries < 20) {
      delay(500);
      now = time(nullptr);
      ntp_retries++;
      yield();
    }
    if (now >= 8 * 3600 * 2) {
      Serial.println("\nTime synchronized.");
    } else {
      Serial.println("\nNTP time sync failed. Timestamps will use uptime as fallback.");
    }
  } else {
    Serial.println("Skipping NTP time initialization (No WiFi connection).");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/open-gate", HTTP_POST, handleOpenGate);
  server.onNotFound(handleNotFound);

  if (WiFi.status() == WL_CONNECTED) {
    server.begin();
  }

  for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
    logEntries[i] = {"", "", "", "", "", ""};
  }

  for (int i = 0; i < MAX_CAR_HISTORY_ENTRIES; i++) {
    carHistory[i] = {"", "", "", false, 0.0f};
  }

  MegaSerial.println("REQUEST_DATA");
}

String getMegaLikeTimestamp() {
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long remainder_ms = ms % 1000;
  unsigned long remainder_seconds = seconds % 60;
  unsigned long remainder_minutes = minutes % 60;

  char buf[20];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%03lu", hours % 24, remainder_minutes, remainder_seconds, remainder_ms);
  return String(buf);
}

String getCurrentTimestamp() {
  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) {
    unsigned long ms = millis();
    unsigned long s = ms / 1000;
    unsigned long m = s / 60;
    unsigned long h = m / 60;
    char buf[20];
    snprintf(buf, sizeof(buf), "T+%02lu:%02lu:%02lu", h % 24, m % 60, s % 60);
    return String(buf);
  }
  struct tm *timeinfo = localtime(&now);
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(timeStr);
}

time_t convertTimestampToTimeT(String timestampStr) {
  struct tm tm;
  int year, month, day, hour, minute, second;

  sscanf(timestampStr.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);

  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = -1;

  return mktime(&tm);
}

float calculateParkingCost(time_t entryTime, time_t exitTime) {
  if (entryTime == (time_t)-1 || exitTime == (time_t)-1 || exitTime < entryTime) {
    return 0.0f;
  }

  double durationSeconds = difftime(exitTime, entryTime);
  float durationHours = durationSeconds / 3600.0f;

  return durationHours * HOURLY_RATE;
}

void addLogEntry(String messageType, String uid, String action, String status, String details) {
  String webTimestamp = getCurrentTimestamp();
  String megaLikeTimestamp = getMegaLikeTimestamp();

  logEntries[logIndex] = {webTimestamp, messageType, uid, action, status, details};
  logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;

  String serialOutput = megaLikeTimestamp + " -> ";

  if (messageType == "SLOT_UPDATE") {
    serialOutput += "Empty Slots: " + emptySlots + " | Filled Slots: " + filledSlots;
  } else if (messageType == "ENTRY_SCAN") {
    serialOutput += "Entry Scan: Card ID: " + uid;
  } else if (messageType == "EXIT_SCAN") {
    serialOutput += "Exit Scan: Card ID: " + uid;
  } else if (messageType == "ENTRY_STATUS") {
    serialOutput += "Entry Status: UID: " + uid + " | Status: " + status;
    if (details != "") serialOutput += " | Details: " + details;
  } else if (messageType == "EXIT_STATUS") {
    serialOutput += "Exit Status: UID: " + uid + " | Status: " + status;
    if (details != "") serialOutput += " | Details: " + details;
  } else if (messageType == "GATE_OPERATION") {
    serialOutput += "GATE_OPERATION: " + status;
  } else if (messageType == "CAR_PARKED") {
    serialOutput += "Car Parked: UID: " + uid;
  } else if (messageType == "CAR_EXITED") {
    serialOutput += "Car Exited: UID: " + uid;
  } else if (messageType == "ACK") {
    serialOutput += "ACK: " + details;
  } else if (messageType == "MEGA_ERROR") {
    serialOutput += "ERROR: " + details;
  } else if (messageType == "SYSTEM_WARN") {
    serialOutput += "WARNING: " + details;
  } else if (messageType == "PARSE_ERROR") {
    serialOutput += "PARSE ERROR: " + details;
  } else if (messageType == "GATE_COMMAND") {
    serialOutput += "GATE_COMMAND (Manual): " + status + " (" + details + ")";
  } else if (messageType == "GATE_TIMEOUT") {
    serialOutput += "GATE_TIMEOUT: " + details;
  } else if (messageType == "CAR_HISTORY_UPDATE") {
    serialOutput += "CAR_HISTORY_UPDATE: UID: " + uid + " | Action: " + action + " | Status: " + status + " | Details: " + details;
  }
  else {
    serialOutput += "UNKNOWN_MESSAGE: Type=" + messageType + " | Raw=" + details;
  }

  Serial.println(serialOutput);
}

bool listContains(const String& list, int slotNumber) {
  String target = String(slotNumber);
  if (list == "") return false;
  String tempList = "," + list + ",";
  String tempTarget = "," + target + ",";
  return tempList.indexOf(tempTarget) != -1;
}

void updateCarHistoryEntry(String uid, bool isEntry) {
  int foundIndex = -1;
  for (int i = 0; i < carHistoryCount; i++) {
    if (carHistory[i].uid == uid && carHistory[i].isActive) {
      foundIndex = i;
      break;
    }
  }

  if (isEntry) {
    if (foundIndex != -1) {
      time_t entryTime = convertTimestampToTimeT(carHistory[foundIndex].entryTimestamp);
      time_t exitTime = time(nullptr);
      carHistory[foundIndex].exitTimestamp = getCurrentTimestamp();
      carHistory[foundIndex].isActive = false;
      carHistory[foundIndex].cost = calculateParkingCost(entryTime, exitTime);
      addLogEntry("CAR_HISTORY_UPDATE", uid, "Entry (Re-entry detected)", "Conflict", "Car re-entered before old entry was formally exited. Old entry closed. Cost: " + String(carHistory[foundIndex].cost, 2) + " Taka");
      foundIndex = -1;
    }

    if (carHistoryCount < MAX_CAR_HISTORY_ENTRIES) {
      carHistory[carHistoryCount].uid = uid;
      carHistory[carHistoryCount].entryTimestamp = getCurrentTimestamp();
      carHistory[carHistoryCount].exitTimestamp = "";
      carHistory[carHistoryCount].isActive = true;
      carHistory[carHistoryCount].cost = 0.0f;
      addLogEntry("CAR_HISTORY_UPDATE", uid, "Entry", "New Entry", "Car " + uid + " recorded entering.");
      carHistoryCount++;
    } else {
      addLogEntry("SYSTEM_WARN", uid, "Entry", "History Full", "Could not add car " + uid + " to history, max entries reached.");
    }
  } else {
    if (foundIndex != -1) {
      time_t entryTime = convertTimestampToTimeT(carHistory[foundIndex].entryTimestamp);
      time_t exitTime = time(nullptr);
      carHistory[foundIndex].exitTimestamp = getCurrentTimestamp();
      carHistory[foundIndex].isActive = false;
      carHistory[foundIndex].cost = calculateParkingCost(entryTime, exitTime);
      addLogEntry("CAR_HISTORY_UPDATE", uid, "Exit", "Logged", "Car " + uid + " recorded exiting. Cost: " + String(carHistory[foundIndex].cost, 2) + " Taka");
    } else {
      addLogEntry("SYSTEM_WARN", uid, "Exit", "Not Found", "Car " + uid + " exited but no active entry found in history to close.");
    }
  }
}

void processMessage(String message) {
  message.trim();

  if (message.length() == 0) {
    return;
  }

  int colonIndex = message.indexOf(':');
  if (colonIndex == -1) {
    addLogEntry("PARSE_ERROR", "", "System", "Rejected", "Malformed (no colon): " + message);
    return;
  }

  String messageType = message.substring(0, colonIndex);
  String fullData = message.substring(colonIndex + 1);

  if (messageType == "SLOT_UPDATE") {
    int firstPipe = fullData.indexOf('|');
    int secondPipe = fullData.indexOf('|', firstPipe + 1);

    if (firstPipe == -1 || secondPipe == -1) {
      addLogEntry("PARSE_ERROR", "", "System", "Rejected", "Malformed SLOT_UPDATE: " + message);
      return;
    }

    String availableStr = fullData.substring(0, firstPipe);
    String emptyStr = fullData.substring(firstPipe + 1, secondPipe);
    String filledStr = fullData.substring(secondPipe + 1);

    availableSlots = availableStr.toInt();
    emptySlots = emptyStr;
    filledSlots = filledStr;

    addLogEntry("SLOT_UPDATE", "", "System", "Updated", "");
    yield();
  } else if (messageType == "ENTRY_SCAN") {
    String uid = fullData;
    addLogEntry("ENTRY_SCAN", uid, "Entry", "Detected", "");
    yield();
  } else if (messageType == "EXIT_SCAN") {
    String uid = fullData;
    addLogEntry("EXIT_SCAN", uid, "Exit", "Detected", "");
    yield();
  } else if (messageType == "ENTRY_STATUS" || messageType == "EXIT_STATUS") {
    int firstPipe = fullData.indexOf('|');
    int secondPipe = fullData.indexOf('|', firstPipe + 1);

    if (firstPipe == -1 || secondPipe == -1) {
      addLogEntry("PARSE_ERROR", "", "System", "Rejected", "Malformed " + messageType + ": " + message);
      return;
    }

    String timestampFromMega = fullData.substring(0, firstPipe);
    String uid = fullData.substring(firstPipe + 1, secondPipe);
    String status = fullData.substring(secondPipe + 1);

    String action = (messageType == "ENTRY_STATUS") ? "Entry" : "Exit";
    String logDetails = "Mega Time: " + timestampFromMega;

    addLogEntry(messageType, uid, action, status, logDetails);
    yield();

    if (status == "Granted") {
      if (messageType == "ENTRY_STATUS") {
        lastEntryUID = uid;
        updateCarHistoryEntry(uid, true);
      } else {
        updateCarHistoryEntry(uid, false);
      }
    }
  } else if (messageType == "GATE_OPERATION") {
    int firstPipe = fullData.indexOf('|');
    String direction = fullData.substring(0, firstPipe);
    String status = fullData.substring(firstPipe + 1);

    gateDirection = direction;

    addLogEntry("GATE_OPERATION", "", direction, status, "");
    yield();

    gateStateChangeTime = millis();
    if (status == "Opening") {
      gateState = OPENING;
    } else if (status == "Opened") {
      gateState = OPENED;
    } else if (status == "Closing") {
      gateState = GATE_CLOSING;
    } else if (status == "Closed") {
      gateState = GATE_CLOSED;
    }
  } else if (messageType == "CAR_PARKED") {
    String uid = fullData;
    addLogEntry("CAR_PARKED", uid, "Parked", "Confirmed", "");
    yield();
  } else if (messageType == "CAR_EXITED") {
    String uid = fullData;
    addLogEntry("CAR_EXITED", uid, "Exited", "Confirmed", "");
    yield();
  } else if (messageType == "ACK") {
    String ackMsg = fullData;
    addLogEntry("ACK", "", "System", "Acknowledged", ackMsg);
    yield();
  } else if (messageType == "ERROR") {
    String errorDesc = fullData;
    addLogEntry("MEGA_ERROR", "", "System", "Error", errorDesc);
    yield();
  } else {
    addLogEntry("UNKNOWN_MSG", "", "System", "Received", message);
    yield();
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html lang='en'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<title>Smart Parking System Monitor</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 0 auto; max-width: 1000px; padding: 20px; background-color: #f0f0f0; }";
  html += "h1, h2 { color: #333; text-align: center; }";
  html += ".card { background: #fff; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += ".button { background-color: #5cb85c; border: none; color: white; padding: 12px 25px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 10px 2px; cursor: pointer; border-radius: 5px; transition: background-color 0.3s ease; }";
  html += ".button:hover { background-color: #4cae4c; }";
  html += ".status { font-weight: bold; }";
  html += ".available { color: #5cb85c; }";
  html += ".full { color: #d9534f; }";
  html += ".granted { color: #5cb85c; }";
  html += ".rejected { color: #d9534f; }";
  html += ".detected { color: #f0ad4e; }";
  html += ".system { color: #5bc0de; }";
  html += "table { width: 100%; border-collapse: collapse; margin-top: 20px; }";
  html += "th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }";
  html += "th { background-color: #f8f8f8; color: #333; font-weight: bold;}";
  html += ".slot-available td { background-color: #dff0d8; }";
  html += ".slot-filled td { background-color: #f2dede; }";
  html += ".log-container { max-height: 350px; overflow-y: auto; border: 1px solid #eee; padding: 10px; background-color: #fdfdfd; border-radius: 5px; margin-top: 10px;}";
  html += ".log-entry { margin-bottom: 8px; padding: 8px; border-left: 5px solid #ccc; }";
  html += ".log-entry strong { color: #555; }";
  html += ".log-entry.status-granted { border-left-color: #5cb85c; }";
  html += ".log-entry.status-rejected { border-left-color: #d9534f; }";
  html += ".log-entry.status-detected { border-left-color: #f0ad4e; }";
  html += ".log-entry.status-updated, .log-entry.status-acknowledged { border-left-color: #5bc0de; }";
  html += ".log-entry.status-opening, .log-entry.status-closing { border-left-color: #f0ad4e; }";
  html += ".log-entry.status-opened { border-left-color: #5cb85c; }";
  html += ".log-entry.status-closed { border-left-color: #aaa; }";
  html += ".log-entry.status-error { border-left-color: #d9534f; background-color: #f2dede; }";
  html += ".log-entry.status-success { border-left-color: #5cb85c; }";
  html += ".log-entry.status-conflict, .log-entry.status-mismatch, .log-entry.status-invalidslot, .log-entry.status-logicerror { border-left-color: #f0ad4e; }";
  html += ".log-entry.status-cleared { border-left-color: #5bc0de; }";
  html += ".log-entry.status-timeout { border-left-color: #d9534f; }";
  html += ".log-entry.status-newentry { border-left-color: #28a745; }";
  html += ".log-entry.status-logged { border-left-color: #007bff; }";
  html += "</style>";
  html += "</head><body>";
  html += "<h1>Smart Parking System Monitor</h1>";

  html += "<div class='card'>";
  html += "<h2>System Status</h2>";
  html += "<p>Parking Status: <span id='parkingStatusText' class='status'></span></p>";
  html += "<p>Empty Slot Numbers: <span id='emptySlotsText'></span></p>";
  html += "<p>Filled Slot Numbers: <span id='filledSlotsText'></span></p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Manual Gate Control</h2>";
  html += "<form action='/open-gate' method='POST' style='text-align:center;'>";
  html += "<button type='submit' class='button'>Open Entry Gate Manually</button>";
  html += "</form>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Parking Slots Detail</h2>";
  html += "<table id='parkingSlotsTable'>";
  html += "<thead><tr><th>Slot Number</th><th>Physical Status</th></tr></thead>";
  html += "<tbody></tbody>";
  html += "</table>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Car Movement History</h2>";
  html += "<div class='log-container'>";
  html += "<table id='carHistoryTable'>";
  html += "<thead><tr><th>Car UID</th><th>Entry Time</th><th>Exit Time</th><th>Status</th><th>Cost</th></tr></thead>";
  html += "<tbody></tbody>";
  html += "</table>";
  html += "</div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Access & System Log</h2>";
  html += "<div id='accessLogContainer' class='log-container'>";
  html += "</div>";
  html += "</div>";

  html += "<p style='text-align: center; color: #666; font-size: 0.9em;'>ESP8266 Time: <span id='esp32Time'></span> | Hostname: smartparking.local</p>";
  html += "<script>";
  html += R"rawliteral(
    async function updateData() {
      try {
        const response = await fetch('/data');
        const data = await response.json();

        const parkingStatusText = document.getElementById('parkingStatusText');
        if (parkingStatusText) {
          parkingStatusText.textContent = data.availableSlots > 0 ? `${data.availableSlots} Space(s) Available` : 'Parking Full';
          parkingStatusText.className = 'status ' + (data.availableSlots > 0 ? 'available' : 'full');
        }
        const emptySlotsText = document.getElementById('emptySlotsText');
        if (emptySlotsText) {
          emptySlotsText.textContent = data.emptySlots === "" ? "None" : data.emptySlots;
        }
        const filledSlotsText = document.getElementById('filledSlotsText');
        if (filledSlotsText) {
          filledSlotsText.textContent = data.filledSlots === "" ? "None" : data.filledSlots;
        }

        const parkingSlotsTableBody = document.getElementById('parkingSlotsTable').querySelector('tbody');
        if (parkingSlotsTableBody) {
          parkingSlotsTableBody.innerHTML = '';
          const totalSlots = 7;
          const filledSlotsArray = data.filledSlots.split(',').filter(s => s !== '').map(Number);

          for (let i = 1; i <= totalSlots; i++) {
            const row = parkingSlotsTableBody.insertRow();
            const isPhysicallyFilled = filledSlotsArray.includes(i);
            const physicalStatus = isPhysicallyFilled ? 'Filled' : 'Available';
            const slotClass = isPhysicallyFilled ? 'slot-filled' : 'slot-available';
            row.className = slotClass;

            const slotNumCell = row.insertCell();
            slotNumCell.textContent = i;
            const physicalStatusCell = row.insertCell();
            physicalStatusCell.textContent = physicalStatus;
          }
        }

        const carHistoryTableBody = document.getElementById('carHistoryTable').querySelector('tbody');
        if (carHistoryTableBody) {
            carHistoryTableBody.innerHTML = '';
            if (data.carHistory && data.carHistory.length > 0) {
                for (let i = data.carHistory.length - 1; i >= 0; i--) {
                    const entry = data.carHistory[i];
                    const row = carHistoryTableBody.insertRow();
                    const statusText = entry.isActive ? "<span style='color:green; font-weight: bold;'>Inside</span>" : "<span style='color:red;'>Exited</span>";
                    const costText = entry.cost > 0 ? `${entry.cost.toFixed(2)} Taka` : "-";

                    row.insertCell().textContent = entry.uid;
                    row.insertCell().textContent = entry.entryTimestamp;
                    row.insertCell().textContent = entry.exitTimestamp === "" ? "-" : entry.exitTimestamp;
                    row.insertCell().innerHTML = statusText;
                    row.insertCell().textContent = costText;
                }
            } else {
                const row = carHistoryTableBody.insertRow();
                const cell = row.insertCell();
                cell.colSpan = 5;
                cell.textContent = 'No car movement history yet.';
            }
        }

        const accessLogContainer = document.getElementById('accessLogContainer');
        if (accessLogContainer) {
            accessLogContainer.innerHTML = '';
            if (data.logEntries && data.logEntries.length > 0) {
                data.logEntries.forEach(logEntry => {
                    if (logEntry.timestamp !== "") {
                        let statusClass = logEntry.status.toLowerCase().replace(/ /g, '');
                        if (statusClass === "newentry") statusClass = "newentry";
                        else if (statusClass === "logged") statusClass = "logged";

                        const logDiv = document.createElement('div');
                        logDiv.className = `log-entry status-${statusClass}`;
                        let logHtml = `<strong>${logEntry.timestamp}</strong><br>`;
                        logHtml += `<strong>Event:</strong> ${logEntry.messageType} `;
                        logHtml += `<strong>Action:</strong> ${logEntry.action} `;
                        logHtml += `<strong>Status:</strong> <span class='${statusClass}'>${logEntry.status}</span>`;
                        if (logEntry.uid !== "") {
                            logHtml += `<br><strong>Car UID:</strong> ${logEntry.uid}`;
                        }
                        if (logEntry.details !== "") {
                            logHtml += `<br><strong>Details:</strong> ${logEntry.details}`;
                        }
                        logDiv.innerHTML = logHtml;
                        accessLogContainer.appendChild(logDiv);
                    }
                });
            } else {
                accessLogContainer.innerHTML = '<p>No log entries yet.</p>';
            }
        }

        const esp32TimeSpan = document.getElementById('esp32Time');
        if (esp32TimeSpan) {
            esp32TimeSpan.textContent = new Date().toLocaleString();
        }

      } catch (error) {
        const statusElement = document.getElementById('parkingStatusText');
        if (statusElement) {
          statusElement.textContent = 'Error: Cannot connect to ESP8266!';
          statusElement.className = 'status full';
        }
      }
    }

    updateData();
    setInterval(updateData, 3000);

    document.addEventListener('DOMContentLoaded', () => {
        const openGateForm = document.querySelector('form[action="/open-gate"]');
        if (openGateForm) {
            openGateForm.addEventListener('submit', async (event) => {
                event.preventDefault();
                try {
                    const response = await fetch('/open-gate', { method: 'POST' });
                    if (response.ok) {

                    } else {
                        alert('Failed to open gate. Server responded with status: ' + response.status);
                    }
                } catch (error) {
                    alert('Error: Could not connect to ESP8266 to open gate.');
                }
            });
        }
    });

  )rawliteral";
  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleData() {
  DynamicJsonDocument doc(4096);

  doc["availableSlots"] = availableSlots;
  doc["emptySlots"] = emptySlots;
  doc["filledSlots"] = filledSlots;
  doc["gateStatus"] = (gateState == OPENING) ? "Opening" :
                       (gateState == OPENED) ? "Opened" :
                       (gateState == GATE_CLOSING) ? "Closing" :
                       (gateState == GATE_CLOSED) ? "Closed" : "Idle";

  JsonArray carHistoryJson = doc.createNestedArray("carHistory");
  for (int i = 0; i < carHistoryCount; i++) {
    if (carHistory[i].uid != "") {
      JsonObject entry = carHistoryJson.createNestedObject();
      entry["uid"] = carHistory[i].uid;
      entry["entryTimestamp"] = carHistory[i].entryTimestamp;
      entry["exitTimestamp"] = carHistory[i].exitTimestamp;
      entry["isActive"] = carHistory[i].isActive;
      entry["cost"] = carHistory[i].cost;
    }
    yield();
  }

  JsonArray logs = doc.createNestedArray("logEntries");
  for (int i = 0; i < MAX_LOG_ENTRIES; ++i) {
    int currentReadIndex = (logIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;

    if (logEntries[currentReadIndex].timestamp != "") {
      JsonObject logEntry = logs.createNestedObject();
      logEntry["timestamp"] = logEntries[currentReadIndex].timestamp;
      logEntry["messageType"] = logEntries[currentReadIndex].messageType;
      logEntry["uid"] = logEntries[currentReadIndex].uid;
      logEntry["action"] = logEntries[currentReadIndex].action;
      logEntry["status"] = logEntries[currentReadIndex].status;
      logEntry["details"] = logEntries[currentReadIndex].details;
    }
    yield();
  }

  String jsonData;
  serializeJsonPretty(doc, jsonData);
  server.send(200, "application/json", jsonData);
  yield();
}

void handleOpenGate() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain", "Service Unavailable (No WiFi connection)");
    return;
  }

  addLogEntry("GATE_COMMAND", "", "Manual", "Requested", "Open Gate via Web UI");
  MegaSerial.println("OPEN_GATE");

  server.send(200, "text/plain", "Gate open command sent.");
  yield();
}

void handleNotFound() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  yield();
}

void updateGateStateDisplay() {
  unsigned long timeSinceUpdate = millis() - gateStateChangeTime;
  const unsigned long GATE_TIMEOUT = 10000;
  const unsigned long OPENED_TIMEOUT = 15000;

  if ((gateState == OPENING || gateState == GATE_CLOSING) && timeSinceUpdate > GATE_TIMEOUT) {
    if (gateState != GATE_CLOSED) {
      gateState = GATE_CLOSED;
      addLogEntry("GATE_TIMEOUT", "", "System", "Timeout", "Gate stuck in Opening/Closing or not confirmed, defaulting to Closed");
      gateStateChangeTime = millis();
    }
  }

  if (gateState == OPENED && timeSinceUpdate > OPENED_TIMEOUT) {
    if (gateState != GATE_CLOSED) {
      gateState = GATE_CLOSED;
      addLogEntry("GATE_TIMEOUT", "", "System", "Timeout", "Gate left Opened for too long, defaulting to Closed");
      gateStateChangeTime = millis();
    }
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    MDNS.update();
  }
  yield();

  updateGateStateDisplay();
  yield();

  int reading = digitalRead(gateButtonPin);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading == LOW && lastButtonState == HIGH) {
      addLogEntry("GATE_COMMAND", "", "Manual", "Pressed", "Physical button pressed to open gate");
      MegaSerial.println("OPEN_GATE");
    }
  }
  lastButtonState = reading;
  yield();

  static String serialBuffer = "";
  while (MegaSerial.available()) {
    char inChar = MegaSerial.read();
    if (inChar != -1) {
      serialBuffer += inChar;
      if (inChar == '\n') {
        serialBuffer.trim();

        if (serialBuffer.length() > 0) {
          processMessage(serialBuffer);
        }
        serialBuffer = "";
      }
    }
    yield();
  }
  yield();

  static unsigned long lastDataRequestTime = 0;
  const unsigned long DATA_REQUEST_INTERVAL = 10000;
  if (millis() - lastDataRequestTime > DATA_REQUEST_INTERVAL) {
    MegaSerial.println("REQUEST_DATA");
    lastDataRequestTime = millis();
  }
  yield();

  delay(10);
}
