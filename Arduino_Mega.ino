#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// --- Configuration ---
// RFID Reader Pins (Connected to Mega's digital pins via SPI)
#define RST_PIN A0
#define SDA_ENTRY A8 // SS pin for Entry Reader (e.g., connected to Mega pin 62 / A8)
#define SDA_EXIT A10 // SS pin for Exit Reader (e.g., connected to Mega pin 64 / A10)
#define NR_OF_READERS 2

// LED Pins
#define Blue 34
#define Yellow 36
#define Red 38
#define Green 40
#define White 42

// LCD Parameters
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Servo Motor Pin
#define GATE_SERVO_PIN 8
Servo gateServo;

// Parking Slot IR Sensor Pins
#define NUM_SLOTS 7
int irSensors[NUM_SLOTS] = {22, 23, 24, 25, 26, 27, 28};

// Authorized Card UIDs (Ensure these are correct and lowercase if from ESP32/8266)
String authorizedCards[] = {"331171f7", "c3d77df7", "634a83f7", "33c775f7", "e3507af7", "324e8c", "23a6dac", "1cc4c11"};
const int numAuthorizedCards = sizeof(authorizedCards) / sizeof(authorizedCards[0]);

// --- Global Variables ---
MFRC522 rfidReaders[NR_OF_READERS];
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 5000; // Send slot status every 5 seconds

// --- Helper Functions ---
bool isCardAuthorized(String uid) {
    for (int i = 0; i < numAuthorizedCards; i++) {
        if (authorizedCards[i] == uid) {
            return true;
        }
    }
    return false;
}

String getCurrentTime() {
    unsigned long currentMillis = millis();
    unsigned long seconds = currentMillis / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    
    seconds %= 60;
    minutes %= 60;
    hours %= 24;
    
    char timeStr[9];
    sprintf(timeStr, "%02lu:%02lu:%02lu", hours, minutes, seconds);
    return String(timeStr);
}

void sendToESP32(String messageType, String data) {
    String message;
    if (messageType == "SLOT_UPDATE") {
        message = messageType + ":" + data; // No timestamp for SLOT_UPDATE
    } else {
        message = messageType + ":" + getCurrentTime() + "|" + data; // Include timestamp for other messages
    }
    Serial1.println(message);
    Serial.println("Sent to ESP: " + message); // For debugging
}

int getAvailableSlots(bool forceUpdate = false) {
    static int previousAvailableSlots = -1;
    static int previousSlotStatus[NUM_SLOTS] = {0};
    static int previousEmptyCount = 0;
    static int previousFilledCount = 0;
    int currentAvailableSlots = 0; // Renamed to avoid confusion with the function name
    bool statusChanged = false;
    
    // Check for any changes in slot status or if a force update is requested
    for (int i = 0; i < NUM_SLOTS; i++) {
        int currentStatus = digitalRead(irSensors[i]);
        if (currentStatus != previousSlotStatus[i]) {
            statusChanged = true;
        }
        previousSlotStatus[i] = currentStatus; // Always update previous status for next check
    }

    // Only proceed with LCD and serial updates if status changed or forceUpdate is true
    if (!statusChanged && !forceUpdate && currentAvailableSlots == previousAvailableSlots) {
        return currentAvailableSlots;
    }

    lcd.setCursor(5, 0);
    lcd.print("Slot Status");
    lcd.setCursor(0, 1);
    lcd.print("Available: ");
    lcd.setCursor(0, 2);
    lcd.print("Empty: ");
    lcd.setCursor(0, 3);
    lcd.print("Filled:");

    String emptySlotsList = "";
    String filledSlotsList = "";
    int emptyIndex = 0;
    int filledIndex = 0;
    
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (previousSlotStatus[i] == HIGH) { // Slot is Empty
            currentAvailableSlots++;
            if (emptySlotsList != "") emptySlotsList += ",";
            emptySlotsList += String(i+1);
            
            lcd.setCursor(7 + (emptyIndex * 2), 2);
            lcd.print(i + 1);
            emptyIndex++;
        } else { // Slot is Filled
            if (filledSlotsList != "") filledSlotsList += ",";
            filledSlotsList += String(i+1);
            
            lcd.setCursor(7 + (filledIndex * 2), 3);
            lcd.print(i + 1);
            filledIndex++;
        }
    }

    // Clear only the extra leftover numbers from previous state
    for (int i = emptyIndex; i < previousEmptyCount; i++) {
        lcd.setCursor(7 + (i * 2), 2);
        lcd.print("  "); // Print two spaces to clear a two-digit number
    }
    for (int i = filledIndex; i < previousFilledCount; i++) {
        lcd.setCursor(7 + (i * 2), 3);
        lcd.print("  "); // Print two spaces to clear a two-digit number
    }

    lcd.setCursor(11, 1);
    lcd.print("   "); // Clear previous available count
    lcd.setCursor(11, 1);
    lcd.print(currentAvailableSlots);
    previousAvailableSlots = currentAvailableSlots;
    
    Serial.println("Empty Slots: " + emptySlotsList + " | Filled Slots: " + filledSlotsList);
    sendToESP32("SLOT_UPDATE", String(currentAvailableSlots) + "|" + emptySlotsList + "|" + filledSlotsList);

    previousEmptyCount = emptyIndex;
    previousFilledCount = filledIndex;

    if (currentAvailableSlots == 0) {
        digitalWrite(Blue, HIGH);
    } else {
        digitalWrite(Blue, LOW);
    }

    return currentAvailableSlots;
}

void openGate(String direction) {
    
    lcd.setCursor(0, 3);
    digitalWrite(Red, LOW);
    digitalWrite(Green, HIGH);
    lcd.print("Gate Opening...");
    gateServo.write(120); // Open position
    
    sendToESP32("GATE_OPERATION", direction + "|Opening");
    
    delay(3000); // Gate open duration
    lcd.setCursor(0, 3);
    lcd.print("Gate Closing...");
    digitalWrite(Green, LOW);
    digitalWrite(Yellow, HIGH);
    
    sendToESP32("GATE_OPERATION", direction + "|Closing");
    
    delay(2000); // Gate closing duration
    digitalWrite(Yellow, LOW);
    digitalWrite(Red, HIGH);
    gateServo.write(0); // Closed position
    lcd.setCursor(0, 3);
    lcd.print("Gate Closed.      ");
    
    sendToESP32("GATE_OPERATION", direction + "|Closed");

}

// --- Setup ---
void setup() {
    Serial.begin(9600);  // For debugging to PC (using Mega's USB port)
    Serial1.begin(9600); // For communication with ESP8266 (on Mega pins 18/19)
    
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("Arduino Mega Parking System Initializing...");
    
    SPI.begin();
    gateServo.attach(GATE_SERVO_PIN);
    gateServo.write(0);
    
    pinMode(Red, OUTPUT);
    pinMode(Green, OUTPUT);
    pinMode(Yellow, OUTPUT);
    pinMode(White, OUTPUT);
    pinMode(Blue, OUTPUT);
    digitalWrite(Red, HIGH);
    digitalWrite(Green, LOW);
    digitalWrite(Yellow, LOW);
    digitalWrite(White, LOW);
    digitalWrite(Blue, LOW);

    int ssPins[] = {SDA_ENTRY, SDA_EXIT};
    for (uint8_t i = 0; i < NR_OF_READERS; i++) {
        rfidReaders[i].PCD_Init(ssPins[i], RST_PIN);
        rfidReaders[i].PCD_DumpVersionToSerial(); // This will print RFID version to Serial (PC)
    }

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 1);
    lcd.print("RFID Based Automated");
    lcd.setCursor(1, 2);
    lcd.print("Car Parking System");
    delay(2000);
    lcd.clear();

    for (int i = 0; i < NUM_SLOTS; i++) {
        pinMode(irSensors[i], INPUT);
    }
    
    Serial.println("System Ready.");
    getAvailableSlots(true); // Initial status update to LCD and ESP8266
}

// --- Main Loop ---
void loop() {
    // Handle communication with ESP8266
    if (Serial1.available()) {
        String command = Serial1.readStringUntil('\n');
        command.trim();
        
        if (command == "OPEN_GATE") {
            Serial.println("Received OPEN_GATE command from ESP.");

            lcd.clear();       
            openGate("REMOTE");
            Serial1.println("ACK:Gate operation completed"); // Acknowledge to ESP
            lcd.clear();

        } else if (command == "REQUEST_DATA") {
            getAvailableSlots(true); // Send current slot data to ESP
        }
    }
    
    // Periodically send slot status (if not explicitly requested or changed)
    if (millis() - lastLogTime > LOG_INTERVAL) {
        getAvailableSlots(); // Checks for changes and sends if necessary
        lastLogTime = millis();
    }
    
    // Forward data from PC (Serial) to ESP8266 (Serial1) - Passthrough for debugging
    if (Serial.available()) {
        String message = Serial.readStringUntil('\n');
        Serial1.println(message);
        Serial.println("Forwarded from PC to ESP: " + message);
    }
    
    // Process RFID readers
    processEntryScan(0); // Reader 0 for Entry
    processExitScan(1);  // Reader 1 for Exit
    
    // Small delay to prevent busy-waiting
    delay(10);
}

// --- Entry Gate Processing ---
void processEntryScan(int readerIndex) {
    if (rfidReaders[readerIndex].PICC_IsNewCardPresent() && rfidReaders[readerIndex].PICC_ReadCardSerial()) {
        String cardUID = "";
        for (byte i = 0; i < rfidReaders[readerIndex].uid.size; i++) {
            cardUID += String(rfidReaders[readerIndex].uid.uidByte[i], HEX);
        }
        cardUID.toLowerCase(); // Convert UID to lowercase for consistent comparison

        Serial.print("ENTRY SCAN - Card UID: ");
        Serial.println(cardUID);
        sendToESP32("ENTRY_SCAN", cardUID);
        
        if (getAvailableSlots(true) > 0) { // Force update slots before deciding entry
            if (isCardAuthorized(cardUID)) {
                Serial.println("ENTRY: Access Granted");
                sendToESP32("ENTRY_STATUS", cardUID + "|Granted");
                
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Entry: Granted");
                digitalWrite(Red, LOW);
                digitalWrite(Green, HIGH);
                
                openGate("ENTRY");
                delay(2000); // Delay after gate operation for car to pass
                
                digitalWrite(Green, LOW);
                digitalWrite(Red, HIGH);
                lcd.clear();
                getAvailableSlots(true); // Re-update slots after car enters
                
                sendToESP32("CAR_PARKED", cardUID); // Confirm car parked
            } else {
                Serial.println("ENTRY: Access Denied - Not Authorized");
                sendToESP32("ENTRY_STATUS", cardUID + "|Denied|Not Authorized");
                
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Entry: Denied");
                lcd.setCursor(0, 2);
                lcd.print("Please Contact Admin");
                digitalWrite(Red, LOW);
                digitalWrite(White, HIGH); // Flash White for unauthorized
                delay(2000);
                digitalWrite(White, LOW);
                digitalWrite(Red, HIGH);
                lcd.clear();
                getAvailableSlots(true);
            }
        } else {
            Serial.println("ENTRY: Parking Full");
            sendToESP32("ENTRY_STATUS", cardUID + "|Denied|Parking Full");
            
            lcd.clear();
            lcd.setCursor(7, 1);
            lcd.print("Sorry!");
            lcd.setCursor(4, 2);
            lcd.print("Parking Full");
            
            if (isCardAuthorized(cardUID)) { // If authorized, flash Green (still denied due to full)
                digitalWrite(Red, LOW);
                for(int i = 0; i < 5; i++) {
                    digitalWrite(Green, HIGH);
                    delay(500);
                    digitalWrite(Green, LOW);
                    delay(500);
                }
                digitalWrite(Red, HIGH);
            } else { // If unauthorized AND full, flash White
                digitalWrite(Red, LOW);
                for(int i = 0; i < 5; i++) {
                    digitalWrite(White, HIGH);
                    delay(500);
                    digitalWrite(White, LOW);
                    delay(500);
                }
                digitalWrite(Red, HIGH);
            }
            delay(2000);
            lcd.clear();
            getAvailableSlots(true);
        }
        rfidReaders[readerIndex].PICC_HaltA();
        rfidReaders[readerIndex].PCD_StopCrypto1();
    }
}

// --- Exit Gate Processing ---
void processExitScan(int readerIndex) {
    if (rfidReaders[readerIndex].PICC_IsNewCardPresent() && rfidReaders[readerIndex].PICC_ReadCardSerial()) {
        String cardUID = "";
        for (byte i = 0; i < rfidReaders[readerIndex].uid.size; i++) {
            cardUID += String(rfidReaders[readerIndex].uid.uidByte[i], HEX);
        }
        cardUID.toLowerCase(); // Convert UID to lowercase for consistent comparison
        
        Serial.print("EXIT SCAN - Card UID: ");
        Serial.println(cardUID);
        sendToESP32("EXIT_SCAN", cardUID);

        if (isCardAuthorized(cardUID)) {
            Serial.println("EXIT: Access Granted");
            sendToESP32("EXIT_STATUS", cardUID + "|Granted");
            
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Exit: Granted");
            digitalWrite(Red, LOW); // Turn off Red for exit, will turn back on later
            digitalWrite(Green, HIGH); // Green for exit
            
            openGate("EXIT");
            delay(2000); // Delay after gate operation for car to pass
            
            digitalWrite(Green, LOW);
            digitalWrite(Red, HIGH); // Turn Red back on
            lcd.clear();
            getAvailableSlots(true); // Re-update slots after car exits
            
            sendToESP32("CAR_EXITED", cardUID); // Confirm car exited
        } else {
            Serial.println("EXIT: Access Denied - Not Authorized");
            sendToESP32("EXIT_STATUS", cardUID + "|Denied|Not Authorized");
            
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Exit: Denied");
            lcd.setCursor(0, 2);
            lcd.print("Please Contact Admin");
            digitalWrite(Red, LOW);
            digitalWrite(White, HIGH); // Flash White for unauthorized exit
            delay(2000);
            digitalWrite(White, LOW);
            digitalWrite(Red, HIGH);
            lcd.clear();
            getAvailableSlots(true);
        }
        rfidReaders[readerIndex].PICC_HaltA();
        rfidReaders[readerIndex].PCD_StopCrypto1();
    }
}
