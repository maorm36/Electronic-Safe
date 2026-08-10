/* USER CODE BEGIN Header */

// Maor Mordo & Buraq Yassin

/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
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

#include <string.h>
#include "lcd.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

#define UART_TX_BUF_SIZE 256                // The buffer size for passing data
#define CODE_LEN 4                          // master code and user code are exactly 4 digits
#define TIMEOUT_MS 5000                     // 5 seconds for a timeout
#define TIMEOUT_TICKS (TIMEOUT_MS * 10)     // each millisecond is 10 ticks so there is 0.1ms resolution
#define BLUE_RESET_TICKS 20000              // 2 seconds for blue button reset
#define TIMER_PERIOD 60000                  // The timer period for the wrap-around calculation: 0,1,...,N-1 (N=60000)
                                            // It is used for calculating the remaining ticks from start to the wrap
                                            // TIMER_PERIOD - start Plus ticks after wrap up to now

typedef enum {
	SETUP_UART,
	LOCKED_IDLE,
	LOCKED_INPUT,
	UNLOCKED,
	LOCKED_ERROR,
	LOCKED_TIMEOUT
} SafeState;

typedef enum {
	BTN_NONE = 5,   // exists for the state of none button pressed so MUST NOT collide with 0-4
	BTN_SELECT = 0,
	BTN_LEFT = 1,
	BTN_UP = 2,
	BTN_DOWN = 3,
	BTN_RIGHT = 4
} Button;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

DMA_HandleTypeDef hdma_lpuart1_tx;
ADC_HandleTypeDef hadc1;
UART_HandleTypeDef hlpuart1;
TIM_HandleTypeDef htim2;

static uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static volatile uint8_t uart_tx_busy = 0;

static volatile uint8_t uart_pending_code_set_msg = 0; // a flag that is used in order to print the "Code set"
                                                       // confirmation message at the right time

static const char uart_code_set_msg[] = "\r\nCode Set! Safe Locked.\r\n";

static volatile SafeState state;

/* Codes */
static uint8_t master_code[CODE_LEN];
static uint8_t user_code[CODE_LEN];

/* Indexes */
static volatile uint8_t uart_index;
static volatile uint8_t input_index;

/* Timing */
static uint32_t t_start;
static volatile uint8_t timeout_latched;

/* UART */
static volatile uint8_t uart_rx;

/* Blue button reset */
static uint32_t blue_btn_start;
static uint8_t blue_pressed;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

/* USER CODE BEGIN PFP */

static void MX_DMA_Init(void);
static HAL_StatusTypeDef UART_SendDMA(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t len);
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ICACHE_Init(void);
static void MX_ADC1_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_TIM2_Init(void);
static uint16_t ADC_ReadOnce(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ===== Timer helpers ===== */
static uint32_t TIMER_NOW(void) {
	return __HAL_TIM_GET_COUNTER(&htim2);
}

// returns a value in ticks
static uint32_t ELAPSED(uint32_t start, uint32_t now) {
	if (now >= start) {
		return now - start;
	} else {
		/* Timer wrapped around */
		return (TIMER_PERIOD - start) + now;
	}
}

/* ===== LCD screens ===== */
static void LCD_ShowLockedEmpty(void) {
	LCD_Clear();
	LCD_SetCursor(0, 0);
	LCD_Print("LOCKED");
	LCD_SetCursor(0, 1);
	LCD_Print("CODE:----");
}

static void LCD_ShowProgress(uint8_t pos) {
	LCD_SetCursor(5 + pos, 1);
	LCD_WriteChar('*');
}

static void LCD_ShowUnlocked(void) {
	LCD_Clear();
	LCD_SetCursor(0, 0);
	LCD_Print("UNLOCKED");
	LCD_SetCursor(0, 1);
	LCD_Print("OK");
}

static void LCD_ShowError(void) {
	LCD_Clear();
	LCD_SetCursor(0, 0);
	LCD_Print("LOCKED");
	LCD_SetCursor(0, 1);
	LCD_Print("ERROR");
}

static void LCD_ShowTimeout(void) {
	LCD_Clear();
	LCD_SetCursor(0, 0);
	LCD_Print("LOCKED");
	LCD_SetCursor(0, 1);
	LCD_Print("TIMEOUT");
}

/* ===== Compare codes ===== */
static uint8_t CompareCodes(void) {
	for (uint8_t i = 0; i < CODE_LEN; i++) {
		if (master_code[i] != user_code[i])
			return 0;
	}
	return 1;
}

/* ===== Return to idle after error/timeout ===== */
static void ReturnToLockedIdle(void) {
	input_index = 0;
	timeout_latched = 0;
	state = LOCKED_IDLE;
	LCD_ShowLockedEmpty();
}

/* ===== Read LCD button using ADC (raw) ===== */
static Button Read_LCD_Button_Raw(void) {
	uint16_t adc_value = ADC_ReadOnce();

	if (adc_value < 50) {
		return BTN_RIGHT;
	} else if (adc_value < 500) {
		return BTN_UP;
	} else if (adc_value < 1100) {
		return BTN_DOWN;
	} else if (adc_value < 1600) {
		return BTN_LEFT;
	} else if (adc_value < 2500) {
		return BTN_SELECT;
	} else {
		return BTN_NONE;
	}
}

/* ===== Buttons handler with edge detect ===== */
static void CheckButtons(void) {
	static Button last_btn = BTN_NONE;
	Button current = Read_LCD_Button_Raw();

	if ((current != BTN_NONE) && (last_btn == BTN_NONE)) {


		if ((state == LOCKED_ERROR) || (state == LOCKED_TIMEOUT)) {


			ReturnToLockedIdle();


		} else if ((state == LOCKED_IDLE) || (state == LOCKED_INPUT)) {


			if (state == LOCKED_IDLE) {
				state = LOCKED_INPUT;
				t_start = TIMER_NOW();
				input_index = 0;
				timeout_latched = 0;
				LCD_ShowLockedEmpty();
			}


			if (input_index < CODE_LEN) {
				user_code[input_index] = (uint8_t) current;
				LCD_ShowProgress(input_index);
				input_index++;

				if (input_index == CODE_LEN) {
					if (CompareCodes()) {
						state = UNLOCKED;
						LCD_ShowUnlocked();
					} else {
						state = LOCKED_ERROR;
						LCD_ShowError();
					}
				}
			}


		}
	}

	last_btn = current;
}

/* ===== Logical reset ===== */
static void Safe_Reset(void) {
	state = SETUP_UART;
	uart_index = 0;
	input_index = 0;
	timeout_latched = 0;
	blue_pressed = 0;

	char banner[] =
	        "\r\nSET CODE (4 digits, 0-4):\r\n"
	        "0=SELECT,\r\n"
	        "1=LEFT,\r\n"
	        "2=UP,\r\n"
	        "3=DOWN,\r\n"
	        "4=RIGHT\r\n";

	UART_SendDMA(&hlpuart1, (const uint8_t*)banner, (uint16_t)(sizeof(banner) - 1));

	/* Start UART receive interrupt */
	HAL_UART_Receive_IT(&hlpuart1, (uint8_t*) &uart_rx, 1);
}

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

	/* USER CODE BEGIN 1 */

	/* USER CODE END 1 */

	/* MCU Configuration--------------------------------------------------------*/

	/* Reset of all peripherals, Initializes the Flash interface and the Systick. */
	HAL_Init();

	/* USER CODE BEGIN Init */

	/* USER CODE END Init */

	/* Configure the system clock */
	SystemClock_Config();

	/* USER CODE BEGIN SysInit */

	/* USER CODE END SysInit */

	/* Initialize all configured peripherals */
	MX_DMA_Init();
	MX_GPIO_Init();
	MX_ICACHE_Init();
	MX_ADC1_Init();
	MX_LPUART1_UART_Init();
	MX_TIM2_Init();

	/* USER CODE BEGIN 2 */

	LCD_Init();

	/* ADC calibration (recommended once at startup) */
	(void) HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);

	/* Start TIM2 for time measurement */
	HAL_TIM_Base_Start(&htim2);

	/* Initialize safe - show banner and start UART receive */
	Safe_Reset();

	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */
	while (1) {
		/* USER CODE END WHILE */

		/* USER CODE BEGIN 3 */

		/* Send pending UART confirmations (non-blocking) */
		if (uart_pending_code_set_msg && (uart_tx_busy == 0)) {
			uart_pending_code_set_msg = 0;
			UART_SendDMA(&hlpuart1,
					(const uint8_t*)uart_code_set_msg,
					(uint16_t)(sizeof(uart_code_set_msg) - 1));
		}

		/* Check timeout */
		if ((state == LOCKED_INPUT) && (timeout_latched == 0)) {
			if (ELAPSED(t_start, TIMER_NOW()) > TIMEOUT_TICKS) {
				timeout_latched = 1;
				state = LOCKED_TIMEOUT;
				LCD_ShowTimeout();
			}
		}

		/* Check LCD buttons */
		CheckButtons();

		/* Blue button long press detection (polled) */
		if (HAL_GPIO_ReadPin(BLUE_BUTTON_GPIO_Port, BLUE_BUTTON_Pin) == GPIO_PIN_SET) {
			if (blue_pressed == 0) {
				blue_pressed = 1;
				blue_btn_start = TIMER_NOW();
			} else {
				if (ELAPSED(blue_btn_start, TIMER_NOW()) > BLUE_RESET_TICKS) {
					/* Long press detected - reset to LOCKED state (preserve master code) */
					if (state != SETUP_UART) {
						ReturnToLockedIdle();
					}
					blue_pressed = 0; /* Reset to allow new detection */
				}
			}
		} else {
			blue_pressed = 0;
		}
	}

	/* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/** Configure the main internal regulator output voltage */
	if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE0) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the RCC Oscillators according to the specified parameters
	 * in the RCC_OscInitTypeDef structure. */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_MSI;
	RCC_OscInitStruct.MSIState = RCC_MSI_ON;
	RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
	RCC_OscInitStruct.PLL.PLLM = 1;
	RCC_OscInitStruct.PLL.PLLN = 55;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
	RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
	RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the CPU, AHB and APB buses clocks */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			| RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
		Error_Handler();
	}
}

/**
 * @brief ADC1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_ADC1_Init(void) {

	/* USER CODE BEGIN ADC1_Init 0 */

	/* USER CODE END ADC1_Init 0 */

	ADC_MultiModeTypeDef multimode = { 0 };
	ADC_ChannelConfTypeDef sConfig = { 0 };

	/* USER CODE BEGIN ADC1_Init 1 */

	/* USER CODE END ADC1_Init 1 */

	/** Common config */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc1.Init.LowPowerAutoWait = DISABLE;
	hadc1.Init.ContinuousConvMode = DISABLE;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	hadc1.Init.DMAContinuousRequests = DISABLE;
	hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
	hadc1.Init.OversamplingMode = DISABLE;
	if (HAL_ADC_Init(&hadc1) != HAL_OK) {
		Error_Handler();
	}

	/** Configure the ADC multi-mode */
	multimode.Mode = ADC_MODE_INDEPENDENT;
	if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK) {
		Error_Handler();
	}

	/** Configure Regular Channel */
	sConfig.Channel = ADC_CHANNEL_7;
	sConfig.Rank = ADC_REGULAR_RANK_1;
	sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
	sConfig.SingleDiff = ADC_SINGLE_ENDED;
	sConfig.OffsetNumber = ADC_OFFSET_NONE;
	sConfig.Offset = 0;
	if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
		Error_Handler();
	}

	/* USER CODE BEGIN ADC1_Init 2 */

	/* USER CODE END ADC1_Init 2 */
}

/**
 * @brief ICACHE Initialization Function
 * @param None
 * @retval None
 */
static void MX_ICACHE_Init(void) {

	/* USER CODE BEGIN ICACHE_Init 0 */

	/* USER CODE END ICACHE_Init 0 */

	/* USER CODE BEGIN ICACHE_Init 1 */

	/* USER CODE END ICACHE_Init 1 */

	/** Enable instruction cache in 1-way (direct mapped cache) */
	if (HAL_ICACHE_ConfigAssociativityMode(ICACHE_1WAY) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_ICACHE_Enable() != HAL_OK) {
		Error_Handler();
	}

	/* USER CODE BEGIN ICACHE_Init 2 */

	/* USER CODE END ICACHE_Init 2 */
}

/**
 * @brief LPUART1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_LPUART1_UART_Init(void) {

	/* USER CODE BEGIN LPUART1_Init 0 */

	/* USER CODE END LPUART1_Init 0 */

	/* USER CODE BEGIN LPUART1_Init 1 */

	/* USER CODE END LPUART1_Init 1 */

	hlpuart1.Instance = LPUART1;
	hlpuart1.Init.BaudRate = 115200;
	hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
	hlpuart1.Init.StopBits = UART_STOPBITS_1;
	hlpuart1.Init.Parity = UART_PARITY_NONE;
	hlpuart1.Init.Mode = UART_MODE_TX_RX;
	hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
	hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	hlpuart1.FifoMode = UART_FIFOMODE_DISABLE;
	if (HAL_UART_Init(&hlpuart1) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK) {
		Error_Handler();
	}

	/* USER CODE BEGIN LPUART1_Init 2 */

	/* USER CODE END LPUART1_Init 2 */
}

/**
 * @brief TIM2 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM2_Init(void) {

	/* USER CODE BEGIN TIM2_Init 0 */

	/* USER CODE END TIM2_Init 0 */

	TIM_ClockConfigTypeDef sClockSourceConfig = { 0 };
	TIM_MasterConfigTypeDef sMasterConfig = { 0 };

	/* USER CODE BEGIN TIM2_Init 1 */

	/* USER CODE END TIM2_Init 1 */

	htim2.Instance = TIM2;
	htim2.Init.Prescaler = 11000 - 1;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim2.Init.Period = 60000 - 1;
	htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
		Error_Handler();
	}
	sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) {
		Error_Handler();
	}
	sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
	sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) {
		Error_Handler();
	}

	/* USER CODE BEGIN TIM2_Init 2 */

	/* USER CODE END TIM2_Init 2 */
}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };

	/* USER CODE BEGIN MX_GPIO_Init_1 */

	/* USER CODE END MX_GPIO_Init_1 */

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOF_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	HAL_PWREx_EnableVddIO2();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOF, LCD_RS_Pin | LCD_D7_Pin | LCD_D4_Pin, GPIO_PIN_RESET);

	/* Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOE, LCD_D6_Pin | LCD_D5_Pin, GPIO_PIN_RESET);

	/* Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOD, LCD_BL_Pin | LCD_E_Pin, GPIO_PIN_RESET);

	/* Configure GPIO pin : BLUE_BUTTON_Pin (simple input, polled) */
	GPIO_InitStruct.Pin = BLUE_BUTTON_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(BLUE_BUTTON_GPIO_Port, &GPIO_InitStruct);

	/* Configure GPIO pins : LCD_RS_Pin LCD_D7_Pin LCD_D4_Pin */
	GPIO_InitStruct.Pin = LCD_RS_Pin | LCD_D7_Pin | LCD_D4_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

	/* Configure GPIO pins : LCD_D6_Pin LCD_D5_Pin */
	GPIO_InitStruct.Pin = LCD_D6_Pin | LCD_D5_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

	/* Configure GPIO pins : LCD_BL_Pin LCD_E_Pin */
	GPIO_InitStruct.Pin = LCD_BL_Pin | LCD_E_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

	/* USER CODE BEGIN MX_GPIO_Init_2 */

	/* USER CODE END MX_GPIO_Init_2 */
}


/**
  * @brief DMA Initialization Function
  * @param None
  * @retval None
  */
static void MX_DMA_Init(void)
{
  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}
/* USER CODE BEGIN 4 */

/* Send data via UART DMA */
static HAL_StatusTypeDef UART_SendDMA(UART_HandleTypeDef *huart,
                                      const uint8_t *data,
                                      uint16_t len)
{
    if (len == 0U)
        return HAL_OK;

    /* Copy to buffer */
    if (len > UART_TX_BUF_SIZE)
        len = UART_TX_BUF_SIZE;

    if (uart_tx_busy)
        return HAL_BUSY;

    memcpy(uart_tx_buf, data, len);

    uart_tx_busy = 1U;
    HAL_StatusTypeDef st = HAL_UART_Transmit_DMA(huart, uart_tx_buf, len);
    if (st != HAL_OK)
    {
        uart_tx_busy = 0U;
    }
    return st;
}

/**
 * @brief Read ADC value once
 * @retval ADC value (0-4095)
 */
static uint16_t ADC_ReadOnce(void) {
	uint16_t value = 0;
	if (HAL_ADC_Start(&hadc1) == HAL_OK) {
		if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
			value = (uint16_t) HAL_ADC_GetValue(&hadc1);
		}
		(void) HAL_ADC_Stop(&hadc1);
	}
	return value;
}

/**
 * @brief UART Rx Complete Callback
 * @param huart UART handle
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance != LPUART1) {
		return;
	}

	/* Only process UART input during SETUP_UART state */
	if (state == SETUP_UART) {
		if ((uart_rx >= '0') && (uart_rx <= '4')) {

			// (X - '0') is a standard C trick to convert an ASCII digit character into its numeric value
			// '0' - '0' = 48 - 48 = 0
            // '1' - '0' = 49 - 48 = 1
			// '4' - '0' = 52 - 48 = 4
			master_code[uart_index++] = (uint8_t) (uart_rx - '0');

			/* Echo the received character (non-blocking DMA) */
			UART_SendDMA(&hlpuart1, (const uint8_t*)&uart_rx, 1);

			if (uart_index == CODE_LEN) {
				input_index = 0;
				timeout_latched = 0;

				state = LOCKED_IDLE;
				LCD_ShowLockedEmpty();

				uart_pending_code_set_msg = 1;

				return;
			}
		}

		HAL_UART_Receive_IT(&hlpuart1, (uint8_t*) &uart_rx, 1);
	}
}

/* TX done - clear busy flag */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1)
    {
        uart_tx_busy = 0;
    }
}

/* UART error - clear busy flag */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1)
    {
        uart_tx_busy = 0;
    }
}

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
	/* USER CODE BEGIN Error_Handler_Debug */
	__disable_irq();
	while (1) {
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
