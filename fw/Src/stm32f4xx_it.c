#include "stm32f4xx_hal.h"
#include "dbg.h"

extern DMA_HandleTypeDef hdma_spi2_rx;

/* HardFault: PC in Backup-Register sichern; der Watchdog setzt das Board zurueck, main() schreibt den Wert in /LOG.TXT */
void hardfault_c(uint32_t *sp) { DBG_FAULT_PC = sp[6]; for (;;) {} }
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile ("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b hardfault_c\n");
}
void NMI_Handler(void) { for (;;) {} }
void MemManage_Handler(void) { HardFault_Handler(); }
void BusFault_Handler(void) { HardFault_Handler(); }
void UsageFault_Handler(void) { HardFault_Handler(); }
void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}
void SysTick_Handler(void) { HAL_IncTick(); }
void DMA1_Stream3_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_spi2_rx); }
