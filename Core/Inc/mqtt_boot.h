/*
 * mqtt_boot.h
 *
 *  Created on: 04-Feb-2026
 *      Author: Anjana Roy
 */

#ifndef INC_MQTT_BOOT_H_
#define INC_MQTT_BOOT_H_

#include <stdbool.h>

void mqtt_boot_init(void);
void mqtt_boot_periodic(void);

void mqtt_publish_status(const char *status);
void mqtt_publish_voltage(float voltage);
void mqtt_publish_log(const char *msg);

bool mqtt_is_connected(void);

#endif /* INC_MQTT_BOOT_H_ */
