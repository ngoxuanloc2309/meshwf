#include "splc_boot_button_port.h"

#if SPLC_BOOT_BTN_ENABLE

#include <stdbool.h>
#include <stdint.h>

#include "main.h"                /* CubeMX: Button_Pin / Button_GPIO_Port, HAL, CMSIS */
#include "splc_log.h"
#include "FreeRTOS.h"
#include "task.h"

#if (SPLC_BOOT_BTN_ACTIVE_LEVEL < -1) || (SPLC_BOOT_BTN_ACTIVE_LEVEL > 1)
#error "SPLC_BOOT_BTN_ACTIVE_LEVEL must be -1 (auto), 0 or 1"
#endif

static const char *TAG = "BOOT_BTN";

#define SAMPLE_PERIOD_MS   10U

static bool     s_inited;
static uint8_t  s_idle_level;     /* pin level when the button is released */
static bool     s_pressing;
static uint32_t s_press_start_ms;
static uint32_t s_next_sample_ms;

static uint8_t read_level(void)
{
    return (HAL_GPIO_ReadPin(Button_GPIO_Port, Button_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/* Jump to the ROM bootloader. Never returns.
 *
 * The last steps are inline assembly on purpose: this runs inside a FreeRTOS task, i.e. on PSP.
 * The bootloader expects Thread mode on MSP, so CONTROL.SPSEL must be cleared; once SP changes
 * the C locals (addressed from SP) are gone, so the three values travel in registers. */
static void __attribute__((noreturn)) enter_bootloader_mode(void)
{
    const uint32_t boot_msp   = *(volatile uint32_t *)SYSTEM_BOOTLOADER_ADDRESS;
    const uint32_t boot_reset = *(volatile uint32_t *)(SYSTEM_BOOTLOADER_ADDRESS + 4U);

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

#if defined(HAL_ICACHE_MODULE_ENABLED)
    (void)HAL_ICACHE_Disable();
#endif
    HAL_RCC_DeInit();
    HAL_DeInit();

    for (uint32_t i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;   /* RTOS tick/switch not left pending */

    SCB->VTOR = SYSTEM_BOOTLOADER_ADDRESS;

    /* Cortex-M33: remove any stack limit that could block the ROM bootloader. */
    __set_MSPLIM(0U);
    __set_PSPLIM(0U);

    __DSB();
    __ISB();

    __asm volatile (
        "msr control, %2   \n"
        "isb               \n"
        "msr msp, %0       \n"
        "bx  %1            \n"
        :
        : "r"(boot_msp), "r"(boot_reset), "r"(0U)
        : "memory");

    for (;;) {
        __NOP();
    }
}

void splc_boot_button_poll(uint32_t now_ms)
{
    if (s_inited && (int32_t)(now_ms - s_next_sample_ms) < 0) {
        return;
    }
    s_next_sample_ms = now_ms + SAMPLE_PERIOD_MS;

    const uint8_t level = read_level();

    if (!s_inited) {
        s_inited = true;
#if SPLC_BOOT_BTN_ACTIVE_LEVEL < 0
        s_idle_level = level;
        log_info(TAG, "button PA0: auto polarity, released level = %u (hold %u ms = bootloader; "
                      "do not hold it at power-up)", (unsigned)s_idle_level,
                 (unsigned)SPLC_BOOT_BTN_HOLD_MS);
#else
        s_idle_level = (SPLC_BOOT_BTN_ACTIVE_LEVEL == 1) ? 0U : 1U;
        log_info(TAG, "button PA0: fixed polarity, pressed level = %d (hold %u ms = bootloader)",
                 (int)SPLC_BOOT_BTN_ACTIVE_LEVEL, (unsigned)SPLC_BOOT_BTN_HOLD_MS);
#endif
        return;
    }

    const bool pressed = (level != s_idle_level);

    if (!pressed) {
        if (s_pressing) {
            log_debug(TAG, "button released after %u ms", (unsigned)(now_ms - s_press_start_ms));
            s_pressing = false;
        }
        return;
    }

    if (!s_pressing) {
        s_pressing       = true;
        s_press_start_ms = now_ms;
        log_debug(TAG, "button pressed");
        return;
    }

    if ((int32_t)(now_ms - s_press_start_ms) >= (int32_t)SPLC_BOOT_BTN_HOLD_MS) {
        log_info(TAG, "button held >= %u ms: jumping to the system bootloader at 0x%08lX",
                 (unsigned)SPLC_BOOT_BTN_HOLD_MS, (unsigned long)SYSTEM_BOOTLOADER_ADDRESS);
        vTaskDelay(pdMS_TO_TICKS(100U));        /* let the log UART drain */
        enter_bootloader_mode();
    }
}

#endif /* SPLC_BOOT_BTN_ENABLE */