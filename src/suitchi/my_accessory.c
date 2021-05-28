/*
 * my_accessory.c
 * Define the accessory in C language using the Macro in characteristics.h
 *
 *  Created on: 2020-12-01
 *      Author: amd989 (Alejandro Mora)
 */

#include <homekit/homekit.h>
#include <homekit/characteristics.h>

#include "Constants.h"

void my_accessory_identify(homekit_value_t _value) {
	printf("accessory identify\n");
}

// format: bool; HAP section 9.70; write the .setter function to get the switch-event sent from iOS Home APP.
homekit_characteristic_t cha_switch_on = HOMEKIT_CHARACTERISTIC_(ON, false);

// format: string; HAP section 9.62; max length 64
homekit_characteristic_t cha_name = HOMEKIT_CHARACTERISTIC_(NAME, "Switch");


homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_bridge, .services=(homekit_service_t*[]) {
    	// HAP section 8.17:
    	// For a bridge accessory, only the primary HAP accessory object must contain this(INFORMATION) service.
    	// But in my test,
    	// the bridged accessories must contain an INFORMATION service,
    	// otherwise the HomeKit will reject to pair.
    	HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
            HOMEKIT_CHARACTERISTIC(NAME, bridgeName),
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Arduino HomeKit"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, serialNumber),
            HOMEKIT_CHARACTERISTIC(MODEL, "ESP8266/ESP32"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.0"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, my_accessory_identify),
            NULL
        }),
        NULL
    }),
	HOMEKIT_ACCESSORY(.id=2, .category=homekit_accessory_category_sensor, .services=(homekit_service_t*[]) {
    	HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
			HOMEKIT_CHARACTERISTIC(NAME, "Switch"),
			HOMEKIT_CHARACTERISTIC(IDENTIFY, my_accessory_identify),
			NULL
		}),
    	HOMEKIT_SERVICE(SWITCH, .primary=true, .characteristics=(homekit_characteristic_t*[]){
      &cha_switch_on,
      &cha_name,
      NULL
    }),
		NULL
	}),		
    NULL
};


homekit_server_config_t config = {
		.accessories = accessories,
		.password = homeKitPassword,
    .setupId=homeKitPin,
};
