/*
 * Read_Voltage.c
 *
 *  Created on: Nov 17, 2025
 *      Author: Harshit Sharma
 */

#include "Read_Voltage.h"
#include "stm32h7xx_hal_adc.h"   /* optional — the header already included stm32h7xx_hal.h */


#define ADC_REF        3.26f
#define ADC_MAX        4095.0f

/* Real-world voltage divider scale
   Based on user measurements:
   7V  -> 1.08V
   12V -> 1.823V
   Average scale factor = 6.283
*/
#define VIN_SCALE      5.545454f
/* Define the global variable (as declared extern in header) */
float adcVoltage = 0.0f;

float Read_Voltage(ADC_HandleTypeDef *hadc)
{
    /* Start conversion */
    HAL_ADC_Start(hadc);

    /* Wait until done */
    HAL_ADC_PollForConversion(hadc, HAL_MAX_DELAY);

    /* Read raw value */
    uint32_t raw = HAL_ADC_GetValue(hadc);

    /* Convert to ADC-side voltage */
    float adcVoltage = (ADC_REF * raw) / ADC_MAX;

    /* Convert to actual Vin */
    float vin = adcVoltage * VIN_SCALE;

    return vin;
}
