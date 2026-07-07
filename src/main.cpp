#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <LittleFS.h>

// --- WiFi Credentials ---
const char* ssid = "ExoShieldSystem";
const char* password = "ExoShield123";

// --- Web Server ---
ESP8266WebServer server(80);

// --- OLED Display Settings ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Pin Definitions ---
// D4 (GPIO2) for the Blue LED (WiFi Status)
const int BLUE_LED_PIN = 2;
// D3 (GPIO0) for the Green LED (Normal Status)
const int GREEN_LED_PIN = 0;
// D8 (GPIO15) for the Red LED (Alert Status)
const int RED_LED_PIN = 15;
// D5 (GPIO14) for DS18B20
const int ONE_WIRE_BUS = 14; 
// D7 (GPIO13) for the Relay
const int RELAY_PIN = 13;    
// D6 (GPIO12) for the Buzzer
const int BUZZER_PIN = 12;

// --- Sensor Setup ---
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Variables ---
float thresholdTemp = 30.0; // Default threshold (adjust as needed)
float currentTemp = 0.0;
bool isPumpOn = false;      // Track state for Web Server and Display

// --- Cooling System Protection (Hysteresis & Cooldown) ---
float hysteresis = 1.0; // Wait until temperature drops 1.0 degree BELOW threshold before turning pump OFF
unsigned long pumpCooldown = 5000; // Minimum 5 seconds between switching ON/OFF (protects pump)
unsigned long pumpLastSwitched = 0;

// EEPROM Settings
#define EEPROM_INIT_FLAG 0xAA // Used to check if EEPROM has been formatted by us

// Timing variables for non-blocking operations
unsigned long lastTempRequest = 0;
const int tempConversionDelay = 800; // 12-bit resolution takes max 750ms.
bool conversionPending = false;

// Buzzer timing variables
unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// --- Helper Function for RGB LED Control ---
void updateStatusLEDs() {
  if (currentTemp <= -100.0) {
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);
    return;
  }
  if (currentTemp >= thresholdTemp) {
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, HIGH);
  } else {
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(RED_LED_PIN, LOW);
  }
}

// --- EEPROM Helper Functions ---
void loadThreshold() {
  EEPROM.begin(512);
  if (EEPROM.read(0) == EEPROM_INIT_FLAG) {
    EEPROM.get(1, thresholdTemp);
    Serial.print("Loaded threshold from EEPROM: ");
    Serial.println(thresholdTemp);
  } else {
    Serial.println("EEPROM empty. Using default threshold.");
  }
}

void saveThreshold(float newTemp) {
  thresholdTemp = newTemp;
  EEPROM.write(0, EEPROM_INIT_FLAG); 
  EEPROM.put(1, thresholdTemp);      
  EEPROM.commit();                   
  Serial.print("Saved new threshold to EEPROM: ");
  Serial.println(thresholdTemp);
}

// --- Web Server Handlers ---
void handleData() {
  int healthVal = (currentTemp <= -100.0) ? 0 : 100;
  String json = "{";
  json += "\"temp\":" + String(currentTemp, 1) + ",";
  json += "\"pump\":" + String(isPumpOn ? "true" : "false") + ",";
  json += "\"health\":" + String(healthVal) + ",";
  json += "\"threshold\":" + String(thresholdTemp, 1);
  json += "}";
  server.send(200, "application/json", json);
}

void handleUpdate() {
  if (server.hasArg("threshold")) {
    float newThresh = server.arg("threshold").toFloat();
    saveThreshold(newThresh);
  }
  server.send(200, "text/plain", "OK"); 
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting Temperature Control System...");

  loadThreshold();

  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  
  digitalWrite(BLUE_LED_PIN, LOW); 
  digitalWrite(GREEN_LED_PIN, LOW); 
  digitalWrite(RED_LED_PIN, LOW); 
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); 
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 

  sensors.begin();
  sensors.setWaitForConversion(false); 

  if (!LittleFS.begin()) {
    Serial.println("An Error has occurred while mounting LittleFS");
    // Continue running so hardware still works even if FS fails
  } else {
    Serial.println("LittleFS Mounted Successfully");
  }
  
  // --- OLED & WiFi Setup Sequence ---
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed. Check I2C wiring."));
    for(;;); 
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(F("System Booting..."));
  display.println(F("Connecting WiFi:"));
  display.print(ssid);
  display.display();

  WiFi.begin(ssid, password);
  int ledState = LOW;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    ledState = !ledState;
    digitalWrite(BLUE_LED_PIN, ledState); 
    display.print("."); 
    display.display();
  }
  digitalWrite(BLUE_LED_PIN, LOW); 
  Serial.println(F("\nWiFi connected"));
  
  // --- ROBUST WEB SERVER SETUP ---
  // Explicitly serve ONLY the files we have so it doesn't swallow our /data API endpoint!
  server.serveStatic("/", LittleFS, "/index.html");
  
  // Bulletproof Logo Routing: Matches any uppercase/lowercase combination
  server.serveStatic("/Logo.png", LittleFS, "/Logo.png", "max-age=86400"); 
  server.serveStatic("/Logo.png", LittleFS, "/logo.png", "max-age=86400"); 
  server.serveStatic("/logo.png", LittleFS, "/logo.png", "max-age=86400"); 
  server.serveStatic("/logo.png", LittleFS, "/Logo.png", "max-age=86400"); 
  
  server.on("/data", handleData); 
  server.on("/update", HTTP_POST, handleUpdate);
  server.begin();
  Serial.println("Web server started");

  // Show IP Address momentarily
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println(F("WiFi Connected!"));
  display.print(F("IP: "));
  display.println(WiFi.localIP());
  display.display();
  delay(6000); 
  display.clearDisplay(); 

  Serial.println("System Initialized.");
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();

  if (!conversionPending) {
    sensors.requestTemperatures();
    lastTempRequest = currentMillis;
    conversionPending = true;
  }

  if (conversionPending && (currentMillis - lastTempRequest >= tempConversionDelay)) {
    currentTemp = sensors.getTempCByIndex(0);
    conversionPending = false; 

    bool canSwitch = (currentMillis - pumpLastSwitched >= pumpCooldown);

    if (currentTemp >= thresholdTemp) {
      if (!isPumpOn && canSwitch) {
        digitalWrite(RELAY_PIN, HIGH);
        isPumpOn = true;
        pumpLastSwitched = currentMillis;
      }
    } else if (currentTemp <= (thresholdTemp - hysteresis)) {
      if (isPumpOn && canSwitch) {
        digitalWrite(RELAY_PIN, LOW); 
        isPumpOn = false;
        pumpLastSwitched = currentMillis;
      }
    } else if (currentTemp <= -100.0) {
      if (isPumpOn) {
         digitalWrite(RELAY_PIN, LOW); 
         isPumpOn = false;
         pumpLastSwitched = currentMillis;
      }
    }

    updateStatusLEDs();

    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 0);
    if (currentTemp > -100.0) {
        display.print(currentTemp, 1); 
        display.print((char)247);      
        display.println("C");
    } else {
        display.println("ERR");
    }

    display.setTextSize(1);
    display.setCursor(0, 32);
    display.print("Pump: ");
    
    if (isPumpOn) {
        display.println("ON");
        display.setCursor(0, 48);
        display.println("Cooling Active");
    } else {
        display.println("OFF");
        display.setCursor(0, 48);
        display.println("Cooling Inactive");
    }
    display.display(); 
  }

  if (currentTemp >= thresholdTemp) {
    float tempDifference = currentTemp - thresholdTemp;
    int beepInterval = 1000 - (int)(tempDifference * 200);
    if (beepInterval < 100) beepInterval = 100; 

    if (currentMillis - lastBuzzerToggle >= (unsigned long)beepInterval) {
      buzzerState = !buzzerState; 
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      lastBuzzerToggle = currentMillis;
    }
  } else {
    if (buzzerState) {
      buzzerState = false;
      digitalWrite(BUZZER_PIN, LOW);
    }
  }

  // Yield to the ESP8266 background Wi-Fi tasks to prevent dropped connections
  yield(); 
}