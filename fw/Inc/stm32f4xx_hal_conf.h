#ifndef STM32F4xx_HAL_CONF_H
#define STM32F4xx_HAL_CONF_H
#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_I2S_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_RTC_MODULE_ENABLED
#define HAL_SD_MODULE_ENABLED

#ifndef HSE_VALUE
#define HSE_VALUE              8000000U   /* WeAct F446: 8 MHz (per -DHSE_VALUE=... ueberschreibbar) */
#endif
#define HSE_STARTUP_TIMEOUT    100U
#define HSI_VALUE              16000000U
#define LSI_VALUE              32000U
#define LSE_VALUE              32768U
#define LSE_STARTUP_TIMEOUT    5000U
#define EXTERNAL_CLOCK_VALUE   12288000U
#define VDD_VALUE              3300U
#define TICK_INT_PRIORITY      15U
#define USE_RTOS               0U
#define PREFETCH_ENABLE        1U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE      1U

#define USE_HAL_I2S_REGISTER_CALLBACKS 0U
#define USE_HAL_SD_REGISTER_CALLBACKS  0U
#define USE_HAL_RTC_REGISTER_CALLBACKS 0U
#define USE_SD_TRANSCEIVER             0U

#include "stm32f4xx_hal_rcc.h"
#include "stm32f4xx_hal_gpio.h"
#include "stm32f4xx_hal_dma.h"
#include "stm32f4xx_hal_cortex.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_i2s.h"
#include "stm32f4xx_hal_pwr.h"
#include "stm32f4xx_hal_rtc.h"
#include "stm32f4xx_hal_sd.h"

#define assert_param(expr) ((void)0U)

#ifdef __cplusplus
}
#endif
#endif
