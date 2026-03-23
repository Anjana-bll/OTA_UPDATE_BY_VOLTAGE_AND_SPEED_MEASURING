/*
 * motor_driver.c
 *
 *  Created on: 18-Mar-2026
 *      Author: Anjana Roy
 */

#include "motor_driver.h"

/* === H-Bridge Pins (BTS7960) === */
#define H_IN1_GPIO_Port   GPIOF
#define H_IN1_Pin         GPIO_PIN_2   // RPWM

#define H_IN2_GPIO_Port   GPIOF
#define H_IN2_Pin         GPIO_PIN_9   // LPWM

#define H_IN3_GPIO_Port   GPIOE
#define H_IN3_Pin         GPIO_PIN_13  // R_EN

#define H_IN4_GPIO_Port   GPIOE
#define H_IN4_Pin         GPIO_PIN_14  // L_EN

static motor_dir_t last_dir = (motor_dir_t)(-1);

static void motor_stop(void)
{
    HAL_GPIO_WritePin(H_IN1_GPIO_Port, H_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(H_IN2_GPIO_Port, H_IN2_Pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(H_IN3_GPIO_Port, H_IN3_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(H_IN4_GPIO_Port, H_IN4_Pin, GPIO_PIN_RESET);
}

static void motor_forward(void)
{
    /* Enable both sides */
    HAL_GPIO_WritePin(H_IN3_GPIO_Port, H_IN3_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(H_IN4_GPIO_Port, H_IN4_Pin, GPIO_PIN_SET);

    /* Direction */
    HAL_GPIO_WritePin(H_IN1_GPIO_Port, H_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(H_IN2_GPIO_Port, H_IN2_Pin, GPIO_PIN_SET);
}

static void motor_reverse(void)
{
    /* Enable both sides */
    HAL_GPIO_WritePin(H_IN3_GPIO_Port, H_IN3_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(H_IN4_GPIO_Port, H_IN4_Pin, GPIO_PIN_SET);

    /* Direction */
    HAL_GPIO_WritePin(H_IN1_GPIO_Port, H_IN1_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(H_IN2_GPIO_Port, H_IN2_Pin, GPIO_PIN_RESET);
}

void motor_init(void)
{
    motor_stop();
}

void motor_set_direction(motor_dir_t dir)
{
    if (dir == last_dir) return;

    /* Safe direction change */
    if ((last_dir == MOTOR_FORWARD && dir == MOTOR_REVERSE) ||
        (last_dir == MOTOR_REVERSE && dir == MOTOR_FORWARD))
    {
        motor_stop();
        HAL_Delay(5);  // dead time
    }

    last_dir = dir;

    switch (dir)
    {
        case MOTOR_STOP:
            motor_stop();
            break;

        case MOTOR_FORWARD:
            motor_forward();
            break;

        case MOTOR_REVERSE:
            motor_reverse();
            break;
    }
}
