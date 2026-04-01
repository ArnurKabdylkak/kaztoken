/* system.c — Clock, GPIO, UART, DWT init for STM32F411 glitcher */

#include "stm32f411_glitcher.h"

/* ============================================================
 *  Clock: HSE 25 MHz → PLL → 100 MHz SYSCLK
 *  APB1 = 50 MHz, APB2 = 100 MHz
 *  (Black Pill has 25 MHz crystal; adjust PLLM if 8 MHz)
 * ============================================================ */
void clock_init_100mhz(void) {
  /* Enable HSE */
  RCC_CR |= (1 << 16); /* HSEON */
  while (!(RCC_CR & (1 << 17)))
    ; /* wait HSERDY */

  /* Flash latency: 3 WS for 100 MHz */
  FLASH_ACR = (FLASH_ACR & ~0xF) | 3 | (1 << 8) |
              (1 << 9); /* 3WS + prefetch + icache */

  /* PLL config: HSE/25 * 200 / 2 = 100 MHz
   * PLLM=25, PLLN=200, PLLP=2 (/2), PLLQ=4, PLLSRC=HSE */
  RCC_PLLCFGR = (25 << 0)    /* PLLM = 25 */
                | (200 << 6) /* PLLN = 200 */
                | (0 << 16)  /* PLLP = /2 (00) */
                | (1 << 22)  /* PLLSRC = HSE */
                | (4 << 24); /* PLLQ = 4 */

  RCC_CR |= (1 << 24); /* PLLON */
  while (!(RCC_CR & (1 << 25)))
    ; /* wait PLLRDY */

  /* APB1 = /2 (50 MHz), APB2 = /1 (100 MHz) */
  RCC_CFGR = (RCC_CFGR & ~0xFCF0) | (0x4 << 10) /* PPRE1 = /2 */
             | (0x0 << 13);                     /* PPRE2 = /1 */

  /* Switch to PLL */
  RCC_CFGR = (RCC_CFGR & ~0x3) | 0x2;
  while ((RCC_CFGR & 0xC) != 0x8)
    ; /* wait SWS = PLL */
}

/* ============================================================
 *  DWT cycle counter — for nanosecond-level timing
 *  At 100 MHz: 1 tick = 10 ns
 * ============================================================ */
void dwt_init(void) {
  SCB_DEMCR |= (1 << 24); /* TRCENA */
  DWT_CYCCNT = 0;
  DWT_CTRL |= 1; /* CYCCNTENA */
}

/* ============================================================
 *  Delays using DWT
 * ============================================================ */
void delay_us(uint32_t us) {
  uint32_t start = DWT_CYCCNT;
  uint32_t ticks = us * 100; /* 100 MHz */
  while ((DWT_CYCCNT - start) < ticks)
    ;
}

void delay_ms(uint32_t ms) {
  while (ms--)
    delay_us(1000);
}

/* ============================================================
 *  GPIO init
 * ============================================================ */
void gpio_init(void) {
  /* Enable clocks: GPIOA, GPIOB, GPIOC, SYSCFG */
  RCC_AHB1ENR |= (1 << 0) | (1 << 1) | (1 << 2);
  RCC_APB2ENR |= (1 << 14); /* SYSCFG */

  /* PA8 — AF1 (TIM1_CH1), very high speed */
  GPIOA->MODER = (GPIOA->MODER & ~(3 << (8 * 2))) | (2 << (8 * 2)); /* AF */
  GPIOA->OSPEEDR |= (3 << (8 * 2));                     /* very high */
  GPIOA->AFRH = (GPIOA->AFRH & ~(0xF << 0)) | (1 << 0); /* AF1 */

  /* PA0 — input (trigger), pull-down */
  GPIOA->MODER &= ~(3 << (0 * 2)); /* input */
  GPIOA->PUPDR =
      (GPIOA->PUPDR & ~(3 << (0 * 2))) | (2 << (0 * 2)); /* pull-down */

  /* PA1 — output push-pull (target nRST), open drain recommended */
  GPIOA->MODER = (GPIOA->MODER & ~(3 << (1 * 2))) | (1 << (1 * 2)); /* output */
  GPIOA->OTYPER |= (1 << 1); /* open drain */
  GPIOA->BSRR = (1 << 1);    /* nRST high (not asserted) */

  /* PC13 — output (LED) */
  GPIOC->MODER =
      (GPIOC->MODER & ~(3 << (13 * 2))) | (1 << (13 * 2)); /* output */
  led_off();
}

/* ============================================================
 *  USART2 — PC control interface (PA2=TX, PA3=RX)
 *  APB1 = 50 MHz
 * ============================================================ */
void uart_pc_init(uint32_t baud) {
  RCC_APB1ENR |= (1 << 17); /* USART2 clock enable */

  /* PA2 AF7, PA3 AF7 */
  GPIOA->MODER = (GPIOA->MODER & ~(3 << (2 * 2))) | (2 << (2 * 2));
  GPIOA->MODER = (GPIOA->MODER & ~(3 << (3 * 2))) | (2 << (3 * 2));
  GPIOA->AFRL = (GPIOA->AFRL & ~(0xF << (2 * 4))) | (7 << (2 * 4));
  GPIOA->AFRL = (GPIOA->AFRL & ~(0xF << (3 * 4))) | (7 << (3 * 4));

  USART2->CR1 = 0;
  USART2->BRR = 50000000 / baud;                 /* APB1=50MHz */
  USART2->CR1 = (1 << 13) | (1 << 3) | (1 << 2); /* UE, TE, RE */
}

void uart_pc_putc(char c) {
  while (!(USART2->SR & USART_SR_TXE))
    ;
  USART2->DR = c;
}

void uart_pc_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      uart_pc_putc('\r');
    uart_pc_putc(*s++);
  }
}

void uart_pc_put_hex32(uint32_t val) {
  const char hex[] = "0123456789ABCDEF";
  uart_pc_puts("0x");
  for (int i = 28; i >= 0; i -= 4)
    uart_pc_putc(hex[(val >> i) & 0xF]);
}

void uart_pc_put_dec(uint32_t val) {
  char buf[12];
  int i = 0;
  if (val == 0) {
    uart_pc_putc('0');
    return;
  }
  while (val > 0) {
    buf[i++] = '0' + (val % 10);
    val /= 10;
  }
  while (--i >= 0)
    uart_pc_putc(buf[i]);
}

int uart_pc_getc(void) {
  while (!(USART2->SR & USART_SR_RXNE))
    ;
  return USART2->DR & 0xFF;
}

int uart_pc_getc_timeout(uint32_t timeout_ms) {
  uint32_t start = DWT_CYCCNT;
  uint32_t ticks = timeout_ms * 100000; /* 100 MHz */
  while ((DWT_CYCCNT - start) < ticks) {
    if (USART2->SR & USART_SR_RXNE)
      return USART2->DR & 0xFF;
  }
  return -1;
}

/* ============================================================
 *  USART1 — Target LPC1768 (PA9=TX, PA10=RX)
 *  APB2 = 100 MHz
 * ============================================================ */
void uart_target_init(uint32_t baud) {
  RCC_APB2ENR |= (1 << 4); /* USART1 clock enable */

  /* PA9 AF7, PA10 AF7 */
  GPIOA->MODER = (GPIOA->MODER & ~(3 << (9 * 2))) | (2 << (9 * 2));
  GPIOA->MODER = (GPIOA->MODER & ~(3 << (10 * 2))) | (2 << (10 * 2));
  GPIOA->AFRH = (GPIOA->AFRH & ~(0xF << ((9 - 8) * 4))) | (7 << ((9 - 8) * 4));
  GPIOA->AFRH =
      (GPIOA->AFRH & ~(0xF << ((10 - 8) * 4))) | (7 << ((10 - 8) * 4));

  USART1->CR1 = 0;
  USART1->BRR = 100000000 / baud;                /* APB2=100MHz */
  USART1->CR1 = (1 << 13) | (1 << 3) | (1 << 2); /* UE, TE, RE */
}

void uart_target_putc(char c) {
  while (!(USART1->SR & USART_SR_TXE))
    ;
  USART1->DR = c;
}

void uart_target_puts(const char *s) {
  while (*s)
    uart_target_putc(*s++);
}

int uart_target_getc(void) {
  while (!(USART1->SR & USART_SR_RXNE))
    ;
  return USART1->DR & 0xFF;
}

int uart_target_getc_timeout(uint32_t timeout_ms) {
  uint32_t start = DWT_CYCCNT;
  uint32_t ticks = timeout_ms * 100000;
  while ((DWT_CYCCNT - start) < ticks) {
    if (USART1->SR & USART_SR_RXNE)
      return USART1->DR & 0xFF;
  }
  return -1;
}

int uart_target_read_line(char *buf, int maxlen, uint32_t timeout_ms) {
  int i = 0;
  while (i < maxlen - 1) {
    int c = uart_target_getc_timeout(timeout_ms);
    if (c < 0)
      break;
    if (c == '\r' || c == '\n') {
      if (i > 0)
        break; /* ignore leading CR/LF */
      continue;
    }
    buf[i++] = (char)c;
  }
  buf[i] = '\0';
  return i;
}