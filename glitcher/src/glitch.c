/* glitch.c — Voltage glitch engine using TIM1 one-pulse mode
 *
 * TIM1 runs at 100 MHz (APB2 timer clock, prescaler=0).
 * 1 tick = 10 ns.
 *
 * TIM1_CH1 (PA8) drives the MOSFET gate:
 *   - Normally LOW  → MOSFET off → target VCC is normal
 *   - Pulse HIGH    → MOSFET on  → target VCC pulled to GND (glitch)
 *
 * Sequence:
 *   1) Trigger event (EXTI0 or manual)
 *   2) DWT delay for coarse delay (microseconds)
 *   3) TIM1 one-pulse for fine delay + pulse width
 *      ARR  = delay_ticks + width_ticks
 *      CCR1 = delay_ticks   (rising edge)
 *      Pulse width = ARR - CCR1 = width_ticks
 */

#include "stm32f411_glitcher.h"

/* Global glitch parameters */
static volatile glitch_params_t *g_params = 0;
static volatile uint8_t g_triggered = 0;

/* ============================================================
 *  Convert ns to timer ticks (100 MHz → 10 ns/tick)
 * ============================================================ */
static uint32_t ns_to_ticks(uint32_t ns) {
  /* Round to nearest 10 ns */
  return (ns + 5) / 10;
}

/* ============================================================
 *  Set glitch parameters
 * ============================================================ */
void glitch_set_params(glitch_params_t *p, uint32_t delay_ns,
                       uint32_t width_ns) {
  p->delay_ns = delay_ns;
  p->width_ns = width_ns;
  p->delay_ticks = ns_to_ticks(delay_ns);
  p->width_ticks = ns_to_ticks(width_ns);

  /* Clamp minimum */
  if (p->width_ticks < 1)
    p->width_ticks = 1;
  if (p->delay_ticks < 1)
    p->delay_ticks = 1;
}

/* ============================================================
 *  Init TIM1 for one-pulse mode on CH1 (PA8)
 *
 *  PWM mode 1, OPM, output active high through MOSFET gate.
 *  The pulse starts after CCR1 ticks and lasts (ARR-CCR1) ticks.
 * ============================================================ */
void timer_glitch_init(void) {
  RCC_APB2ENR |= (1 << 0); /* TIM1 clock */

  TIM1->CR1 = 0;
  TIM1->CR2 = 0;
  TIM1->SMCR = 0;
  TIM1->PSC = 0; /* 100 MHz tick (10 ns) */

  /* CH1: PWM mode 2 (output high when CNT >= CCR1)
   * OC1M = 111 (PWM mode 2), OC1PE = 1 */
  TIM1->CCMR1 = (7 << 4) | (1 << 3);

  /* Enable CH1 output, active high */
  TIM1->CCER = (1 << 0); /* CC1E */

  /* BDTR: MOE (main output enable), must be set for TIM1 */
  TIM1->BDTR = (1 << 15); /* MOE */

  /* One-pulse mode */
  TIM1->CR1 |= (1 << 3); /* OPM */

  /* Start with output LOW */
  TIM1->CCR1 = 0xFFFF;
  TIM1->ARR = 0xFFFF;
}

/* ============================================================
 *  Fire a single glitch pulse (blocking)
 *
 *  Uses TIM1 one-pulse: after trigger, waits delay_ticks,
 *  then outputs HIGH for width_ticks, then stops.
 * ============================================================ */
void glitch_fire_manual(glitch_params_t *p) {
  /* Disable timer */
  TIM1->CR1 &= ~1;
  TIM1->CNT = 0;

  /* Configure timing:
   * ARR  = delay + width  (total period)
   * CCR1 = delay          (pulse starts here)
   * Pulse width = ARR - CCR1 = width_ticks
   */
  uint32_t total = p->delay_ticks + p->width_ticks;
  if (total > 0xFFFF)
    total = 0xFFFF; /* 16-bit timer limit */

  TIM1->ARR = total;
  TIM1->CCR1 = p->delay_ticks;

  /* Force update to load shadow registers */
  TIM1->EGR = 1; /* UG */

  /* Clear update flag */
  TIM1->SR = 0;

  /* Start! (one-pulse will auto-stop) */
  TIM1->CR1 |= 1; /* CEN */

  /* Wait for completion */
  while (TIM1->CR1 & 1)
    ;

  /* Ensure output is LOW */
  /* (OPM mode should return to inactive level) */
}

/* ============================================================
 *  EXTI0 trigger setup (PA0 rising edge)
 * ============================================================ */
void trigger_exti_init(void) {
  /* SYSCFG: EXTI0 → PA0 */
  SYSCFG_EXTICR1 = (SYSCFG_EXTICR1 & ~0xF) | 0x0; /* PA0 */

  /* EXTI0: rising edge, unmask */
  EXTI_RTSR |= (1 << 0);
  EXTI_IMR |= (1 << 0);

  /* Enable EXTI0 IRQ (position 6) in NVIC */
  NVIC_ISER0 |= (1 << 6);
}

/* ============================================================
 *  Arm: wait for trigger, then fire glitch
 * ============================================================ */
void glitch_arm(glitch_params_t *p) {
  g_params = p;
  g_triggered = 0;
  p->armed = 1;

  /* Pre-configure timer so ISR is fast */
  TIM1->CR1 &= ~1;
  TIM1->CNT = 0;

  uint32_t total = p->delay_ticks + p->width_ticks;
  if (total > 0xFFFF)
    total = 0xFFFF;

  TIM1->ARR = total;
  TIM1->CCR1 = p->delay_ticks;
  TIM1->EGR = 1;
  TIM1->SR = 0;

  led_on(); /* indicate armed */
}

/* ============================================================
 *  EXTI0 ISR — trigger fires the glitch immediately
 *
 *  This must be as fast as possible.
 *  The timer was pre-configured in glitch_arm().
 * ============================================================ */
void EXTI0_IRQHandler(void) __attribute__((section(".text")));
void EXTI0_IRQHandler(void) {
  /* Clear EXTI pending */
  EXTI_PR = (1 << 0);

  if (g_params && g_params->armed) {
    /* FIRE! Start pre-configured timer */
    TIM1->CNT = 0;
    TIM1->CR1 |= 1; /* CEN — one-pulse starts */

    g_params->armed = 0;
    g_triggered = 1;
    led_off();
  }
}

/* ============================================================
 *  Wait for trigger to fire (blocking with timeout)
 *  Returns 1 if triggered, 0 if timeout
 * ============================================================ */
static int wait_trigger(uint32_t timeout_ms) {
  uint32_t start = DWT_CYCCNT;
  uint32_t ticks = timeout_ms * 100000;
  while ((DWT_CYCCNT - start) < ticks) {
    if (g_triggered)
      return 1;
  }
  return 0;
}

/* ============================================================
 *  Combined: arm + wait + report
 * ============================================================ */
void glitch_fire_on_trigger(glitch_params_t *p) {
  glitch_arm(p);

  if (wait_trigger(5000)) {
    /* Wait for one-pulse to finish */
    while (TIM1->CR1 & 1)
      ;
    uart_pc_puts("[GLITCH] Triggered and fired\n");
  } else {
    p->armed = 0;
    led_off();
    uart_pc_puts("[GLITCH] Timeout — no trigger\n");
  }
}

/* ============================================================
 *  Target control
 * ============================================================ */
void target_reset(void) {
  /* Assert nRST (low) */
  GPIOA->BSRR = (1 << (PIN_NRST + 16)); /* PA1 low */
  delay_ms(50);
  /* Release nRST (high via open-drain pullup) */
  GPIOA->BSRR = (1 << PIN_NRST); /* PA1 high */
  delay_ms(50);
}

void target_power_on(void) {
  /* If you have power control MOSFET, toggle it here */
  /* For now just reset */
  target_reset();
}

void target_enter_isp(void) {
  /* LPC1768 ISP entry:
   * Hold P2.10 (ISP enable) LOW during reset.
   * If connected, pull ISP pin low, then reset.
   * Here we just reset — assumes ISP pin is hardwired LOW */
  uart_pc_puts("[TARGET] Entering ISP mode (reset)\n");
  target_reset();
  delay_ms(100);

  /* Sync with ISP bootloader: send '?' until we get "Synchronized" */
  for (int i = 0; i < 50; i++) {
    uart_target_putc('?');
    delay_ms(20);
    char buf[64];
    int n = uart_target_read_line(buf, sizeof(buf), 100);
    if (n > 4 && buf[0] == 'S') { /* "Synchronized" */
      uart_pc_puts("[TARGET] ISP sync: ");
      uart_pc_puts(buf);
      uart_pc_puts("\n");

      /* Respond with "Synchronized\r\n" */
      uart_target_puts("Synchronized\r\n");
      delay_ms(20);

      /* Read echo + OK */
      uart_target_read_line(buf, sizeof(buf), 100);
      uart_target_read_line(buf, sizeof(buf), 100);

      /* Send crystal frequency in kHz */
      uart_target_puts("12000\r\n");
      delay_ms(20);
      uart_target_read_line(buf, sizeof(buf), 100);
      uart_target_read_line(buf, sizeof(buf), 100);

      uart_pc_puts("[TARGET] ISP ready\n");
      return;
    }
  }
  uart_pc_puts("[TARGET] ISP sync FAILED\n");
}