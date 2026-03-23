/*
 * window_controller.c
 *
 *  Created on: 18-Mar-2026
 *      Author: Anjana Roy
 */

#include "window_controller.h"
#include "motor_driver.h"
#include "feature_engine.h"
#include "main.h"

/* === Configuration === */
#define DEAD_TIME_MS      5U

extern UART_HandleTypeDef huart3;

/* === State Machine === */
typedef enum {
    STATE_STOP = 0,
    STATE_UP,
    STATE_DOWN,
    STATE_PINCH
} WindowState;

/* === Internal Variables === */
static WindowState state = STATE_STOP;
static WindowState last_motor_applied = (WindowState)(-1);

static uint8_t motionTimer = 0;
static uint8_t pinchAlertTimer = 0;

/* === Init === */
void window_init(void)
{
    state = STATE_STOP;
    last_motor_applied = (WindowState)(-1);
    motionTimer = 0;
    pinchAlertTimer = 0;
}

/* === LED + Timer Task (100ms) === */
void window_100ms_task(void)
{
    static uint8_t toggle = 0;
    toggle ^= 1;

    switch (state)
    {
        case STATE_STOP:
            BSP_LED_Off(LED_GREEN);
            BSP_LED_Off(LED_YELLOW);
            BSP_LED_Off(LED_RED);
            break;

        case STATE_UP:
            BSP_LED_On(LED_GREEN);
            BSP_LED_Off(LED_YELLOW);
            BSP_LED_Off(LED_RED);

            if (motionTimer > 0)
                motionTimer--;
            else
                state = STATE_STOP;
            break;

        case STATE_DOWN:
            BSP_LED_Off(LED_GREEN);
            if (toggle) BSP_LED_On(LED_YELLOW);
            else BSP_LED_Off(LED_YELLOW);
            BSP_LED_Off(LED_RED);

            if (motionTimer > 0)
                motionTimer--;
            else
                state = STATE_STOP;
            break;

        case STATE_PINCH:
            BSP_LED_Off(LED_GREEN);
            BSP_LED_Off(LED_YELLOW);

            if (toggle) BSP_LED_On(LED_RED);
            else BSP_LED_Off(LED_RED);

            if (pinchAlertTimer > 0)
            {
                pinchAlertTimer--;
            }
            else
            {
                state = STATE_DOWN;
                motionTimer = 30; // 3 seconds
            }
            break;
    }
}

/* === Motor Control Sync === */
void window_motor_task(void)
{
    if (last_motor_applied == state)
        return;

    /* Safe direction change */
    if ((last_motor_applied == STATE_UP && state == STATE_DOWN) ||
        (last_motor_applied == STATE_DOWN && state == STATE_UP))
    {
        motor_set_direction(MOTOR_STOP);
        HAL_Delay(DEAD_TIME_MS);
    }

    last_motor_applied = state;

    switch (state)
    {
        case STATE_STOP:
            motor_set_direction(MOTOR_STOP);
            break;

        case STATE_UP:
            motor_set_direction(MOTOR_FORWARD);
            break;

        case STATE_DOWN:
            motor_set_direction(MOTOR_REVERSE);
            break;

        case STATE_PINCH:
            motor_set_direction(MOTOR_STOP);
            break;
    }
}

/* === Pinch Trigger === */
void window_trigger_pinch(void)
{
    if (feature_is_enabled_ci("ANTIPINCH") && state == STATE_UP)
    {
        state = STATE_PINCH;
        pinchAlertTimer = 20; // 2 sec
        motionTimer = 0;

        last_motor_applied = (WindowState)(-1);

        const char msg[] = "Anti-pinch: reversing window\r\n";
        HAL_UART_Transmit(&huart3, (uint8_t*)msg, sizeof(msg)-1, 50);
    }
}

/* === Button Handling === */
void window_user_button_pressed(void)
{
    static uint32_t last = 0;
    uint32_t now = HAL_GetTick();

    if ((now - last) < 80)
        return;

    last = now;

    if (state == STATE_STOP)
    {
        state = STATE_UP;
        motionTimer = 50; // 5 sec
        last_motor_applied = (WindowState)(-1);
    }
    else if (state == STATE_UP)
    {
        if (feature_is_enabled_ci("ANTIPINCH"))
        {
            window_trigger_pinch();
        }
    }
}
