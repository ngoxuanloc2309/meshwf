/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    logging_config_template.h
  * @author  ST67 Application Team
  * @brief   Header file for the W6X Logging configuration module
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
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
#ifndef LOGGING_CONFIG_TEMPLATE_H
#define LOGGING_CONFIG_TEMPLATE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Includes ------------------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported constants --------------------------------------------------------*/
/*
 * All available configuration defines can be found in
 * Middlewares\ST\ST67W6X_Network_Driver\Conf\logging_config_template.h
 */

/** Global verbosity level (LOG_NONE, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG) */
#define LOG_LEVEL                               LOG_DEBUG

/** Enable 'task name' metadata in the log */
#define LOG_INCLUDE_TASKNAME                    1

/** Enable 'file name' + 'line number' metadata in the log */
#define LOG_INCLUDE_FILENAME                    1

/** Enable 'timestamp' metadata in the log */
#define LOG_INCLUDE_TIMESTAMP                   1

/** Max metadata length */
#define LOG_INCLUDE_MAX_LENGTH                  100U

/** Max message length */
#define MAX_LOG_MESSAGE_LENGTH                  2000U

/** Max queue length */
#define MAX_LOG_QUEUE_LENGTH                    200U

/** Max log level */
#define MAX_LOG_LEVEL                           LOG_DEBUG

/** Max log level */
#define MAX_LOG_MEMORY                          4096U

/** Log id. must be unique per queue producer */
#define LOG_MESSAGE_ID                          1U

/** Log thread stack size.
 * NOTE: xTaskCreate uses (LOG_THREAD_STACK_SIZE >> 2) words. At 256 that is
 * only 64 words which causes a stack overflow when OutputTask calls printf/
 * malloc/sprintf – crashing FreeRTOS and freezing the whole system.
 * 2048 → 512 words, safely covers the stack usage. */
#define LOG_THREAD_STACK_SIZE                   2048U

/** Log thread priority */
#ifndef LOG_THREAD_PRIO
#define LOG_THREAD_PRIO                         6
#endif

/** Log memory allocator */
#define LOG_MALLOC                              pvPortMalloc

/** Log memory deallocator */
#define LOG_FREE                                vPortFree

/* USER CODE BEGIN EC */

/* USER CODE END EC */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LOGGING_CONFIG_TEMPLATE_H */
