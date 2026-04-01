#ifndef STM32F411_GLITCHER_H
#define STM32F411_GLITCHER_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 *  STM32F411 Bare-Metal Voltage Glitcher for LPC1768
 *  Register-level definitions — no HAL, no CMSIS headers needed
 * ============================================================ */

/* ---------- Cortex-M4 core ---------- */
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100)
#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104)
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08)
#define SYSTICK_CTRL (*(volatile uint32_t *)0xE000E010)
#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014)
#define SYSTICK_VAL (*(volatile uint32_t *)0xE000E018)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#define SCB_DEMCR (*(volatile uint32_t *)0xE000EDFC)

/* ---------- RCC ---------- */
#define RCC_BASE 0x40023800
#define RCC_CR (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_PLLCFGR (*(volatile uint32_t *)(RCC_BASE + 0x04))
#define RCC_CFGR (*(volatile uint32_t *)(RCC_BASE + 0x08))
#define RCC_AHB1ENR (*(volatile uint32_t *)(RCC_BASE + 0x30))
#define RCC_APB1ENR (*(volatile uint32_t *)(RCC_BASE + 0x40))
#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x44))

/* ---------- FLASH ---------- */
#define FLASH_ACR (*(volatile uint32_t *)0x40023C00)

/* ---------- GPIO ---------- */
typedef struct {
  volatile uint32_t MODER;
  volatile uint32_t OTYPER;
  volatile uint32_t OSPEEDR;
  volatile uint32_t PUPDR;
  volatile uint32_t IDR;
  volatile uint32_t ODR;
  volatile uint32_t BSRR;
  volatile uint32_t LCKR;
  volatile uint32_t AFRL;
  volatile uint32_t AFRH;
} GPIO_TypeDef;

#define GPIOA ((GPIO_TypeDef *)0x40020000)
#define GPIOB ((GPIO_TypeDef *)0x40020400)
#define GPIOC ((GPIO_TypeDef *)0x40020800)

/* ---------- USART ---------- */
typedef struct {
  volatile uint32_t SR;
  volatile uint32_t DR;
  volatile uint32_t BRR;
  volatile uint32_t CR1;
  volatile uint32_t CR2;
  volatile uint32_t CR3;
  volatile uint32_t GTPR;
} USART_TypeDef;

#define USART1 ((USART_TypeDef *)0x40011000)
#define USART2 ((USART_TypeDef *)0x40004400)

/* USART SR bits */
#define USART_SR_TXE (1 << 7)
#define USART_SR_RXNE (1 << 5)
#define USART_SR_TC (1 << 6)

/* ---------- TIM1 (Advanced timer — glitch pulse) ---------- */
typedef struct {
  volatile uint32_t CR1;
  volatile uint32_t CR2;
  volatile uint32_t SMCR;
  volatile uint32_t DIER;
  volatile uint32_t SR;
  volatile uint32_t EGR;
  volatile uint32_t CCMR1;
  volatile uint32_t CCMR2;
  volatile uint32_t CCER;
  volatile uint32_t CNT;
  volatile uint32_t PSC;
  volatile uint32_t ARR;
  volatile uint32_t RCR;
  volatile uint32_t CCR1;
  volatile uint32_t CCR2;
  volatile uint32_t CCR3;
  volatile uint32_t CCR4;
  volatile uint32_t BDTR;
  volatile uint32_t DCR;
  volatile uint32_t DMAR;
} TIM_TypeDef;

#define TIM1 ((TIM_TypeDef *)0x40010000)
#define TIM2 ((TIM_TypeDef *)0x40000000)
#define TIM3 ((TIM_TypeDef *)0x40000400)

/* ---------- EXTI ---------- */
#define EXTI_BASE 0x40013C00
#define EXTI_IMR (*(volatile uint32_t *)(EXTI_BASE + 0x00))
#define EXTI_RTSR (*(volatile uint32_t *)(EXTI_BASE + 0x08))
#define EXTI_FTSR (*(volatile uint32_t *)(EXTI_BASE + 0x0C))
#define EXTI_PR (*(volatile uint32_t *)(EXTI_BASE + 0x14))

#define SYSCFG_EXTICR1 (*(volatile uint32_t *)0x40013808)
#define SYSCFG_EXTICR2 (*(volatile uint32_t *)0x4001380C)

/* ============================================================
 *  Pin Assignment  (directly on STM32F411 "Black Pill" / Nucleo)
 * ============================================================
 *
 *  PA8  — TIM1_CH1  → GLITCH MOSFET gate (PWM one-pulse)
 *  PA0  — TRIGGER input (EXTI0) ← from target GPIO/UART
 *  PA1  — nRST output → target LPC1768 reset line
 *
 *  PA9  — USART1 TX → target LPC1768 RXD (ISP UART)
 *  PA10 — USART1 RX ← target LPC1768 TXD
 *
 *  PA2  — USART2 TX → PC (control/logging)
 *  PA3  — USART2 RX ← PC (control/commands)
 *
 *  PC13 — on-board LED (status)
 * ============================================================ */

/* Pin helpers */
#define PIN_GLITCH 8  /* PA8  — TIM1_CH1 */
#define PIN_TRIGGER 0 /* PA0  — EXTI      */
#define PIN_NRST 1    /* PA1  — target nRST */
#define PIN_LED 13    /* PC13 — status LED */

/* ============================================================
 *  Glitch parameters structure
 * ============================================================ */
typedef struct {
  uint32_t delay_ns;    /* delay from trigger to glitch start */
  uint32_t width_ns;    /* glitch pulse width                 */
  uint32_t delay_ticks; /* precomputed: delay in timer ticks  */
  uint32_t width_ticks; /* precomputed: width in timer ticks  */
  uint32_t repeat;      /* how many times to try per setting  */
  uint8_t armed;        /* 1 = waiting for trigger            */
} glitch_params_t;

/* ============================================================
 *  Glitch result
 * ============================================================ */
typedef enum {
  GLITCH_NORMAL = 0,  /* target responded normally       */
  GLITCH_MUTE = 1,    /* target did not respond (reset?) */
  GLITCH_SUCCESS = 2, /* unexpected / desired response   */
  GLITCH_RESET = 3,   /* target rebooted                 */
} glitch_result_t;

/* ============================================================
 *  Function prototypes
 * ============================================================ */

/* System init */
void clock_init_100mhz(void);
void gpio_init(void);
void uart_pc_init(uint32_t baud);
void uart_target_init(uint32_t baud);
void timer_glitch_init(void);
void trigger_exti_init(void);
void dwt_init(void);

/* Delay */
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

/* UART helpers */
void uart_pc_putc(char c);
void uart_pc_puts(const char *s);
void uart_pc_put_hex32(uint32_t val);
void uart_pc_put_dec(uint32_t val);
int uart_pc_getc(void);
int uart_pc_getc_timeout(uint32_t timeout_ms);

void uart_target_putc(char c);
void uart_target_puts(const char *s);
int uart_target_getc(void);
int uart_target_getc_timeout(uint32_t timeout_ms);
int uart_target_read_line(char *buf, int maxlen, uint32_t timeout_ms);

/* Glitch engine */
void glitch_set_params(glitch_params_t *p, uint32_t delay_ns,
                       uint32_t width_ns);
void glitch_arm(glitch_params_t *p);
void glitch_fire_manual(glitch_params_t *p);
void glitch_fire_on_trigger(glitch_params_t *p);

/* Target control */
void target_reset(void);
void target_power_on(void);
void target_enter_isp(void);

/* Command parser */
void cmd_process(const char *line);

/* LED */
static inline void led_on(void) {
  GPIOC->BSRR = (1 << (PIN_LED + 16));
} /* active low */
static inline void led_off(void) { GPIOC->BSRR = (1 << PIN_LED); }
static inline void led_toggle(void) { GPIOC->ODR ^= (1 << PIN_LED); }

#endif /* STM32F411_GLITCHER_H */