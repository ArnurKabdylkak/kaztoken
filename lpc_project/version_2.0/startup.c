/**
 * startup.c — минимальный стартап для LPC1768 (Cortex-M3)
 *
 * Выполняет три задачи до вызова main():
 *   1. Копирует секцию .data из Flash в SRAM
 *   2. Обнуляет .bss
 *   3. Вызывает main()
 *
 * Таблица векторов содержит только те записи, которые нужны для
 * bare-metal примера (Reset + HardFault + Default).
 */

#include <stdint.h>

/* Символы из linker.ld */
extern uint32_t _data_load;
extern uint32_t _data_start;
extern uint32_t _data_end;
extern uint32_t _bss_start;
extern uint32_t _bss_end;
extern uint32_t _stack_top;

/* Прототип main */
extern int main(void);

/* ---- Обработчики по умолчанию ---- */
void Default_Handler(void) __attribute__((weak));
void Default_Handler(void) { while (1); }

/* HardFault — бесконечный цикл для отладки */
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void NMI_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)__attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)   __attribute__((weak, alias("Default_Handler")));

/* ---- Reset handler ---- */
void Reset_Handler(void)
{
    /* 1. Копируем .data из Flash (LMA) → SRAM (VMA) */
    uint32_t *src = &_data_load;
    uint32_t *dst = &_data_start;
    while (dst < &_data_end) {
        *dst++ = *src++;
    }

    /* 2. Обнуляем .bss */
    dst = &_bss_start;
    while (dst < &_bss_end) {
        *dst++ = 0u;
    }

    /* 3. Передаём управление main */
    main();

    /* На случай возврата из main */
    while (1);
}

/* ---- Таблица векторов Cortex-M3 ---- */
/* Должна лежать в секции .isr_vector → попадёт на адрес 0x0000_0000 */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) =
{
    /* 0: начальный стек (указатель, а не функция — приводим тип) */
    (void (*)(void))&_stack_top,

    /* 1: Reset */
    Reset_Handler,

    /* 2-14: системные исключения Cortex-M3 */
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    Default_Handler,    /* зарезервировано */
    Default_Handler,
    Default_Handler,
    Default_Handler,
    SVC_Handler,
    Default_Handler,    /* Debug Monitor */
    Default_Handler,
    PendSV_Handler,
    SysTick_Handler,

    /* 16+: внешние прерывания LPC1768 — все → Default_Handler */
    /* IRQ0  WDT     */ Default_Handler,
    /* IRQ1  TIMER0  */ Default_Handler,
    /* IRQ2  TIMER1  */ Default_Handler,
    /* IRQ3  TIMER2  */ Default_Handler,
    /* IRQ4  TIMER3  */ Default_Handler,
    /* IRQ5  UART0   */ Default_Handler,
    /* IRQ6  UART1   */ Default_Handler,
    /* IRQ7  UART2   */ Default_Handler,
    /* IRQ8  UART3   */ Default_Handler,
    /* IRQ9  PWM1    */ Default_Handler,
    /* IRQ10 I2C0    */ Default_Handler,
    /* IRQ11 I2C1    */ Default_Handler,
    /* IRQ12 I2C2    */ Default_Handler,
    /* IRQ13 SPI     */ Default_Handler,
    /* IRQ14 SSP0    */ Default_Handler,
    /* IRQ15 SSP1    */ Default_Handler,
    /* IRQ16 PLL0    */ Default_Handler,
    /* IRQ17 RTC     */ Default_Handler,
    /* IRQ18 EINT0   */ Default_Handler,
    /* IRQ19 EINT1   */ Default_Handler,
    /* IRQ20 EINT2   */ Default_Handler,
    /* IRQ21 EINT3   */ Default_Handler,
    /* IRQ22 ADC     */ Default_Handler,
    /* IRQ23 BOD     */ Default_Handler,
    /* IRQ24 USB     */ Default_Handler,
    /* IRQ25 CAN     */ Default_Handler,
    /* IRQ26 GPDMA   */ Default_Handler,
    /* IRQ27 I2S     */ Default_Handler,
    /* IRQ28 ENET    */ Default_Handler,
    /* IRQ29 RIT     */ Default_Handler,
    /* IRQ30 MCPWM   */ Default_Handler,
    /* IRQ31 QEI     */ Default_Handler,
    /* IRQ32 PLL1    */ Default_Handler,
};