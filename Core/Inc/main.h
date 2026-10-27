/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h5xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED1_Pin GPIO_PIN_13
#define LED1_GPIO_Port GPIOC
#define LED2_Pin GPIO_PIN_14
#define LED2_GPIO_Port GPIOC
#define LED3_Pin GPIO_PIN_15
#define LED3_GPIO_Port GPIOC
#define Button_Pin GPIO_PIN_0
#define Button_GPIO_Port GPIOA
#define ST67CS_Pin GPIO_PIN_4
#define ST67CS_GPIO_Port GPIOA
#define ST67W_RDY_Pin GPIO_PIN_0
#define ST67W_RDY_GPIO_Port GPIOB
#define ST67W_32K_Pin GPIO_PIN_1
#define ST67W_32K_GPIO_Port GPIOB
#define ST67W_Wakeup_Pin GPIO_PIN_2
#define ST67W_Wakeup_GPIO_Port GPIOB
#define IN4_Pin GPIO_PIN_10
#define IN4_GPIO_Port GPIOB
#define IN3_Pin GPIO_PIN_12
#define IN3_GPIO_Port GPIOB
#define IN2_Pin GPIO_PIN_13
#define IN2_GPIO_Port GPIOB
#define IN1_Pin GPIO_PIN_8
#define IN1_GPIO_Port GPIOA
#define RS485_RDE_Pin GPIO_PIN_15
#define RS485_RDE_GPIO_Port GPIOA
#define OUT4_Pin GPIO_PIN_3
#define OUT4_GPIO_Port GPIOB
#define OUT1_Pin GPIO_PIN_4
#define OUT1_GPIO_Port GPIOB
#define OUT2_Pin GPIO_PIN_5
#define OUT2_GPIO_Port GPIOB
#define OUT3_Pin GPIO_PIN_8
#define OUT3_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
