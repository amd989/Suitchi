/*
 * suitchi.ino
 *
 *  Created on: 2020-12-01
 *      Author: amd989 (Alejandro Mora)

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

// Include the UI lib
#include "SSD1306Brzo.h"
#include "OLEDDisplayUi.h"

#include <DHT.h>
#include <ArduinoOTA.h>
#include <SimpleTimer.h>

#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

#define LOG_D(fmt, ...)   printf_P(PSTR(fmt "\n") , ##__VA_ARGS__);


//////////////////////////
// Setting up the pin and DHT version
#define DHTTYPE DHT11   // DHT Shield uses DHT 11

#define RELAYPIN D8     // Relay Shield uses pin D0
#define OCCUPANCYPIN D3 // Occupancy uses pin D3
#define DHTPIN D4       // DHT Shield uses pin D4
#define SWITCHPIN D7    // Relay Shield uses pin D7

// Initialize the OLED display using Arduino Wire:
SSD1306Brzo display(0x3c, SDA, SCL, GEOMETRY_64_48 );  // ADDRESS, SDA, SCL
//OLEDDisplayUi ui     ( &display );
DHT dht(DHTPIN, DHTTYPE);

// Listen for HTTP requests on standard port 80
ESP8266WebServer server(80);

//Sensor variables
float humidity, temperature, hindex;                         // Raw float values from the sensor
char str_humidity[10], str_temperature[10], str_hindex[10];  // Rounded sensor values as strings
bool pushButton, ocuppancy, motion = false;

// Generally, you should use "unsigned long" for variables that hold time
unsigned long previousMillis = 0;            // When the sensor was last read
const long interval = 2000;                  // Wait this long until reading again

// countdown variables and timer
const int CountdownTime = 60;                // Time to wait until occupancy is turned off.
int CountdownTimer;
SimpleTimer occupancyTimer;                  // this timer is used to shut off the occupancy after a certain ammount of time

//Sets up the initial page
void handle_root() {
  server.send(200, "text/plain", "Welcome to Suitchi Web Server.");
  delay(100);
}

void setup() {
  Serial.begin(115200);  
  Serial.println("Starting...");

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

  Serial.println("Setup Timers...");
  // Setup occupancy timer
  CountdownTimer = occupancyTimer.setInterval(CountdownTime * 1000L, CountdownTimerFunction);
  occupancyTimer.disable(CountdownTimer);

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
  });

  ArduinoOTA.begin();
  ArduinoOTA.onStart([]() {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2 - 10, "OTA Update");
    display.display();
  });

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
  
  // Start the web server
  server.begin();

  Serial.println("Setup Homekit...");
  //homekit_storage_reset(); // to remove the previous HomeKit pairing storage when you first run this HomeKit sketch
  my_homekit_setup();
  
  Serial.println("Startup Complete.");
}

// This array keeps function pointers to all frames
// frames are the single views that slide in
// FrameCallback frames[] = { drawFrame1 };

// how many frames are there?
// int frameCount = 1;

// Overlays are statically drawn on top of a frame eg. a clock
OverlayCallback overlays[] = {  };
int overlaysCount = 0;

void loop() {
  ArduinoOTA.handle();
  occupancyTimer.run();
  ESPButton.loop();
  read_sensor();
  my_homekit_loop();
  drawFrame1(&display, 0, 0);
  delay(10);
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

// Called when the value is read by iOS Home APP
homekit_value_t cha_switch_on_getter() {
  // Should always return "null" for reading, see HAP section 9.75
  return HOMEKIT_NULL_CPP();
}

//Called when the switch value is changed by iOS Home APP
void cha_switch_on_setter(const homekit_value_t value) {
  bool on = value.bool_value;
  cha_switch_on.value.bool_value = on;  //sync the value
  LOG_D("Switch: %s", on ? "ON" : "OFF");
  digitalWrite(RELAYPIN, on ? HIGH : LOW);
}

void read_sensor() {

  motion = digitalRead(OCCUPANCYPIN) == 1;
  if (motion) {    
    // Report Occupancy
    ocuppancy = true;     
    cha_occupancy.value.uint8_value = (uint8_t)ocuppancy;
    homekit_characteristic_notify(&cha_occupancy, cha_occupancy.value);  
    occupancyTimer.disable(CountdownTimer);
    occupancyTimer.enable(CountdownTimer);
  }

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
  }
}

void my_homekit_setup() {
  pinMode(RELAYPIN, OUTPUT); // Set the relay output up
  pinMode(OCCUPANCYPIN, INPUT); // Set the occupancy input up
  pinMode(SWITCHPIN, INPUT_PULLUP); // Set the switch input up

  digitalWrite(RELAYPIN, LOW); // Turn Relay Off

  ESPButton.add(0, SWITCHPIN, LOW, true, true);
  ESPButton.setCallback([&](uint8_t id, ESPButtonEvent event) {
    // Only one button is added, no need to check the id.
    LOG_D("Button Event: %s", ESPButton.getButtonEventDescription(event));
    //    uint8_t cha_value = 0;
    //    if (event == ESPBUTTONEVENT_SINGLECLICK) {
    //      cha_value = HOMEKIT_PROGRAMMABLE_SWITCH_EVENT_SINGLE_PRESS;
    //    } else if (event == ESPBUTTONEVENT_DOUBLECLICK) {
    //      cha_value = HOMEKIT_PROGRAMMABLE_SWITCH_EVENT_DOUBLE_PRESS;
    //    } else if (event == ESPBUTTONEVENT_LONGCLICK) {
    //      cha_value = HOMEKIT_PROGRAMMABLE_SWITCH_EVENT_LONG_PRESS;
    //    }

    bool switchValue = !cha_switch_on.value.bool_value;
    cha_switch_on.value.bool_value = switchValue; //sync the value
    digitalWrite(RELAYPIN, switchValue ? HIGH : LOW);
    LOG_D("Switch: %s", switchValue ? "ON" : "OFF");
    homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
  });
  ESPButton.begin();

  //Add the .setter function to get the switch-event sent from iOS Home APP.
  //The .setter should be added before arduino_homekit_setup.
  //HomeKit sever uses the .setter_ex internally, see homekit_accessories_init function.
  //Maybe this is a legacy design issue in the original esp-homekit library,
  //and I have no reason to modify this "feature".
  cha_switch_on.setter = cha_switch_on_setter;
  // cha_switch_on.getter = cha_switch_on_getter;
  arduino_homekit_setup(&config);
}

static uint32_t next_heap_millis = 0;
static uint32_t next_report_millis = 0;

void my_homekit_loop() {
  arduino_homekit_loop();
  const uint32_t t = millis();
  if (t > next_report_millis) {
    // report sensor values every 10 seconds
    next_report_millis = t + 10 * 1000;
    my_homekit_report();
  }
  if (t > next_heap_millis) {
    // Show heap info every 5 seconds
    next_heap_millis = t + 5 * 1000;
    LOG_D("Free heap: %d, HomeKit clients: %d",
          ESP.getFreeHeap(), arduino_homekit_connected_clients_count());

  }
}

void my_homekit_report() {

  // Report Motion
  cha_motion.value.bool_value = motion;
  homekit_characteristic_notify(&cha_motion, cha_motion.value);

  // Report Occupancy
  cha_occupancy.value.uint8_value = (uint8_t)ocuppancy;
  homekit_characteristic_notify(&cha_occupancy, cha_occupancy.value);

  // Report Switch
  homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);

  if (!(isnan(humidity) || isnan(temperature)) && (humidity < 100 || temperature < 50))
  {
    float t = temperature;
    float h = humidity;

    // Report Temperature
    cha_temperature.value.float_value = t;
    homekit_characteristic_notify(&cha_temperature, cha_temperature.value);

    // Report Humidity
    cha_humidity.value.float_value = h;
    homekit_characteristic_notify(&cha_humidity, cha_humidity.value);
  }

  LOG_D("t %.1f, h %.1f, m %u, o %u", temperature, humidity, (uint8_t)motion, (uint8_t)ocuppancy);
}

void CountdownTimerFunction()
{
  //Serial.print("Countdown function called "); 
  // Report Occupancy
  ocuppancy = false;     
  cha_occupancy.value.uint8_value = (uint8_t)ocuppancy;
  homekit_characteristic_notify(&cha_occupancy, cha_occupancy.value);  
  occupancyTimer.disable(CountdownTimer); // stop timer
}

void drawFrame0(SSD1306Brzo *display, int16_t x, int16_t y) {
  display->clear();
  display->setFont(ArialMT_Plain_10);
  display->drawString(10 + x, 18 + y, "Starting...");
  display->display();
}

void drawFrame1(SSD1306Brzo *display, int16_t x, int16_t y) {

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
  display->drawString(47 + x, 4  + y, String((uint8_t)ocuppancy));
  display->drawString(47 + x, 28 + y, cha_switch_on.value.bool_value ? "3" : "2");
  display->display();
}
