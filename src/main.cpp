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
#include <Preferences.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <vector>

// ----- Pin Definitions -----
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_RESET -1
#define DS18B20_PIN 17
#define BUZZER_PIN 32

#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_SS    18
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_FREQ  915E6 // adjust for region

// ----- Hardware -----
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET);
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
Preferences preferences;
AsyncWebServer server(80);

// ----- Node Data -----
struct NodeTemp {
  String id;
  float temp;
  unsigned long lastUpdate;
};
std::vector<NodeTemp> nodeTemps;
String nodeID;
String nodeList; // Comma-separated
String wifiSSID = "ESP32Probe";
String wifiPASS = "";
float myTemp = NAN;
String lastLoRaStatus = "OK";

// ----- Timekeeping -----
unsigned long storedEpoch = 0;    // seconds since epoch
unsigned long storedMillis = 0;   // millis() when time was set
void loadTime() {
  preferences.begin("probe", false);
  storedEpoch = preferences.getULong("epoch", 0);
  storedMillis = preferences.getULong("epochMillis", 0);
  preferences.end();
}
void saveTime(unsigned long epoch) {
  preferences.begin("probe", false);
  preferences.putULong("epoch", epoch);
  preferences.putULong("epochMillis", millis());
  preferences.end();
  storedEpoch = epoch;
  storedMillis = millis();
}
struct tm getLocalTime() {
  unsigned long secondsSinceSet = (millis() - storedMillis) / 1000;
  time_t now = storedEpoch + secondsSinceSet;
  struct tm t;
  localtime_r(&now, &t);
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
  WiFi.macAddress(mac);
  char nodeid[7];
  sprintf(nodeid, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(nodeid);
}
void loadConfig() {
  preferences.begin("probe", false);
  nodeID = preferences.getString("nodeid", "");
  wifiSSID = preferences.getString("ssid", wifiSSID);
  wifiPASS = preferences.getString("pass", "");
  preferences.end();
  if (nodeID == "" || nodeID.length() != 6) {
    nodeID = getDefaultNodeID();
    preferences.begin("probe", false);
    preferences.putString("nodeid", nodeID);
    preferences.end();
  }
  if (wifiPASS == "" || wifiPASS.length() < 6) {
    wifiPASS = nodeID;
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
void setupLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    lastLoRaStatus = "LoRa init failed";
  } else {
    lastLoRaStatus = "LoRa OK";
  }
}
void broadcastHeartbeat() {
  LoRa.beginPacket();
  LoRa.print(nodeID);
  LoRa.print(",HEARTBEAT,");
  LoRa.print((millis()/1000));
  LoRa.endPacket();
}
void broadcastTemperature(float temp) {
  LoRa.beginPacket();
  LoRa.print(nodeID);
  LoRa.print(",TEMP,");
  LoRa.print(temp, 2);
  LoRa.endPacket();
}
void broadcastNodeList() {
  LoRa.beginPacket();
  LoRa.print("NODELIST,");
  LoRa.print(nodeList);
  LoRa.endPacket();
}
void broadcastAlarm(const String& downNodeID) {
  LoRa.beginPacket();
  LoRa.print(nodeID);
  LoRa.print(",ALARM,");
  LoRa.print(downNodeID);
  LoRa.endPacket();
}
void updateNodeTemp(String id, float temp) {
  unsigned long now = millis();
  for (auto& n : nodeTemps) {
    if (n.id == id) {
      n.temp = temp;
      n.lastUpdate = now;
      return;
    }
  }
  NodeTemp nt = {id, temp, now};
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
    updateNodeTemp(peerID, temp);
  } else if (incoming.indexOf(",HEARTBEAT,") > 0) {
    int idx1 = incoming.indexOf(",");
    String peerID = incoming.substring(0, idx1);
    updateNodeTemp(peerID, NAN); // update heartbeat timestamp only
  } else if (incoming.indexOf(",ALARM,") > 0) {
    // Optionally handle remote alarms
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
    if (!isnan(n.temp)) display.print(n.temp,1); else display.print("-");
    display.print("C");
    y -= 8; if (y < 40) break;
  }
  display.display();
}

// ----- Web Server -----
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
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
      html += "<li>" + n.id + ": " + (isnan(n.temp) ? String("-") : String(n.temp,2)) + " C</li>";
    }
    html += "</ul>";
    html += "<p>REST API: <a href='/api/temps'>/api/temps</a></p>";
    request->send(200, "text/html", html);
  });

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
    saveTime(epoch);
    request->redirect("/");
  });

  server.on("/addnode", HTTP_POST, [](AsyncWebServerRequest *request){
    String newnode = request->getParam("newnode", true)->value();
    if (newnode.length() == 6 && nodeList.indexOf(newnode) == -1) {
      nodeList += "," + newnode;
      saveNodeList(nodeList);
      broadcastNodeList();
    }
    request->redirect("/");
  });

  server.on("/api/temps", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    for (size_t i = 0; i < nodeTemps.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"id\":\"" + nodeTemps[i].id + "\",\"temp\":" + String(nodeTemps[i].temp,2) + "}";
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  server.begin();
}

// ----- Setup & Main Loop -----
void setup() {
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  Serial.begin(115200);
  loadConfig();
  loadNodeList();
  loadTime();
  loadSilence();
  loadLastWebCheckin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) { delay(500); tries++; }

  setupLoRa();
  sensors.begin();
  setupWebServer();

  updateNodeTemp(nodeID, NAN); // Add self to nodeTemps
}

unsigned long lastSend = 0, lastRead = 0, lastHeartbeat = 0;

void loop() {
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
      saveTime(epoch);
      Serial.println("Time updated: " + String(epoch));
    }
  }

  // Read DS18B20 every 5s
  if (millis() - lastRead > 5000) {
    sensors.requestTemperatures();
    myTemp = sensors.getTempCByIndex(0);
    updateNodeTemp(nodeID, myTemp);
    lastRead = millis();
    showOLED();
  }

  // Send LoRa temp every 10s
  if (millis() - lastSend > 10000) {
    broadcastTemperature(myTemp);
    lastSend = millis();
  }
  // Heartbeat every 15s
  if (millis() - lastHeartbeat > 15000) {
    broadcastHeartbeat();
    lastHeartbeat = millis();
  }

  // Listen for LoRa packets
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available())
      incoming += (char)LoRa.read();
    handleLoRaPacket(incoming);
  }

  // Node-down and checkin alarms
  bool silenceActive = millis() < silenceUntil;
  bool noWebCheckin = (millis() - lastWebCheckin) > DAY_MS;
  std::vector<String> ids = getNodeIDs();
  for (auto& nid : ids) {
    if (nid == nodeID) continue;
    bool found = false;
    for (auto& n : nodeTemps) {
      if (n.id == nid && (millis() - n.lastUpdate < 30000)) found = true;
    }
    if (!found && !silenceActive) {
      display.clearDisplay();
      display.setCursor(0,0);
      display.println("ALARM! Node Down:");
      display.println(nid);
      if (isDaytime()) buzzAlarm();
    }
  }
  if (!silenceActive && noWebCheckin) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("CHECKIN ALARM!");
    display.display();
    if (isDaytime()) buzzAlarm();
  }
}
