/*
 * window_controller.h
 *
 *  Created on: 18-Mar-2026
 *      Author: Anjana Roy
 */

#ifndef INC_WINDOW_CONTROLLER_H_
#define INC_WINDOW_CONTROLLER_H_

#include <stdint.h>

/* Public APIs */
void window_init(void);
void window_100ms_task(void);
void window_motor_task(void);
void window_user_button_pressed(void);

/* Optional: external trigger (future pinch detection) */
void window_trigger_pinch(void);

#endif /* INC_WINDOW_CONTROLLER_H_ */
