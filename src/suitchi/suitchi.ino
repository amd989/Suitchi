/*
   suitchi.ino

    Created on: 2020-12-01
        Author: amd989 (Alejandro Mora)

   This code represents a bridge (aka gateway) which contains multiple accessories.

   This includes 6 sensors:
   1. Temperature Sensor (HAP section 8.41)
   2. Humidity Sensor (HAP section 8.20)
   4. Motion Sensor (HAP section 8.28)
   5. Occupancy Sensor (HAP section 8.29)
   6. Switch (HAP section 8.38)

   You should:
   1. erase the full flash or call homekit_storage_reset() in setup()
      to remove the previous HomeKit pairing storage and
      enable the pairing with the new accessory of this new HomeKit example.
*/

#include <Arduino.h>
#include <arduino_homekit_server.h>
#include "wifi_info.h"
#include "custom_fonts.h"
#include "ESPButton.h"
#include "Constants.h"

#include "SSD1306Wire.h"

#include <DHT.h>
#include <ArduinoOTA.h>

#include <EEPROM.h>
#ifdef MQTT_ENABLED
#include <PubSubClient.h>
#endif
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

#define LOG_D(fmt, ...)   printf_P(PSTR(fmt "\n") , ##__VA_ARGS__);

//////////////////////////
// EEPROM relay state persistence
// Byte 0: magic marker (0xA5 = valid data written)
// Byte 1: relay state (0 or 1)
#define EEPROM_SIZE       4
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_RELAY_ADDR 1
#define EEPROM_MAGIC      0xA5

// Heap threshold: if free heap drops below this, do a controlled restart
#define LOW_HEAP_THRESHOLD 4096

void save_relay_state(bool on) {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  EEPROM.write(EEPROM_RELAY_ADDR, on ? 1 : 0);
  EEPROM.commit();
}

bool load_relay_state() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC) {
    return false; // no saved state, default to off
  }
  return EEPROM.read(EEPROM_RELAY_ADDR) == 1;
}


//////////////////////////
// Setting up the pin and DHT version
#define DHTTYPE DHT11   // DHT Shield uses DHT 11

#define RELAYPIN D8     // Relay Shield uses pin D8
#define OCCUPANCYPIN D3 // Occupancy uses pin D3
#define DHTPIN D4       // DHT Shield uses pin D4
#define SWITCHPIN D7    // Relay Shield uses pin D7

// Initialize the OLED display using Arduino Wire:
SSD1306Wire display(0x3c, SDA, SCL, GEOMETRY_64_48 );  // ADDRESS, SDA, SCL
DHT dht(DHTPIN, DHTTYPE);

// Listen for HTTP requests on standard port 80
ESP8266WebServer server(80);

#ifdef MQTT_ENABLED
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
static uint32_t last_mqtt_millis = 0;
static uint32_t last_mqtt_reconnect_millis = 0;
// Forward declarations
void mqtt_on_message(char* topic, uint8_t* payload, unsigned int length);
void set_switch(bool on);

// State topic helpers
void mqtt_state_topic(char* buf, size_t len, const char* suffix) {
  snprintf(buf, len, "%s/%s", MQTT_TOPIC_PREFIX, suffix);
}

void mqtt_publish(const char* subtopic, const char* payload, bool retained = false) {
  if (!mqttClient.connected()) return;
  char topic[80];
  mqtt_state_topic(topic, sizeof(topic), subtopic);
  mqttClient.publish(topic, payload, retained);
}

// Publish a single HA discovery config message.
// Reuses a shared buffer to keep stack usage low.
void mqtt_publish_discovery(const char* component, const char* object_id,
                            const char* name, const char* state_suffix,
                            const char* device_class, const char* unit,
                            const char* extra_json) {
  char disc_topic[128];
  snprintf(disc_topic, sizeof(disc_topic),
    "homeassistant/%s/%s/%s/config", component, MQTT_NODE_ID, object_id);

  char state_topic[80];
  mqtt_state_topic(state_topic, sizeof(state_topic), state_suffix);

  char avail_topic[80];
  mqtt_state_topic(avail_topic, sizeof(avail_topic), "status");

  char payload[512];
  int len = snprintf(payload, sizeof(payload),
    "{"
    "\"name\":\"%s\","
    "\"unique_id\":\"%s_%s\","
    "\"state_topic\":\"%s\","
    "\"availability_topic\":\"%s\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\","
    "\"device\":{"
      "\"identifiers\":[\"%s\"],"
      "\"name\":\"%s\","
      "\"manufacturer\":\"Suitchi\","
      "\"model\":\"ESP8266\","
      "\"sw_version\":\"2.0\""
    "}",
    name, MQTT_NODE_ID, object_id,
    state_topic,
    avail_topic,
    MQTT_NODE_ID, bridgeName);

  if (device_class && strlen(device_class) > 0) {
    len += snprintf(payload + len, sizeof(payload) - len,
      ",\"device_class\":\"%s\"", device_class);
  }
  if (unit && strlen(unit) > 0) {
    len += snprintf(payload + len, sizeof(payload) - len,
      ",\"unit_of_measurement\":\"%s\"", unit);
  }
  if (extra_json && strlen(extra_json) > 0) {
    len += snprintf(payload + len, sizeof(payload) - len, ",%s", extra_json);
  }
  snprintf(payload + len, sizeof(payload) - len, "}");

  mqttClient.publish(disc_topic, payload, true);
  yield(); // let WiFi breathe between discovery messages
}

void mqtt_send_discovery() {
  LOG_D("MQTT sending HA discovery configs...");

  // Temperature sensor
  mqtt_publish_discovery("sensor", "temperature",
    "Temperature", "temperature",
    "temperature", "\u00b0C", NULL);

  // Humidity sensor
  mqtt_publish_discovery("sensor", "humidity",
    "Humidity", "humidity",
    "humidity", "%", NULL);

  // Heat index sensor
  mqtt_publish_discovery("sensor", "heat_index",
    "Heat Index", "heat_index",
    "temperature", "\u00b0C", NULL);

  // Motion binary sensor
  mqtt_publish_discovery("binary_sensor", "motion",
    "Motion", "motion",
    "motion", NULL,
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"");

  // Occupancy binary sensor
  mqtt_publish_discovery("binary_sensor", "occupancy",
    "Occupancy", "occupancy",
    "occupancy", NULL,
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"");

  // Switch with command topic for HA control
  char cmd_topic[80];
  mqtt_state_topic(cmd_topic, sizeof(cmd_topic), "switch/set");
  char switch_extra[160];
  snprintf(switch_extra, sizeof(switch_extra),
    "\"command_topic\":\"%s\","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
    "\"state_on\":\"ON\",\"state_off\":\"OFF\"",
    cmd_topic);
  mqtt_publish_discovery("switch", "relay",
    "Switch", "switch",
    "switch", NULL, switch_extra);

  // Diagnostic sensors
  mqtt_publish_discovery("sensor", "heap",
    "Free Heap", "heap",
    NULL, "B",
    "\"entity_category\":\"diagnostic\",\"icon\":\"mdi:memory\"");

  mqtt_publish_discovery("sensor", "uptime",
    "Uptime", "uptime",
    "duration", "s",
    "\"entity_category\":\"diagnostic\"");

  mqtt_publish_discovery("sensor", "reset_reason",
    "Reset Reason", "reset_reason",
    NULL, NULL,
    "\"entity_category\":\"diagnostic\",\"icon\":\"mdi:restart-alert\"");

  LOG_D("MQTT discovery configs sent");
}

void mqtt_reconnect() {
  if (mqttClient.connected()) return;
  const uint32_t t = millis();
  if (t - last_mqtt_reconnect_millis < 5000) return;
  last_mqtt_reconnect_millis = t;

  LOG_D("MQTT connecting to %s:%d...", MQTT_SERVER, MQTT_PORT);

  // LWT: broker publishes "offline" to status topic when we disconnect
  char willTopic[80];
  mqtt_state_topic(willTopic, sizeof(willTopic), "status");

  bool connected;
  if (strlen(MQTT_USER) > 0) {
    connected = mqttClient.connect(otaName, MQTT_USER, MQTT_PASS, willTopic, 0, true, "offline");
  } else {
    connected = mqttClient.connect(otaName, willTopic, 0, true, "offline");
  }

  if (connected) {
    LOG_D("MQTT connected");

    // Subscribe to switch command topic
    char cmd_topic[80];
    mqtt_state_topic(cmd_topic, sizeof(cmd_topic), "switch/set");
    mqttClient.subscribe(cmd_topic);

    // Publish availability
    mqtt_publish("status", "online", true);

    // Send discovery configs (re-send on every reconnect to handle HA restarts)
    mqtt_send_discovery();

    // Publish reset reason
    mqtt_publish("reset_reason", ESP.getResetReason().c_str(), true);
  } else {
    LOG_D("MQTT connection failed, state: %d", mqttClient.state());
  }
}

void mqtt_on_message(char* topic, uint8_t* payload, unsigned int length) {
  // Only one subscription: switch/set command
  char cmd[8];
  unsigned int copy_len = length < sizeof(cmd) - 1 ? length : sizeof(cmd) - 1;
  memcpy(cmd, payload, copy_len);
  cmd[copy_len] = '\0';

  if (strcmp(cmd, "ON") == 0) {
    set_switch(true);
  } else if (strcmp(cmd, "OFF") == 0) {
    set_switch(false);
  }
}

void mqtt_setup() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(512); // discovery payloads are large
  mqttClient.setCallback(mqtt_on_message);
  mqtt_reconnect();
}

void mqtt_publish_state() {
  char buf[16];

  // Sensor values
  if (!(isnan(temperature) || isnan(humidity))) {
    dtostrf(temperature, 1, 1, buf);
    mqtt_publish("temperature", buf, true);
    dtostrf(humidity, 1, 1, buf);
    mqtt_publish("humidity", buf, true);
    dtostrf(hindex, 1, 1, buf);
    mqtt_publish("heat_index", buf, true);
  }

  // Switch state
  mqtt_publish("switch", cha_switch_on.value.bool_value ? "ON" : "OFF", true);

  // Motion & occupancy
  mqtt_publish("motion", motion ? "ON" : "OFF", true);
  mqtt_publish("occupancy", occupancy ? "ON" : "OFF", true);

  // Diagnostics
  snprintf(buf, sizeof(buf), "%d", ESP.getFreeHeap());
  mqtt_publish("heap", buf);

  snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
  mqtt_publish("uptime", buf);
}

void mqtt_loop() {
  mqtt_reconnect();
  mqttClient.loop();
  const uint32_t t = millis();
  if (t - last_mqtt_millis >= MQTT_PUBLISH_INTERVAL) {
    last_mqtt_millis = t;
    mqtt_publish_state();
  }
}

void mqtt_publish_event(const char* subtopic, const char* payload) {
  mqtt_publish(subtopic, payload, true);
}
#endif

//Sensor variables
float humidity, temperature, hindex;                         // Raw float values from the sensor
char str_humidity[10], str_temperature[10], str_hindex[10];  // Rounded sensor values as strings

bool occupancy = false, motion = false;
int motionState = 0;         // current state of the motion sensor
int lastMotionState = 0;     // previous state of the motion sensor
bool displayDirty = true;    // flag to redraw OLED only when state changes

// Generally, you should use "unsigned long" for variables that hold time
unsigned long previousMillis = 0;            // When the sensor was last read
const long interval = 2000;                  // Wait this long until reading again

//Sets up the initial page
void handle_root() {
  server.send(200, "text/plain", "Welcome to Suitchi Web Server.");
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting...");
  Serial.printf("Reset reason: %s\n", ESP.getResetReason().c_str());

  EEPROM.begin(EEPROM_SIZE);

  Serial.println("Setup DHT...");
  dht.begin();

  Serial.println("Setup UI...");
  // Initialising the UI will init the display too.
  display.init();
  display.flipScreenVertically();
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
  drawFrame0(&display, 0, 0);

  Serial.println("Setup WiFi...");
  wifi_connect(); // in wifi_info.h

  Serial.println("Setup OTA...");
  // For OTA - Use your own device identifying name (in Constants.h)
  ArduinoOTA.setHostname(otaName);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2 - 10, "OTA Update");
    display.display();
  });

  ArduinoOTA.begin();

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    display.drawProgressBar(0, 32, 63, 8, progress / (total / 100) );
    display.display();
  });

  ArduinoOTA.onEnd([]() {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "Restart");
    display.display();
  });

  Serial.println("Setup Web Server...");
  // Handle http request display root
  server.on("/", HTTP_GET, handle_root);

  // Handle http requests display temp+hum value
  server.on("/TH", []() {
    read_sensor();
    char response[50];
    snprintf(response, 50, "{ \"Temperature\": %s, \"Humidity\" : %s }", str_temperature, str_humidity);
    server.send(200, "application/json", response);
  });

  // Handle http requests display temp+hum value
  server.on("/motion", []() {
    char response[55];
    snprintf(response, 55, "{ \"Motion\": %s, \"Occupancy\" : %s }", motion ? "1" : "0", occupancy ? "1" : "0");
    server.send(200, "application/json", response);
  });

  // Handle http requests display status info
  server.on("/status", []() {
    char response[256];
    snprintf(response, sizeof(response),
      "{ \"Heap\": %d, \"Clients\": %d, \"Uptime\": %lu, \"ResetReason\": \"%s\" }",
      ESP.getFreeHeap(),
      arduino_homekit_connected_clients_count(),
      millis() / 1000,
      ESP.getResetReason().c_str());
    server.send(200, "application/json", response);
  });

  // Start the web server
  server.begin();

  Serial.println("Setup Homekit...");
  //homekit_storage_reset(); // to remove the previous HomeKit pairing storage when you first run this HomeKit sketch
  my_homekit_setup();

#ifdef MQTT_ENABLED
  Serial.println("Setup MQTT...");
  mqtt_setup();
#endif

  Serial.println("Startup Complete.");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  ESPButton.loop();
  read_sensor();
  my_homekit_loop();
#ifdef MQTT_ENABLED
  mqtt_loop();
#endif
  if (displayDirty) {
    drawFrame1(&display, 0, 0);
    displayDirty = false;
  }
  yield();
}

//==============================
// HomeKit setup and loop
//==============================

extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t cha_temperature;
extern "C" homekit_characteristic_t cha_humidity;
extern "C" homekit_characteristic_t cha_switch_on;
extern "C" homekit_characteristic_t cha_motion;
extern "C" homekit_characteristic_t cha_occupancy;

#define HOMEKIT_OCCUPANCY_DETECTED                  1
#define HOMEKIT_OCCUPANCY_NOT_DETECTED              0
#define HOMEKIT_PROGRAMMABLE_SWITCH_EVENT_SINGLE_PRESS   0
#define HOMEKIT_PROGRAMMABLE_SWITCH_EVENT_DOUBLE_PRESS   1
#define HOMEKIT_PROGRAMMABLE_SWITCH_EVENT_LONG_PRESS     2

// Unified switch control — called from HomeKit, physical button, and MQTT
void set_switch(bool on) {
  cha_switch_on.value.bool_value = on;
  digitalWrite(RELAYPIN, on ? HIGH : LOW);
  save_relay_state(on);
  displayDirty = true;
  homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
  LOG_D("Switch: %s", on ? "ON" : "OFF");
#ifdef MQTT_ENABLED
  mqtt_publish_event("switch", on ? "ON" : "OFF");
#endif
}

//Called when the switch value is changed by iOS Home APP
void cha_switch_on_setter(const homekit_value_t value) {
  set_switch(value.bool_value);
}

void read_sensor() {
  // read the occupancy input:
  motionState = digitalRead(OCCUPANCYPIN);
  if (motionState != lastMotionState) {
    motion = (motionState == HIGH);
    occupancy = motion;
    cha_motion.value.bool_value = motion;
    cha_occupancy.value.uint8_value = (uint8_t)occupancy;
    homekit_characteristic_notify(&cha_motion, cha_motion.value);
    homekit_characteristic_notify(&cha_occupancy, cha_occupancy.value);
    displayDirty = true;
#ifdef MQTT_ENABLED
    mqtt_publish_event("motion", motion ? "ON" : "OFF");
    mqtt_publish_event("occupancy", occupancy ? "ON" : "OFF");
#endif

    LOG_D("Motion: %s", motion ? "ON" : "OFF");        
  }

  // save the current state as the last state, for next time through the loop
  lastMotionState = motionState;

  // Wait at least 2 seconds seconds between measurements.
  // If the difference between the current time and last time you read
  // the sensor is bigger than the interval you set, read the sensor.
  // Works better than delay for things happening elsewhere also.
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    // Save the last time you read the sensor
    previousMillis = currentMillis;

    // Reading temperature and humidity takes about 250 milliseconds!
    // Sensor readings may also be up to 2 seconds 'old' (it's a very slow sensor)
    humidity = dht.readHumidity();        // Read humidity as a percent
    temperature = dht.readTemperature();  // Read temperature as Celsius

    // Check if any reads failed and exit early (to try again).
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("Failed to read from DHT sensor!");
      return;
    }

    hindex = dht.computeHeatIndex(temperature, humidity, false);  // Read temperature as Celsius

    // Convert the floats to strings and round to 2 decimal places
    dtostrf(hindex, 1, 2, str_hindex);
    dtostrf(humidity, 1, 2, str_humidity);
    dtostrf(temperature, 1, 2, str_temperature);
    displayDirty = true;
  }
}

void my_homekit_setup() {
  pinMode(RELAYPIN, OUTPUT); // Set the relay output up
  pinMode(OCCUPANCYPIN, INPUT); // Set the occupancy input up
  pinMode(SWITCHPIN, INPUT_PULLUP); // Set the switch input up

  // Restore relay state from EEPROM so restarts don't change the light
  bool savedState = load_relay_state();
  digitalWrite(RELAYPIN, savedState ? HIGH : LOW);
  cha_switch_on.value.bool_value = savedState;
  LOG_D("Restored relay state: %s", savedState ? "ON" : "OFF");

  ESPButton.add(0, SWITCHPIN, LOW, false, true);
  ESPButton.setCallback([&](uint8_t id, ESPButtonEvent event) {
    // Only one button is added, no need to check the id.
    LOG_D("Button Event: %s", ESPButton.getButtonEventDescription(event));

    if (event == ESPBUTTONEVENT_SINGLECLICK) {
      set_switch(!cha_switch_on.value.bool_value);
    } else if (event == ESPBUTTONEVENT_LONGCLICK) {
      drawFrame0(&display, 0, 0);
      homekit_storage_reset(); // to remove the previous HomeKit pairing storage
      delay(1000);
      Serial.println("Restarting...");
      ESP.restart();
    }
  });
  ESPButton.begin();

  //Add the .setter function to get the switch-event sent from iOS Home APP.
  //The .setter should be added before arduino_homekit_setup.
  //HomeKit sever uses the .setter_ex internally, see homekit_accessories_init function.
  //Maybe this is a legacy design issue in the original esp-homekit library,
  //and I have no reason to modify this "feature".
  cha_switch_on.setter = cha_switch_on_setter;
  arduino_homekit_setup(&config);

  cha_motion.value.bool_value = false;
  cha_occupancy.value.bool_value = false;  
}

static uint32_t last_report_millis = 0;
static uint32_t last_heap_millis = 0;

void my_homekit_loop() {
  arduino_homekit_loop();
  const uint32_t t = millis();
  if (t - last_report_millis >= 10000) {
    // report sensor values every 10 seconds
    last_report_millis = t;
    my_homekit_report();
  }
  if (t - last_heap_millis >= 5000) {
    // Show heap info every 5 seconds
    last_heap_millis = t;
    uint32_t freeHeap = ESP.getFreeHeap();
    LOG_D("Free heap: %d, HomeKit clients: %d",
          freeHeap, arduino_homekit_connected_clients_count());

    // If heap is critically low, do a controlled restart (relay state is already saved)
    if (freeHeap < LOW_HEAP_THRESHOLD) {
      LOG_D("Heap critically low (%d bytes), restarting...", freeHeap);
#ifdef MQTT_ENABLED
      mqtt_publish("reset_reason", "low_heap", true);
      mqtt_publish("status", "offline", true);
      mqttClient.disconnect();
#endif
      ESP.restart();
    }
  }
}

void my_homekit_report() {
  if (!(isnan(humidity) || isnan(temperature)) && (humidity <= 100 && temperature <= 50))
  {
    // Report Temperature
    cha_temperature.value.float_value = temperature;
    homekit_characteristic_notify(&cha_temperature, cha_temperature.value);

    // Report Humidity
    cha_humidity.value.float_value = humidity;
    homekit_characteristic_notify(&cha_humidity, cha_humidity.value);
  }

  // Report Switch
  homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);

  LOG_D("t %.1f, h %.1f, m %u, o %u", temperature, humidity, (uint8_t)motion, (uint8_t)occupancy);
}

void drawFrame0(SSD1306Wire *display, int16_t x, int16_t y) {
  display->clear();
  display->setFont(ArialMT_Plain_10);
  display->drawString(10 + x, 18 + y, "Starting...");
  display->display();
}

void drawFrame1(SSD1306Wire *display, int16_t x, int16_t y) {

  // converts to int removing unnecessary decimal points
  int hAsInt = int(humidity);
  int iAsInt = int(hindex);
  int tAsInt = int(temperature);

  display->clear();

  display->drawLine(40 + x, 2  + y, 40 + x, 45 + y);
  display->drawLine(44 + x, 23 + y, 63 + x, 23 + y);

  display->setFont(ArialMT_Plain_16);
  display->drawString(0 + x,  2 + y, String(tAsInt) + "°C");
  display->setFont(ArialMT_Plain_10);

  display->drawString(0 + x, 18 + y, "H:" + String(hAsInt) + "%");
  display->drawString(0 + x, 30 + y, "I:" + String(iAsInt) + "°C");

  display->setFont(Icons_16);
  display->drawString(47 + x, 4  + y, String((uint8_t)occupancy));
  display->drawString(47 + x, 28 + y, cha_switch_on.value.bool_value ? "3" : "2");
  display->display();
}
