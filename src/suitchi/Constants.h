/*
 * Constants.h
 *
 *  Created on: 2020-12-01
 *      Author: amd989 (Alejandro Mora)
 */

#ifndef CONSTANTS_H_
#define CONSTANTS_H_

#define homeKitPin "1CB8"
#define homeKitPassword "111-11-111"
#define bridgeName "Suitchi 1CB8"
#define otaName "suitchi_1CB8"
#define serialNumber "123457"

//////////////////////////
// MQTT + Home Assistant Autodiscovery (optional)
// Uncomment the line below to enable MQTT with HA autodiscovery
// #define MQTT_ENABLED

#ifdef MQTT_ENABLED
#define MQTT_SERVER         "192.168.1.100"
#define MQTT_PORT           1883
#define MQTT_USER           ""                // leave empty if no auth
#define MQTT_PASS           ""
#define MQTT_NODE_ID        "suitchi_1cb8"    // HA device identifier (a-z, 0-9, _ only)
#define MQTT_TOPIC_PREFIX   "suitchi/1CB8"    // state/command topics published under this prefix
#define MQTT_PUBLISH_INTERVAL 30000           // telemetry publish interval in ms
#endif

#endif /* CONSTANTS_H_ */
