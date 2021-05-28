/*
 * suitchi.ino
 *
 *  Created on: 2020-12-01
 *      Author: amd989 (Alejandro Mora)

   This code represents a bridge (aka gateway) which contains multiple accessories.

   This includes 6 sensors:
   1. Switch (HAP section 8.38)

   You should:
   1. erase the full flash or call homekit_storage_reset() in setup()
      to remove the previous HomeKit pairing storage and
      enable the pairing with the new accessory of this new HomeKit example.
*/

#include <Arduino.h>
#include <arduino_homekit_server.h>
#include "wifi_info.h"
#include "ESPButton.h"
#include "Constants.h"

#include <ArduinoOTA.h>

#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

#define LOG_D(fmt, ...)   printf_P(PSTR(fmt "\n") , ##__VA_ARGS__);

//////////////////////////
// Setting up the pins

#define LEDPIN   13     // LED Pin
#define RELAYPIN 12     // Relay Pin
#define SWITCHPIN 0     // Switch Pin

// Listen for HTTP requests on standard port 80
ESP8266WebServer server(80);

bool pushButton = false;

//Sets up the initial page
void handle_root() {
  server.send(200, "text/plain", "Welcome to Suitchi Web Server.");
  delay(100);
}

void(* resetFunc) (void) = 0;  // declare reset fuction at address 0

void setup() {
  Serial.begin(115200);  
  Serial.println("Starting...");

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
  });

  ArduinoOTA.begin();
  
  Serial.println("Setup Web Server...");
  // Handle http request display root
  server.on("/", HTTP_GET, handle_root);

  // Start the web server
  server.begin();

  Serial.println("Setup Homekit...");  
  my_homekit_setup();
    
  Serial.println("Startup Complete.");
  toggleLed(250); 
  toggleLed(250);
  toggleLed(250);
}


void loop() {
  ArduinoOTA.handle();
  ESPButton.loop();
  my_homekit_loop();
  delay(10);
}

//==============================
// HomeKit setup and loop
//==============================

extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t cha_switch_on;

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

void toggleLed(int ms){
   digitalWrite(LEDPIN, LOW); // LOW will turn on the LED
   delay(ms);
   digitalWrite(LEDPIN, HIGH); // HIGH will turn off the LED
   delay(ms);
}

void my_homekit_setup() {
  pinMode(LEDPIN, OUTPUT); // Set the relay output up
  pinMode(RELAYPIN, OUTPUT); // Set the relay output up
  pinMode(SWITCHPIN, INPUT_PULLUP); // Set the switch input up
  digitalWrite(RELAYPIN, LOW); // Turn Relay Off
  digitalWrite(LEDPIN, HIGH); // Turn LED Off

  ESPButton.add(0, SWITCHPIN, LOW, true, true);
  ESPButton.setCallback([&](uint8_t id, ESPButtonEvent event) {
    // Only one button is added, no need to check the id.
    LOG_D("Button Event: %s", ESPButton.getButtonEventDescription(event));
      
    if (event == ESPBUTTONEVENT_SINGLECLICK) {
        bool switchValue = !cha_switch_on.value.bool_value;
        cha_switch_on.value.bool_value = switchValue; // sync the value
        digitalWrite(RELAYPIN, switchValue ? HIGH : LOW);
        digitalWrite(LEDPIN, switchValue ? LOW : HIGH); // LOW will turn on the LED
        LOG_D("Switch: %s", switchValue ? "ON" : "OFF");
        homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
    } else if (event == ESPBUTTONEVENT_DOUBLECLICK) {            
    } else if (event == ESPBUTTONEVENT_LONGCLICK) {

      // Rapidly flash LED to signal reset
      toggleLed(500); 
      toggleLed(500);
      toggleLed(500);
      toggleLed(500);
            
      homekit_storage_reset(); // to remove the previous HomeKit pairing storage   
      delay(1000);
      Serial.println("Restarting...");
      resetFunc(); //call reset
    }   
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
  // Report Switch
  homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
}
