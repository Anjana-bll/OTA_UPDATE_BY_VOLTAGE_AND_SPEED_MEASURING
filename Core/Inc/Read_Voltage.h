/*
 * Read_Voltage.h
 *
 *  Created on: Nov 17, 2025
 *      Author: Harshit Sharma
 */

/* Core/Inc/Read_Voltage.h */
#ifndef READ_VOLTAGE_H_
#define READ_VOLTAGE_H_




#include "stm32h7xx_hal.h"   /* ensures ADC_HandleTypeDef is known */

/* returns voltage in millivolts */
float Read_Voltage(ADC_HandleTypeDef *hadc);



#endif /* READ_VOLTAGE_H_ */
