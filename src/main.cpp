/*
  keep_it_cold - ESP32 LoRa DS18B20 Multi-Node Temperature Monitor
  Author: tscofield
  License: MIT

  Features:
  - DS18B20 temperature sensor
  - LoRa peer node sync (node list & temps)
  - OLED monitor
  - REST API (JSON)
  - Web config (node ID, WiFi, alarms, time, node list)
  - Node-down & check-in alarm (daytime buzzer only, manual timekeeping)
*/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <vector>
#include <Wire.h>
#include <TimeLib.h>
#include <DS3231.h>
#include <LittleFS.h>
#include "CryptoHelper.h"

// ----- Pin Definitions -----
#define OLED_RESET 21
#define DS18B20_PIN 33
#define BUZZER_PIN 32

#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_SS    8
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_DIO0  14
#define LORA_FREQ  915E6 // adjust for region

// ----- Hardware -----
TwoWire twi = TwoWire(1);
Adafruit_SSD1306 display(128, 64, &twi, OLED_RESET);
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
Preferences preferences;
AsyncWebServer server(80);
DNSServer dnsServer;
DS3231 rtc;


// ----- Node Data -----
struct NodeTemp {
  String id;
  float temp1;
  float temp2;
  float temp3;
  time_t lastUpdate;
  bool hasrtc;
};
struct NodeConfig {
  String id;
  String name;
  bool hasrtc;
  time_t lastSeen;
  String temp1_name;
  String temp2_name;
  String temp3_name;
  bool temp1_enabled;
  bool temp2_enabled;
  bool temp3_enabled;
  float temp1_alarm_low;
  float temp1_alarm_high;
  float temp2_alarm_low;
  float temp2_alarm_high;
  float temp3_alarm_low;
  float temp3_alarm_high;
};

String loraPassphrase = "bowman#1";
uint8_t loraKey[16];
uint8_t loraIV[16] = {0};
size_t encLen = 0;
size_t decLen = 0;


std::vector<NodeTemp> nodeTemps;
String nodeID;
String nodeList; // Comma-separated
String wifiSSID = "";
String wifiPASS = "";
float myTemp = NAN;
int16_t lastLoRaStatus = 0;
volatile bool loraPacketReceived = false;
String loraBuffer= "";
bool doIhaveRTC = false;
bool needTime = false;
const char* logFile = "/templog.csv"; // CSV: epoch,temp
unsigned long lastLog = 0;
//const unsigned long logInterval = 15UL * 60UL * 1000UL; // 15 minutes in millis


// Create the radio object (Module: NSS, IRQ(DIO1), RST, BUSY)
Module myModule(LORA_SS, LORA_DIO0, LORA_RST, LORA_BUSY);
SX1262 radio = SX1262(&myModule);


// ----- Timekeeping -----
unsigned long storedEpoch = 0;    // seconds since epoch
unsigned long storedMillis = 0;   // millis() when time was set
time_t nextLog = 0;


struct tm getLocalTime() {
  //unsigned long secondsSinceSet = (millis() - storedMillis) / 1000;
  time_t tnow = now();
  struct tm t;
  localtime_r(&tnow, &t);
  return t;
}
bool isDaytime() {
  struct tm t = getLocalTime();
  int hour = t.tm_hour;
  return hour >= 8 && hour < 20;
}
String getTimeString() {
  struct tm t = getLocalTime();
  char buf[20];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d", t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min);
  return String(buf);
}

// ----- Alarm/Checkin -----
unsigned long silenceUntil = 0;
unsigned long lastWebCheckin = 0;
void loadSilence() {
  preferences.begin("probe", false);
  silenceUntil = preferences.getULong("silenceUntil", 0);
  preferences.end();
}
void setSilence(unsigned long ms) {
  silenceUntil = millis() + ms;
  preferences.begin("probe", false);
  preferences.putULong("silenceUntil", silenceUntil);
  preferences.end();
}
void loadLastWebCheckin() {
  preferences.begin("probe", false);
  lastWebCheckin = preferences.getULong("lastWebCheckin", 0);
  preferences.end();
}
void updateWebCheckin() {
  lastWebCheckin = millis();
  preferences.begin("probe", false);
  preferences.putULong("lastWebCheckin", lastWebCheckin);
  preferences.end();
}
#define DAY_MS 86400000UL
void buzzAlarm() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);
}

// ----- Node List Management -----
NodeTemp* findNodeById(const String &id) {
  for (auto &node : nodeTemps) {
    if (node.id == id) {
      return &node;  // return pointer to the entry
    }
  }
  return nullptr;  // not found
}

void loadNodeList() {
  preferences.begin("probe", false);
  nodeList = preferences.getString("nodelist", "");
  preferences.end();
  if (!nodeList.length()) nodeList = nodeID;
}
void saveNodeList(const String& list) {
  preferences.begin("probe", false);
  preferences.putString("nodelist", list);
  preferences.end();
  nodeList = list;
}
std::vector<String> getNodeIDs() {
  std::vector<String> ids;
  int start = 0;
  while (start < nodeList.length()) {
    int comma = nodeList.indexOf(',', start);
    String nid = nodeList.substring(start, comma == -1 ? nodeList.length() : comma);
    ids.push_back(nid);
    start = (comma == -1 ? nodeList.length() : comma + 1);
  }
  return ids;
}

// ----- WiFi/NodeID Config -----
String getDefaultNodeID() {
  uint8_t mac[6];
  //WiFi.macAddress(mac);
  char nodeid[7];
  sprintf(nodeid, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(nodeid);
}
void loadConfig() {
  preferences.begin("probe", false);
  nodeID = preferences.getString("nodeid", "");
  wifiSSID = preferences.getString("ssid", "");
  wifiPASS = preferences.getString("pass", "");
  preferences.end();
  if (nodeID == "" || nodeID.length() != 6) {
    nodeID = getDefaultNodeID();
    preferences.begin("probe", false);
    preferences.putString("nodeid", nodeID);
    preferences.end();
  }
  if (wifiSSID == "") {
    wifiSSID = "KIC-" + nodeID;
    preferences.begin("probe", false);
    preferences.putString("ssid", wifiSSID);
    preferences.end();
  } 
  
  if (wifiPASS == "" || wifiPASS.length() < 8) {
    wifiPASS = "KeepItCold";
    preferences.begin("probe", false);
    preferences.putString("pass", wifiPASS);
    preferences.end();
  }
}
void saveNodeID(const String& id) {
  preferences.begin("probe", false);
  preferences.putString("nodeid", id);
  String curPass = preferences.getString("pass", "");
  if (curPass == nodeID || curPass == "" || curPass.length() < 6) {
    preferences.putString("pass", id);
    wifiPASS = id;
  }
  preferences.end();
  nodeID = id;
}
void saveWiFi(const String& ssid, const String& pass) {
  preferences.begin("probe", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
  wifiSSID = ssid;
  wifiPASS = pass;
}

// ----- LoRa -----
void setLoraFlag(void) {
  loraPacketReceived = true;
}

void setupLoRa() {
  // generate encryption key
  CryptoHelper::deriveKey(loraPassphrase, loraKey);



  Serial.println("SPI begin");
  //SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
//  Serial.println("LoRa setPins");
//  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  Serial.println("LoRa begin");
  lastLoRaStatus = radio.begin(915.0);

  if (lastLoRaStatus == RADIOLIB_ERR_NONE) {
    Serial.println("LoRa init OK");
  } else {
    Serial.print("LoRa init failed: ");
    Serial.println(lastLoRaStatus);
  }
  radio.setOutputPower(13);
  
  Serial.println("LoRa setup done");
//  Serial.println(lastLoRaStatus);

  // attach call back
  radio.setDio1Action(setLoraFlag);
  int16_t state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) { 
    Serial.println("LoRa RX started");
  } else {
    Serial.print("LoRa RX failed, code ");
    Serial.println(state);
  } 
}


void broadcastNodeList() {
  String msg = "NODELIST," + nodeList;
  int16_t state = radio.transmit(msg);
  if(state == RADIOLIB_ERR_NONE){
    Serial.println("Node list sent: " + msg);
  } else {
    Serial.print("Node list failed, code: ");
    Serial.println(state);
  }
}

void broadcastAlarm(const String& downNodeID) {
  String msg = nodeID + ",ALARM," + downNodeID;
  int16_t state = radio.transmit(msg);
  if(state == RADIOLIB_ERR_NONE){
    Serial.println("Alarm sent: " + msg);
  } else {
    Serial.print("Alarm failed, code: ");
    Serial.println(state);
  }
}

void broadcastKIC() {
  NodeTemp* nt = findNodeById(nodeID);
  if (!nt) return;   // safety check

  String msg = "KIC," +
               nt->id + "," +
               String(nt->temp1, 2) + "," +
               String(nt->temp2, 2) + "," +
               String(nt->temp3, 2) + "," +
               String((unsigned long)nt->lastUpdate) + "," +
               String(nt->hasrtc ? 1 : 0);
  
  // Convert to bytes
  size_t len = msg.length();
  //size_t paddedLen = ((len + 15) / 16) * 16; // AES-CBC block padding
  uint8_t input[len];
  memcpy(input, msg.c_str(), len);

  uint8_t output[256];
  size_t outLen = 0;


  //memset(input, 0, paddedLen);

  // Encrypt
  //CryptoHelper::aesEncrypt(loraKey, loraIV, input, paddedLen, output);
  CryptoHelper::aesEncrypt(loraKey, input, len, output, outLen);

  if (outLen == 0) {
    Serial.println("Encryption failed, skipping send.");
    return;
  }

  //int16_t state = radio.transmit(msg);
  int16_t state = radio.transmit(output, outLen);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("Send NodeTemp: " + msg);
    Serial.print("Send Encrypted (hex): ");
    for (size_t i = 0; i < outLen; i++) {
      if (output[i] < 16) Serial.print("0");
      Serial.print(output[i], HEX);
    }
    Serial.println();
  } else {
    Serial.print("Send failed: ");
    Serial.println(state);
  }

  // Ensure we always go back into RX mode
  radio.startReceive();
}

void updateNodeTemp(String id, float temp, float temp2, float temp3, bool hasRTC) {
  for (auto& n : nodeTemps) {
    if (n.id == id) {
      n.temp1 = temp;
      n.temp2 = temp2;
      n.temp3 = temp3;
      n.lastUpdate = now();
      n.hasrtc = hasRTC;
      return;
    }
  }
  NodeTemp nt = {id, temp, temp2, temp3, now(), hasRTC};
  nodeTemps.push_back(nt);
}

void handleLoRaPacket(String incoming) {
  if (incoming.startsWith("NODELIST,")) {
    String newList = incoming.substring(9);
    if (nodeList != newList) {
      saveNodeList(newList);
    }
  } else if (incoming.indexOf(",TEMP,") > 0) {
    int idx1 = incoming.indexOf(",");
    int idx2 = incoming.indexOf(",TEMP,");
    String peerID = incoming.substring(0, idx1);
    float temp = incoming.substring(idx2 + 6).toFloat();
//    updateNodeTemp(peerID, temp);
  } else if (incoming.indexOf(",HEARTBEAT,") > 0) {
    int idx1 = incoming.indexOf(",");
    String peerID = incoming.substring(0, idx1);
//    updateNodeTemp(peerID, NAN); // update heartbeat timestamp only
  } else if (incoming.indexOf(",ALARM,") > 0) {
    // Optionally handle remote alarms
  } else if (incoming.startsWith("KIC,")) {
    // parse node temp struct
    int idx1 = incoming.indexOf(",", 4);
    if (idx1 < 0) return;
    int idx2 = incoming.indexOf(",", idx1 + 1);
    if (idx2 < 0) return;
    int idx3 = incoming.indexOf(",", idx2 + 1);
    if (idx3 < 0) return;
    int idx4 = incoming.indexOf(",", idx3 + 1);
    if (idx4 < 0) return;
    int idx5 = incoming.indexOf(",", idx4 + 1);
    if (idx5 < 0) return;
    String peerID = incoming.substring(4, idx1);
    float temp1 = incoming.substring(idx1 + 1, idx2).toFloat();
    float temp2 = incoming.substring(idx2 + 1, idx3).toFloat();
    float temp3 = incoming.substring(idx3 + 1, idx4).toFloat();
    unsigned long lu = incoming.substring(idx4 + 1, idx5).toInt();
    bool hasrtc = incoming.substring(idx5 + 1).toInt() != 0;
    // ignoe the local node for updateing data
    if (peerID == nodeID) {
      Serial.println("Ignoring my own KIC msg");
      return;
    }
    // update existing node entry
    for (auto& n : nodeTemps) {
      if (n.id == peerID) {
        n.temp1 = temp1;
        n.temp2 = temp2;
        n.temp3 = temp3;
        n.lastUpdate = lu;
        n.hasrtc = hasrtc;
        return;
      }
    }
    // creata new node entry
    NodeTemp nt = {peerID, temp1, temp2, temp3, lu, hasrtc};
    nodeTemps.push_back(nt);

    // if a remote node has RTC and we don't, update time sync
    if (peerID != nodeID && hasrtc && !doIhaveRTC && needTime) {
      // update local time from lu
      Serial.println("Updating local time from " + peerID + " to " + String(lu));
      setTime((time_t)lu);

      needTime = false;
    }
  } else {
    Serial.println("Unknown LoRa msg: " + incoming);
  }
}

// ----- OLED Display -----
void showOLED() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("Node: "); display.println(nodeID);
  display.print("Temp: "); display.print(myTemp,1); display.println(" C");
  display.print("WiFi: "); display.println(wifiSSID);
  display.print("PASS: "); display.println(wifiPASS);
  display.print(getTimeString()); display.println();
  int y = 56;
  for (auto& n : nodeTemps) {
    display.setCursor(0, y);
    display.print(n.id); display.print(": ");
    if (!isnan(n.temp1)) display.print(n.temp1,1); else display.print("-");
    display.print("C");
    y -= 8; if (y < 40) break;
  }
  display.display();
}

// ----- Web Server -----
void WebServerRoot(AsyncWebServerRequest *request){
    //String newID = request->getParam("nodeid", true)->value();
    updateWebCheckin();
    String html = "<h2>Keep It Cold Node</h2>";
    html += "<p>NodeID: <b>" + nodeID + "</b></p>";
    html += "<p>Temperature: <b>" + String(myTemp, 2) + " C</b></p>";
    html += "<p>WiFi SSID: <b>" + wifiSSID + "</b> PASS: <b>" + wifiPASS + "</b></p>";
    html += "<p>System Time: <b>" + getTimeString() + "</b></p>";
    html += "<form method='POST' action='/setnodeid'>NodeID: <input name='nodeid' value='" + nodeID + "' maxlength='6'><button type='submit'>Set NodeID</button></form>";
    html += "<form method='POST' action='/setwifi'>WiFi SSID: <input name='ssid' value='" + wifiSSID + "'> PASS: <input name='pass' value='" + wifiPASS + "'><button type='submit'>Set WiFi</button></form>";
    html += "<form method='POST' action='/silence'><button type='submit'>Silence Alarms (1h)</button></form>";
    html += "<form method='POST' action='/settime'>Year: <input name='year' size='4'> Month: <input name='month' size='2'> Day: <input name='day' size='2'> Hour: <input name='hour' size='2'> Min: <input name='min' size='2'><button type='submit'>Set Time</button></form>";
    // Node List
    html += "<h3>Node List</h3><ul>";
    for (auto& nid : getNodeIDs()) html += "<li>" + nid + "</li>";
    html += "</ul><form method='POST' action='/addnode'>Add NodeID: <input name='newnode' maxlength='6'><button type='submit'>Add</button></form>";
    // Temps
    html += "<h3>Node Temperatures</h3><ul>";
    for (auto& n : nodeTemps) {
      html += "<li>" + n.id + ": " + (isnan(n.temp1) ? String("-") : String(n.temp1,2)) + " C</li>";
    }
    html += "</ul>";
    html += "<p>REST API: <a href='/api/temps'>/api/temps</a></p>";
    html += "<p>Log File (CSV): <a href='/log'>/log</a></p>";
  

    //String html = "<!DOCTYPE html><html><head><title>Keep It Cold Node</title></head><body>";
    //html += "<h2>Keep It Cold Node</h2>";
    request->send(200, "text/html", html);
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/brr");
  });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request){
    String logfile = "/templog.csv";
    if (LittleFS.exists(logFile)) {
      File file = LittleFS.open(logFile, "r");
      if (file) {
        String content = file.readString();
        file.close();
        request->send(200, "text/csv", content);
        return;
      }
    }
    request->send(404, "text/plain", "Log file not found");
  });


  server.on("/brr", HTTP_GET, WebServerRoot);

  server.on("/setnodeid", HTTP_POST, [](AsyncWebServerRequest *request){
    String newID = request->getParam("nodeid", true)->value();
    if (newID.length() == 6) {
      saveNodeID(newID);
      request->redirect("/");
      return;
    }
    request->send(400, "text/plain", "Invalid NodeID");
  });

  server.on("/setwifi", HTTP_POST, [](AsyncWebServerRequest *request){
    String ssid = request->getParam("ssid", true)->value();
    String pass = request->getParam("pass", true)->value();
    saveWiFi(ssid, pass);
    request->redirect("/");
    delay(1000);
    ESP.restart();
  });

  server.on("/silence", HTTP_POST, [](AsyncWebServerRequest *request){
    setSilence(3600000UL); // 1 hour
    request->redirect("/");
  });

  server.on("/settime", HTTP_POST, [](AsyncWebServerRequest *request){
    int year = request->getParam("year", true)->value().toInt();
    int month = request->getParam("month", true)->value().toInt();
    int day = request->getParam("day", true)->value().toInt();
    int hour = request->getParam("hour", true)->value().toInt();
    int min = request->getParam("min", true)->value().toInt();
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = 0;
    time_t epoch = mktime(&t);
//    saveTime(epoch);
    request->redirect("/");
  });

  server.on("/addnode", HTTP_POST, [](AsyncWebServerRequest *request){
    String newnode = request->getParam("newnode", true)->value();
    if (newnode.length() == 6 && nodeList.indexOf(newnode) == -1) {
      nodeList += "," + newnode;
      saveNodeList(nodeList);
//      broadcastNodeList();
    }
    request->redirect("/");
  });

  server.on("/api/temps", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    for (size_t i = 0; i < nodeTemps.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"id\":\"" + nodeTemps[i].id + "\",\"temp\":" + String(nodeTemps[i].temp1,2) + "}";
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // critical for captave portal to work
  // redirect all not-found to /brr
  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("/brr");
  });

  server.begin();
}

//void bindServerCallback() {
//  // wifiManager.server is a pointer to WebServer
//  wifiManager.server->on("/", []() {
//    //wifiManager.server->send(200, "text/html", "<h1>My Root Page</h1>");
//    wifiManager.server->sendHeader("Location", "/brr");
//    wifiManager.server->send(301);
//  });
//}

// ----- Temperature Logging -----
void setupLogFile() {
  Serial.println("Mounting LittleFS...");
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed");
    while(1);
  }
  if (!LittleFS.exists(logFile)) {
    File file = LittleFS.open(logFile, "w");
    if (file) {
      file.println("epoch,node,temp1,temp2,temp3");
      file.close();
    }
  }
}

String timeAsYMDHMS(time_t t) {
  char buf[25];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
           month(t), day(t), year(t),
           hour(t), minute(t), second(t));
  return String(buf);
}

time_t nextLogEpoch() {
  time_t t = now();  // current epoch in seconds
  
  struct tm tmNext;
  localtime_r(&t, &tmNext);

  int currentMin = tmNext.tm_min;

  // Find the next quarter-hour (0, 15, 30, 45)
  int nextQuarter = ((currentMin / 15) + 1) * 15;
  if(nextQuarter >= 60) {
    tmNext.tm_hour += 1;   // bump hour
    tmNext.tm_min = 0;     // reset minutes
  } else {
    tmNext.tm_min = nextQuarter;
  }
  tmNext.tm_sec = 0;       // always align on the minute

  // mktime() normalizes the struct (handles day, month, year rollover)
  time_t nextEpoch = mktime(&tmNext);

  // In case we're exactly on a boundary but seconds > 0, skip to the next
  if(nextEpoch <= t) {
    nextEpoch += 15 * 60;
  }

  return nextEpoch;
}

void logloop() {
  time_t t = now();

  // get t into month/day/year hour:min:sec format
  String tstamp = timeAsYMDHMS(t);

  if(nextLog == 0) {
    nextLog = nextLogEpoch();
    Serial.println("Next log at epoch: " + String(nextLog) + " (" + tstamp + ")");
  }

  if(t >= nextLog) {
    Serial.println("Logging temperature at epoch: " + String(t) + " (" + tstamp + ")");  
    // Read temperature
    sensors.requestTemperatures();
    float temp = sensors.getTempCByIndex(0);
    if(temp == DEVICE_DISCONNECTED_C) temp = NAN;

    // Append to LittleFS CSV
    File f = LittleFS.open("/templog.csv", FILE_APPEND);
    if(f){
      // fora all the node data we have in nodeTemps
      for (auto& n : nodeTemps) {
        if (n.id == nodeID) {
          // use myTemp for self entry
          f.printf("%s,%s,%.2f,%.2f,%.2f\n", tstamp, n.id.c_str(), temp, n.temp2, n.temp3);
        } else {
          // use stored temps for other nodes
          f.printf("%s,%s,%.2f,%.2f,%.2f\n", tstamp, n.id.c_str(), n.temp1, n.temp2, n.temp3);
        }
      }
      f.close();
      Serial.printf("Logged: %02d:%02d -> %.2f\n", hour(t), minute(t), temp);
    }

    // Schedule next log
    nextLog = nextLogEpoch(); // next quarter-hour
    Serial.println("Next log at epoch: " + String(nextLog) + " (" + String(year(nextLog)) + "-" + String(month(nextLog)) + "-" + String(day(nextLog)) + " " + String(hour(nextLog)) + ":" + String(minute(nextLog)) + ":" + String(second(nextLog)) + ")");
  }
}

// ----- Setup & Main Loop -----
void setup() {
  Serial.begin(115200);
  Serial.println("Keep It Cold Node Starting...");

  // OLED power control (Heltec Vext pin)
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  // Heltec delays for 100ms in their example
  delay(100);


  twi.begin(SDA_OLED, SCL_OLED);
  
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED display not found!"));
    while (true); // Stop here if display is not found
  }
  //display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  //display.begin();
  display.clearDisplay();
  display.setTextSize(1);
  //display.setTextColor(SSD1306_WHITE);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("Keep It Cold");
  display.display();

  // setup rtc
  Wire.begin(42,41);
  if (rtc.getSecond()> 60){
    Serial.println("Couldn't find RTC");
    Serial.println("we should ask one of the nodes for the time");
    needTime = true;
  } else {
    doIhaveRTC = true;
    bool h12, pm;
    bool century = false;
    setTime(
      rtc.getHour(h12, pm),
      rtc.getMinute(),
      rtc.getSecond(),
      rtc.getDate(),
      rtc.getMonth(century),
      2000 + rtc.getYear()
    );
  }
  time_t epoch = now();
  Serial.print("year: "); Serial.println(year(epoch));
  Serial.print("month: "); Serial.println(month(epoch));
  Serial.print("day: "); Serial.println(day(epoch));
  Serial.print("hour: "); Serial.println(hour(epoch));
  Serial.print("min: "); Serial.println(minute(epoch));
  Serial.print("sec: "); Serial.println(second(epoch));

  loadConfig();
  loadNodeList();
  loadSilence();
  loadLastWebCheckin();

  Serial.println("NodeID: " + nodeID);
  Serial.println("WiFi SSID: " + wifiSSID + " PASS: " + wifiPASS);
  Serial.println("Node List: " + nodeList);
  Serial.println("Stored Time: " + getTimeString());
  Serial.println("Silence Until: " + String(silenceUntil) + " Last Web Checkin: " + String(lastWebCheckin));
  showOLED();

  Serial.println("Starting WiFi AP...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(wifiSSID.c_str(), wifiPASS.c_str());
  //WiFi.softAP(wifiSSID.c_str());
  delay(1000); // Wait for AP to start
  Serial.println("AP IP address: " + WiFi.softAPIP().toString());

  Serial.println("Starting LoRa...");
  setupLoRa();
  Serial.println("Starting sensors...");
  sensors.begin();
  Serial.println("Starting web server...");
  setupWebServer();

  Serial.println("Starting DNS server...");
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Mount LittleFS
  setupLogFile();

  Serial.println("Update own temp...");
  updateNodeTemp(nodeID, NAN, NAN, NAN, doIhaveRTC); // Add self to nodeTemps

  Serial.println("Setup complete.");
}

unsigned long lastSend = 0, lastRead = 0, lastHeartbeat = 0;

void radioloop() {
  if (loraPacketReceived) {
    loraPacketReceived = false;
    uint8_t incoming[256];
    int16_t len = radio.getPacketLength();
    int16_t state = radio.readData(incoming, len);

    if (state == RADIOLIB_ERR_NONE) {
      //size_t len = sizeof(incoming);
      // packet received successfully
      //Serial.println("Received: " + incoming);
      Serial.println("Receive LoRa packet");
      Serial.print("Receive Raw bytes: ");
      for (int i = 0; i < len; i++) {
        Serial.printf("%02X ", incoming[i]);
      }
      Serial.println();

      //decrypt
      uint8_t decrypted[256];
      //CryptoHelper::aesDecrypt(loraKey, loraIV, incoming, len, decrypted);
      CryptoHelper::aesDecrypt(loraKey, incoming, len, decrypted, decLen);

      // Convert to String using known length
      String msg = "";
      unsigned int dlen = sizeof(decrypted);
      for (int i = 0; i < dlen; i++) {
        if (decrypted[i] == 0) break; // stop at null if there is one
        msg += (char)decrypted[i];
      }

      Serial.println("Receive Decrypted msg: " + msg);
      Serial.println("Receive length: " + String(msg.length()));
      handleLoRaPacket(msg);
    } else {
      Serial.print("Receive failed, code: ");
      Serial.println(state);
    }
    // start listening again
    int16_t state2 = radio.startReceive();
    if (state2 == RADIOLIB_ERR_NONE) { 
      //Serial.println("LoRa RX started");
    } else {
      Serial.print("Receive LoRa RX failed, code ");
      Serial.println(state2);
    } 
  }


  // send struct every ~ 30s
  int randomDelay = random(0,5000);
  if (millis() - lastSend > 30000+randomDelay) {
    Serial.println("Sending Broadcasting struct...");
    //broadcastStruct();
    broadcastKIC();
    lastSend = millis();
  }
//  // Send LoRa temp every 10s
//  if (millis() - lastSend > 10000) {
//    Serial.println("Broadcasting temp...");
//    broadcastTemperature(myTemp);
//    lastSend = millis();
//  }

//  // Heartbeat every 15s
//  if (millis() - lastHeartbeat > 15000) {
//    Serial.println("Broadcasting heartbeat...");
//    broadcastHeartbeat();
//    lastHeartbeat = millis();
//  }
}

void processSerialCommands() {
  // Serial config (for debugging/config)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("SETNODEID:") && cmd.length() == 15) {
      String newID = cmd.substring(10, 16);
      saveNodeID(newID);
      Serial.println("NodeID updated to: " + nodeID);
    }
    if (cmd.startsWith("SETWIFI:")) {
      int sep = cmd.indexOf(',',8);
      if (sep > 8) {
        String ssid = cmd.substring(8, sep);
        String pass = cmd.substring(sep+1);
        saveWiFi(ssid, pass);
        Serial.println("WiFi updated, rebooting...");
        delay(1000);
        ESP.restart();
      }
    }
    if (cmd.startsWith("SETTIME:")) {
      // SETTIME:YYYY,MM,DD,HH,mm
      int y = cmd.substring(8,12).toInt();
      int m = cmd.substring(13,15).toInt();
      int d = cmd.substring(16,18).toInt();
      int h = cmd.substring(19,21).toInt();
      int mi = cmd.substring(22,24).toInt();
      struct tm t = {0};
      t.tm_year = y-1900; t.tm_mon = m-1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = 0;
      time_t epoch = mktime(&t);
//      saveTime(epoch);
      Serial.println("Time updated: " + String(epoch));
    }
  }

}


void loop() {
  dnsServer.processNextRequest();
  processSerialCommands();

  bool tempprobedisconnected = false;
  // Read DS18B20 every 5s
  if (millis() - lastRead > 5000) {
    sensors.requestTemperatures();
    myTemp = sensors.getTempCByIndex(0);
    if (myTemp == DEVICE_DISCONNECTED_C) {
      myTemp = NAN;
      tempprobedisconnected = true;
    }
    updateNodeTemp(nodeID, myTemp, NAN, NAN, doIhaveRTC);
    lastRead = millis();
    showOLED();
  }

  radioloop();
  logloop();

  // Node-down and checkin alarms
  bool silenceActive = millis() < silenceUntil;
  bool noWebCheckin = (millis() - lastWebCheckin) > DAY_MS;
  std::vector<String> ids = getNodeIDs();
  for (auto& nid : ids) {
    if (nid == nodeID) continue;
    bool found = false;
    for (auto& n : nodeTemps) {
      if (n.id == nid && (now() - n.lastUpdate < 300)) found = true;
    }
    if (!found && !silenceActive) {
      display.clearDisplay();
      display.setCursor(0,0);
      display.println("ALARM! Node Down:");
      display.println(nid);
      if (isDaytime()) buzzAlarm();
    }
  }
  // Temp probe disconnected alarm
  if (tempprobedisconnected && !silenceActive) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("ALARM! Temp Probe");
    display.println("Disconnected!");
    if (isDaytime()) buzzAlarm();
  }
/*
  if (!silenceActive && noWebCheckin) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("CHECKIN ALARM!");
    display.println("Connect to WiFi: " + wifiSSID);
    display.display();
    if (isDaytime()) buzzAlarm();
  }
*/
}
