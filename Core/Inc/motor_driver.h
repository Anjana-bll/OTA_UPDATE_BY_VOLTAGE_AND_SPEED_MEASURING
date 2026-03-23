/*
 * motor_driver.h
 *
 *  Created on: 18-Mar-2026
 *      Author: Anjana Roy
 */

#ifndef INC_MOTOR_DRIVER_H_
#define INC_MOTOR_DRIVER_H_

#include "main.h"

typedef enum {
    MOTOR_STOP = 0,
    MOTOR_FORWARD,
    MOTOR_REVERSE
} motor_dir_t;

/* Initialize motor GPIOs (optional if already done in MX_GPIO) */
void motor_init(void);

/* Set motor direction */
void motor_set_direction(motor_dir_t dir);

#endif /* INC_MOTOR_DRIVER_H_ */
