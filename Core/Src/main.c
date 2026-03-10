/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "feature_engine.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc3;

UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */

/* === Motor Hardware Pins (H-Bridge) === */
#define H_IN1_GPIO_Port   GPIOF
#define H_IN1_Pin         GPIO_PIN_2   // pin1 -> PF2
#define H_IN2_GPIO_Port   GPIOF
#define H_IN2_Pin         GPIO_PIN_9   // pin2 -> PF9
#define H_IN3_GPIO_Port   GPIOE
#define H_IN3_Pin         GPIO_PIN_13  // pin3 -> PE13
#define H_IN4_GPIO_Port   GPIOE
#define H_IN4_Pin         GPIO_PIN_14  // pin4 -> PE14

#define DEAD_TIME_MS      5U  // safe dead-time between direction changes

/* === Window state machine === */
typedef enum {
    STATE_STOP = 0,
    STATE_UP,
    STATE_DOWN,
    STATE_PINCH
} WindowState;

volatile WindowState state = STATE_STOP;
/* Remember last motor-applied state to avoid repeatedly driving pins */
static WindowState last_motor_applied = (WindowState)(-1);

/* 100ms timers */
volatile uint8_t motionTimer = 0;     // UP/DOWN duration (ticks of 100ms)
volatile uint8_t pinchAlertTimer = 0; // PINCH alert duration (ticks of 100ms)
static uint32_t last_tick_ms = 0;

/* Safer UART receive buffers */
#define RX_BUF_SIZE 128
uint8_t  rx_buf[RX_BUF_SIZE];
uint16_t rx_idx = 0;
uint8_t  uart_rx_byte;
volatile uint8_t frame_active = 0; // Inside a '<...>' frame?
volatile uint8_t frame_ready  = 0; // A full frame was received

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC3_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_ADC1_Init(void);
/* USER CODE BEGIN PFP */
/* === Motor API prototypes === */
static void set_forward(void);
static void set_reverse(void);
static void set_stop(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Option 2: feature handler to enable anti-pinch flag */
static feat_status_t on_antipinch_enable(const char *nonce, const char *rawName)
{
    (void)nonce; (void)rawName;
    return FEAT_OK;
}
/* Simple LED patterns using BSP LEDs (no PWM):
   - UP    : GREEN ON steady
   - DOWN  : YELLOW blink
   - PINCH : RED fast flash (then auto DOWN)
   - STOP  : all OFF
*/
static void updateLED_100ms_simple(void)
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
            if (motionTimer > 0) motionTimer--; else state = STATE_STOP;
            break;

        case STATE_DOWN:
            BSP_LED_Off(LED_GREEN);
            if (toggle) BSP_LED_On(LED_YELLOW); else BSP_LED_Off(LED_YELLOW);
            BSP_LED_Off(LED_RED);
            if (motionTimer > 0) motionTimer--; else state = STATE_STOP;
            break;

        case STATE_PINCH:
            // fast flash RED for 2s, then auto DOWN 3s
            BSP_LED_Off(LED_GREEN);
            BSP_LED_Off(LED_YELLOW);
            if (toggle) BSP_LED_On(LED_RED); else BSP_LED_Off(LED_RED);

            if (pinchAlertTimer > 0) {
                pinchAlertTimer--;
            } else {
                state = STATE_DOWN;
                motionTimer = 30; // 3 seconds (30 * 100ms)
            }
            break;
    }
}

/* Call this when your pinch sensor triggers (or for testing) */
static void onPinchDetected(void)
{
	if (feature_is_enabled_ci("ANTIPINCH") && state == STATE_UP)
	{
	    /* same as before */
	    state = STATE_PINCH;
	    pinchAlertTimer = 20;
	    motionTimer = 0; //stop-UP
	    last_motor_applied = (WindowState)(-1); //force immediate motor sync on next iteration

	    const char msg[] = "Anti-pinch: reversing window\r\n";
	    HAL_UART_Transmit(&huart3, (uint8_t*)msg, sizeof(msg)-1, 50);
	}else {
        const char msg[] = "PINCH ignored (disabled or not UP)\r\n";
        HAL_UART_Transmit(&huart3, (uint8_t*)msg, sizeof(msg)-1, 50);
    }
}

/* Low-level write helper */
static void motor_sync_with_state(void)
{
    if (last_motor_applied == state) return;
    last_motor_applied = state;

    switch (state)
    {
        case STATE_STOP:
            set_stop();
            break;
        case STATE_UP:
            set_forward();
            break;
        case STATE_DOWN:
            set_reverse();
            break;
        case STATE_PINCH:
            set_stop();  // stop immediately during PINCH alert
            break;
    }
}

static inline void pins_write(uint8_t in1, uint8_t in2, uint8_t in3, uint8_t in4)
{
  HAL_GPIO_WritePin(H_IN1_GPIO_Port, H_IN1_Pin, in1 ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(H_IN2_GPIO_Port, H_IN2_Pin, in2 ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(H_IN3_GPIO_Port, H_IN3_Pin, in3 ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(H_IN4_GPIO_Port, H_IN4_Pin, in4 ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void all_pins_low(void) { pins_write(0,0,0,0); }

static void set_forward(void)
{
	const char msg[] = "Motor Forward\r\n";

  all_pins_low();
  HAL_Delay(DEAD_TIME_MS);
  /* Keep your original pattern exactly (as in your motor code) */
  // PF2 + PE13 HIGH
  pins_write(1,0,1,0);
  HAL_UART_Transmit(&huart3,(uint8_t*)msg,sizeof(msg)-1,50);
}

static void set_reverse(void)
{
  all_pins_low();
  HAL_Delay(DEAD_TIME_MS);
  /* Keep your original pattern exactly */
  // PF9 + PE14 HIGH
  pins_write(0,1,0,1);
  const char msg[] = "Motor Reverse\r\n";
  HAL_UART_Transmit(&huart3,(uint8_t*)msg,sizeof(msg)-1,50);

}

static void set_stop(void)
{
  /* All low => electrical brake */
  all_pins_low();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks */
  PeriphCommonClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Initialize the feature engine */
  feature_engine_init();

  /* Register features (Option 2) */
  #if FE_USE_TABLE
  feature_register("ANTIPINCH", on_antipinch_enable);
  /* Accept aliases */
  feature_add_alias("ANTIPINCH", "ANTI-PINCH_WINDOWS");
  /* You can register others and   */
  #endif
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC3_Init();
  MX_USART3_UART_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1);
  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);
  BSP_LED_Init(LED_YELLOW);
  BSP_LED_Init(LED_RED);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  // 100 ms soft tick
	  uint32_t now = HAL_GetTick();
	  if ((now - last_tick_ms) >= 100) {
	      last_tick_ms = now;
	      updateLED_100ms_simple();
	      motor_sync_with_state();
	  }

	  if (frame_ready)
	  {
	      frame_ready = 0;

	      BSP_LED_On(LED_YELLOW);

	      char featureId[32] = {0};
	      char nonce[48] = {0};
	      char ack[96] = {0};

	      int parsed = sscanf((char*)rx_buf, "<ACT,%31[^,],%47[^>]>", featureId, nonce);

	      if (parsed == 2)
	      {
	    	feature_engine_handle_activation(featureId, nonce, ack, sizeof(ack));
	    	BSP_LED_On(LED_RED); /* indicate an ACK was produced */
	      } else {
	    	snprintf(ack, sizeof(ack), "<ACK,?,? ,ERROR_FORMAT>");
	      }

	      HAL_UART_Transmit(&huart3, (uint8_t*)ack, strlen(ack), 100);

	      rx_idx = 0;
	      memset(rx_buf, 0, sizeof(rx_buf));
	  }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 24;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV8;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enables the Clock Security System
  */
  HAL_RCC_EnableCSS();
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInitStruct.PLL2.PLL2M = 1;
  PeriphClkInitStruct.PLL2.PLL2N = 24;
  PeriphClkInitStruct.PLL2.PLL2P = 5;
  PeriphClkInitStruct.PLL2.PLL2Q = 2;
  PeriphClkInitStruct.PLL2.PLL2R = 2;
  PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_3;
  PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
  PeriphClkInitStruct.PLL2.PLL2FRACN = 0;
  PeriphClkInitStruct.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  hadc1.Init.OversamplingMode = DISABLE;
  hadc1.Init.Oversampling.Ratio = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_16;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_64CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  sConfig.OffsetSignedSaturation = DISABLE;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC3_Init(void)
{

  /* USER CODE BEGIN ADC3_Init 0 */

  /* USER CODE END ADC3_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC3_Init 1 */

  /* USER CODE END ADC3_Init 1 */

  /** Common config
  */
  hadc3.Instance = ADC3;
  hadc3.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc3.Init.Resolution = ADC_RESOLUTION_12B;
  hadc3.Init.DataAlign = ADC3_DATAALIGN_RIGHT;
  hadc3.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc3.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc3.Init.LowPowerAutoWait = DISABLE;
  hadc3.Init.ContinuousConvMode = DISABLE;
  hadc3.Init.NbrOfConversion = 1;
  hadc3.Init.DiscontinuousConvMode = DISABLE;
  hadc3.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc3.Init.DMAContinuousRequests = DISABLE;
  hadc3.Init.SamplingMode = ADC_SAMPLING_MODE_NORMAL;
  hadc3.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadc3.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc3.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  hadc3.Init.OversamplingMode = DISABLE;
  hadc3.Init.Oversampling.Ratio = ADC3_OVERSAMPLING_RATIO_2;
  if (HAL_ADC_Init(&hadc3) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_VREFINT;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC3_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  sConfig.OffsetSign = ADC3_OFFSET_SIGN_NEGATIVE;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC3_Init 2 */

  /* USER CODE END ADC3_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOF, GPIO_PIN_2|GPIO_PIN_9, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_13|GPIO_PIN_14, GPIO_PIN_RESET);

  /*Configure GPIO pins : PF2 PF9 */
  GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /*Configure GPIO pins : PE13 PE14 */
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_14;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : PG11 PG13 */
  GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
static inline void rx_reset_state(void) { frame_active = 0; rx_idx = 0; }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3) return;

    const char c = (char)uart_rx_byte;

    if (!frame_active) {
        if (c == '<')
        {
        	frame_active = 1;
        	rx_idx = 0;
        	rx_buf[rx_idx++] = (uint8_t)c;
        }
    } else {
        if (c == '>') {
            if (rx_idx < RX_BUF_SIZE - 1) {
                rx_buf[rx_idx++] = (uint8_t)c;
                rx_buf[rx_idx]   = '\0';
                frame_ready = 1;
            }
            frame_active = 0;
        } else {
            if (rx_idx < RX_BUF_SIZE - 1) rx_buf[rx_idx++] = (uint8_t)c;
            else rx_reset_state(); // overflow: drop frame
        }
    }

    HAL_UART_Receive_IT(&huart3, &uart_rx_byte, 1);
}

// This overrides the weak BSP callback
void BSP_PB_Callback(Button_TypeDef Button)
{
    if (Button == BUTTON_USER)
    {
        static uint32_t last = 0;
        uint32_t now = HAL_GetTick();
        if ((now - last) < 80) return;
        last = now;

        /* FIRST PRESS -> WINDOW UP */
        if (state == STATE_STOP)
        {
            state = STATE_UP;
            motionTimer = 100;   // run longer
        }

        /* SECOND PRESS -> PINCH EVENT */
        else if (state == STATE_UP)
        {
            if (feature_is_enabled_ci("ANTIPINCH"))
            {
                onPinchDetected();   // triggers anti-pinch logic
            }
        }
    }
}
/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
